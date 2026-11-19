#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/newminhh_abi.h"
#include "../common/newminhh_log.h"

typedef enum {
    NM_DL_NATIVE_WRAPPED = 1,
    NM_DL_GUEST_DBT      = 2,
    NM_DL_PATCHED_X32    = 3
} nm_dl_kind_t;

typedef struct {
    uint32_t magic;
    nm_dl_kind_t kind;
    void *real_handle;
    int cdecl_mode;
    char name[256];
} nm_dl_handle_t;

#define NM_DL_HANDLE_MAGIC 0x4E4D444Cu /* "NMDL" */
#define NM_DL_MAX_HANDLES  128

static nm_dl_backend_t g_dl_backend = {0};
static pthread_mutex_t g_dl_lock = PTHREAD_MUTEX_INITIALIZER;
static nm_dl_handle_t *g_handles[NM_DL_MAX_HANDLES];

static __thread char tls_dlerror_buf[512];
static __thread int tls_dlerror_pending = 0;

static void set_dlerror(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tls_dlerror_buf, sizeof(tls_dlerror_buf), fmt, ap);
    va_end(ap);
    tls_dlerror_pending = 1;
    NM_LOG_ERROR("dl", "%s", tls_dlerror_buf);
}

void newminhh_register_dl_backend(const nm_dl_backend_t *backend) {
    pthread_mutex_lock(&g_dl_lock);
    if (backend) {
        g_dl_backend = *backend;
    } else {
        memset(&g_dl_backend, 0, sizeof(g_dl_backend));
    }
    pthread_mutex_unlock(&g_dl_lock);
}

static const char *map_canonical_system_lib(const char *filename) {
    if (!filename) return NULL;
    const char *base = strrchr(filename, '/');
    base = base ? (base + 1) : filename;

    if (strcmp(base, "libc.so.6") == 0 || strcmp(base, "libc.so") == 0) {
        return "libc.so.6";
    }
    if (strcmp(base, "libm.so.6") == 0 || strcmp(base, "libm.so") == 0) {
        return "libm.so.6";
    }
    if (strcmp(base, "libpthread.so.0") == 0 || strcmp(base, "libpthread.so") == 0) {
        return "libpthread.so.0";
    }
    if (strcmp(base, "libdl.so.2") == 0 || strcmp(base, "libdl.so") == 0) {
        return "libdl.so.2";
    }
    if (strcmp(base, "librt.so.1") == 0 || strcmp(base, "librt.so") == 0) {
        return "librt.so.1";
    }
    if (strcmp(base, "libgcc_s.so.1") == 0) {
        return "libgcc_s.so.1";
    }
    if (strcmp(base, "libstdc++.so.6") == 0) {
        return "libstdc++.so.6";
    }
    return NULL;
}

static int is_file_elf32_i386(const char *path) {
    if (!path) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    Elf32_Ehdr eh;
    ssize_t n = read(fd, &eh, sizeof(eh));
    close(fd);
    if (n != (ssize_t)sizeof(eh)) return 0;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) return 0;
    return (eh.e_ident[EI_CLASS] == ELFCLASS32 && eh.e_machine == EM_386);
}

