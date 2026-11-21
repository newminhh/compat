#define _GNU_SOURCE
#include <asm/prctl.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>

#include "../common/newminhh_abi.h"
#include "../common/newminhh_log.h"

/* i386 syscall numbers (asm/unistd_32.h) */
#define I386_NR_exit                1
#define I386_NR_read                3
#define I386_NR_write               4
#define I386_NR_open                5
#define I386_NR_close               6
#define I386_NR_lseek               19
#define I386_NR_getpid              20
#define I386_NR_getuid              24
#define I386_NR_dup                 41
#define I386_NR_pipe                42
#define I386_NR_brk                 45
#define I386_NR_getgid              47
#define I386_NR_geteuid             49
#define I386_NR_getegid             50
#define I386_NR_ioctl               54
#define I386_NR_fcntl               55
#define I386_NR_dup2                63
#define I386_NR_setrlimit           75
#define I386_NR_getrlimit           76
#define I386_NR_old_mmap            90
#define I386_NR_munmap              91
#define I386_NR_stat                106
#define I386_NR_lstat               107
#define I386_NR_fstat               108
#define I386_NR_uname               122
#define I386_NR_mprotect            125
#define I386_NR__llseek             140
#define I386_NR_readv               145
#define I386_NR_writev              146
#define I386_NR_nanosleep           162
#define I386_NR_rt_sigaction        174
#define I386_NR_rt_sigprocmask      175
#define I386_NR_ugetrlimit          191
#define I386_NR_mmap2               192
#define I386_NR_stat64              195
#define I386_NR_lstat64             196
#define I386_NR_fstat64             197
#define I386_NR_getuid32            199
#define I386_NR_getgid32            200
#define I386_NR_geteuid32           201
#define I386_NR_getegid32           202
#define I386_NR_fcntl64             221
#define I386_NR_gettid              224
#define I386_NR_futex               240
#define I386_NR_set_thread_area     243
#define I386_NR_get_thread_area     244
#define I386_NR_exit_group          252
#define I386_NR_set_tid_address     258
#define I386_NR_clock_gettime       265
#define I386_NR_clock_nanosleep     267
#define I386_NR_openat              295
#define I386_NR_fstatat64           300
#define I386_NR_set_robust_list     311
#define I386_NR_dup3                330
#define I386_NR_pipe2               331
#define I386_NR_prlimit64           340
#define I386_NR_getrandom           355
#define I386_NR_statx               383
#define I386_NR_clock_gettime64     403
#define I386_NR_clock_nanosleep_t64 407
#define I386_NR_futex_time64        422

static nm_brk_hook_t NM_NULLABLE_PTR g_brk_hook = nullptr;

static thread_local uint32_t tls_bases[3] = {};
static thread_local uint8_t  tls_used[3] = {};
static thread_local uint32_t tls_gs_base = 0;

void newminhh_register_brk_hook(nm_brk_hook_t NM_NULLABLE_PTR brk_hook) {
    g_brk_hook = brk_hook;
}

static inline int64_t nm_raw_syscall6(uint32_t x32_nr,
                                      uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6) {
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    register uint64_t r9  __asm__("r9")  = a6;
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)x32_nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static int futex_op_has_timeout(uint32_t op) {
    uint32_t cmd = op & (uint32_t)FUTEX_CMD_MASK;
    return (cmd == FUTEX_WAIT ||
            cmd == FUTEX_LOCK_PI ||
            cmd == FUTEX_WAIT_BITSET ||
            cmd == FUTEX_WAIT_REQUEUE_PI
#ifdef FUTEX_LOCK_PI2
            || cmd == FUTEX_LOCK_PI2
#endif
    );
}

