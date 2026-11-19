#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "../common/newminhh_abi.h"
#include "../common/newminhh_log.h"

extern char **environ;
extern int optind, opterr, optopt;
extern char *optarg;

/* Data exports for i386 R_386_COPY / GOT references */
FILE *compat_stdin = NULL;
FILE *compat_stdout = NULL;
FILE *compat_stderr = NULL;
char *compat_optarg = NULL;
int compat_optind = 1;
int compat_opterr = 1;
int compat_optopt = '?';
char **compat_environ = NULL;

__attribute__((constructor))
static void compat_init_data_exports(void) {
    compat_stdin = stdin;
    compat_stdout = stdout;
    compat_stderr = stderr;
    compat_optarg = optarg;
    compat_optind = optind;
    compat_opterr = opterr;
    compat_optopt = optopt;
    compat_environ = environ;
}

void newminhh_get_stdio_streams(FILE **out_stdin, FILE **out_stdout, FILE **out_stderr) {
    if (!compat_stdin) compat_stdin = stdin;
    if (!compat_stdout) compat_stdout = stdout;
    if (!compat_stderr) compat_stderr = stderr;
    if (out_stdin) *out_stdin = compat_stdin;
    if (out_stdout) *out_stdout = compat_stdout;
    if (out_stderr) *out_stderr = compat_stderr;
}

/* --- Shadow maps for pthread_mutex_t (i386=24B vs x32=32B) and pthread_cond_t --- */

#define NM_MUTEX_MAP_CAP 256
typedef struct {
    const void *guest_ptr;
    pthread_mutex_t host_mutex;
    int in_use;
} nm_mutex_entry_t;

static pthread_mutex_t g_mutex_map_lock = PTHREAD_MUTEX_INITIALIZER;
static nm_mutex_entry_t g_mutex_map[NM_MUTEX_MAP_CAP];

static pthread_mutex_t *lookup_or_create_host_mutex(void *guest_mutex, int reinit,
                                                    const pthread_mutexattr_t *attr) {
    if (!guest_mutex) return NULL;
    pthread_mutex_lock(&g_mutex_map_lock);
    int free_idx = -1;
    for (int i = 0; i < NM_MUTEX_MAP_CAP; i++) {
        if (g_mutex_map[i].in_use) {
            if (g_mutex_map[i].guest_ptr == guest_mutex) {
                if (reinit) {
                    pthread_mutex_destroy(&g_mutex_map[i].host_mutex);
                    pthread_mutex_init(&g_mutex_map[i].host_mutex, attr);
                }
                pthread_mutex_t *res = &g_mutex_map[i].host_mutex;
                pthread_mutex_unlock(&g_mutex_map_lock);
                return res;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        pthread_mutex_unlock(&g_mutex_map_lock);
        NM_LOG_ERROR("wrappers", "pthread_mutex shadow table full");
        return NULL;
    }
    g_mutex_map[free_idx].guest_ptr = guest_mutex;
    g_mutex_map[free_idx].in_use = 1;
    pthread_mutex_init(&g_mutex_map[free_idx].host_mutex, attr);
    pthread_mutex_t *res = &g_mutex_map[free_idx].host_mutex;
    pthread_mutex_unlock(&g_mutex_map_lock);
    return res;
}

int compat_pthread_mutex_init(void *guest_mutex, const void *attr) {
    (void)attr;
    pthread_mutex_t *m = lookup_or_create_host_mutex(guest_mutex, 1, NULL);
    return m ? 0 : EINVAL;
}

int compat_pthread_mutex_destroy(void *guest_mutex) {
    if (!guest_mutex) return EINVAL;
    pthread_mutex_lock(&g_mutex_map_lock);
    for (int i = 0; i < NM_MUTEX_MAP_CAP; i++) {
        if (g_mutex_map[i].in_use && g_mutex_map[i].guest_ptr == guest_mutex) {
            int rc = pthread_mutex_destroy(&g_mutex_map[i].host_mutex);
            g_mutex_map[i].in_use = 0;
            g_mutex_map[i].guest_ptr = NULL;
            pthread_mutex_unlock(&g_mutex_map_lock);
            return rc;
        }
    }
    pthread_mutex_unlock(&g_mutex_map_lock);
    return 0;
}

int compat_pthread_mutex_lock(void *guest_mutex) {
    pthread_mutex_t *m = lookup_or_create_host_mutex(guest_mutex, 0, NULL);
    return m ? pthread_mutex_lock(m) : EINVAL;
}

int compat_pthread_mutex_trylock(void *guest_mutex) {
    pthread_mutex_t *m = lookup_or_create_host_mutex(guest_mutex, 0, NULL);
    return m ? pthread_mutex_trylock(m) : EINVAL;
}

int compat_pthread_mutex_unlock(void *guest_mutex) {
    pthread_mutex_t *m = lookup_or_create_host_mutex(guest_mutex, 0, NULL);
    return m ? pthread_mutex_unlock(m) : EINVAL;
}

int compat_pthread_join(uint32_t thread, void **retval) {
    return pthread_join((pthread_t)thread, retval);
}

int compat_pthread_detach(uint32_t thread) {
    return pthread_detach((pthread_t)thread);
}

uint32_t compat_pthread_self(void) {
    return (uint32_t)pthread_self();
}

void compat_pthread_exit(void *retval) {
    pthread_exit(retval);
}

/* --- Condition variables (48B on both i386 and x32, shadow-mapped for align safety) --- */
#define NM_COND_MAP_CAP 128
typedef struct {
    const void *guest_ptr;
    pthread_cond_t host_cond;
    int in_use;
} nm_cond_entry_t;

static pthread_mutex_t g_cond_map_lock = PTHREAD_MUTEX_INITIALIZER;
static nm_cond_entry_t g_cond_map[NM_COND_MAP_CAP];

static pthread_cond_t *lookup_or_create_host_cond(void *guest_cond, int reinit) {
    if (!guest_cond) return NULL;
    pthread_mutex_lock(&g_cond_map_lock);
    int free_idx = -1;
    for (int i = 0; i < NM_COND_MAP_CAP; i++) {
        if (g_cond_map[i].in_use) {
            if (g_cond_map[i].guest_ptr == guest_cond) {
                if (reinit) {
                    pthread_cond_destroy(&g_cond_map[i].host_cond);
                    pthread_cond_init(&g_cond_map[i].host_cond, NULL);
                }
                pthread_cond_t *res = &g_cond_map[i].host_cond;
                pthread_mutex_unlock(&g_cond_map_lock);
                return res;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        pthread_mutex_unlock(&g_cond_map_lock);
        return NULL;
    }
    g_cond_map[free_idx].guest_ptr = guest_cond;
    g_cond_map[free_idx].in_use = 1;
    pthread_cond_init(&g_cond_map[free_idx].host_cond, NULL);
    pthread_cond_t *res = &g_cond_map[free_idx].host_cond;
    pthread_mutex_unlock(&g_cond_map_lock);
    return res;
}

int compat_pthread_cond_init(void *cond, const void *attr) {
    (void)attr;
    return lookup_or_create_host_cond(cond, 1) ? 0 : EINVAL;
}

int compat_pthread_cond_destroy(void *cond) {
    if (!cond) return EINVAL;
    pthread_mutex_lock(&g_cond_map_lock);
    for (int i = 0; i < NM_COND_MAP_CAP; i++) {
        if (g_cond_map[i].in_use && g_cond_map[i].guest_ptr == cond) {
            int rc = pthread_cond_destroy(&g_cond_map[i].host_cond);
            g_cond_map[i].in_use = 0;
            g_cond_map[i].guest_ptr = NULL;
            pthread_mutex_unlock(&g_cond_map_lock);
            return rc;
        }
    }
    pthread_mutex_unlock(&g_cond_map_lock);
    return 0;
}

int compat_pthread_cond_wait(void *cond, void *mutex) {
    pthread_cond_t *c = lookup_or_create_host_cond(cond, 0);
    pthread_mutex_t *m = lookup_or_create_host_mutex(mutex, 0, NULL);
    return (c && m) ? pthread_cond_wait(c, m) : EINVAL;
}

int compat_pthread_cond_timedwait(void *cond, void *mutex, const newminhh_timespec32_t *abstime) {
    pthread_cond_t *c = lookup_or_create_host_cond(cond, 0);
    pthread_mutex_t *m = lookup_or_create_host_mutex(mutex, 0, NULL);
    struct timespec ts;
    if (!c || !m || newminhh_repack_timespec_from_32(abstime, &ts) != NM_OK) return EINVAL;
    return pthread_cond_timedwait(c, m, &ts);
}

int compat_pthread_cond_signal(void *cond) {
    pthread_cond_t *c = lookup_or_create_host_cond(cond, 0);
    return c ? pthread_cond_signal(c) : EINVAL;
}

int compat_pthread_cond_broadcast(void *cond) {
    pthread_cond_t *c = lookup_or_create_host_cond(cond, 0);
    return c ? pthread_cond_broadcast(c) : EINVAL;
}

/* --- Memory / String / Conversion --- */

void *compat_malloc(uint32_t sz) { return malloc((size_t)sz); }
void *compat_calloc(uint32_t n, uint32_t sz) { return calloc((size_t)n, (size_t)sz); }
void *compat_realloc(void *p, uint32_t sz) { return realloc(p, (size_t)sz); }
void compat_free(void *p) { free(p); }
void *compat_memset(void *s, int c, uint32_t n) { return memset(s, c, (size_t)n); }
void *compat_memcpy(void *d, const void *s, uint32_t n) { return memcpy(d, s, (size_t)n); }
void *compat_memmove(void *d, const void *s, uint32_t n) { return memmove(d, s, (size_t)n); }
int compat_memcmp(const void *a, const void *b, uint32_t n) { return memcmp(a, b, (size_t)n); }

uint32_t compat_strlen(const char *s) { return (uint32_t)strlen(s); }
int compat_strcmp(const char *a, const char *b) { return strcmp(a, b); }
int compat_strncmp(const char *a, const char *b, uint32_t n) { return strncmp(a, b, (size_t)n); }
char *compat_strcpy(char *d, const char *s) { return strcpy(d, s); }
char *compat_strncpy(char *d, const char *s, uint32_t n) { return strncpy(d, s, (size_t)n); }
char *compat_strcat(char *d, const char *s) { return strcat(d, s); }
char *compat_strchr(const char *s, int c) { return (char *)(uintptr_t)strchr(s, c); }
char *compat_strrchr(const char *s, int c) { return (char *)(uintptr_t)strrchr(s, c); }
char *compat_strstr(const char *h, const char *n) { return (char *)(uintptr_t)strstr(h, n); }
char *compat_strdup(const char *s) { return strdup(s); }

int32_t compat_strtol(const char *nptr, char **endptr, int base) {
    return (int32_t)strtol(nptr, endptr, base);
}
uint32_t compat_strtoul(const char *nptr, char **endptr, int base) {
    return (uint32_t)strtoul(nptr, endptr, base);
}
int64_t compat_strtoll(const char *nptr, char **endptr, int base) {
    return (int64_t)strtoll(nptr, endptr, base);
}
uint64_t compat_strtoull(const char *nptr, char **endptr, int base) {
    return (uint64_t)strtoull(nptr, endptr, base);
}
int32_t compat___isoc23_strtol(const char *nptr, char **endptr, int base) {
    return (int32_t)strtol(nptr, endptr, base);
}
uint32_t compat___isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return (uint32_t)strtoul(nptr, endptr, base);
}
int64_t compat___isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return (int64_t)strtoll(nptr, endptr, base);
}
uint64_t compat___isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return (uint64_t)strtoull(nptr, endptr, base);
}
int compat_atoi(const char *s) { return atoi(s); }
int32_t compat_atol(const char *s) { return (int32_t)atol(s); }

char *compat_getenv(const char *name) { return getenv(name); }
int compat_setenv(const char *name, const char *val, int overwrite) {
    int rc = setenv(name, val, overwrite);
    compat_environ = environ;
    return rc;
}
int compat_unsetenv(const char *name) {
    int rc = unsetenv(name);
    compat_environ = environ;
    return rc;
}

/* --- Stdio --- */

int compat_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int compat_fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}

