#define _GNU_SOURCE
#include <asm/prctl.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../common/newminhh_abi.h"
#include "../common/newminhh_log.h"

extern void newminhh_cb_common(void);
extern void newminhh_out_common(void);
extern uint64_t newminhh_invoke_cdecl_asm(uintptr_t target_fn,
                                          const uint32_t *cdecl_words,
                                          int nwords,
                                          uint8_t ret_kind,
                                          double *out_fp);

static nm_guest_invoker_t g_guest_invoker = NULL;

void newminhh_register_guest_invoker(nm_guest_invoker_t invoker) {
    g_guest_invoker = invoker;
}

nm_guest_invoker_t newminhh_get_guest_invoker(void) {
    return g_guest_invoker;
}

#define NM_NATIVE_RANGE_CAP 128

typedef struct {
    uint32_t start;
    uint32_t end;
} nm_native_range_t;

static pthread_mutex_t g_native_ranges_lock = PTHREAD_MUTEX_INITIALIZER;
static nm_native_range_t g_native_ranges[NM_NATIVE_RANGE_CAP];
static int g_native_range_count = 0;

void newminhh_register_native_cdecl_range(uint32_t start_va, uint32_t end_va) {
    if (start_va == 0 || start_va >= end_va) {
        return;
    }
    pthread_mutex_lock(&g_native_ranges_lock);
    for (int i = 0; i < g_native_range_count; i++) {
        if (g_native_ranges[i].start == start_va && g_native_ranges[i].end == end_va) {
            pthread_mutex_unlock(&g_native_ranges_lock);
            return;
        }
    }
    if (g_native_range_count < NM_NATIVE_RANGE_CAP) {
        g_native_ranges[g_native_range_count].start = start_va;
        g_native_ranges[g_native_range_count].end = end_va;
        g_native_range_count++;
    } else {
        NM_LOG_ERROR("callbacks", "native cdecl range table full (%d)", NM_NATIVE_RANGE_CAP);
    }
    pthread_mutex_unlock(&g_native_ranges_lock);
}