static nm_dl_handle_t *register_handle(nm_dl_kind_t kind, void *real_handle,
                                       const char *name, int cdecl_mode) {
    nm_dl_handle_t *h = (nm_dl_handle_t *)calloc(1, sizeof(*h));
    if (!h) {
        set_dlerror("newminhh_compat: out of memory allocating dl handle");
        return NULL;
    }
    h->magic = NM_DL_HANDLE_MAGIC;
    h->kind = kind;
    h->real_handle = real_handle;
    h->cdecl_mode = cdecl_mode;
    if (name) {
        snprintf(h->name, sizeof(h->name), "%s", name);
    }

    pthread_mutex_lock(&g_dl_lock);
    int placed = 0;
    for (int i = 0; i < NM_DL_MAX_HANDLES; i++) {
        if (g_handles[i] == NULL) {
            g_handles[i] = h;
            placed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_dl_lock);
    if (!placed) {
        free(h);
        set_dlerror("newminhh_compat: dl handle table full (%d)", NM_DL_MAX_HANDLES);
        return NULL;
    }
    return h;
}

static nm_dl_handle_t *validate_handle(void *raw) {
    if (!raw) return NULL;
    pthread_mutex_lock(&g_dl_lock);
    for (int i = 0; i < NM_DL_MAX_HANDLES; i++) {
        if (g_handles[i] == raw && g_handles[i]->magic == NM_DL_HANDLE_MAGIC) {
            nm_dl_handle_t *h = g_handles[i];
            pthread_mutex_unlock(&g_dl_lock);
            return h;
        }
    }
    pthread_mutex_unlock(&g_dl_lock);
    return NULL;
}

void *compat_dlopen(const char *filename, int flags) {
    int cdecl_mode = (flags & NM_RTLD_CDECL) ? 1 : 0;
    int real_flags = flags & ~NM_RTLD_CDECL;
    if ((real_flags & (RTLD_LAZY | RTLD_NOW)) == 0) {
        real_flags |= RTLD_NOW;
    }

    if (filename == NULL) {
        void *rh = dlopen(NULL, real_flags);
        if (!rh) {
            const char *e = dlerror();
            set_dlerror("dlopen(NULL) failed: %s", e ? e : "unknown error");
            return NULL;
        }
        void *h = register_handle(NM_DL_PATCHED_X32, rh, "<main>", cdecl_mode);
        if (!h) {
            dlclose(rh);
        }
        return h;
    }

    if (filename[0] == '\0') {
        set_dlerror("compat_dlopen: empty library filename");
        return NULL;
    }

    const char *canon = map_canonical_system_lib(filename);
    if (canon) {
        void *rh = dlopen(canon, real_flags);
        if (!rh) {
            const char *e = dlerror();
            set_dlerror("compat_dlopen(%s -> %s) failed: %s",
                        filename, canon, e ? e : "unknown error");
            return NULL;
        }
        void *h = register_handle(NM_DL_NATIVE_WRAPPED, rh, canon, cdecl_mode);
        if (!h) {
            dlclose(rh);
        }
        return h;
    }

    /* Check if this is an unpatched i386 ELF32 .so */
    int is_i386 = is_file_elf32_i386(filename);
    if (!is_i386 && g_dl_backend.is_guest_elf_fn) {
        is_i386 = g_dl_backend.is_guest_elf_fn(filename);
    }
    if (is_i386) {
        pthread_mutex_lock(&g_dl_lock);
        nm_dl_backend_t backend = g_dl_backend;
        pthread_mutex_unlock(&g_dl_lock);
        if (!backend.dlopen_fn) {
            set_dlerror("compat_dlopen: cannot load unpatched i386 shared object '%s' (no DBT backend registered)",
                        filename);
            return NULL;
        }
        void *gh = backend.dlopen_fn(filename, real_flags);
        if (!gh) {
            const char *e = backend.dlerror_fn ? backend.dlerror_fn() : "DBT dlopen failed";
            set_dlerror("compat_dlopen(%s) DBT failed: %s", filename, e ? e : "unknown");
            return NULL;
        }
        void *h = register_handle(NM_DL_GUEST_DBT, gh, filename, cdecl_mode);
        if (!h && backend.dlclose_fn) {
            backend.dlclose_fn(gh);
        }
        return h;
    }

    /* Native or statically patched x32 .so */
    dlerror();
    void *rh = dlopen(filename, real_flags);
    if (!rh) {
        const char *e = dlerror();
        set_dlerror("compat_dlopen(%s) failed: %s", filename, e ? e : "cannot open shared object file");
        return NULL;
    }
    void *h = register_handle(NM_DL_PATCHED_X32, rh, filename, cdecl_mode);
    if (!h) {
        dlclose(rh);
    }
    return h;
}

static void *dlsym_internal(void *handle, const char *symbol, int force_cdecl) {
    nm_dl_handle_t *h = validate_handle(handle);
    if (!h) {
        set_dlerror("compat_dlsym: invalid or closed handle (%p) for symbol '%s'",
                    handle, symbol ? symbol : "(null)");
        return NULL;
    }
    if (!symbol || symbol[0] == '\0') {
        set_dlerror("compat_dlsym: NULL or empty symbol name (lib=%s)", h->name);
        return NULL;
    }

    if (h->kind == NM_DL_NATIVE_WRAPPED) {
        nm_sym_entry_t entry;
        if (newminhh_compat_lookup_wrapper(symbol, &entry) != NM_OK) {
            set_dlerror("compat_dlsym: unsupported native symbol '%s' in '%s' (no i386<->x32 ABI wrapper)",
                        symbol, h->name);
            return NULL;
        }
        if ((force_cdecl || h->cdecl_mode) && !(entry.flags & NM_SYM_FLAG_DATA)) {
            void *tramp = newminhh_alloc_out_trampoline_ex(entry.wrapper_fn, &entry.sig, entry.flags);
            if (!tramp) {
                set_dlerror("compat_dlsym: failed to allocate outgoing bridge for '%s'", symbol);
                return NULL;
            }
            return tramp;
        }
        return entry.wrapper_fn;
    }

    if (h->kind == NM_DL_GUEST_DBT) {
        pthread_mutex_lock(&g_dl_lock);
        nm_dl_backend_t backend = g_dl_backend;
        pthread_mutex_unlock(&g_dl_lock);
        if (!backend.dlsym_fn) {
            set_dlerror("compat_dlsym: DBT backend unavailable for '%s'", symbol);
            return NULL;
        }
        void *sym = backend.dlsym_fn(h->real_handle, symbol);
        if (!sym) {
            const char *e = backend.dlerror_fn ? backend.dlerror_fn() : "symbol not found";
            set_dlerror("compat_dlsym(%s, %s) failed: %s",
                        h->name, symbol, e ? e : "undefined symbol");
            return NULL;
        }
        return sym;
    }

    /* NM_DL_PATCHED_X32 */
    dlerror();
    void *sym = dlsym(h->real_handle, symbol);
    const char *e = dlerror();
    if (e || !sym) {
        set_dlerror("compat_dlsym(%s, %s) failed: %s",
                    h->name, symbol, e ? e : "undefined symbol");
        return NULL;
    }
    return sym;
}

void *compat_dlsym(void *handle, const char *symbol) {
    return dlsym_internal(handle, symbol, 0);
}

void *compat_dlsym_cdecl(void *handle, const char *symbol) {
    return dlsym_internal(handle, symbol, 1);
}

const char *compat_dlerror(void) {
    if (!tls_dlerror_pending) {
        return NULL;
    }
    tls_dlerror_pending = 0;
    return tls_dlerror_buf;
}

int compat_dlclose(void *handle) {
    if (!handle) {
        set_dlerror("compat_dlclose: NULL handle");
        return -1;
    }
    pthread_mutex_lock(&g_dl_lock);
    nm_dl_handle_t *h = NULL;
    int slot = -1;
    for (int i = 0; i < NM_DL_MAX_HANDLES; i++) {
        if (g_handles[i] == handle && g_handles[i]->magic == NM_DL_HANDLE_MAGIC) {
            h = g_handles[i];
            slot = i;
            g_handles[i] = NULL;
            break;
        }
    }
    nm_dl_backend_t backend = g_dl_backend;
    pthread_mutex_unlock(&g_dl_lock);

    if (!h || slot < 0) {
        set_dlerror("compat_dlclose: invalid or already-closed handle (%p)", handle);
        return -1;
    }

    int rc = 0;
    if (h->kind == NM_DL_GUEST_DBT) {
        if (backend.dlclose_fn) {
            rc = backend.dlclose_fn(h->real_handle);
        }
    } else if (h->real_handle) {
        rc = dlclose(h->real_handle);
    }
    if (rc != 0) {
        const char *e = dlerror();
        set_dlerror("compat_dlclose(%s) failed: %s", h->name, e ? e : "close error");
    }
    h->magic = 0;
    free(h);
    return rc;
}