int compat_sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(str, fmt, ap);
    va_end(ap);
    return r;
}

int compat_snprintf(char *str, uint32_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, (size_t)size, fmt, ap);
    va_end(ap);
    return r;
}

static int format_from_cdecl_words(FILE *fp, char *dst, size_t dst_sz, int to_file,
                                    const char *fmt, const uint32_t *args);

int compat_vprintf(const char *fmt, va_list ap) {
    return format_from_cdecl_words(stdout, NULL, 0, 1, fmt,
                                    (const uint32_t *)(const void *)ap);
}
int compat_vfprintf(FILE *fp, const char *fmt, va_list ap) {
    return format_from_cdecl_words(fp, NULL, 0, 1, fmt,
                                    (const uint32_t *)(const void *)ap);
}
int compat_vsprintf(char *str, const char *fmt, va_list ap) {
    return format_from_cdecl_words(NULL, str, (size_t)0x7FFFFFFFu, 0, fmt,
                                    (const uint32_t *)(const void *)ap);
}
int compat_vsnprintf(char *str, uint32_t size, const char *fmt, va_list ap) {
    return format_from_cdecl_words(NULL, str, (size_t)size, 0, fmt,
                                    (const uint32_t *)(const void *)ap);
}

/*
 * Format-aware printf/snprintf from i386 cdecl word array (for Part 4 / cdecl variadics).
 * Handles %d/%u/%x/%s/%p/%c/%lld/%llu/%llx/%f/%lf/%g/%e/%*d etc.
 */