static int32_t do_set_thread_area(uint32_t a1, newminhh_cpu_t *cpu) {
    if (a1 == 0) {
        NM_LOG_ERROR("syscall", "set_thread_area: NULL user_desc pointer");
        return -EFAULT;
    }
    newminhh_user_desc32_t desc;
    memcpy(&desc, (const void *)(uintptr_t)a1, sizeof(desc));

    int slot = -1;
    if (desc.entry_number == (uint32_t)-1) {
        for (int i = 0; i < 3; i++) {
            uint8_t used = cpu ? (cpu->tls_bases[i] != 0 || tls_used[i]) : tls_used[i];
            if (!used) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            /* Reuse slot 0 if all exhausted, or return -ESRCH; glibc allocates 1 slot per thread */
            return -ESRCH;
        }
        desc.entry_number = (uint32_t)(6 + slot);
        memcpy((void *)(uintptr_t)a1, &desc, sizeof(desc));
    } else if (desc.entry_number >= 6 && desc.entry_number <= 8) {
        slot = (int)(desc.entry_number - 6);
    } else {
        NM_LOG_ERROR("syscall", "set_thread_area: invalid entry_number=%u", desc.entry_number);
        return -EINVAL;
    }

    tls_used[slot] = 1;
    tls_bases[slot] = desc.base_addr;
    tls_gs_base = desc.base_addr;
    if (cpu) {
        cpu->tls_bases[slot] = desc.base_addr;
        cpu->gs_base = desc.base_addr;
    }

    int64_t r = nm_raw_syscall6(__NR_arch_prctl, ARCH_SET_GS, (uint64_t)desc.base_addr, 0, 0, 0, 0);
    if (r < 0) {
        NM_LOG_ERROR("syscall", "arch_prctl(ARCH_SET_GS, 0x%x) failed: %lld",
                     desc.base_addr, (long long)r);
        return (int32_t)r;
    }
    return 0;
}

static int32_t do_get_thread_area(uint32_t a1, newminhh_cpu_t *cpu) {
    if (a1 == 0) {
        return -EFAULT;
    }
    newminhh_user_desc32_t desc;
    memcpy(&desc, (const void *)(uintptr_t)a1, sizeof(desc));
    if (desc.entry_number < 6 || desc.entry_number > 8) {
        return -EINVAL;
    }
    int slot = (int)(desc.entry_number - 6);
    desc.base_addr = cpu ? cpu->tls_bases[slot] : tls_bases[slot];
    desc.limit = 0xFFFFFU;
    desc.flags = 0x51U;
    memcpy((void *)(uintptr_t)a1, &desc, sizeof(desc));
    return 0;
}

uint32_t newminhh_compat_get_tls_base(uint32_t selector_or_idx) {
    uint32_t idx = selector_or_idx;
    if (idx > 8) {
        idx >>= 3;
    }
    if (idx >= 6 && idx <= 8) {
        return tls_bases[idx - 6];
    }
    return tls_gs_base;
}

void newminhh_compat_set_gs_sel(uint32_t selector) {
    uint32_t idx = (selector >> 3);
    if (idx >= 6 && idx <= 8) {
        uint32_t base = tls_bases[idx - 6];
        tls_gs_base = base;
        nm_raw_syscall6(__NR_arch_prctl, ARCH_SET_GS, (uint64_t)base, 0, 0, 0, 0);
    }
}

