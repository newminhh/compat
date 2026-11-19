#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

#include "../common/newminhh_abi.h"
#include "../common/newminhh_log.h"

int newminhh_repack_timespec_to_32(const struct timespec *src, newminhh_timespec32_t *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_timespec_to_32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    if (src->tv_sec < INT32_MIN || src->tv_sec > INT32_MAX ||
        src->tv_nsec < 0 || src->tv_nsec >= 1000000000L) {
        NM_LOG_ERROR("structs", "repack_timespec_to_32: overflow tv_sec=%lld tv_nsec=%lld",
                     (long long)src->tv_sec, (long long)src->tv_nsec);
        errno = EOVERFLOW;
        return NM_ERR_ABI;
    }
    dst->tv_sec = (int32_t)src->tv_sec;
    dst->tv_nsec = (int32_t)src->tv_nsec;
    return NM_OK;
}

int newminhh_repack_timespec_from_32(const newminhh_timespec32_t *src, struct timespec *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_timespec_from_32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    if (src->tv_nsec < 0 || src->tv_nsec >= 1000000000L) {
        NM_LOG_ERROR("structs", "repack_timespec_from_32: invalid tv_nsec=%d", src->tv_nsec);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    dst->tv_sec = (time_t)src->tv_sec;
    dst->tv_nsec = (long)src->tv_nsec;
    return NM_OK;
}

int newminhh_repack_timespec_to_64t32(const struct timespec *src, newminhh_timespec64_t32_t *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_timespec_to_64t32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    if (src->tv_nsec < 0 || src->tv_nsec >= 1000000000L) {
        NM_LOG_ERROR("structs", "repack_timespec_to_64t32: invalid tv_nsec=%lld",
                     (long long)src->tv_nsec);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    newminhh_timespec64_t32_t tmp;
    tmp.tv_sec = (int64_t)src->tv_sec;
    tmp.tv_nsec = (int32_t)src->tv_nsec;
    tmp._pad = 0;
    memcpy(dst, &tmp, sizeof(tmp));
    return NM_OK;
}

int newminhh_repack_timespec_from_64t32(const newminhh_timespec64_t32_t *src, struct timespec *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_timespec_from_64t32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    newminhh_timespec64_t32_t tmp;
    memcpy(&tmp, src, sizeof(tmp));
    if (tmp.tv_nsec < 0 || tmp.tv_nsec >= 1000000000L) {
        NM_LOG_ERROR("structs", "repack_timespec_from_64t32: invalid tv_nsec=%d", tmp.tv_nsec);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    dst->tv_sec = (time_t)tmp.tv_sec;
    dst->tv_nsec = (long)tmp.tv_nsec;
    return NM_OK;
}

int newminhh_repack_rlimit_to_32(const struct rlimit *src, newminhh_rlimit32_t *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_rlimit_to_32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    dst->rlim_cur = (src->rlim_cur == RLIM_INFINITY || src->rlim_cur >= 0xFFFFFFFFULL)
                        ? 0xFFFFFFFFU
                        : (uint32_t)src->rlim_cur;
    dst->rlim_max = (src->rlim_max == RLIM_INFINITY || src->rlim_max >= 0xFFFFFFFFULL)
                        ? 0xFFFFFFFFU
                        : (uint32_t)src->rlim_max;
    return NM_OK;
}

int newminhh_repack_rlimit_from_32(const newminhh_rlimit32_t *src, struct rlimit *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_rlimit_from_32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    dst->rlim_cur = (src->rlim_cur == 0xFFFFFFFFU) ? RLIM_INFINITY : (rlim_t)src->rlim_cur;
    dst->rlim_max = (src->rlim_max == 0xFFFFFFFFU) ? RLIM_INFINITY : (rlim_t)src->rlim_max;
    return NM_OK;
}

int newminhh_repack_rlimit_to_64t32(const struct rlimit *src, newminhh_rlimit64_t32_t *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_rlimit_to_64t32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    newminhh_rlimit64_t32_t tmp;
    tmp.rlim_cur = (uint64_t)src->rlim_cur;
    tmp.rlim_max = (uint64_t)src->rlim_max;
    memcpy(dst, &tmp, sizeof(tmp));
    return NM_OK;
}

int newminhh_repack_rlimit_from_64t32(const newminhh_rlimit64_t32_t *src, struct rlimit *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_rlimit_from_64t32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    newminhh_rlimit64_t32_t tmp;
    memcpy(&tmp, src, sizeof(tmp));
    dst->rlim_cur = (rlim_t)tmp.rlim_cur;
    dst->rlim_max = (rlim_t)tmp.rlim_max;
    return NM_OK;
}

int newminhh_repack_stat_to_stat32(const struct stat *src, newminhh_stat32_t *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_stat_to_stat32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    if ((uint64_t)src->st_ino > UINT32_MAX ||
        src->st_size < INT32_MIN || src->st_size > INT32_MAX ||
        src->st_blocks < INT32_MIN || src->st_blocks > INT32_MAX ||
        (uint64_t)src->st_nlink > UINT32_MAX) {
        NM_LOG_ERROR("structs", "repack_stat_to_stat32: EOVERFLOW (ino=%llu size=%lld blocks=%lld)",
                     (unsigned long long)src->st_ino,
                     (long long)src->st_size,
                     (long long)src->st_blocks);
        errno = EOVERFLOW;
        return NM_ERR_ABI;
    }

    newminhh_stat32_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (newminhh_repack_timespec_to_32(&src->st_atim, &tmp.st_atim) != NM_OK ||
        newminhh_repack_timespec_to_32(&src->st_mtim, &tmp.st_mtim) != NM_OK ||
        newminhh_repack_timespec_to_32(&src->st_ctim, &tmp.st_ctim) != NM_OK) {
        return NM_ERR_ABI;
    }

    tmp.st_dev = (uint64_t)src->st_dev;
    tmp.st_ino = (uint32_t)src->st_ino;
    /* Note: x32 has st_nlink before st_mode; i386 has st_mode before st_nlink */
    tmp.st_mode = (uint32_t)src->st_mode;
    tmp.st_nlink = (uint32_t)src->st_nlink;
    tmp.st_uid = (uint32_t)src->st_uid;
    tmp.st_gid = (uint32_t)src->st_gid;
    tmp.st_rdev = (uint64_t)src->st_rdev;
    tmp.st_size = (int32_t)src->st_size;
    tmp.st_blksize = (int32_t)src->st_blksize;
    tmp.st_blocks = (int32_t)src->st_blocks;
    memcpy(dst, &tmp, sizeof(tmp));
    return NM_OK;
}

int newminhh_repack_stat_to_stat64(const struct stat *src, newminhh_stat64_t *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_stat_to_stat64: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    if ((uint64_t)src->st_nlink > UINT32_MAX) {
        NM_LOG_ERROR("structs", "repack_stat_to_stat64: st_nlink overflow (%llu)",
                     (unsigned long long)src->st_nlink);
        errno = EOVERFLOW;
        return NM_ERR_ABI;
    }

    newminhh_stat64_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    newminhh_timespec32_t atim, mtim, ctim;
    if (newminhh_repack_timespec_to_32(&src->st_atim, &atim) != NM_OK ||
        newminhh_repack_timespec_to_32(&src->st_mtim, &mtim) != NM_OK ||
        newminhh_repack_timespec_to_32(&src->st_ctim, &ctim) != NM_OK) {
        return NM_ERR_ABI;
    }

    tmp.st_dev = (uint64_t)src->st_dev;
    tmp.__st_ino = (uint32_t)src->st_ino;
    tmp.st_mode = (uint32_t)src->st_mode;
    tmp.st_nlink = (uint32_t)src->st_nlink;
    tmp.st_uid = (uint32_t)src->st_uid;
    tmp.st_gid = (uint32_t)src->st_gid;
    tmp.st_rdev = (uint64_t)src->st_rdev;
    tmp.st_size = (int64_t)src->st_size;
    tmp.st_blksize = (int32_t)src->st_blksize;
    tmp.st_blocks = (int64_t)src->st_blocks;
    tmp.st_atim = atim;
    tmp.st_mtim = mtim;
    tmp.st_ctim = ctim;
    tmp.st_ino = (uint64_t)src->st_ino;
    memcpy(dst, &tmp, sizeof(tmp));
    return NM_OK;
}

int newminhh_repack_stat_to_stat64_t64(const struct stat *src, newminhh_stat64_t64_t *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_stat_to_stat64_t64: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    if ((uint64_t)src->st_nlink > UINT32_MAX) {
        NM_LOG_ERROR("structs", "repack_stat_to_stat64_t64: st_nlink overflow");
        errno = EOVERFLOW;
        return NM_ERR_ABI;
    }

    newminhh_stat64_t64_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    newminhh_timespec64_t32_t atim, mtim, ctim;
    if (newminhh_repack_timespec_to_64t32(&src->st_atim, &atim) != NM_OK ||
        newminhh_repack_timespec_to_64t32(&src->st_mtim, &mtim) != NM_OK ||
        newminhh_repack_timespec_to_64t32(&src->st_ctim, &ctim) != NM_OK) {
        return NM_ERR_ABI;
    }

    tmp.st_dev = (uint64_t)src->st_dev;
    tmp.st_ino = (uint64_t)src->st_ino;
    tmp.st_mode = (uint32_t)src->st_mode;
    tmp.st_nlink = (uint32_t)src->st_nlink;
    tmp.st_uid = (uint32_t)src->st_uid;
    tmp.st_gid = (uint32_t)src->st_gid;
    tmp.st_rdev = (uint64_t)src->st_rdev;
    tmp.st_size = (int64_t)src->st_size;
    tmp.st_blksize = (int32_t)src->st_blksize;
    tmp.st_blocks = (int64_t)src->st_blocks;
    tmp.st_atim = atim;
    tmp.st_mtim = mtim;
    tmp.st_ctim = ctim;
    memcpy(dst, &tmp, sizeof(tmp));
    return NM_OK;
}

int newminhh_repack_stat_to_kstat32(const struct stat *src, newminhh_kstat32_t *dst) {
    if (!src || !dst) {
        NM_LOG_ERROR("structs", "repack_stat_to_kstat32: NULL argument (src=%p, dst=%p)",
                     (const void *)src, (void *)dst);
        errno = EINVAL;
        return NM_ERR_ABI;
    }
    if ((uint64_t)src->st_ino > UINT32_MAX ||
        src->st_size < 0 || (uint64_t)src->st_size > UINT32_MAX ||
        (uint64_t)src->st_blocks > UINT32_MAX ||
        (uint64_t)src->st_nlink > UINT16_MAX ||
        (uint64_t)src->st_uid > UINT16_MAX ||
        (uint64_t)src->st_gid > UINT16_MAX ||
        src->st_atim.tv_sec < 0 || src->st_atim.tv_sec > UINT32_MAX ||
        src->st_mtim.tv_sec < 0 || src->st_mtim.tv_sec > UINT32_MAX ||
        src->st_ctim.tv_sec < 0 || src->st_ctim.tv_sec > UINT32_MAX) {
        NM_LOG_ERROR("structs", "repack_stat_to_kstat32: EOVERFLOW (ino=%llu size=%lld)",
                     (unsigned long long)src->st_ino, (long long)src->st_size);
        errno = EOVERFLOW;
        return NM_ERR_ABI;
    }

    newminhh_kstat32_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.st_dev = (uint32_t)src->st_dev;
    tmp.st_ino = (uint32_t)src->st_ino;
    tmp.st_mode = (uint16_t)src->st_mode;
    tmp.st_nlink = (uint16_t)src->st_nlink;
    tmp.st_uid = (uint16_t)src->st_uid;
    tmp.st_gid = (uint16_t)src->st_gid;
    tmp.st_rdev = (uint32_t)src->st_rdev;
    tmp.st_size = (uint32_t)src->st_size;
    tmp.st_blksize = (uint32_t)src->st_blksize;
    tmp.st_blocks = (uint32_t)src->st_blocks;
    tmp.st_atime_sec = (uint32_t)src->st_atim.tv_sec;
    tmp.st_atime_nsec = (uint32_t)src->st_atim.tv_nsec;
    tmp.st_mtime_sec = (uint32_t)src->st_mtim.tv_sec;
    tmp.st_mtime_nsec = (uint32_t)src->st_mtim.tv_nsec;
    tmp.st_ctime_sec = (uint32_t)src->st_ctim.tv_sec;
    tmp.st_ctime_nsec = (uint32_t)src->st_ctim.tv_nsec;
    memcpy(dst, &tmp, sizeof(tmp));
    return NM_OK;
}