static int format_from_cdecl_words(FILE *fp, char *dst, size_t dst_sz, int to_file,
                                    const char *fmt, const uint32_t *args) {
    if (!fmt) {
        errno = EINVAL;
        return -1;
    }
    size_t idx = 0;
    size_t out_pos = 0;
    int total_chars = 0;

#define EMIT_CH(ch)                                         \
    do {                                                    \
        char _c = (ch);                                     \
        if (to_file) {                                      \
            if (fputc(_c, fp) == EOF) return -1;            \
        } else if (dst && out_pos + 1 < dst_sz) {           \
            dst[out_pos++] = _c;                            \
        }                                                   \
        total_chars++;                                      \
    } while (0)

#define EMIT_STR(s, n)                                      \
    do {                                                    \
        const char *_s = (s);                               \
        size_t _n = (n);                                    \
        for (size_t _k = 0; _k < _n; _k++) EMIT_CH(_s[_k]); \
    } while (0)

    for (const char *p = fmt; *p != '\0'; ) {
        if (*p != '%') {
            EMIT_CH(*p++);
            continue;
        }
        const char *spec_start = p++;
        if (*p == '%') {
            EMIT_CH('%');
            p++;
            continue;
        }
        char spec_buf[64];
        size_t slen = 0;
        spec_buf[slen++] = '%';

        /* Flags */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
            p++;
        }
        /* Width */
        int dyn_w = 0, w_val = 0;
        if (*p == '*') {
            dyn_w = 1;
            w_val = (int)(int32_t)args[idx++];
            if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
                p++;
            }
        }
        /* Precision */
        int dyn_p = 0, p_val = 0;
        if (*p == '.') {
            if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
            p++;
            if (*p == '*') {
                dyn_p = 1;
                p_val = (int)(int32_t)args[idx++];
                if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
                    p++;
                }
            }
        }
        /* Length modifier */
        int is_ll = 0, is_l = 0, is_z = 0;
        if (*p == 'l') {
            if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
            p++;
            if (*p == 'l') {
                is_ll = 1;
                if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
                p++;
            } else {
                is_l = 1;
            }
        } else if (*p == 'h') {
            if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
            p++;
            if (*p == 'h') {
                if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
                p++;
            }
        } else if (*p == 'z' || *p == 't' || *p == 'j') {
            if (*p == 'j') is_ll = 1;
            else is_z = 1;
            if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = *p;
            p++;
        }
        (void)is_l;
        (void)is_z;

        char conv = *p;
        if (conv == '\0') {
            EMIT_STR(spec_start, (size_t)(p - spec_start));
            break;
        }
        if (slen + 2 < sizeof(spec_buf)) spec_buf[slen++] = conv;
        spec_buf[slen] = '\0';
        p++;

        char tmp[512];
        int n = 0;
        if (conv == 's') {
            const char *s = (const char *)(uintptr_t)args[idx++];
            if (!s) s = "(null)";
            if (dyn_w && dyn_p) n = snprintf(tmp, sizeof(tmp), spec_buf, w_val, p_val, s);
            else if (dyn_w) n = snprintf(tmp, sizeof(tmp), spec_buf, w_val, s);
            else if (dyn_p) n = snprintf(tmp, sizeof(tmp), spec_buf, p_val, s);
            else n = snprintf(tmp, sizeof(tmp), spec_buf, s);
            if (n > 0) {
                if ((size_t)n < sizeof(tmp)) {
                    EMIT_STR(tmp, (size_t)n);
                } else {
                    char *dyn = (char *)malloc((size_t)n + 1);
                    if (dyn) {
                        if (dyn_w && dyn_p) snprintf(dyn, (size_t)n + 1, spec_buf, w_val, p_val, s);
                        else if (dyn_w) snprintf(dyn, (size_t)n + 1, spec_buf, w_val, s);
                        else if (dyn_p) snprintf(dyn, (size_t)n + 1, spec_buf, p_val, s);
                        else snprintf(dyn, (size_t)n + 1, spec_buf, s);
                        EMIT_STR(dyn, (size_t)n);
                        free(dyn);
                    }
                }
            }
        } else if (conv == 'p') {
            void *ptr = (void *)(uintptr_t)args[idx++];
            n = snprintf(tmp, sizeof(tmp), spec_buf, ptr);
            if (n > 0) EMIT_STR(tmp, (size_t)n);
        } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
                   conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
            uint64_t raw = ((uint64_t)args[idx + 1] << 32) | args[idx];
            idx += 2;
            double d = 0.0;
            memcpy(&d, &raw, sizeof(d));
            if (dyn_w && dyn_p) n = snprintf(tmp, sizeof(tmp), spec_buf, w_val, p_val, d);
            else if (dyn_w) n = snprintf(tmp, sizeof(tmp), spec_buf, w_val, d);
            else if (dyn_p) n = snprintf(tmp, sizeof(tmp), spec_buf, p_val, d);
            else n = snprintf(tmp, sizeof(tmp), spec_buf, d);
            if (n > 0) EMIT_STR(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
        } else if (conv == 'n') {
            int *out_n = (int *)(uintptr_t)args[idx++];
            if (out_n) *out_n = total_chars;
        } else if (is_ll) {
            uint64_t v = ((uint64_t)args[idx + 1] << 32) | args[idx];
            idx += 2;
            if (dyn_w && dyn_p) n = snprintf(tmp, sizeof(tmp), spec_buf, w_val, p_val, v);
            else if (dyn_w) n = snprintf(tmp, sizeof(tmp), spec_buf, w_val, v);
            else if (dyn_p) n = snprintf(tmp, sizeof(tmp), spec_buf, p_val, v);
            else n = snprintf(tmp, sizeof(tmp), spec_buf, v);
            if (n > 0) EMIT_STR(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
        } else {
            uint32_t v = args[idx++];
            if (dyn_w && dyn_p) n = snprintf(tmp, sizeof(tmp), spec_buf, w_val, p_val, v);
            else if (dyn_w) n = snprintf(tmp, sizeof(tmp), spec_buf, w_val, v);
            else if (dyn_p) n = snprintf(tmp, sizeof(tmp), spec_buf, p_val, v);
            else n = snprintf(tmp, sizeof(tmp), spec_buf, v);
            if (n > 0) EMIT_STR(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
        }
    }
    if (!to_file && dst && dst_sz > 0) {
        dst[out_pos < dst_sz ? out_pos : dst_sz - 1] = '\0';
    }
    return total_chars;
#undef EMIT_CH
#undef EMIT_STR
}

int compat_printf_cdecl(const uint32_t *cdecl_args) {
    const char *fmt = (const char *)(uintptr_t)cdecl_args[0];
    return format_from_cdecl_words(stdout, NULL, 0, 1, fmt, cdecl_args + 1);
}

int compat_fprintf_cdecl(const uint32_t *cdecl_args) {
    FILE *fp = (FILE *)(uintptr_t)cdecl_args[0];
    const char *fmt = (const char *)(uintptr_t)cdecl_args[1];
    return format_from_cdecl_words(fp, NULL, 0, 1, fmt, cdecl_args + 2);
}

int compat_sprintf_cdecl(const uint32_t *cdecl_args) {
    char *dst = (char *)(uintptr_t)cdecl_args[0];
    const char *fmt = (const char *)(uintptr_t)cdecl_args[1];
    return format_from_cdecl_words(NULL, dst, (size_t)0x7FFFFFFFu, 0, fmt, cdecl_args + 2);
}

int compat_snprintf_cdecl(const uint32_t *cdecl_args) {
    char *dst = (char *)(uintptr_t)cdecl_args[0];
    uint32_t sz = cdecl_args[1];
    const char *fmt = (const char *)(uintptr_t)cdecl_args[2];
    return format_from_cdecl_words(NULL, dst, (size_t)sz, 0, fmt, cdecl_args + 3);
}

int compat_puts(const char *s) { return puts(s); }
int compat_fputs(const char *s, FILE *fp) { return fputs(s, fp); }
int compat_putchar(int c) { return putchar(c); }
int compat_fputc(int c, FILE *fp) { return fputc(c, fp); }
uint32_t compat_fwrite(const void *p, uint32_t sz, uint32_t n, FILE *fp) {
    return (uint32_t)fwrite(p, (size_t)sz, (size_t)n, fp);
}
uint32_t compat_fread(void *p, uint32_t sz, uint32_t n, FILE *fp) {
    return (uint32_t)fread(p, (size_t)sz, (size_t)n, fp);
}
FILE *compat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
FILE *compat_fopen64(const char *path, const char *mode) { return fopen(path, mode); }
int compat_fclose(FILE *fp) { return fclose(fp); }
int compat_fflush(FILE *fp) { return fflush(fp); }
int compat_fseek(FILE *fp, int32_t off, int whence) { return fseek(fp, (long)off, whence); }
int32_t compat_ftell(FILE *fp) { return (int32_t)ftell(fp); }
char *compat_fgets(char *s, int size, FILE *fp) { return fgets(s, size, fp); }
void compat_perror(const char *s) { perror(s); }
int compat_setvbuf(FILE *fp, char *buf, int mode, uint32_t size) {
    return setvbuf(fp, buf, mode, (size_t)size);
}