static int32_t dispatch_syscall_internal(uint32_t nr, uint32_t a1, uint32_t a2,
                                         uint32_t a3, uint32_t a4, uint32_t a5,
                                         uint32_t a6, newminhh_cpu_t *cpu) {
    int64_t r = -ENOSYS;

    switch (nr) {
    case I386_NR_exit:
        r = nm_raw_syscall6(__NR_exit, (uint64_t)(int32_t)a1, 0, 0, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_exit_group:
        r = nm_raw_syscall6(__NR_exit_group, (uint64_t)(int32_t)a1, 0, 0, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_read:
        r = nm_raw_syscall6(__NR_read, (uint64_t)(int32_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_write:
        r = nm_raw_syscall6(__NR_write, (uint64_t)(int32_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_open:
        r = nm_raw_syscall6(__NR_open, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_openat:
        r = nm_raw_syscall6(__NR_openat, (uint64_t)(int32_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, 0, 0);
        return (int32_t)r;

    case I386_NR_close:
        r = nm_raw_syscall6(__NR_close, (uint64_t)(int32_t)a1, 0, 0, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_lseek:
        r = nm_raw_syscall6(__NR_lseek, (uint64_t)(int32_t)a1, (uint64_t)(int64_t)(int32_t)a2, (uint64_t)a3, 0, 0, 0);
        return (int32_t)r;

    case I386_NR__llseek: {
        if (a4 == 0) return -EFAULT;
        int64_t off = ((int64_t)(int32_t)a2 << 32) | (uint64_t)a3;
        r = nm_raw_syscall6(__NR_lseek, (uint64_t)(int32_t)a1, (uint64_t)off, (uint64_t)a5, 0, 0, 0);
        if (r < 0) return (int32_t)r;
        uint64_t res = (uint64_t)r;
        memcpy((void *)(uintptr_t)a4, &res, sizeof(res));
        return 0;
    }

    case I386_NR_stat:
    case I386_NR_lstat:
    case I386_NR_fstat: {
        if (a2 == 0) return -EFAULT;
        struct stat st;
        uint32_t xnr = (nr == I386_NR_stat) ? __NR_stat
                     : (nr == I386_NR_lstat) ? __NR_lstat : __NR_fstat;
        uint64_t arg1 = (nr == I386_NR_fstat) ? (uint64_t)(int32_t)a1 : (uint64_t)a1;
        r = nm_raw_syscall6(xnr, arg1, (uint64_t)(uintptr_t)&st, 0, 0, 0, 0);
        if (r < 0) return (int32_t)r;
        if (newminhh_repack_stat_to_kstat32(&st, (newminhh_kstat32_t *)(uintptr_t)a2) != NM_OK) {
            return -EOVERFLOW;
        }
        return 0;
    }

    case I386_NR_stat64:
    case I386_NR_lstat64:
    case I386_NR_fstat64: {
        if (a2 == 0) return -EFAULT;
        struct stat st;
        uint32_t xnr = (nr == I386_NR_stat64) ? __NR_stat
                     : (nr == I386_NR_lstat64) ? __NR_lstat : __NR_fstat;
        uint64_t arg1 = (nr == I386_NR_fstat64) ? (uint64_t)(int32_t)a1 : (uint64_t)a1;
        r = nm_raw_syscall6(xnr, arg1, (uint64_t)(uintptr_t)&st, 0, 0, 0, 0);
        if (r < 0) return (int32_t)r;
        if (newminhh_repack_stat_to_stat64(&st, (newminhh_stat64_t *)(uintptr_t)a2) != NM_OK) {
            return -EOVERFLOW;
        }
        return 0;
    }

    case I386_NR_fstatat64: {
        if (a3 == 0) return -EFAULT;
        struct stat st;
        r = nm_raw_syscall6(__NR_newfstatat, (uint64_t)(int32_t)a1, (uint64_t)a2,
                            (uint64_t)(uintptr_t)&st, (uint64_t)a4, 0, 0);
        if (r < 0) return (int32_t)r;
        if (newminhh_repack_stat_to_stat64(&st, (newminhh_stat64_t *)(uintptr_t)a3) != NM_OK) {
            return -EOVERFLOW;
        }
        return 0;
    }

    case I386_NR_statx:
        r = nm_raw_syscall6(__NR_statx, (uint64_t)(int32_t)a1, (uint64_t)a2,
                            (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, 0);
        return (int32_t)r;

    case I386_NR_brk:
        if (g_brk_hook) {
            return (int32_t)g_brk_hook(a1);
        }
        r = nm_raw_syscall6(__NR_brk, (uint64_t)a1, 0, 0, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_old_mmap: {
        if (a1 == 0) return -EFAULT;
        newminhh_mmap_arg_struct32_t ma;
        memcpy(&ma, (const void *)(uintptr_t)a1, sizeof(ma));
        r = nm_raw_syscall6(__NR_mmap, (uint64_t)ma.addr, (uint64_t)ma.len,
                            (uint64_t)ma.prot, (uint64_t)ma.flags,
                            (uint64_t)(int64_t)ma.fd, (uint64_t)ma.offset);
        return (int32_t)r;
    }

    case I386_NR_mmap2:
        r = nm_raw_syscall6(__NR_mmap, (uint64_t)a1, (uint64_t)a2,
                            (uint64_t)a3, (uint64_t)a4,
                            (uint64_t)(int64_t)(int32_t)a5,
                            ((uint64_t)a6) << 12);
        return (int32_t)r;

    case I386_NR_munmap:
        r = nm_raw_syscall6(__NR_munmap, (uint64_t)a1, (uint64_t)a2, 0, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_mprotect:
        r = nm_raw_syscall6(__NR_mprotect, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_getpid:
        r = nm_raw_syscall6(__NR_getpid, 0, 0, 0, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_getuid:
        r = nm_raw_syscall6(__NR_getuid, 0, 0, 0, 0, 0, 0);
        return (r < 0) ? (int32_t)r : (int32_t)((r > 0xFFFF) ? 0xFFFE : r);

    case I386_NR_getgid:
        r = nm_raw_syscall6(__NR_getgid, 0, 0, 0, 0, 0, 0);
        return (r < 0) ? (int32_t)r : (int32_t)((r > 0xFFFF) ? 0xFFFE : r);

    case I386_NR_geteuid:
        r = nm_raw_syscall6(__NR_geteuid, 0, 0, 0, 0, 0, 0);
        return (r < 0) ? (int32_t)r : (int32_t)((r > 0xFFFF) ? 0xFFFE : r);

    case I386_NR_getegid:
        r = nm_raw_syscall6(__NR_getegid, 0, 0, 0, 0, 0, 0);
        return (r < 0) ? (int32_t)r : (int32_t)((r > 0xFFFF) ? 0xFFFE : r);

    case I386_NR_getuid32:
        return (int32_t)nm_raw_syscall6(__NR_getuid, 0, 0, 0, 0, 0, 0);

    case I386_NR_getgid32:
        return (int32_t)nm_raw_syscall6(__NR_getgid, 0, 0, 0, 0, 0, 0);

    case I386_NR_geteuid32:
        return (int32_t)nm_raw_syscall6(__NR_geteuid, 0, 0, 0, 0, 0, 0);

    case I386_NR_getegid32:
        return (int32_t)nm_raw_syscall6(__NR_getegid, 0, 0, 0, 0, 0, 0);

    case I386_NR_gettid:
        return (int32_t)nm_raw_syscall6(__NR_gettid, 0, 0, 0, 0, 0, 0);

    case I386_NR_clock_gettime: {
        if (a2 == 0) return -EFAULT;
        struct timespec ts;
        r = nm_raw_syscall6(__NR_clock_gettime, (uint64_t)(int32_t)a1, (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0);
        if (r < 0) return (int32_t)r;
        if (newminhh_repack_timespec_to_32(&ts, (newminhh_timespec32_t *)(uintptr_t)a2) != NM_OK) {
            return -EOVERFLOW;
        }
        return 0;
    }

    case I386_NR_clock_gettime64: {
        if (a2 == 0) return -EFAULT;
        struct timespec ts;
        r = nm_raw_syscall6(__NR_clock_gettime, (uint64_t)(int32_t)a1, (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0);
        if (r < 0) return (int32_t)r;
        if (newminhh_repack_timespec_to_64t32(&ts, (newminhh_timespec64_t32_t *)(uintptr_t)a2) != NM_OK) {
            return -EINVAL;
        }
        return 0;
    }

    case I386_NR_nanosleep: {
        if (a1 == 0) return -EFAULT;
        struct timespec req, rem;
        if (newminhh_repack_timespec_from_32((const newminhh_timespec32_t *)(uintptr_t)a1, &req) != NM_OK) {
            return -EINVAL;
        }
        r = nm_raw_syscall6(__NR_nanosleep, (uint64_t)(uintptr_t)&req,
                            a2 ? (uint64_t)(uintptr_t)&rem : 0, 0, 0, 0, 0);
        if (a2 && r == -EINTR) {
            newminhh_repack_timespec_to_32(&rem, (newminhh_timespec32_t *)(uintptr_t)a2);
        }
        return (int32_t)r;
    }

    case I386_NR_clock_nanosleep: {
        if (a3 == 0) return -EFAULT;
        struct timespec req, rem;
        if (newminhh_repack_timespec_from_32((const newminhh_timespec32_t *)(uintptr_t)a3, &req) != NM_OK) {
            return -EINVAL;
        }
        r = nm_raw_syscall6(__NR_clock_nanosleep, (uint64_t)(int32_t)a1, (uint64_t)a2,
                            (uint64_t)(uintptr_t)&req,
                            a4 ? (uint64_t)(uintptr_t)&rem : 0, 0, 0);
        if (a4 && r == -EINTR) {
            newminhh_repack_timespec_to_32(&rem, (newminhh_timespec32_t *)(uintptr_t)a4);
        }
        return (int32_t)r;
    }

    case I386_NR_clock_nanosleep_t64: {
        if (a3 == 0) return -EFAULT;
        struct timespec req, rem;
        if (newminhh_repack_timespec_from_64t32((const newminhh_timespec64_t32_t *)(uintptr_t)a3, &req) != NM_OK) {
            return -EINVAL;
        }
        r = nm_raw_syscall6(__NR_clock_nanosleep, (uint64_t)(int32_t)a1, (uint64_t)a2,
                            (uint64_t)(uintptr_t)&req,
                            a4 ? (uint64_t)(uintptr_t)&rem : 0, 0, 0);
        if (a4 && r == -EINTR) {
            newminhh_repack_timespec_to_64t32(&rem, (newminhh_timespec64_t32_t *)(uintptr_t)a4);
        }
        return (int32_t)r;
    }

    case I386_NR_getrlimit:
    case I386_NR_ugetrlimit: {
        if (a2 == 0) return -EFAULT;
        struct rlimit rl;
        r = nm_raw_syscall6(__NR_getrlimit, (uint64_t)a1, (uint64_t)(uintptr_t)&rl, 0, 0, 0, 0);
        if (r < 0) return (int32_t)r;
        newminhh_rlimit32_t rl32;
        if (newminhh_repack_rlimit_to_32(&rl, &rl32) != NM_OK) return -EINVAL;
        if (nr == I386_NR_getrlimit) {
            if (rl32.rlim_cur > 0x7FFFFFFFU) rl32.rlim_cur = 0x7FFFFFFFU;
            if (rl32.rlim_max > 0x7FFFFFFFU) rl32.rlim_max = 0x7FFFFFFFU;
        }
        memcpy((void *)(uintptr_t)a2, &rl32, sizeof(rl32));
        return 0;
    }

    case I386_NR_setrlimit: {
        if (a2 == 0) return -EFAULT;
        struct rlimit rl;
        if (newminhh_repack_rlimit_from_32((const newminhh_rlimit32_t *)(uintptr_t)a2, &rl) != NM_OK) {
            return -EINVAL;
        }
        r = nm_raw_syscall6(__NR_setrlimit, (uint64_t)a1, (uint64_t)(uintptr_t)&rl, 0, 0, 0, 0);
        return (int32_t)r;
    }

    case I386_NR_prlimit64: {
        struct rlimit new_rl, old_rl;
        uint64_t p_new = 0, p_old = 0;
        if (a3) {
            if (newminhh_repack_rlimit_from_64t32((const newminhh_rlimit64_t32_t *)(uintptr_t)a3, &new_rl) != NM_OK) {
                return -EINVAL;
            }
            p_new = (uint64_t)(uintptr_t)&new_rl;
        }
        if (a4) {
            p_old = (uint64_t)(uintptr_t)&old_rl;
        }
        r = nm_raw_syscall6(__NR_prlimit64, (uint64_t)(int32_t)a1, (uint64_t)a2, p_new, p_old, 0, 0);
        if (r < 0) return (int32_t)r;
        if (a4) {
            newminhh_repack_rlimit_to_64t32(&old_rl, (newminhh_rlimit64_t32_t *)(uintptr_t)a4);
        }
        return 0;
    }

    case I386_NR_set_thread_area:
        return do_set_thread_area(a1, cpu);

    case I386_NR_get_thread_area:
        return do_get_thread_area(a1, cpu);

    case I386_NR_uname:
        if (a1 == 0) return -EFAULT;
        r = nm_raw_syscall6(__NR_uname, (uint64_t)a1, 0, 0, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_ioctl:
        r = nm_raw_syscall6(__NR_ioctl, (uint64_t)(int32_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_fcntl:
    case I386_NR_fcntl64:
        r = nm_raw_syscall6(__NR_fcntl, (uint64_t)(int32_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);
        return (int32_t)r;

    case I386_NR_dup:
        return (int32_t)nm_raw_syscall6(__NR_dup, (uint64_t)(int32_t)a1, 0, 0, 0, 0, 0);

    case I386_NR_dup2:
        return (int32_t)nm_raw_syscall6(__NR_dup2, (uint64_t)(int32_t)a1, (uint64_t)(int32_t)a2, 0, 0, 0, 0);

    case I386_NR_dup3:
        return (int32_t)nm_raw_syscall6(__NR_dup3, (uint64_t)(int32_t)a1, (uint64_t)(int32_t)a2, (uint64_t)a3, 0, 0, 0);

    case I386_NR_pipe:
        return (int32_t)nm_raw_syscall6(__NR_pipe2, (uint64_t)a1, 0, 0, 0, 0, 0);

    case I386_NR_pipe2:
        return (int32_t)nm_raw_syscall6(__NR_pipe2, (uint64_t)a1, (uint64_t)a2, 0, 0, 0, 0);

    case I386_NR_readv:
        return (int32_t)nm_raw_syscall6(__NR_readv, (uint64_t)(int32_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);

    case I386_NR_writev:
        return (int32_t)nm_raw_syscall6(__NR_writev, (uint64_t)(int32_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);

    case I386_NR_rt_sigaction:
        return (int32_t)nm_raw_syscall6(__NR_rt_sigaction, (uint64_t)(int32_t)a1, (uint64_t)a2,
                                        (uint64_t)a3, (uint64_t)a4, 0, 0);

    case I386_NR_rt_sigprocmask:
        return (int32_t)nm_raw_syscall6(__NR_rt_sigprocmask, (uint64_t)(int32_t)a1, (uint64_t)a2,
                                        (uint64_t)a3, (uint64_t)a4, 0, 0);

    case I386_NR_futex:
    case I386_NR_futex_time64: {
        uint64_t arg4 = (uint64_t)a4;
        struct timespec xts;
        if (a4 && futex_op_has_timeout(a2)) {
            int rc = (nr == I386_NR_futex)
                   ? newminhh_repack_timespec_from_32((const newminhh_timespec32_t *)(uintptr_t)a4, &xts)
                   : newminhh_repack_timespec_from_64t32((const newminhh_timespec64_t32_t *)(uintptr_t)a4, &xts);
            if (rc != NM_OK) return -EINVAL;
            arg4 = (uint64_t)(uintptr_t)&xts;
        }
        return (int32_t)nm_raw_syscall6(__NR_futex, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3,
                                        arg4, (uint64_t)a5, (uint64_t)a6);
    }

    case I386_NR_getrandom:
        return (int32_t)nm_raw_syscall6(__NR_getrandom, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, 0, 0, 0);

    case I386_NR_set_tid_address:
        return (int32_t)nm_raw_syscall6(__NR_set_tid_address, (uint64_t)a1, 0, 0, 0, 0, 0);

    case I386_NR_set_robust_list:
        return (int32_t)nm_raw_syscall6(__NR_set_robust_list, (uint64_t)a1, (uint64_t)a2, 0, 0, 0, 0);

    default:
        NM_LOG_ERROR("syscall", "unsupported i386 syscall nr=%u (args: 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x)",
                     nr, a1, a2, a3, a4, a5, a6);
        return -ENOSYS;
    }
}

int32_t newminhh_compat_syscall(uint32_t nr, uint32_t a1, uint32_t a2,
                                uint32_t a3, uint32_t a4, uint32_t a5,
                                uint32_t a6) {
    return dispatch_syscall_internal(nr, a1, a2, a3, a4, a5, a6, nullptr);
}

int32_t newminhh_compat_syscall_cpu(newminhh_cpu_t *cpu) {
    if (!cpu) {
        NM_LOG_ERROR("syscall", "newminhh_compat_syscall_cpu: NULL cpu pointer");
        return -EINVAL;
    }
    int32_t ret = dispatch_syscall_internal(cpu->gpr[NM_REG_EAX],
                                            cpu->gpr[NM_REG_EBX],
                                            cpu->gpr[NM_REG_ECX],
                                            cpu->gpr[NM_REG_EDX],
                                            cpu->gpr[NM_REG_ESI],
                                            cpu->gpr[NM_REG_EDI],
                                            cpu->gpr[NM_REG_EBP],
                                            cpu);
    cpu->gpr[NM_REG_EAX] = (uint32_t)ret;
    return ret;
}