void newminhh_unregister_native_cdecl_range(uint32_t start_va, uint32_t end_va) {
    pthread_mutex_lock(&g_native_ranges_lock);
    for (int i = 0; i < g_native_range_count; i++) {
        if (g_native_ranges[i].start == start_va && g_native_ranges[i].end == end_va) {
            g_native_ranges[i] = g_native_ranges[g_native_range_count - 1];
            g_native_range_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_native_ranges_lock);
}

int newminhh_is_native_cdecl_va(uint32_t va) {
    if (va == 0) {
        return 0;
    }
    pthread_mutex_lock(&g_native_ranges_lock);
    int found = 0;
    for (int i = 0; i < g_native_range_count; i++) {
        if (va >= g_native_ranges[i].start && va < g_native_ranges[i].end) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_native_ranges_lock);
    return found;
}

int newminhh_mode_for_va(uint32_t guest_fn_va) {
    if (newminhh_is_native_cdecl_va(guest_fn_va)) {
        return NM_CB_MODE_NATIVE_CDECL;
    }
    return (g_guest_invoker != NULL) ? NM_CB_MODE_DBT : NM_CB_MODE_NATIVE_CDECL;
}

/* --- Trampoline Pools (16B slots, pre-populated W^X) --- */

#define NM_CB_POOL_CAP  512
#define NM_OUT_POOL_CAP 256

typedef struct {
    uint32_t guest_fn_va;
    nm_fn_sig_t sig;
    int mode;
    uint32_t flags;
    int in_use;
} nm_cb_slot_t;

typedef struct {
    void *target_x32_fn;
    nm_fn_sig_t sig;
    uint32_t flags;
    int in_use;
} nm_out_slot_t;

static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static nm_cb_slot_t g_cb_slots[NM_CB_POOL_CAP];
static uint8_t *g_cb_pool_page = NULL;
static uint32_t g_cb_pool_next = 0;

static nm_out_slot_t g_out_slots[NM_OUT_POOL_CAP];
static uint8_t *g_out_pool_page = NULL;
static uint32_t g_out_pool_next = 0;

static void emit_16b_slot(uint8_t *slot, uint32_t slot_id, uintptr_t common_target) {
    /* movl $slot_id, %r11d  (6B: 41 bb <u32>) */
    slot[0] = 0x41;
    slot[1] = 0xBB;
    memcpy(&slot[2], &slot_id, 4);

    int64_t rel = (int64_t)common_target - (int64_t)((uintptr_t)slot + 11);
    if (rel >= INT32_MIN && rel <= INT32_MAX) {
        int32_t rel32 = (int32_t)rel;
        /* jmp rel32 (5B: e9 <i32>) + 5x nop */
        slot[6] = 0xE9;
        memcpy(&slot[7], &rel32, 4);
        slot[11] = 0x90;
        slot[12] = 0x90;
        slot[13] = 0x90;
        slot[14] = 0x90;
        slot[15] = 0x90;
    } else {
        uint32_t addr32 = (uint32_t)common_target;
        /* movl $addr32, %r10d (6B: 41 ba <u32>) + jmp *%r10 (3B: 41 ff e2) + nop */
        slot[6] = 0x41;
        slot[7] = 0xBA;
        memcpy(&slot[8], &addr32, 4);
        slot[12] = 0x41;
        slot[13] = 0xFF;
        slot[14] = 0xE2;
        slot[15] = 0x90;
    }
}

static uint8_t *alloc_exec_pool_pages(uint32_t cap, uintptr_t common_target) {
    size_t sz = (size_t)cap * 16u;
    size_t page_sz = (sz + 4095u) & ~(size_t)4095u;
    /* Hint near common_target so rel32 jmp reaches in 100% of cases */
    void *hint = (void *)((common_target + 0x100000u) & ~(uintptr_t)0xFFFu);
    uint8_t *mem = (uint8_t *)mmap(hint, page_sz, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        mem = (uint8_t *)mmap(NULL, page_sz, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (mem == MAP_FAILED) {
        NM_LOG_ERROR("callbacks", "mmap trampoline pool failed: errno=%d", errno);
        return NULL;
    }
    for (uint32_t i = 0; i < cap; i++) {
        emit_16b_slot(mem + (size_t)i * 16u, i, common_target);
    }
    if (mprotect(mem, page_sz, PROT_READ | PROT_EXEC) != 0) {
        NM_LOG_ERROR("callbacks", "mprotect(PROT_READ|PROT_EXEC) failed: errno=%d", errno);
        munmap(mem, page_sz);
        return NULL;
    }
    return mem;
}

static int invoke_guest_fn(uint32_t guest_fn_va, const nm_fn_sig_t *sig,
                           const uint32_t *cdecl_words, int nwords,
                           int mode, uint64_t *out_gpr, double *out_fp) {
    if (guest_fn_va == 0) {
        NM_LOG_ERROR("callbacks", "attempted to invoke NULL guest function pointer");
        return NM_ERR_ABI;
    }
    if (mode == NM_CB_MODE_DBT) {
        if (!g_guest_invoker) {
            NM_LOG_ERROR("callbacks", "DBT guest invoker not registered for guest_fn=0x%x",
                         guest_fn_va);
            return NM_ERR_ABI;
        }
        return g_guest_invoker(guest_fn_va, sig, cdecl_words, nwords, out_gpr, out_fp);
    }
    uint8_t rk = sig ? sig->ret_kind : NM_RET_U32;
    double fp_local = 0.0;
    uint64_t gpr = newminhh_invoke_cdecl_asm((uintptr_t)guest_fn_va, cdecl_words, nwords,
                                             rk, out_fp ? out_fp : &fp_local);
    if (out_gpr) {
        *out_gpr = gpr;
    }
    return NM_OK;
}

static int marshal_x32_to_cdecl_words(const nm_fn_sig_t *sig,
                                      const uint64_t *x32_gprs,
                                      const uint64_t *x32_xmms,
                                      const uint64_t *x32_stack,
                                      uint32_t flags,
                                      uint32_t *out_words,
                                      int max_words) {
    int nwords = 0;
    if ((flags & NM_CB_FLAG_UNTYPED) || (sig == NULL)) {
        for (int i = 0; i < 6 && nwords < max_words; i++) {
            out_words[nwords++] = (uint32_t)x32_gprs[i];
        }
        return nwords;
    }

    int gpr_idx = 0;
    int xmm_idx = 0;
    int stk_idx = 0;
    int nargs = sig->nargs;
    if (nargs > NM_MAX_SIG_ARGS) nargs = NM_MAX_SIG_ARGS;

    for (int i = 0; i < nargs; i++) {
        uint8_t k = sig->arg_kinds[i];
        uint64_t val = 0;
        if (k == NM_ARG_F32 || k == NM_ARG_F64) {
            if (xmm_idx < 8) {
                val = x32_xmms[xmm_idx++];
            } else {
                val = x32_stack ? x32_stack[stk_idx++] : 0;
            }
            if (k == NM_ARG_F32) {
                if (nwords + 1 > max_words) break;
                out_words[nwords++] = (uint32_t)val;
            } else {
                if (nwords + 2 > max_words) break;
                out_words[nwords++] = (uint32_t)(val & 0xFFFFFFFFULL);
                out_words[nwords++] = (uint32_t)(val >> 32);
            }
        } else {
            if (gpr_idx < 6) {
                val = x32_gprs[gpr_idx++];
            } else {
                val = x32_stack ? x32_stack[stk_idx++] : 0;
            }
            if (k == NM_ARG_I64 || k == NM_ARG_U64) {
                if (nwords + 2 > max_words) break;
                out_words[nwords++] = (uint32_t)(val & 0xFFFFFFFFULL);
                out_words[nwords++] = (uint32_t)(val >> 32);
            } else {
                if (nwords + 1 > max_words) break;
                out_words[nwords++] = (uint32_t)val;
            }
        }
    }
    return nwords;
}

uint64_t newminhh_in_bridge(uint32_t guest_fn_va,
                            const nm_fn_sig_t *sig,
                            const uint64_t *x32_gprs,
                            const uint64_t *x32_xmms,
                            const uint64_t *x32_stack,
                            int mode,
                            double *out_fp_ret) {
    uint32_t words[32];
    uint64_t dummy_gprs[6] = {0};
    uint64_t dummy_xmms[8] = {0};
    int nwords = marshal_x32_to_cdecl_words(
        sig,
        x32_gprs ? x32_gprs : dummy_gprs,
        x32_xmms ? x32_xmms : dummy_xmms,
        x32_stack,
        (sig == NULL) ? NM_CB_FLAG_UNTYPED : 0,
        words, 32);
    uint64_t ret = 0;
    invoke_guest_fn(guest_fn_va, sig, words, nwords, mode, &ret, out_fp_ret);
    return ret;
}

uint64_t newminhh_cb_dispatch(uint32_t slot_id,
                              const uint64_t *gprs,
                              const uint64_t *xmms,
                              const uint64_t *caller_stack,
                              double *out_fp,
                              uint32_t *out_ret_kind) {
    if (slot_id >= NM_CB_POOL_CAP || !g_cb_slots[slot_id].in_use) {
        NM_LOG_ERROR("callbacks", "invalid callback slot_id=%u", slot_id);
        if (out_ret_kind) *out_ret_kind = NM_RET_U32;
        return 0;
    }
    nm_cb_slot_t slot = g_cb_slots[slot_id];
    if (out_ret_kind) {
        *out_ret_kind = slot.sig.ret_kind;
    }
    uint32_t words[32];
    int nwords = marshal_x32_to_cdecl_words(&slot.sig, gprs, xmms, caller_stack,
                                             slot.flags, words, 32);
    uint64_t ret = 0;
    invoke_guest_fn(slot.guest_fn_va, &slot.sig, words, nwords, slot.mode, &ret, out_fp);
    return ret;
}

void newminhh_out_dispatch(uint32_t slot_id,
                           const uint32_t *cdecl_args,
                           uint64_t *out_val,
                           uint64_t *out_fp_bits,
                           uint32_t *out_ret_kind) {
    if (out_val) *out_val = 0;
    if (out_fp_bits) *out_fp_bits = 0;
    if (out_ret_kind) *out_ret_kind = NM_RET_U32;

    if (slot_id >= NM_OUT_POOL_CAP || !g_out_slots[slot_id].in_use) {
        NM_LOG_ERROR("callbacks", "invalid outgoing trampoline slot_id=%u", slot_id);
        return;
    }
    nm_out_slot_t slot = g_out_slots[slot_id];
    if (out_ret_kind) {
        *out_ret_kind = slot.sig.ret_kind;
    }
    if (slot.flags & NM_SYM_FLAG_CDECL_DIRECT) {
        typedef int (*cdecl_direct_fn_t)(const uint32_t *cdecl_args);
        cdecl_direct_fn_t fn = (cdecl_direct_fn_t)slot.target_x32_fn;
        int res = fn(cdecl_args);
        if (out_val) {
            *out_val = (uint64_t)(int64_t)res;
        }
        return;
    }
    uint64_t fp_bits = 0;
    uint64_t val = newminhh_call_x32_sig(slot.target_x32_fn, &slot.sig, cdecl_args, &fp_bits);
    if (out_val) *out_val = val;
    if (out_fp_bits) *out_fp_bits = fp_bits;
}

void *newminhh_alloc_cb_trampoline(uint32_t guest_fn_va,
                                   const nm_fn_sig_t *sig,
                                   int mode,
                                   uint32_t flags) {
    if (guest_fn_va == 0) {
        NM_LOG_ERROR("callbacks", "alloc_cb_trampoline: NULL guest_fn_va");
        return NULL;
    }
    pthread_mutex_lock(&g_pool_lock);
    if (!g_cb_pool_page) {
        g_cb_pool_page = alloc_exec_pool_pages(NM_CB_POOL_CAP, (uintptr_t)newminhh_cb_common);
        if (!g_cb_pool_page) {
            pthread_mutex_unlock(&g_pool_lock);
            return NULL;
        }
    }
    if (g_cb_pool_next >= NM_CB_POOL_CAP) {
        pthread_mutex_unlock(&g_pool_lock);
        NM_LOG_ERROR("callbacks", "callback trampoline pool exhausted (%u slots)", NM_CB_POOL_CAP);
        return NULL;
    }
    uint32_t id = g_cb_pool_next++;
    g_cb_slots[id].guest_fn_va = guest_fn_va;
    if (sig) {
        g_cb_slots[id].sig = *sig;
    } else {
        memset(&g_cb_slots[id].sig, 0, sizeof(nm_fn_sig_t));
        g_cb_slots[id].sig.ret_kind = NM_RET_U32;
        flags |= NM_CB_FLAG_UNTYPED;
    }
    g_cb_slots[id].mode = mode;
    g_cb_slots[id].flags = flags;
    g_cb_slots[id].in_use = 1;
    void *addr = g_cb_pool_page + (size_t)id * 16u;
    pthread_mutex_unlock(&g_pool_lock);
    return addr;
}

void *newminhh_alloc_out_trampoline_ex(void *target_x32_fn, const nm_fn_sig_t *sig, uint32_t flags) {
    if (!target_x32_fn || !sig) {
        NM_LOG_ERROR("callbacks", "alloc_out_trampoline: NULL target or sig");
        return NULL;
    }
    pthread_mutex_lock(&g_pool_lock);
    if (!g_out_pool_page) {
        g_out_pool_page = alloc_exec_pool_pages(NM_OUT_POOL_CAP, (uintptr_t)newminhh_out_common);
        if (!g_out_pool_page) {
            pthread_mutex_unlock(&g_pool_lock);
            return NULL;
        }
    }
    if (g_out_pool_next >= NM_OUT_POOL_CAP) {
        pthread_mutex_unlock(&g_pool_lock);
        NM_LOG_ERROR("callbacks", "outgoing trampoline pool exhausted (%u slots)", NM_OUT_POOL_CAP);
        return NULL;
    }
    uint32_t id = g_out_pool_next++;
    g_out_slots[id].target_x32_fn = target_x32_fn;
    g_out_slots[id].sig = *sig;
    g_out_slots[id].flags = flags;
    g_out_slots[id].in_use = 1;
    void *addr = g_out_pool_page + (size_t)id * 16u;
    pthread_mutex_unlock(&g_pool_lock);
    return addr;
}

void *newminhh_alloc_out_trampoline(void *target_x32_fn, const nm_fn_sig_t *sig) {
    return newminhh_alloc_out_trampoline_ex(target_x32_fn, sig, 0);
}

void *newminhh_wrap_init_array_fn(uint32_t guest_fn_va, int mode) {
    nm_fn_sig_t sig = {
        .ret_kind = NM_RET_VOID,
        .nargs = 3,
        .arg_kinds = {NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR}
    };
    return newminhh_alloc_cb_trampoline(guest_fn_va, &sig, mode, 0);
}

void *newminhh_wrap_fini_array_fn(uint32_t guest_fn_va, int mode) {
    nm_fn_sig_t sig = {
        .ret_kind = NM_RET_VOID,
        .nargs = 0
    };
    return newminhh_alloc_cb_trampoline(guest_fn_va, &sig, mode, 0);
}

void *newminhh_wrap_so_export(uint32_t guest_fn_va, const nm_fn_sig_t *sig, int mode) {
    return newminhh_alloc_cb_trampoline(guest_fn_va, sig, mode, sig ? 0 : NM_CB_FLAG_UNTYPED);
}

/* --- TLS-stack qsort / bsearch --- */

#define NM_QSORT_MAX_DEPTH 16
static __thread uint32_t tls_qsort_cbs[NM_QSORT_MAX_DEPTH];
static __thread int tls_qsort_modes[NM_QSORT_MAX_DEPTH];
static __thread int tls_qsort_depth = 0;

static int compat_qsort_trampoline(const void *a, const void *b) {
    int idx = tls_qsort_depth - 1;
    uint32_t cb = tls_qsort_cbs[idx];
    int mode = tls_qsort_modes[idx];
    uint32_t words[2] = {(uint32_t)(uintptr_t)a, (uint32_t)(uintptr_t)b};
    nm_fn_sig_t sig = {
        .ret_kind = NM_RET_I32,
        .nargs = 2,
        .arg_kinds = {NM_ARG_PTR, NM_ARG_PTR}
    };
    uint64_t ret = 0;
    invoke_guest_fn(cb, &sig, words, 2, mode, &ret, NULL);
    return (int)(int32_t)ret;
}

void compat_qsort(void *base, uint32_t nmemb, uint32_t size, uint32_t guest_compar_va) {
    if (nmemb == 0) return;
    if (!base || guest_compar_va == 0 || size == 0) {
        NM_LOG_ERROR("callbacks", "compat_qsort: invalid arguments (base=%p nmemb=%u size=%u cb=0x%x)",
                     base, nmemb, size, guest_compar_va);
        return;
    }
    if (tls_qsort_depth >= NM_QSORT_MAX_DEPTH) {
        NM_LOG_ERROR("callbacks", "compat_qsort: comparator nesting depth exceeded (%d)", NM_QSORT_MAX_DEPTH);
        return;
    }
    int idx = tls_qsort_depth++;
    tls_qsort_cbs[idx] = guest_compar_va;
    tls_qsort_modes[idx] = newminhh_mode_for_va(guest_compar_va);
    qsort(base, (size_t)nmemb, (size_t)size, compat_qsort_trampoline);
    tls_qsort_depth--;
}

void *compat_bsearch(const void *key, const void *base, uint32_t nmemb,
                     uint32_t size, uint32_t guest_compar_va) {
    if (nmemb == 0) return NULL;
    if (!key || !base || guest_compar_va == 0 || size == 0) {
        NM_LOG_ERROR("callbacks", "compat_bsearch: invalid arguments (key=%p base=%p cb=0x%x)",
                     key, base, guest_compar_va);
        return NULL;
    }
    if (tls_qsort_depth >= NM_QSORT_MAX_DEPTH) {
        NM_LOG_ERROR("callbacks", "compat_bsearch: comparator nesting depth exceeded (%d)", NM_QSORT_MAX_DEPTH);
        return NULL;
    }
    int idx = tls_qsort_depth++;
    tls_qsort_cbs[idx] = guest_compar_va;
    tls_qsort_modes[idx] = newminhh_mode_for_va(guest_compar_va);
    const void *res = bsearch(key, base, (size_t)nmemb, (size_t)size, compat_qsort_trampoline);
    tls_qsort_depth--;
    return (void *)(uintptr_t)res;
}

/* --- Heap-ctx pthread_create with GS base propagation --- */

typedef struct {
    uint32_t guest_fn_va;
    uint32_t guest_arg;
    uint32_t guest_gs_base;
    int mode;
} nm_pthread_ctx_t;

static void *nm_pthread_entry(void *raw_ctx) {
    nm_pthread_ctx_t ctx;
    memcpy(&ctx, raw_ctx, sizeof(ctx));
    free(raw_ctx);

    if (ctx.guest_gs_base != 0) {
        syscall(__NR_arch_prctl, ARCH_SET_GS, (uintptr_t)ctx.guest_gs_base);
    }

    uint32_t word = ctx.guest_arg;
    nm_fn_sig_t sig = {
        .ret_kind = NM_RET_PTR,
        .nargs = 1,
        .arg_kinds = {NM_ARG_PTR}
    };
    uint64_t ret = 0;
    invoke_guest_fn(ctx.guest_fn_va, &sig, &word, 1, ctx.mode, &ret, NULL);
    return (void *)(uintptr_t)(uint32_t)ret;
}

int compat_pthread_create(uint32_t *thread_out, const void *attr,
                          uint32_t start_routine_va, uint32_t arg) {
    (void)attr;
    if (!thread_out || start_routine_va == 0) {
        NM_LOG_ERROR("callbacks", "compat_pthread_create: invalid args (thread_out=%p fn=0x%x)",
                     (void *)thread_out, start_routine_va);
        return EINVAL;
    }
    nm_pthread_ctx_t *ctx = (nm_pthread_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) {
        return ENOMEM;
    }
    unsigned long cur_gs = 0;
    if (syscall(__NR_arch_prctl, ARCH_GET_GS, &cur_gs) != 0) {
        cur_gs = 0;
    }
    ctx->guest_fn_va = start_routine_va;
    ctx->guest_arg = arg;
    ctx->guest_gs_base = (uint32_t)cur_gs;
    ctx->mode = newminhh_mode_for_va(start_routine_va);

    pthread_t pt = 0;
    int rc = pthread_create(&pt, NULL, nm_pthread_entry, ctx);
    if (rc != 0) {
        free(ctx);
        NM_LOG_ERROR("callbacks", "pthread_create failed: rc=%d", rc);
        return rc;
    }
    *thread_out = (uint32_t)pt;
    return 0;
}

/* --- LIFO atexit / __cxa_atexit --- */

#define NM_ATEXIT_CAP 256

typedef struct {
    uint32_t fn_va;
    uint32_t arg;
    uint32_t dso_handle;
    int is_cxa;
    int mode;
    int called;
} nm_atexit_entry_t;

static pthread_mutex_t g_atexit_lock = PTHREAD_MUTEX_INITIALIZER;
static nm_atexit_entry_t g_atexit_list[NM_ATEXIT_CAP];
static int g_atexit_count = 0;
static int g_atexit_hook_registered = 0;

void newminhh_run_guest_atexits(void) {
    for (;;) {
        nm_atexit_entry_t entry;
        int found = 0;
        pthread_mutex_lock(&g_atexit_lock);
        for (int i = g_atexit_count - 1; i >= 0; i--) {
            if (!g_atexit_list[i].called) {
                g_atexit_list[i].called = 1;
                entry = g_atexit_list[i];
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_atexit_lock);
        if (!found) break;

        if (entry.is_cxa) {
            nm_fn_sig_t sig = {
                .ret_kind = NM_RET_VOID,
                .nargs = 1,
                .arg_kinds = {NM_ARG_PTR}
            };
            invoke_guest_fn(entry.fn_va, &sig, &entry.arg, 1, entry.mode, NULL, NULL);
        } else {
            nm_fn_sig_t sig = {
                .ret_kind = NM_RET_VOID,
                .nargs = 0
            };
            invoke_guest_fn(entry.fn_va, &sig, NULL, 0, entry.mode, NULL, NULL);
        }
    }
}

static int register_atexit_internal(uint32_t fn_va, uint32_t arg, uint32_t dso_handle, int is_cxa) {
    if (fn_va == 0) {
        NM_LOG_ERROR("callbacks", "atexit: NULL function pointer");
        return -1;
    }
    pthread_mutex_lock(&g_atexit_lock);
    if (!g_atexit_hook_registered) {
        if (atexit(newminhh_run_guest_atexits) != 0) {
            pthread_mutex_unlock(&g_atexit_lock);
            return -1;
        }
        g_atexit_hook_registered = 1;
    }
    if (g_atexit_count >= NM_ATEXIT_CAP) {
        pthread_mutex_unlock(&g_atexit_lock);
        NM_LOG_ERROR("callbacks", "guest atexit table full (%d entries)", NM_ATEXIT_CAP);
        return -1;
    }
    g_atexit_list[g_atexit_count++] = (nm_atexit_entry_t){
        .fn_va = fn_va,
        .arg = arg,
        .dso_handle = dso_handle,
        .is_cxa = is_cxa,
        .mode = newminhh_mode_for_va(fn_va),
        .called = 0
    };
    pthread_mutex_unlock(&g_atexit_lock);
    return 0;
}

int compat_atexit(uint32_t fn_va) {
    return register_atexit_internal(fn_va, 0, 0, 0);
}

int compat___cxa_atexit(uint32_t fn_va, uint32_t arg, uint32_t dso_handle) {
    return register_atexit_internal(fn_va, arg, dso_handle, 1);
}

void compat___cxa_finalize(uint32_t dso_handle) {
    for (;;) {
        nm_atexit_entry_t entry;
        int found = 0;
        pthread_mutex_lock(&g_atexit_lock);
        for (int i = g_atexit_count - 1; i >= 0; i--) {
            if (!g_atexit_list[i].called &&
                (dso_handle == 0 || g_atexit_list[i].dso_handle == dso_handle)) {
                g_atexit_list[i].called = 1;
                entry = g_atexit_list[i];
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_atexit_lock);
        if (!found) break;

        if (entry.is_cxa) {
            nm_fn_sig_t sig = {
                .ret_kind = NM_RET_VOID,
                .nargs = 1,
                .arg_kinds = {NM_ARG_PTR}
            };
            invoke_guest_fn(entry.fn_va, &sig, &entry.arg, 1, entry.mode, NULL, NULL);
        } else {
            nm_fn_sig_t sig = {
                .ret_kind = NM_RET_VOID,
                .nargs = 0
            };
            invoke_guest_fn(entry.fn_va, &sig, NULL, 0, entry.mode, NULL, NULL);
        }
    }
}

/* --- signal / sigaction --- */

typedef struct {
    uint32_t handler_va;
    int flags;
    int mode;
} nm_sig_slot_t;

static pthread_mutex_t g_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static nm_sig_slot_t g_sig_slots[NSIG];

static void nm_signal_trampoline(int signum, siginfo_t *info, void *uctx) {
    if (signum <= 0 || signum >= NSIG) return;
    nm_sig_slot_t slot = g_sig_slots[signum];
    if (slot.handler_va <= 1u) return;

    if (slot.flags & SA_SIGINFO) {
        uint32_t words[3] = {
            (uint32_t)signum,
            (uint32_t)(uintptr_t)info,
            (uint32_t)(uintptr_t)uctx
        };
        nm_fn_sig_t sig = {
            .ret_kind = NM_RET_VOID,
            .nargs = 3,
            .arg_kinds = {NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR}
        };
        invoke_guest_fn(slot.handler_va, &sig, words, 3, slot.mode, NULL, NULL);
    } else {
        uint32_t word = (uint32_t)signum;
        nm_fn_sig_t sig = {
            .ret_kind = NM_RET_VOID,
            .nargs = 1,
            .arg_kinds = {NM_ARG_I32}
        };
        invoke_guest_fn(slot.handler_va, &sig, &word, 1, slot.mode, NULL, NULL);
    }
}

uint32_t compat_signal(int signum, uint32_t handler_va) {
    if (signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return (uint32_t)(uintptr_t)SIG_ERR;
    }
    pthread_mutex_lock(&g_sig_lock);
    uint32_t prev = g_sig_slots[signum].handler_va;
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    if (handler_va == (uint32_t)(uintptr_t)SIG_DFL ||
        handler_va == (uint32_t)(uintptr_t)SIG_IGN) {
        sa.sa_handler = (void (*)(int))(uintptr_t)handler_va;
        g_sig_slots[signum].handler_va = handler_va;
        g_sig_slots[signum].flags = 0;
    } else {
        sa.sa_sigaction = nm_signal_trampoline;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        g_sig_slots[signum].handler_va = handler_va;
        g_sig_slots[signum].flags = 0;
        g_sig_slots[signum].mode = newminhh_mode_for_va(handler_va);
    }
    int rc = sigaction(signum, &sa, &old_sa);
    pthread_mutex_unlock(&g_sig_lock);
    if (rc != 0) {
        return (uint32_t)(uintptr_t)SIG_ERR;
    }
    if (prev != 0) return prev;
    return (uint32_t)(uintptr_t)old_sa.sa_handler;
}

int compat_sigaction(int signum, const void *act_raw, void *oldact_raw) {
    if (signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    const struct sigaction *act = (const struct sigaction *)act_raw;
    struct sigaction *oldact = (struct sigaction *)oldact_raw;

    pthread_mutex_lock(&g_sig_lock);
    nm_sig_slot_t prev_slot = g_sig_slots[signum];
    struct sigaction host_act, host_old;
    const struct sigaction *p_act = NULL;
    if (act) {
        host_act = *act;
        uint32_t hva = (uint32_t)(uintptr_t)((act->sa_flags & SA_SIGINFO)
                                              ? (void *)act->sa_sigaction
                                              : (void *)act->sa_handler);
        if (hva != (uint32_t)(uintptr_t)SIG_DFL &&
            hva != (uint32_t)(uintptr_t)SIG_IGN) {
            g_sig_slots[signum].handler_va = hva;
            g_sig_slots[signum].flags = act->sa_flags;
            g_sig_slots[signum].mode = newminhh_mode_for_va(hva);
            host_act.sa_sigaction = nm_signal_trampoline;
            host_act.sa_flags |= SA_SIGINFO;
        } else {
            g_sig_slots[signum].handler_va = hva;
            g_sig_slots[signum].flags = 0;
        }
        p_act = &host_act;
    }

    int rc = sigaction(signum, p_act, oldact ? &host_old : NULL);
    if (rc == 0 && oldact) {
        *oldact = host_old;
        if (prev_slot.handler_va > 1u && host_old.sa_sigaction == nm_signal_trampoline) {
            if (prev_slot.flags & SA_SIGINFO) {
                oldact->sa_sigaction = (void (*)(int, siginfo_t *, void *))(uintptr_t)prev_slot.handler_va;
            } else {
                oldact->sa_handler = (void (*)(int))(uintptr_t)prev_slot.handler_va;
            }
            oldact->sa_flags = prev_slot.flags;
        }
    }
    pthread_mutex_unlock(&g_sig_lock);
    return rc;
}