/* --- File / POSIX --- */

int compat_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned int);
        va_end(ap);
        return open(path, flags, mode);
    }
    return open(path, flags);
}

int compat_open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned int);
        va_end(ap);
        return open(path, flags | O_LARGEFILE, mode);
    }
    return open(path, flags | O_LARGEFILE);
}

int compat_openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned int);
        va_end(ap);
        return openat(dirfd, path, flags, mode);
    }
    return openat(dirfd, path, flags);
}

int compat_close(int fd) { return close(fd); }
int32_t compat_read(int fd, void *buf, uint32_t cnt) { return (int32_t)read(fd, buf, (size_t)cnt); }
int32_t compat_write(int fd, const void *buf, uint32_t cnt) { return (int32_t)write(fd, buf, (size_t)cnt); }
int32_t compat_lseek(int fd, int32_t off, int whence) { return (int32_t)lseek(fd, (off_t)off, whence); }
int64_t compat_lseek64(int fd, int64_t off, int whence) { return (int64_t)lseek(fd, (off_t)off, whence); }
int compat_unlink(const char *p) { return unlink(p); }
int compat_symlink(const char *t, const char *l) { return symlink(t, l); }
int32_t compat_readlink(const char *p, char *buf, uint32_t sz) {
    return (int32_t)readlink(p, buf, (size_t)sz);
}
int compat_access(const char *p, int mode) { return access(p, mode); }
int compat_fcntl(int fd, int cmd, uint32_t arg) { return fcntl(fd, cmd, arg); }
int compat_ioctl(int fd, uint32_t req, uint32_t arg) { return ioctl(fd, (unsigned long)req, arg); }
int compat_dup(int fd) { return dup(fd); }
int compat_dup2(int oldfd, int newfd) { return dup2(oldfd, newfd); }
int compat_dup3(int oldfd, int newfd, int flags) { return dup3(oldfd, newfd, flags); }
int compat_pipe(int fds[2]) { return pipe(fds); }
int compat_pipe2(int fds[2], int flags) { return pipe2(fds, flags); }
int compat_isatty(int fd) { return isatty(fd); }
int compat_getpid(void) { return (int)getpid(); }
int compat_gettid(void) { return (int)gettid(); }
uint32_t compat_getuid(void) { return (uint32_t)getuid(); }
uint32_t compat_geteuid(void) { return (uint32_t)geteuid(); }
uint32_t compat_getgid(void) { return (uint32_t)getgid(); }
uint32_t compat_getegid(void) { return (uint32_t)getegid(); }
unsigned int compat_sleep(unsigned int sec) { return sleep(sec); }
int compat_usleep(unsigned int usec) { return usleep(usec); }

void *compat_mmap(void *addr, uint32_t len, int prot, int flags, int fd, int32_t off) {
    return mmap(addr, (size_t)len, prot, flags, fd, (off_t)off);
}
void *compat_mmap64(void *addr, uint32_t len, int prot, int flags, int fd, int64_t off) {
    return mmap(addr, (size_t)len, prot, flags, fd, (off_t)off);
}
int compat_munmap(void *addr, uint32_t len) { return munmap(addr, (size_t)len); }
int compat_mprotect(void *addr, uint32_t len, int prot) { return mprotect(addr, (size_t)len, prot); }
int compat_uname(struct utsname *buf) { return uname(buf); }
int32_t compat_sysconf(int name) { return (int32_t)sysconf(name); }

/* --- Struct-repacking libc shims --- */

int compat_stat(const char *path, newminhh_stat32_t *buf) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat32(&st, buf) == NM_OK) ? 0 : -1;
}

int compat_lstat(const char *path, newminhh_stat32_t *buf) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat32(&st, buf) == NM_OK) ? 0 : -1;
}

int compat_fstat(int fd, newminhh_stat32_t *buf) {
    if (!buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat32(&st, buf) == NM_OK) ? 0 : -1;
}

int compat_fstatat(int dirfd, const char *path, newminhh_stat32_t *buf, int flags) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (fstatat(dirfd, path, &st, flags) != 0) return -1;
    return (newminhh_repack_stat_to_stat32(&st, buf) == NM_OK) ? 0 : -1;
}

int compat_stat64(const char *path, newminhh_stat64_t *buf) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat64(&st, buf) == NM_OK) ? 0 : -1;
}

int compat_lstat64(const char *path, newminhh_stat64_t *buf) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat64(&st, buf) == NM_OK) ? 0 : -1;
}

int compat_fstat64(int fd, newminhh_stat64_t *buf) {
    if (!buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat64(&st, buf) == NM_OK) ? 0 : -1;
}

int compat_fstatat64(int dirfd, const char *path, newminhh_stat64_t *buf, int flags) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (fstatat(dirfd, path, &st, flags) != 0) return -1;
    return (newminhh_repack_stat_to_stat64(&st, buf) == NM_OK) ? 0 : -1;
}

int compat___xstat(int ver, const char *path, newminhh_stat32_t *buf) {
    if (ver != 3) NM_LOG_WARN("wrappers", "__xstat unexpected i386 ver=%d", ver);
    return compat_stat(path, buf);
}
int compat___lxstat(int ver, const char *path, newminhh_stat32_t *buf) {
    if (ver != 3) NM_LOG_WARN("wrappers", "__lxstat unexpected i386 ver=%d", ver);
    return compat_lstat(path, buf);
}
int compat___fxstat(int ver, int fd, newminhh_stat32_t *buf) {
    if (ver != 3) NM_LOG_WARN("wrappers", "__fxstat unexpected i386 ver=%d", ver);
    return compat_fstat(fd, buf);
}
int compat___fxstatat(int ver, int dirfd, const char *path, newminhh_stat32_t *buf, int flags) {
    if (ver != 3) NM_LOG_WARN("wrappers", "__fxstatat unexpected i386 ver=%d", ver);
    return compat_fstatat(dirfd, path, buf, flags);
}

int compat___xstat64(int ver, const char *path, newminhh_stat64_t *buf) {
    if (ver != 3) NM_LOG_WARN("wrappers", "__xstat64 unexpected i386 ver=%d", ver);
    return compat_stat64(path, buf);
}
int compat___lxstat64(int ver, const char *path, newminhh_stat64_t *buf) {
    if (ver != 3) NM_LOG_WARN("wrappers", "__lxstat64 unexpected i386 ver=%d", ver);
    return compat_lstat64(path, buf);
}
int compat___fxstat64(int ver, int fd, newminhh_stat64_t *buf) {
    if (ver != 3) NM_LOG_WARN("wrappers", "__fxstat64 unexpected i386 ver=%d", ver);
    return compat_fstat64(fd, buf);
}
int compat___fxstatat64(int ver, int dirfd, const char *path, newminhh_stat64_t *buf, int flags) {
    if (ver != 3) NM_LOG_WARN("wrappers", "__fxstatat64 unexpected i386 ver=%d", ver);
    return compat_fstatat64(dirfd, path, buf, flags);
}

int compat___stat64_time64(const char *path, newminhh_stat64_t64_t *buf) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat64_t64(&st, buf) == NM_OK) ? 0 : -1;
}
int compat___lstat64_time64(const char *path, newminhh_stat64_t64_t *buf) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat64_t64(&st, buf) == NM_OK) ? 0 : -1;
}
int compat___fstat64_time64(int fd, newminhh_stat64_t64_t *buf) {
    if (!buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (newminhh_repack_stat_to_stat64_t64(&st, buf) == NM_OK) ? 0 : -1;
}
int compat___fstatat64_time64(int dirfd, const char *path, newminhh_stat64_t64_t *buf, int flags) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    struct stat st;
    if (fstatat(dirfd, path, &st, flags) != 0) return -1;
    return (newminhh_repack_stat_to_stat64_t64(&st, buf) == NM_OK) ? 0 : -1;
}

int compat_clock_gettime(int clk_id, newminhh_timespec32_t *tp) {
    if (!tp) { errno = EFAULT; return -1; }
    struct timespec xts;
    if (clock_gettime((clockid_t)clk_id, &xts) != 0) return -1;
    return (newminhh_repack_timespec_to_32(&xts, tp) == NM_OK) ? 0 : -1;
}

int compat___clock_gettime64(int clk_id, newminhh_timespec64_t32_t *tp) {
    if (!tp) { errno = EFAULT; return -1; }
    struct timespec xts;
    if (clock_gettime((clockid_t)clk_id, &xts) != 0) return -1;
    return (newminhh_repack_timespec_to_64t32(&xts, tp) == NM_OK) ? 0 : -1;
}

int compat_nanosleep(const newminhh_timespec32_t *req, newminhh_timespec32_t *rem) {
    if (!req) { errno = EFAULT; return -1; }
    struct timespec xreq, xrem;
    if (newminhh_repack_timespec_from_32(req, &xreq) != NM_OK) return -1;
    int rc = nanosleep(&xreq, rem ? &xrem : NULL);
    if (rc != 0 && errno == EINTR && rem) {
        newminhh_repack_timespec_to_32(&xrem, rem);
    }
    return rc;
}

int compat___nanosleep64(const newminhh_timespec64_t32_t *req, newminhh_timespec64_t32_t *rem) {
    if (!req) { errno = EFAULT; return -1; }
    struct timespec xreq, xrem;
    if (newminhh_repack_timespec_from_64t32(req, &xreq) != NM_OK) return -1;
    int rc = nanosleep(&xreq, rem ? &xrem : NULL);
    if (rc != 0 && errno == EINTR && rem) {
        newminhh_repack_timespec_to_64t32(&xrem, rem);
    }
    return rc;
}

int compat_clock_nanosleep(int clk_id, int flags, const newminhh_timespec32_t *req,
                           newminhh_timespec32_t *rem) {
    if (!req) { errno = EFAULT; return -1; }
    struct timespec xreq, xrem;
    if (newminhh_repack_timespec_from_32(req, &xreq) != NM_OK) return -1;
    int rc = clock_nanosleep((clockid_t)clk_id, flags, &xreq, rem ? &xrem : NULL);
    if (rc == EINTR && rem) {
        newminhh_repack_timespec_to_32(&xrem, rem);
    }
    return rc;
}

int compat___clock_nanosleep64(int clk_id, int flags, const newminhh_timespec64_t32_t *req,
                               newminhh_timespec64_t32_t *rem) {
    if (!req) { errno = EFAULT; return -1; }
    struct timespec xreq, xrem;
    if (newminhh_repack_timespec_from_64t32(req, &xreq) != NM_OK) return -1;
    int rc = clock_nanosleep((clockid_t)clk_id, flags, &xreq, rem ? &xrem : NULL);
    if (rc == EINTR && rem) {
        newminhh_repack_timespec_to_64t32(&xrem, rem);
    }
    return rc;
}

int compat_gettimeofday(void *tv32, void *tz) {
    (void)tz;
    if (!tv32) return 0;
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return -1;
    int32_t out[2] = {(int32_t)tv.tv_sec, (int32_t)tv.tv_usec};
    memcpy(tv32, out, sizeof(out));
    return 0;
}

int32_t compat_time(int32_t *tloc) {
    time_t t = time(NULL);
    if (tloc) *tloc = (int32_t)t;
    return (int32_t)t;
}

int compat_getrlimit(int resource, newminhh_rlimit32_t *rlim) {
    if (!rlim) { errno = EFAULT; return -1; }
    struct rlimit xrl;
    if (getrlimit(resource, &xrl) != 0) return -1;
    return (newminhh_repack_rlimit_to_32(&xrl, rlim) == NM_OK) ? 0 : -1;
}

int compat_setrlimit(int resource, const newminhh_rlimit32_t *rlim) {
    if (!rlim) { errno = EFAULT; return -1; }
    struct rlimit xrl;
    if (newminhh_repack_rlimit_from_32(rlim, &xrl) != NM_OK) return -1;
    return setrlimit(resource, &xrl);
}

int compat_getrlimit64(int resource, newminhh_rlimit64_t32_t *rlim) {
    if (!rlim) { errno = EFAULT; return -1; }
    struct rlimit xrl;
    if (getrlimit(resource, &xrl) != 0) return -1;
    return (newminhh_repack_rlimit_to_64t32(&xrl, rlim) == NM_OK) ? 0 : -1;
}

int compat_setrlimit64(int resource, const newminhh_rlimit64_t32_t *rlim) {
    if (!rlim) { errno = EFAULT; return -1; }
    struct rlimit xrl;
    if (newminhh_repack_rlimit_from_64t32(rlim, &xrl) != NM_OK) return -1;
    return setrlimit(resource, &xrl);
}

int compat_prlimit(int pid, int resource, const newminhh_rlimit32_t *new_lim,
                   newminhh_rlimit32_t *old_lim) {
    struct rlimit xn, xo;
    if (new_lim && newminhh_repack_rlimit_from_32(new_lim, &xn) != NM_OK) return -1;
    if (prlimit((pid_t)pid, resource, new_lim ? &xn : NULL, old_lim ? &xo : NULL) != 0) return -1;
    if (old_lim && newminhh_repack_rlimit_to_32(&xo, old_lim) != NM_OK) return -1;
    return 0;
}

int compat_prlimit64(int pid, int resource, const newminhh_rlimit64_t32_t *new_lim,
                     newminhh_rlimit64_t32_t *old_lim) {
    struct rlimit xn, xo;
    if (new_lim && newminhh_repack_rlimit_from_64t32(new_lim, &xn) != NM_OK) return -1;
    if (prlimit((pid_t)pid, resource, new_lim ? &xn : NULL, old_lim ? &xo : NULL) != 0) return -1;
    if (old_lim && newminhh_repack_rlimit_to_64t32(&xo, old_lim) != NM_OK) return -1;
    return 0;
}

/* --- Process lifecycle --- */

void compat_exit(int status) {
    newminhh_run_guest_atexits();
    exit(status);
}

void compat__exit(int status) {
    _exit(status);
}

void compat_abort(void) {
    abort();
}

void compat___stack_chk_fail(void) {
    NM_LOG_ERROR("runtime", "stack smashing detected (__stack_chk_fail)");
    abort();
}

int *compat___errno_location(void) {
    return __errno_location();
}

int compat___libc_start_main(uint32_t main_va, int argc, char **argv,
                             uint32_t init_va, uint32_t fini_va,
                             uint32_t rtld_fini_va, void *stack_end) {
    (void)rtld_fini_va;
    (void)stack_end;
    compat_init_data_exports();
    if (fini_va) {
        compat_atexit(fini_va);
    }
    if (init_va) {
        nm_fn_sig_t init_sig = {
            .ret_kind = NM_RET_VOID,
            .nargs = 3,
            .arg_kinds = {NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR}
        };
        uint64_t gprs[6] = {(uint64_t)argc, (uint64_t)(uintptr_t)argv, (uint64_t)(uintptr_t)environ, 0, 0, 0};
        newminhh_in_bridge(init_va, &init_sig, gprs, NULL, NULL,
                           newminhh_mode_for_va(init_va),
                           NULL);
    }
    nm_fn_sig_t main_sig = {
        .ret_kind = NM_RET_I32,
        .nargs = 3,
        .arg_kinds = {NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR}
    };
    uint64_t gprs[6] = {(uint64_t)argc, (uint64_t)(uintptr_t)argv, (uint64_t)(uintptr_t)environ, 0, 0, 0};
    uint64_t ret = newminhh_in_bridge(main_va, &main_sig, gprs, NULL, NULL,
                                      newminhh_mode_for_va(main_va),
                                      NULL);
    compat_exit((int)(int32_t)ret);
    return 0;
}

/* --- Math (libm) --- */

double compat_sqrt(double x) { return sqrt(x); }
float compat_sqrtf(float x) { return sqrtf(x); }
double compat_sin(double x) { return sin(x); }
float compat_sinf(float x) { return sinf(x); }
double compat_cos(double x) { return cos(x); }
float compat_cosf(float x) { return cosf(x); }
double compat_tan(double x) { return tan(x); }
float compat_tanf(float x) { return tanf(x); }
double compat_atan2(double y, double x) { return atan2(y, x); }
double compat_pow(double x, double y) { return pow(x, y); }
float compat_powf(float x, float y) { return powf(x, y); }
double compat_exp(double x) { return exp(x); }
double compat_log(double x) { return log(x); }
double compat_log10(double x) { return log10(x); }
double compat_fabs(double x) { return fabs(x); }
float compat_fabsf(float x) { return fabsf(x); }
double compat_floor(double x) { return floor(x); }
double compat_ceil(double x) { return ceil(x); }
double compat_round(double x) { return round(x); }

/* --- Sorted Wrapper Symbol Registry --- */

#define SIG0(r) { (r), 0, 0, 0, {0} }
#define SIG1(r, a1) { (r), 1, 0, 0, {(a1)} }
#define SIG2(r, a1, a2) { (r), 2, 0, 0, {(a1), (a2)} }
#define SIG3(r, a1, a2, a3) { (r), 3, 0, 0, {(a1), (a2), (a3)} }
#define SIG4(r, a1, a2, a3, a4) { (r), 4, 0, 0, {(a1), (a2), (a3), (a4)} }
#define SIG5(r, a1, a2, a3, a4, a5) { (r), 5, 0, 0, {(a1), (a2), (a3), (a4), (a5)} }
#define SIG6(r, a1, a2, a3, a4, a5, a6) { (r), 6, 0, 0, {(a1), (a2), (a3), (a4), (a5), (a6)} }

static nm_sym_entry_t g_sym_table[] = {
    /* Data exports */
    {"stdin",  &compat_stdin,  SIG0(NM_RET_PTR), NM_SYM_FLAG_DATA},
    {"stdout", &compat_stdout, SIG0(NM_RET_PTR), NM_SYM_FLAG_DATA},
    {"stderr", &compat_stderr, SIG0(NM_RET_PTR), NM_SYM_FLAG_DATA},
    {"optarg", &compat_optarg, SIG0(NM_RET_PTR), NM_SYM_FLAG_DATA},
    {"optind", &compat_optind, SIG0(NM_RET_I32), NM_SYM_FLAG_DATA},
    {"opterr", &compat_opterr, SIG0(NM_RET_I32), NM_SYM_FLAG_DATA},
    {"optopt", &compat_optopt, SIG0(NM_RET_I32), NM_SYM_FLAG_DATA},
    {"environ", &compat_environ, SIG0(NM_RET_PTR), NM_SYM_FLAG_DATA},

    /* Alloc / mem */
    {"malloc",  (void *)compat_malloc,  SIG1(NM_RET_PTR, NM_ARG_U32), 0},
    {"calloc",  (void *)compat_calloc,  SIG2(NM_RET_PTR, NM_ARG_U32, NM_ARG_U32), 0},
    {"realloc", (void *)compat_realloc, SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_U32), 0},
    {"free",    (void *)compat_free,    SIG1(NM_RET_VOID, NM_ARG_PTR), 0},
    {"memset",  (void *)compat_memset,  SIG3(NM_RET_PTR, NM_ARG_PTR, NM_ARG_I32, NM_ARG_U32), 0},
    {"memcpy",  (void *)compat_memcpy,  SIG3(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_U32), 0},
    {"memmove", (void *)compat_memmove, SIG3(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_U32), 0},
    {"memcmp",  (void *)compat_memcmp,  SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_U32), 0},

    /* Strings & numeric */
    {"strlen",  (void *)compat_strlen,  SIG1(NM_RET_U32, NM_ARG_PTR), 0},
    {"strcmp",  (void *)compat_strcmp,  SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"strncmp", (void *)compat_strncmp, SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_U32), 0},
    {"strcpy",  (void *)compat_strcpy,  SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"strncpy", (void *)compat_strncpy, SIG3(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_U32), 0},
    {"strcat",  (void *)compat_strcat,  SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"strchr",  (void *)compat_strchr,  SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"strrchr", (void *)compat_strrchr, SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"strstr",  (void *)compat_strstr,  SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"strdup",  (void *)compat_strdup,  SIG1(NM_RET_PTR, NM_ARG_PTR), 0},
    {"strtol",  (void *)compat_strtol,  SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"strtoul", (void *)compat_strtoul, SIG3(NM_RET_U32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"strtoll", (void *)compat_strtoll, SIG3(NM_RET_I64, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"strtoull",(void *)compat_strtoull,SIG3(NM_RET_U64, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"__isoc23_strtol",  (void *)compat___isoc23_strtol,  SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"__isoc23_strtoul", (void *)compat___isoc23_strtoul, SIG3(NM_RET_U32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"__isoc23_strtoll", (void *)compat___isoc23_strtoll, SIG3(NM_RET_I64, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"__isoc23_strtoull",(void *)compat___isoc23_strtoull,SIG3(NM_RET_U64, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"atoi",    (void *)compat_atoi,    SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"atol",    (void *)compat_atol,    SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"getenv",  (void *)compat_getenv,  SIG1(NM_RET_PTR, NM_ARG_PTR), 0},
    {"setenv",  (void *)compat_setenv,  SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"unsetenv",(void *)compat_unsetenv,SIG1(NM_RET_I32, NM_ARG_PTR), 0},

    /* Stdio */
    {"printf",   (void *)compat_printf_cdecl,   SIG1(NM_RET_I32, NM_ARG_PTR), NM_SYM_FLAG_VARIADIC | NM_SYM_FLAG_CDECL_DIRECT},
    {"fprintf",  (void *)compat_fprintf_cdecl,  SIG1(NM_RET_I32, NM_ARG_PTR), NM_SYM_FLAG_VARIADIC | NM_SYM_FLAG_CDECL_DIRECT},
    {"sprintf",  (void *)compat_sprintf_cdecl,  SIG1(NM_RET_I32, NM_ARG_PTR), NM_SYM_FLAG_VARIADIC | NM_SYM_FLAG_CDECL_DIRECT},
    {"snprintf", (void *)compat_snprintf_cdecl, SIG1(NM_RET_I32, NM_ARG_PTR), NM_SYM_FLAG_VARIADIC | NM_SYM_FLAG_CDECL_DIRECT},
    {"vprintf",  (void *)compat_vprintf,  SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"vfprintf", (void *)compat_vfprintf, SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"vsprintf", (void *)compat_vsprintf, SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"vsnprintf",(void *)compat_vsnprintf,SIG4(NM_RET_I32, NM_ARG_PTR, NM_ARG_U32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"puts",     (void *)compat_puts,     SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"fputs",    (void *)compat_fputs,    SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"putchar",  (void *)compat_putchar,  SIG1(NM_RET_I32, NM_ARG_I32), 0},
    {"fputc",    (void *)compat_fputc,    SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"fwrite",   (void *)compat_fwrite,   SIG4(NM_RET_U32, NM_ARG_PTR, NM_ARG_U32, NM_ARG_U32, NM_ARG_PTR), 0},
    {"fread",    (void *)compat_fread,    SIG4(NM_RET_U32, NM_ARG_PTR, NM_ARG_U32, NM_ARG_U32, NM_ARG_PTR), 0},
    {"fopen",    (void *)compat_fopen,    SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"fopen64",  (void *)compat_fopen64,  SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"fclose",   (void *)compat_fclose,   SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"fflush",   (void *)compat_fflush,   SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"fseek",    (void *)compat_fseek,    SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_I32, NM_ARG_I32), 0},
    {"ftell",    (void *)compat_ftell,    SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"fgets",    (void *)compat_fgets,    SIG3(NM_RET_PTR, NM_ARG_PTR, NM_ARG_I32, NM_ARG_PTR), 0},
    {"perror",   (void *)compat_perror,   SIG1(NM_RET_VOID, NM_ARG_PTR), 0},
    {"setvbuf",  (void *)compat_setvbuf,  SIG4(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32, NM_ARG_U32), 0},

    /* File / POSIX */
    {"open",     (void *)compat_open,     SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_I32, NM_ARG_U32), NM_SYM_FLAG_VARIADIC},
    {"open64",   (void *)compat_open64,   SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_I32, NM_ARG_U32), NM_SYM_FLAG_VARIADIC},
    {"openat",   (void *)compat_openat,   SIG4(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_I32, NM_ARG_U32), NM_SYM_FLAG_VARIADIC},
    {"close",    (void *)compat_close,    SIG1(NM_RET_I32, NM_ARG_I32), 0},
    {"read",     (void *)compat_read,     SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_U32), 0},
    {"write",    (void *)compat_write,    SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_U32), 0},
    {"lseek",    (void *)compat_lseek,    SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I32), 0},
    {"lseek64",  (void *)compat_lseek64,  SIG3(NM_RET_I64, NM_ARG_I32, NM_ARG_I64, NM_ARG_I32), 0},
    {"unlink",   (void *)compat_unlink,   SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"symlink",  (void *)compat_symlink,  SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"readlink", (void *)compat_readlink, SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_U32), 0},
    {"access",   (void *)compat_access,   SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_I32), 0},
    {"fcntl",    (void *)compat_fcntl,    SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_U32), NM_SYM_FLAG_VARIADIC},
    {"ioctl",    (void *)compat_ioctl,    SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_U32, NM_ARG_U32), NM_SYM_FLAG_VARIADIC},
    {"dup",      (void *)compat_dup,      SIG1(NM_RET_I32, NM_ARG_I32), 0},
    {"dup2",     (void *)compat_dup2,     SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_I32), 0},
    {"dup3",     (void *)compat_dup3,     SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I32), 0},
    {"pipe",     (void *)compat_pipe,     SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"pipe2",    (void *)compat_pipe2,    SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_I32), 0},
    {"isatty",   (void *)compat_isatty,   SIG1(NM_RET_I32, NM_ARG_I32), 0},
    {"getpid",   (void *)compat_getpid,   SIG0(NM_RET_I32), 0},
    {"gettid",   (void *)compat_gettid,   SIG0(NM_RET_I32), 0},
    {"getuid",   (void *)compat_getuid,   SIG0(NM_RET_U32), 0},
    {"geteuid",  (void *)compat_geteuid,  SIG0(NM_RET_U32), 0},
    {"getgid",   (void *)compat_getgid,   SIG0(NM_RET_U32), 0},
    {"getegid",  (void *)compat_getegid,  SIG0(NM_RET_U32), 0},
    {"sleep",    (void *)compat_sleep,    SIG1(NM_RET_U32, NM_ARG_U32), 0},
    {"usleep",   (void *)compat_usleep,   SIG1(NM_RET_I32, NM_ARG_U32), 0},
    {"mmap",     (void *)compat_mmap,     SIG6(NM_RET_PTR, NM_ARG_PTR, NM_ARG_U32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I32), 0},
    {"mmap64",   (void *)compat_mmap64,   SIG6(NM_RET_PTR, NM_ARG_PTR, NM_ARG_U32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I64), 0},
    {"munmap",   (void *)compat_munmap,   SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_U32), 0},
    {"mprotect", (void *)compat_mprotect, SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_U32, NM_ARG_I32), 0},
    {"uname",    (void *)compat_uname,    SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"sysconf",  (void *)compat_sysconf,  SIG1(NM_RET_I32, NM_ARG_I32), 0},

    /* Struct repacking */
    {"stat",             (void *)compat_stat,             SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"lstat",            (void *)compat_lstat,            SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"fstat",            (void *)compat_fstat,            SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"fstatat",          (void *)compat_fstatat,          SIG4(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"stat64",           (void *)compat_stat64,           SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"lstat64",          (void *)compat_lstat64,          SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"fstat64",          (void *)compat_fstat64,          SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"fstatat64",        (void *)compat_fstatat64,        SIG4(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"__xstat",          (void *)compat___xstat,          SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"__lxstat",         (void *)compat___lxstat,         SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"__fxstat",         (void *)compat___fxstat,         SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"__fxstatat",       (void *)compat___fxstatat,       SIG5(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"__xstat64",        (void *)compat___xstat64,        SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"__lxstat64",       (void *)compat___lxstat64,       SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"__fxstat64",       (void *)compat___fxstat64,       SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"__fxstatat64",     (void *)compat___fxstatat64,     SIG5(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"__stat64_time64",  (void *)compat___stat64_time64,  SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"__lstat64_time64", (void *)compat___lstat64_time64, SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"__fstat64_time64", (void *)compat___fstat64_time64, SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"__fstatat64_time64",(void *)compat___fstatat64_time64,SIG4(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"clock_gettime",    (void *)compat_clock_gettime,    SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"__clock_gettime64",(void *)compat___clock_gettime64,SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"nanosleep",        (void *)compat_nanosleep,        SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"__nanosleep64",    (void *)compat___nanosleep64,    SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"clock_nanosleep",  (void *)compat_clock_nanosleep,  SIG4(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"__clock_nanosleep64",(void *)compat___clock_nanosleep64,SIG4(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"gettimeofday",     (void *)compat_gettimeofday,     SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"time",             (void *)compat_time,             SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"getrlimit",        (void *)compat_getrlimit,        SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"setrlimit",        (void *)compat_setrlimit,        SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"getrlimit64",      (void *)compat_getrlimit64,      SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"setrlimit64",      (void *)compat_setrlimit64,      SIG2(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR), 0},
    {"prlimit",          (void *)compat_prlimit,          SIG4(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"prlimit64",        (void *)compat_prlimit64,        SIG4(NM_RET_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},

    /* Callbacks / pthreads / lifecycle */
    {"qsort",                 (void *)compat_qsort,                 SIG4(NM_RET_VOID, NM_ARG_PTR, NM_ARG_U32, NM_ARG_U32, NM_ARG_U32), 0},
    {"bsearch",               (void *)compat_bsearch,               SIG5(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_U32, NM_ARG_U32, NM_ARG_U32), 0},
    {"pthread_create",        (void *)compat_pthread_create,        SIG4(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_U32, NM_ARG_U32), 0},
    {"pthread_join",          (void *)compat_pthread_join,          SIG2(NM_RET_I32, NM_ARG_U32, NM_ARG_PTR), 0},
    {"pthread_detach",        (void *)compat_pthread_detach,        SIG1(NM_RET_I32, NM_ARG_U32), 0},
    {"pthread_self",          (void *)compat_pthread_self,          SIG0(NM_RET_U32), 0},
    {"pthread_exit",          (void *)compat_pthread_exit,          SIG1(NM_RET_VOID, NM_ARG_PTR), 0},
    {"pthread_mutex_init",    (void *)compat_pthread_mutex_init,    SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"pthread_mutex_destroy", (void *)compat_pthread_mutex_destroy, SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"pthread_mutex_lock",    (void *)compat_pthread_mutex_lock,    SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"pthread_mutex_trylock", (void *)compat_pthread_mutex_trylock, SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"pthread_mutex_unlock",  (void *)compat_pthread_mutex_unlock,  SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"pthread_cond_init",     (void *)compat_pthread_cond_init,     SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"pthread_cond_destroy",  (void *)compat_pthread_cond_destroy,  SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"pthread_cond_wait",     (void *)compat_pthread_cond_wait,     SIG2(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"pthread_cond_timedwait",(void *)compat_pthread_cond_timedwait,SIG3(NM_RET_I32, NM_ARG_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"pthread_cond_signal",   (void *)compat_pthread_cond_signal,   SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"pthread_cond_broadcast",(void *)compat_pthread_cond_broadcast,SIG1(NM_RET_I32, NM_ARG_PTR), 0},
    {"atexit",                (void *)compat_atexit,                SIG1(NM_RET_I32, NM_ARG_U32), 0},
    {"__cxa_atexit",          (void *)compat___cxa_atexit,          SIG3(NM_RET_I32, NM_ARG_U32, NM_ARG_U32, NM_ARG_U32), 0},
    {"__cxa_finalize",        (void *)compat___cxa_finalize,        SIG1(NM_RET_VOID, NM_ARG_U32), 0},
    {"signal",                (void *)compat_signal,                SIG2(NM_RET_U32, NM_ARG_I32, NM_ARG_U32), 0},
    {"sigaction",             (void *)compat_sigaction,             SIG3(NM_RET_I32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"exit",                  (void *)compat_exit,                  SIG1(NM_RET_VOID, NM_ARG_I32), 0},
    {"_exit",                 (void *)compat__exit,                 SIG1(NM_RET_VOID, NM_ARG_I32), 0},
    {"abort",                 (void *)compat_abort,                 SIG0(NM_RET_VOID), 0},
    {"__stack_chk_fail",      (void *)compat___stack_chk_fail,      SIG0(NM_RET_VOID), 0},
    {"__errno_location",      (void *)compat___errno_location,      SIG0(NM_RET_PTR), 0},
    {"__libc_start_main",     (void *)compat___libc_start_main,     SIG6(NM_RET_I32, NM_ARG_U32, NM_ARG_I32, NM_ARG_PTR, NM_ARG_U32, NM_ARG_U32, NM_ARG_U32), 0},

    /* dlfcn */
    {"dlopen",  (void *)compat_dlopen,  SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_I32), 0},
    {"dlsym",   (void *)compat_dlsym,   SIG2(NM_RET_PTR, NM_ARG_PTR, NM_ARG_PTR), 0},
    {"dlerror", (void *)compat_dlerror, SIG0(NM_RET_PTR), 0},
    {"dlclose", (void *)compat_dlclose, SIG1(NM_RET_I32, NM_ARG_PTR), 0},

    /* libm */
    {"sqrt",  (void *)compat_sqrt,  SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"sqrtf", (void *)compat_sqrtf, SIG1(NM_RET_F32, NM_ARG_F32), 0},
    {"sin",   (void *)compat_sin,   SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"sinf",  (void *)compat_sinf,  SIG1(NM_RET_F32, NM_ARG_F32), 0},
    {"cos",   (void *)compat_cos,   SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"cosf",  (void *)compat_cosf,  SIG1(NM_RET_F32, NM_ARG_F32), 0},
    {"tan",   (void *)compat_tan,   SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"tanf",  (void *)compat_tanf,  SIG1(NM_RET_F32, NM_ARG_F32), 0},
    {"atan2", (void *)compat_atan2, SIG2(NM_RET_F64, NM_ARG_F64, NM_ARG_F64), 0},
    {"pow",   (void *)compat_pow,   SIG2(NM_RET_F64, NM_ARG_F64, NM_ARG_F64), 0},
    {"powf",  (void *)compat_powf,  SIG2(NM_RET_F32, NM_ARG_F32, NM_ARG_F32), 0},
    {"exp",   (void *)compat_exp,   SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"log",   (void *)compat_log,   SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"log10", (void *)compat_log10, SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"fabs",  (void *)compat_fabs,  SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"fabsf", (void *)compat_fabsf, SIG1(NM_RET_F32, NM_ARG_F32), 0},
    {"floor", (void *)compat_floor, SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"ceil",  (void *)compat_ceil,  SIG1(NM_RET_F64, NM_ARG_F64), 0},
    {"round", (void *)compat_round, SIG1(NM_RET_F64, NM_ARG_F64), 0},
};

static pthread_once_t g_sym_sort_once = PTHREAD_ONCE_INIT;

static int cmp_sym_entry(const void *a, const void *b) {
    const nm_sym_entry_t *sa = (const nm_sym_entry_t *)a;
    const nm_sym_entry_t *sb = (const nm_sym_entry_t *)b;
    return strcmp(sa->name, sb->name);
}

static void sort_sym_table(void) {
    size_t count = sizeof(g_sym_table) / sizeof(g_sym_table[0]);
    qsort(g_sym_table, count, sizeof(g_sym_table[0]), cmp_sym_entry);
}

int newminhh_compat_lookup_wrapper(const char *name, nm_sym_entry_t *out) {
    if (!name || name[0] == '\0' || !out) {
        NM_LOG_ERROR("wrappers", "lookup_wrapper: invalid argument (name=%p out=%p)",
                     (const void *)name, (void *)out);
        return NM_ERR_ABI;
    }
    pthread_once(&g_sym_sort_once, sort_sym_table);

    nm_sym_entry_t key = {.name = name};
    size_t count = sizeof(g_sym_table) / sizeof(g_sym_table[0]);
    const nm_sym_entry_t *found = (const nm_sym_entry_t *)bsearch(
        &key, g_sym_table, count, sizeof(g_sym_table[0]), cmp_sym_entry);
    if (!found) {
        NM_LOG_ERROR("wrappers", "unsupported symbol '%s' (no i386<->x32 ABI wrapper)", name);
        return NM_ERR_DLSYM;
    }
    *out = *found;
    return NM_OK;
}
