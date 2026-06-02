/**
 *
 * Copyright (C) TidesDB
 *
 * Original Author: Alex Gaetano Padula
 *
 * Licensed under the Mozilla Public License, v. 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.mozilla.org/en-US/MPL/2.0/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __COMPAT_H__
#define __COMPAT_H__

/* compat header for multi-platform support (Windows, POSIX, posix includes macOS) */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

/* fallback for SIZE_MAX, just in case */
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#endif

#ifdef _WIN32
/* require Windows Vista+ APIs (SetFileInformationByHandle, FILE_ALLOCATION_INFO,
 * FILE_END_OF_FILE_INFO) used by tdb_preallocate_extent. defined before any
 * windows.h include below so the right structure declarations are visible. */
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#if !defined(WINVER) || WINVER < 0x0600
#undef WINVER
#define WINVER 0x0600
#endif
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

/* cross-platform line buffering -- Windows doesn't support _IOLBF properly with NULL buffer */
#if defined(_MSC_VER)
#define tdb_setlinebuf(stream) setvbuf((stream), NULL, _IONBF, 0)
#else
#define tdb_setlinebuf(stream) setvbuf((stream), NULL, _IOLBF, 0)
#endif

/* branch prediction hints for hot paths */
#if defined(__GNUC__) || defined(__clang__)
#define TDB_LIKELY(x)   __builtin_expect(!!(x), 1)
#define TDB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define TDB_LIKELY(x)   (x)
#define TDB_UNLIKELY(x) (x)
#endif

/* cross-platform fabs abstraction */
#include <math.h>
#if defined(_MSC_VER)
#define tdb_fabs(x) fabs(x)
#elif defined(__APPLE__)
/* macOS may require explicit declaration in some contexts */
#define tdb_fabs(x) fabs(x)
#else
/* POSIX systems */
#define tdb_fabs(x) fabs(x)
#endif

/* cross-platform fsync abstraction */
#if defined(_WIN32)
#include <io.h>
#define tdb_fsync(fd) _commit(fd)
#else
#include <unistd.h>
#define tdb_fsync(fd) fsync(fd)
#endif

/* file lock error codes */
#define TDB_LOCK_SUCCESS 0 /* lock acquired successfully */
#define TDB_LOCK_HELD    1 /* lock is held by another process (EWOULDBLOCK/EAGAIN) */
#define TDB_LOCK_ERROR   2 /* irrecoverable error */

/* default retry count for EINTR during lock acquisition */
#define TDB_LOCK_DEFAULT_RETRIES 3

/* cross-platform file locking abstraction for database directory lock */
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>

/*
 * tdb_open_lock_file
 * opens a lock file (windows version -- lock acquired separately)
 * @param path the path to the lock file
 * @param lock_result output -- TDB_LOCK_SUCCESS on successful open (lock not yet acquired)
 * @return file descriptor on success (>= 0), -1 on error
 */
static inline int tdb_open_lock_file(const char *path, int *lock_result)
{
    int fd = _open(path, _O_RDWR | _O_CREAT | _O_BINARY, 0644);
    if (fd < 0)
    {
        *lock_result = TDB_LOCK_ERROR;
        return -1;
    }
    *lock_result = TDB_LOCK_SUCCESS; /* caller will call tdb_file_lock_exclusive */
    return fd;
}

/*
 * tdb_file_lock_exclusive
 * acquires an exclusive lock on a file (non-blocking)
 * @param fd the file descriptor to lock
 * @param max_retries maximum retries for transient errors (i.e., signal interrupts)
 * @return TDB_LOCK_SUCCESS on success,
 *         TDB_LOCK_HELD if lock is held by another process,
 *         TDB_LOCK_ERROR on irrecoverable error
 */
static inline int tdb_file_lock_exclusive(int fd, int max_retries)
{
    (void)max_retries; /* windows with LOCKFILE_FAIL_IMMEDIATELY has no retryable errs */

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return TDB_LOCK_ERROR;

    OVERLAPPED ov = {0};
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov))
    {
        return TDB_LOCK_SUCCESS;
    }

    /* with LOCKFILE_FAIL_IMMEDIATELY, ERROR_LOCK_VIOLATION means lock is held
     **** https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex */
    DWORD err = GetLastError();
    if (err == ERROR_LOCK_VIOLATION)
    {
        return TDB_LOCK_HELD;
    }
    return TDB_LOCK_ERROR;
}

/*
 * tdb_file_unlock
 * releases a lock on a file
 * @param fd the file descriptor to unlock
 * @return 0 on success, -1 on error
 */
static inline int tdb_file_unlock(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    OVERLAPPED ov = {0};
    if (!UnlockFileEx(h, 0, 1, 0, &ov))
    {
        return -1;
    }
    return 0;
}
#else
#include <errno.h>
#include <fcntl.h>

/*** linux 3.15+ supports F_OFD_SETLK (Open File Description locks) which are per-fd
 * and have sane semantics. we should utilize these when available, and otherwise fall back to
 * fcntl() F_SETLK. https://lwn.net/Articles/640404/ and https://apenwarr.ca/log/20101213
 *
 * macOS/BSD -- We use fcntl() F_SETLK which has per-process semantics. Critically, fcntl() locks
 * are not inherited across fork(), so child processes will properly fail to acquire the lock.
 * flock() was considered but locks persist across fork(), causing the child to inherit the lock
 * and then block when trying to acquire a new lock on a different fd.
 * https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html
 */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
#define TDB_USE_FLOCK       0
#define TDB_USE_FCNTL_SETLK 1
#include <sys/file.h>
#elif !defined(F_OFD_SETLK)
#define TDB_USE_FLOCK       1
#define TDB_USE_FCNTL_SETLK 0
#include <sys/file.h>
#else
#define TDB_USE_FLOCK       0
#define TDB_USE_FCNTL_SETLK 0
#endif

/*
 * tdb_open_lock_file
 * opens a lock file for locking (lock acquired separately via tdb_file_lock_exclusive)
 * @param path the path to the lock file
 * @param lock_result output -- TDB_LOCK_SUCCESS, TDB_LOCK_HELD, or TDB_LOCK_ERROR
 * @return file descriptor on success (>= 0), -1 on error
 */
static inline int tdb_open_lock_file(const char *path, int *lock_result)
{
    /* open the lock file */
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        *lock_result = TDB_LOCK_ERROR;
        return -1;
    }

#if TDB_USE_FCNTL_SETLK
    /* fcntl() F_SETLK allows same-process re-locking, so check PID file first.
     * read PID before acquiring lock to detect same-process double-open. */
    char pid_buf[32] = {0};
    ssize_t n = pread(fd, pid_buf, sizeof(pid_buf) - 1, 0);
    if (n > 0)
    {
        pid_t file_pid = (pid_t)atol(pid_buf);
        if (file_pid == getpid())
        {
            /* same process already holds lock */
            close(fd);
            *lock_result = TDB_LOCK_HELD;
            return -1;
        }
    }
#endif

    *lock_result = TDB_LOCK_SUCCESS;
    return fd;
}

#if TDB_USE_FCNTL_SETLK
/*
 * tdb_file_lock_write_pid
 * writes the current PID to the lock file after acquiring the lock
 * @param fd the file descriptor of the lock file
 */
static inline void tdb_file_lock_write_pid(const int fd)
{
    char our_pid[32];
    int len = snprintf(our_pid, sizeof(our_pid), "%d\n", (int)getpid());
    if (ftruncate(fd, 0) == 0)
    {
        (void)pwrite(fd, our_pid, len, 0);
    }
}

/*
 * tdb_file_lock_clear_pid
 * clears the PID from the lock file before releasing the lock
 * @param fd the file descriptor of the lock file
 */
static inline void tdb_file_lock_clear_pid(const int fd)
{
    (void)ftruncate(fd, 0);
}
#endif

/*
 * tdb_file_lock_exclusive
 * acquires an exclusive lock on a file (non-blocking)
 * uses fcntl() F_SETLK on macOS/BSD (locks not inherited across fork)
 * uses flock() on older systems without F_OFD_SETLK
 * uses F_OFD_SETLK on linux 3.15+ for per-fd locking
 * @param fd the file descriptor to lock
 * @param max_retries maximum retries for EINTR (signal interrupts)
 * @return TDB_LOCK_SUCCESS on success,
 *         TDB_LOCK_HELD if lock is held by another process,
 *         TDB_LOCK_ERROR on irrecoverable error
 */
static inline int tdb_file_lock_exclusive(const int fd, int max_retries)
{
    int retries = 0;
    if (max_retries <= 0) max_retries = TDB_LOCK_DEFAULT_RETRIES;

#if TDB_USE_FCNTL_SETLK
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0;

    while (retries <= max_retries)
    {
        if (fcntl(fd, F_SETLK, &fl) == 0)
        {
            /* we write PID to lock file for same-process detection */
            tdb_file_lock_write_pid(fd);
            return TDB_LOCK_SUCCESS;
        }

        int err = errno;

#if EWOULDBLOCK == EAGAIN
        if (err == EWOULDBLOCK || err == EACCES)
#else
        if (err == EWOULDBLOCK || err == EAGAIN || err == EACCES)
#endif
        {
            return TDB_LOCK_HELD;
        }
        if (err == EINTR)
        {
            retries++;
            continue;
        }
        return TDB_LOCK_ERROR;
    }
    return TDB_LOCK_ERROR;
#elif TDB_USE_FLOCK
    while (retries <= max_retries)
    {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
            return TDB_LOCK_SUCCESS;
        }

        int err = errno;

#if EWOULDBLOCK == EAGAIN
        if (err == EWOULDBLOCK || err == EACCES)
#else
        if (err == EWOULDBLOCK || err == EAGAIN || err == EACCES)
#endif
        {
            return TDB_LOCK_HELD;
        }
        if (err == EINTR)
        {
            retries++;
            continue;
        }
        return TDB_LOCK_ERROR;
    }
    return TDB_LOCK_ERROR;
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0; /* ignored for OFD locks */

    while (retries <= max_retries)
    {
        if (fcntl(fd, F_OFD_SETLK, &fl) == 0)
        {
            return TDB_LOCK_SUCCESS;
        }

        int err = errno;

#if EWOULDBLOCK == EAGAIN
        if (err == EWOULDBLOCK || err == EACCES)
#else
        if (err == EWOULDBLOCK || err == EAGAIN || err == EACCES)
#endif
        {
            return TDB_LOCK_HELD;
        }
        if (err == EINTR)
        {
            retries++;
            continue;
        }
        return TDB_LOCK_ERROR;
    }
    return TDB_LOCK_ERROR;
#endif
}

/*
 * tdb_file_unlock
 * releases a lock on a file
 * @param fd the file descriptor to unlock
 * @return 0 on success, -1 on error
 */
static inline int tdb_file_unlock(const int fd)
{
#if TDB_USE_FCNTL_SETLK
    tdb_file_lock_clear_pid(fd);

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0;

    if (fcntl(fd, F_SETLK, &fl) != 0)
    {
        return -1;
    }
    return 0;
#elif TDB_USE_FLOCK
    if (flock(fd, LOCK_UN) != 0)
    {
        return -1;
    }
    return 0;
#else
    /* linux with F_OFD_SETLK */
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0;

    if (fcntl(fd, F_OFD_SETLK, &fl) != 0)
    {
        return -1;
    }
    return 0;
#endif
}
#endif

/* cross-platform localtime abstraction */
#if defined(_WIN32)
/* (MSVC and MinGW) use localtime_s with reversed parameter order */
#define tdb_localtime(timer, result) localtime_s((result), (timer))
#else
/* POSIX uses localtime_r */
#define tdb_localtime(timer, result) localtime_r((timer), (result))
#endif

/* https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/stat-functions?view=msvc-170
 * https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fstat-fstat32-fstat64-fstati64-fstat32i64-fstat64i32?view=msvc-170
 * to handle the compiler differences
 */
#if defined(_WIN32)
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_MSC_VER)
#define STAT_STRUCT _stat64
#define STAT_FUNC   _stat64
#define FSTAT_FUNC  _fstat64
#else
#define STAT_STRUCT stat
#define STAT_FUNC   stat
#define FSTAT_FUNC  fstat
#endif

#else /* posix */
#include <sys/stat.h>
#include <sys/statvfs.h>
#define STAT_STRUCT stat
#define STAT_FUNC   stat
#define FSTAT_FUNC  fstat
#endif

#if !defined(_MSC_VER) || _MSC_VER >= 1930
#include <stdatomic.h>
typedef atomic_size_t atomic_size_t;
typedef atomic_uint_fast64_t atomic_uint64_t;
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
#define TDB_SIZE_FMT     "%llu"
#define TDB_U64_FMT      "%llu"
#define TDB_SIZE_CAST(x) ((unsigned long long)(x))
#define TDB_U64_CAST(x)  ((unsigned long long)(x))
#else
#define TDB_SIZE_FMT     "%zu"
#define TDB_U64_FMT      "%" PRIu64
#define TDB_SIZE_CAST(x) ((size_t)(x))
#define TDB_U64_CAST(x)  ((uint64_t)(x))
#endif

/* cross-platform atomic alignment */
#if defined(_MSC_VER)
#define ATOMIC_ALIGN(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
#define ATOMIC_ALIGN(n) __attribute__((aligned(n)))
#else
#define ATOMIC_ALIGN(n)
#endif

/* cross-platform unused attribute for static functions */
#if defined(__GNUC__) || defined(__clang__)
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif

/* cross-platform thread-local storage */
#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL /* fallback -- no thread-local support */
#endif

/* cross-platform prefetch hints for cache optimization */
#if defined(__GNUC__) || defined(__clang__)
/* __builtin_prefetch(addr, rw, locality)
 * rw -- 0 = read, 1 = write
 * locality-- 0 = no temporal locality, 3 = high temporal locality */
#define PREFETCH_READ(addr)  __builtin_prefetch((addr), 0, 3)
#define PREFETCH_WRITE(addr) __builtin_prefetch((addr), 1, 3)
#elif defined(_MSC_VER)
#include <intrin.h>
#define PREFETCH_READ(addr)  _mm_prefetch((const char *)(addr), _MM_HINT_T0)
#define PREFETCH_WRITE(addr) _mm_prefetch((const char *)(addr), _MM_HINT_T0)
#else
/* no prefetch support -- define as no-op */
#define PREFETCH_READ(addr)  ((void)0)
#define PREFETCH_WRITE(addr) ((void)0)
#endif

/* cross-platform count trailing zeros for 64-bit integers */
#if defined(__GNUC__) || defined(__clang__)
#define TDB_CTZ64(x) __builtin_ctzll(x)
#elif defined(_MSC_VER)
/*
 * tdb_ctz64_msvc
 * counts trailing zeros in a 64-bit integer (MSVC version)
 * @param x the value to count trailing zeros in
 * @return number of trailing zero bits (0-63), or 64 if x is 0
 */
static inline int tdb_ctz64_msvc(uint64_t x)
{
    unsigned long index;
#if defined(_WIN64)
    if (_BitScanForward64(&index, x))
    {
        return (int)index;
    }
#else
    /* 32-bit MSVC-- check low and high 32-bit halves */
    if (_BitScanForward(&index, (unsigned long)x))
    {
        return (int)index;
    }
    if (_BitScanForward(&index, (unsigned long)(x >> 32)))
    {
        return (int)(index + 32);
    }
#endif
    return 64; /* all zeros */
}
#define TDB_CTZ64(x) tdb_ctz64_msvc(x)
#else
/* portable fallback using de Bruijn sequence */
/*
 * tdb_ctz64_portable
 * counts trailing zeros in a 64-bit integer (portable version)
 * @param x the value to count trailing zeros in
 * @return number of trailing zero bits (0-63), or 64 if x is 0
 */
static inline int tdb_ctz64_portable(uint64_t x)
{
    if (x == 0) return 64;
    static const int debruijn_table[64] = {
        0,  1,  2,  53, 3,  7,  54, 27, 4,  38, 41, 8,  34, 55, 48, 28, 62, 5,  39, 46, 44, 42,
        22, 9,  24, 35, 59, 56, 49, 18, 29, 11, 63, 52, 6,  26, 37, 40, 33, 47, 61, 45, 43, 21,
        23, 58, 17, 10, 51, 25, 36, 32, 60, 20, 57, 16, 50, 31, 19, 15, 30, 14, 13, 12};
    return debruijn_table[((x & -x) * 0x022FDD63CC95386DULL) >> 58];
}
#define TDB_CTZ64(x) tdb_ctz64_portable(x)
#endif

/* cross-platform thread ID for unique file naming */
#if defined(_WIN32)
#include <windows.h>
#define TDB_THREAD_ID() ((unsigned long)GetCurrentThreadId())
#else
#include <pthread.h>
#define TDB_THREAD_ID() ((unsigned long)pthread_self())
#endif

/* cross-platform process ID */
#if defined(_WIN32)
#include <process.h>
#define TDB_GETPID() _getpid()
#else
#include <sys/wait.h>
#include <unistd.h>
#define TDB_GETPID() getpid()
#endif

/**
 * tdb_spawn_wait
 * spawn a child process running cmd with the given argument vector and block
 * until it exits. argv is NULL terminated and argv[0] is the program name.
 * cmd is resolved like execvp, a PATH search when it contains no separator,
 * and _spawnvp applies the same resolution on Windows.
 * @param cmd executable to run
 * @param argv NULL-terminated argument vector, argv[0] is the program name
 * @return the child exit code on a normal exit, -1 on spawn failure or an
 *         abnormal exit
 */
static inline int tdb_spawn_wait(const char *cmd, char *const argv[])
{
#ifdef _WIN32
    intptr_t rc = _spawnvp(_P_WAIT, cmd, (const char *const *)argv);
    return (rc < 0) ? -1 : (int)rc;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0)
    {
        execvp(cmd, argv);
        _exit(127); /* execvp only returns on failure */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>

#if defined(_MSC_VER)
#pragma warning(disable : 4996) /* disable deprecated warning for windows */
#pragma warning(disable : 4029) /* declared formal parameter list different from definition */
#pragma warning(disable : 4211) /* nonstandard extension used-- redefined extern to static */
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
/* mingw provides POSIX-like headers */
#include <dirent.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

/* mingw mkdir only takes one argument, create a wrapper for POSIX compatibility */
#define mkdir(path, mode) _mkdir(path)
#else
/* msvc needs pthreads-win32 library */
#include "pthread.h"
#endif

#if defined(_MSC_VER)
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef __int64 off_t;
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef __int64 ssize_t;
#endif

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef int mode_t;
#endif

/* ftruncate for windows */
/*
 * ftruncate
 * @param fd the file descriptor to truncate
 * @param length the new length of the file
 * @return 0 on success, -1 on failure
 */
static inline int ftruncate(int fd, off_t length)
{
    return _chsize_s(fd, length);
}

/* open for windows */
/*
 * open
 * @param path the path to open
 * @param flags the flags to use
 * @param mode the mode to use (only used if O_CREAT is set)
 * @return the file descriptor on success, -1 on failure
 */
static inline int _tidesdb_open_wrapper_3(const char *path, int flags, mode_t mode)
{
    return _sopen(path, flags | _O_BINARY | _O_SEQUENTIAL, _SH_DENYNO, mode);
}

/* open for windows */
/*
 * open
 * @param path the path to open
 * @param flags the flags to use
 * @return the file descriptor on success, -1 on failure
 */
static inline int _tidesdb_open_wrapper_2(const char *path, int flags)
{
    return _sopen(path, flags | _O_BINARY, _SH_DENYNO, 0);
}
#define open(...) _tidesdb_open_wrapper_3(__VA_ARGS__)

/* C11 atomics support */
#if defined(__MINGW32__) || defined(__GNUC__)
/* mingw and GCC have proper C11 stdatomic.h support */
#include <stdatomic.h>
#elif _MSC_VER < 1930
/* MSVC < 2022 doesn't have stdatomic.h -- use Windows Interlocked functions */
typedef volatile LONG atomic_int;
typedef volatile LONGLONG atomic_size_t;
typedef volatile LONGLONG atomic_uint64_t;
#define _Atomic(T) volatile T

#ifdef _WIN64
/* 64-bit atomic store */
/*
 * atomic_store_explicit
 * @param ptr the pointer to store the value at
 * @param val the value to store
 * @param order the memory order (unused)
 */
#define atomic_store_explicit(ptr, val, order)                                             \
    do                                                                                     \
    {                                                                                      \
        if (sizeof(*(ptr)) == sizeof(void *))                                              \
        {                                                                                  \
            InterlockedExchangePointer((PVOID volatile *)(ptr), (PVOID)(uintptr_t)(val));  \
        }                                                                                  \
        else if (sizeof(*(ptr)) == 8)                                                      \
        {                                                                                  \
            InterlockedExchange64((LONGLONG volatile *)(ptr), (LONGLONG)(uintptr_t)(val)); \
        }                                                                                  \
        else if (sizeof(*(ptr)) == 4)                                                      \
        {                                                                                  \
            InterlockedExchange((LONG volatile *)(ptr), (LONG)(uintptr_t)(val));           \
        }                                                                                  \
        else                                                                               \
        {                                                                                  \
            *(ptr) = (val);                                                                \
        }                                                                                  \
    } while (0)
#else
/* 32-bit atomic store */
/*
 * atomic_store_explicit
 * @param ptr the pointer to store the value at
 * @param val the value to store
 * @param order the memory order (unused)
 */
#define atomic_store_explicit(ptr, val, order)                                            \
    do                                                                                    \
    {                                                                                     \
        if (sizeof(*(ptr)) == sizeof(void *))                                             \
        {                                                                                 \
            InterlockedExchangePointer((PVOID volatile *)(ptr), (PVOID)(uintptr_t)(val)); \
        }                                                                                 \
        else if (sizeof(*(ptr)) == 8)                                                     \
        {                                                                                 \
            /* 64-bit value on a 32-bit target cast straight to LONGLONG, NOT via         \
             * uintptr_t (4 bytes here) which would truncate the input */                 \
            InterlockedExchange64((LONGLONG volatile *)(ptr), (LONGLONG)(val));           \
        }                                                                                 \
        else if (sizeof(*(ptr)) == 4)                                                     \
        {                                                                                 \
            InterlockedExchange((LONG volatile *)(ptr), (LONG)(uintptr_t)(val));          \
        }                                                                                 \
        else                                                                              \
        {                                                                                 \
            *(ptr) = (val);                                                               \
        }                                                                                 \
    } while (0)
#endif

/* atomic load */
/*
 * _atomic_load_ptr
 * @param ptr the pointer to load the value from
 * @return the value loaded from the pointer
 */
static inline void *_atomic_load_ptr(volatile void *const *ptr)
{
    return (void *)InterlockedCompareExchangePointer((PVOID volatile *)ptr, NULL, NULL);
}

/* atomic load -- available on both _WIN64 and 32-bit (InterlockedCompareExchange64
 * is provided on 32-bit Windows too, so 64-bit atomics work on a 32-bit target) */
/*
 * _atomic_load_i64
 * @param ptr the pointer to load the value from
 * @return the value loaded from the pointer
 */
static inline LONGLONG _atomic_load_i64(volatile LONGLONG *ptr)
{
    return InterlockedCompareExchange64((LONGLONG volatile *)ptr, 0, 0);
}

/* atomic load */
/*
 * _atomic_load_i32
 * @param ptr the pointer to load the value from
 * @return the value loaded from the pointer
 */
static inline LONG _atomic_load_i32(volatile LONG *ptr)
{
    return InterlockedCompareExchange((LONG volatile *)ptr, 0, 0);
}

/* atomic load */
/*
 * _atomic_load_u8
 * @param ptr the pointer to load the value from
 * @return the value loaded from the pointer
 */
static inline unsigned char _atomic_load_u8(volatile unsigned char *ptr)
{
    return *ptr; /* byte reads are atomic on x86/x64 */
}

#ifdef _WIN64
/* atomic load */
/*
 * atomic_load_explicit
 * @param ptr the pointer to load the value from
 * @param order the memory order (unused)
 * @return the value loaded from the pointer
 */
#define atomic_load_explicit(ptr, order)                                                     \
    (sizeof(*(ptr)) == sizeof(void *) ? _atomic_load_ptr((volatile void *const *)(ptr))      \
     : sizeof(*(ptr)) == 8 ? (void *)(uintptr_t)_atomic_load_i64((volatile LONGLONG *)(ptr)) \
     : sizeof(*(ptr)) == 4 ? (void *)(uintptr_t)_atomic_load_i32((volatile LONG *)(ptr))     \
                           : (void *)(uintptr_t)_atomic_load_u8((volatile unsigned char *)(ptr)))
#else
/* atomic load */
/*
 * atomic_load_explicit
 * @param ptr the pointer to load the value from
 * @param order the memory order (unused)
 * @return the value loaded from the pointer
 */
/* NOTE (32-bit MSVC < 2022) this path returns unsigned long long, not void*, so a
 * 64-bit atomic (sizeof==8, e.g. atomic_uint64_t / atomic_size_t) is loaded at full
 * width -- routing it through (void*)(uintptr_t) as the _WIN64 path does would truncate
 * to 32 bits here. Pointer and 32-bit values widen losslessly. A caller assigning the
 * result to a pointer gets an integer->pointer conversion (cast as needed).
 * This whole 32-bit MSVC<2022 atomics path MUST be compiled and tested on the target. */
#define atomic_load_explicit(ptr, order)                                                      \
    (sizeof(*(ptr)) == sizeof(void *)                                                         \
         ? (unsigned long long)(uintptr_t)_atomic_load_ptr((volatile void *const *)(ptr))     \
     : sizeof(*(ptr)) == 8 ? (unsigned long long)_atomic_load_i64((volatile LONGLONG *)(ptr)) \
     : sizeof(*(ptr)) == 4                                                                    \
         ? (unsigned long long)(uintptr_t)_atomic_load_i32((volatile LONG *)(ptr))            \
         : (unsigned long long)_atomic_load_u8((volatile unsigned char *)(ptr)))
#endif

/* atomic exchange */
#ifdef _WIN64
/* atomic exchange */
/*
 * atomic_exchange_explicit
 * @param ptr the pointer to exchange the value at
 * @param val the value to exchange
 * @param order the memory order (unused)
 * @return the value exchanged from the pointer
 */
#define atomic_exchange_explicit(ptr, val, order)                                       \
    (sizeof(*(ptr)) == sizeof(void *)                                                   \
         ? InterlockedExchangePointer((PVOID volatile *)(ptr), (PVOID)(uintptr_t)(val)) \
     : sizeof(*(ptr)) == 8                                                              \
         ? (void *)(uintptr_t)InterlockedExchange64((LONGLONG volatile *)(ptr),         \
                                                    (LONGLONG)(uintptr_t)(val))         \
         : (void *)(uintptr_t)InterlockedExchange((LONG volatile *)(ptr), (LONG)(uintptr_t)(val)))
#else
/* atomic exchange */
/*
 * atomic_exchange_explicit
 * @param ptr the pointer to exchange the value at
 * @param val the value to exchange
 * @param order the memory order (unused)
 * @return the value exchanged from the pointer
 */
/* NOTE (32-bit MSVC < 2022) returns unsigned long long for the same reason as
 * atomic_load_explicit above -- the 8-byte arm must not truncate. Verify on target. */
#define atomic_exchange_explicit(ptr, val, order)                                                  \
    (sizeof(*(ptr)) == sizeof(void *) ? (unsigned long long)(uintptr_t)InterlockedExchangePointer( \
                                            (PVOID volatile *)(ptr), (PVOID)(uintptr_t)(val))      \
     : sizeof(*(ptr)) == 8                                                                         \
         ? (unsigned long long)InterlockedExchange64((LONGLONG volatile *)(ptr), (LONGLONG)(val))  \
         : (unsigned long long)(uintptr_t)InterlockedExchange((LONG volatile *)(ptr),              \
                                                              (LONG)(uintptr_t)(val)))
#endif

#ifdef _WIN64
/* atomic fetch add */
/*
 * atomic_fetch_add
 * @param ptr the pointer to add the value to
 * @param val the value to add
 * @return the value before the addition
 */
#define atomic_fetch_add(ptr, val) \
    InterlockedExchangeAdd64((LONGLONG volatile *)(ptr), (LONGLONG)(val))
#else
/* atomic fetch add */
/*
 * atomic_fetch_add
 * @param ptr the pointer to add the value to
 * @param val the value to add
 * @return the value before the addition
 */
/* 32-bit dispatch on width so an 8-byte counter (atomic_uint64_t / atomic_size_t)
 * uses the 64-bit intrinsic instead of truncating to LONG. Returns unsigned long long. */
#define atomic_fetch_add(ptr, val)                                                    \
    (sizeof(*(ptr)) == 8 ? (unsigned long long)InterlockedExchangeAdd64(              \
                               (LONGLONG volatile *)(ptr), (LONGLONG)(val))           \
                         : (unsigned long long)(unsigned long)InterlockedExchangeAdd( \
                               (LONG volatile *)(ptr), (LONG)(val)))
#endif

/* atomic store */
/*
 * atomic_store
 * @param ptr the pointer to store the value at
 * @param val the value to store
 */
#define atomic_store(ptr, val) atomic_store_explicit(ptr, val, memory_order_seq_cst)
/* atomic load */
/*
 * atomic_load
 * @param ptr the pointer to load the value from
 * @return the value loaded from the pointer
 */
#define atomic_load(ptr)       atomic_load_explicit(ptr, memory_order_seq_cst)
#define memory_order_relaxed   0
#define memory_order_acquire   1
#define memory_order_release   2
#define memory_order_seq_cst   3

/* atomic compare exchange for pointers (MSVC compatibility) */
/*
 * atomic_compare_exchange_strong_ptr
 * @param ptr pointer to atomic pointer
 * @param expected pointer to expected value
 * @param desired new value to store
 * @return 1 if successful, 0 if failed
 */
static inline int atomic_compare_exchange_strong_ptr(void *volatile *ptr, void **expected,
                                                     void *desired)
{
    void *old =
        InterlockedCompareExchangePointer((PVOID volatile *)ptr, (PVOID)desired, (PVOID)*expected);
    if (old == *expected)
    {
        return 1;
    }
    *expected = old;
    return 0;
}

#endif /* _MSC_VER < 1930 */

/* access flags are normally defined in unistd.h, which unavailable under MSVC
 *
 * instead, define the flags as documented at
 * https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/access-waccess */
#ifndef F_OK
#define F_OK 00
#endif
#ifndef W_OK
#define W_OK 02
#endif
#ifndef R_OK
#define R_OK 04
#endif
#endif

#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif
#ifndef O_SEQUENTIAL
#define O_SEQUENTIAL _O_SEQUENTIAL
#endif

#ifndef M_LN2
#define M_LN2 0.69314718055994530942 /* log_e 2 */
#endif

#if defined(_MSC_VER)
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timezone
{
    int tz_minuteswest;
    int tz_dsttime;
};

struct dirent
{
    char d_name[MAX_PATH];
};

typedef struct
{
    HANDLE hFind;
    WIN32_FIND_DATA findFileData;
    struct dirent dirent;
} DIR;

/* mkdir */
/*
 * mkdir
 * @param path the path to create the directory at
 * @param mode the mode to create the directory with (unused on windows)
 * @return 0 on success, -1 on failure
 */
static inline int mkdir(const char *path, mode_t mode)
{
    (void)mode; /* unused on windows */
    return _mkdir(path);
}

/* opendir */
/*
 * opendir
 * @param name the name of the directory to open
 * @return a pointer to the directory stream, or NULL on failure
 */
static inline DIR *opendir(const char *name)
{
    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (dir == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }
    char search_path[MAX_PATH];
    snprintf(search_path, MAX_PATH, "%s\\*", name);
    dir->hFind = FindFirstFile(search_path, &dir->findFileData);
    if (dir->hFind == INVALID_HANDLE_VALUE)
    {
        free(dir);
        return NULL;
    }
    return dir;
}

/* readdir */
/*
 * readdir
 * @param dir the directory stream to read from
 * @return a pointer to the next directory entry, or NULL on failure
 */
static inline struct dirent *readdir(DIR *dir)
{
    if (dir == NULL || dir->hFind == INVALID_HANDLE_VALUE)
    {
        return NULL;
    }
    if (dir->findFileData.cFileName[0] == '\0')
    {
        if (!FindNextFile(dir->hFind, &dir->findFileData))
        {
            return NULL;
        }
    }
    strncpy(dir->dirent.d_name, dir->findFileData.cFileName, MAX_PATH);
    dir->findFileData.cFileName[0] = '\0'; /* reset */
    return &dir->dirent;
}

/* closedir */
/*
 * closedir
 * @param dir the directory stream to close
 * @return 0 on success, -1 on failure
 */
static inline int closedir(DIR *dir)
{
    if (dir == NULL)
    {
        return -1;
    }
    if (dir->hFind != INVALID_HANDLE_VALUE)
    {
        FindClose(dir->hFind);
    }
    free(dir);
    return 0;
}

typedef struct
{
    HANDLE handle;
} sem_t;

/* sem_init */
/*
 * sem_init
 * @param sem the semaphore to initialize
 * @param pshared whether the semaphore is shared between processes (unused on windows)
 * @param value the initial value of the semaphore
 * @return 0 on success, -1 on failure
 */
static inline int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    (void)pshared;
    sem->handle = CreateSemaphore(NULL, value, LONG_MAX, NULL);
    if (sem->handle == NULL)
    {
        errno = GetLastError();
        return -1;
    }
    return 0;
}

/* sem_destroy */
/*
 * sem_destroy
 * @param sem the semaphore to destroy
 * @return 0 on success, -1 on failure
 */
static inline int sem_destroy(sem_t *sem)
{
    if (sem->handle != NULL)
    {
        CloseHandle(sem->handle);
        sem->handle = NULL;
    }
    return 0;
}

/* sem_wait */
/*
 * sem_wait
 * @param sem the semaphore to wait on
 * @return 0 on success, -1 on failure
 */
static inline int sem_wait(sem_t *sem)
{
    DWORD result = WaitForSingleObject(sem->handle, INFINITE);
    return (result == WAIT_OBJECT_0) ? 0 : -1;
}

/* sem_post */
/*
 * sem_post
 * @param sem the semaphore to post
 * @return 0 on success, -1 on failure
 */
static inline int sem_post(sem_t *sem)
{
    return ReleaseSemaphore(sem->handle, 1, NULL) ? 0 : -1;
}

/* file operations macros for cross-platform compatibility */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#endif
#define sleep(seconds)       Sleep((seconds)*1000)
#define usleep(microseconds) Sleep((microseconds) / 1000) /* usleep for Windows */
#define access               _access
#define ftell                _ftelli64
#define fseek                _fseeki64

/* fopen wrapper for windows */
/*
 * tdb_fopen
 * @param filename the filename to open
 * @param mode the mode to open the file in
 * @return a pointer to the opened file, or NULL on failure
 */
static inline FILE *tdb_fopen(const char *filename, const char *mode)
{
    return _fsopen(filename, mode, _SH_DENYNO);
}
#define fopen tdb_fopen

/* fsync for windows */
/*
 * fsync
 * @param fd the file descriptor to sync
 * @return 0 on success, -1 on failure
 */
static inline int fsync(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return -1;
    }
    if (!FlushFileBuffers(h))
    {
        errno = GetLastError();
        return -1;
    }
    return 0;
}

/* fdatasync for MSVC, same as fsync (windows doesn't distinguish) */
/*
 * fdatasync
 * @param fd the file descriptor to sync
 * @return 0 on success, -1 on failure
 */
static inline int fdatasync(int fd)
{
    return fsync(fd);
}

/* clock_gettime for MSVC */
/*
 * clock_gettime
 * @param clk_id the clock ID (unused)
 * @param tp the timespec struct to fill
 * @return 0 on success, -1 on failure
 */
static inline int clock_gettime(int clk_id, struct timespec *tp)
{
    (void)clk_id;
    FILETIME ft;
    ULARGE_INTEGER ui;

    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;

    /* convert 100-nanosecond intervals to seconds and nanoseconds */
    tp->tv_sec = (long)((ui.QuadPart - 116444736000000000ULL) / 10000000ULL);
    tp->tv_nsec = (long)((ui.QuadPart % 10000000ULL) * 100);

    return 0;
}

/* gettimeofday for MSVC */
/*
 * gettimeofday
 * @param tp the timeval struct to fill
 * @param tzp the timezone struct (unused)
 * @return 0 on success, -1 on failure
 */
static inline int gettimeofday(struct timeval *tp, struct timezone *tzp)
{
    (void)tzp;
    FILETIME ft;
    ULARGE_INTEGER ui;

    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;

    /* convert to microseconds */
    tp->tv_sec = (long)((ui.QuadPart - 116444736000000000ULL) / 10000000ULL);
    tp->tv_usec = (long)((ui.QuadPart % 10000000ULL) / 10);

    return 0;
}

/* pread/pwrite for MSVC using OVERLAPPED
 */
/*
 * pread
 * reads data from a file descriptor at a specific offset
 * @param fd the file descriptor to read from
 * @param buf the buffer to read into
 * @param count the number of bytes to read
 * @param offset the offset to read from
 * @return the number of bytes read, or -1 on error
 */
static inline ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    if (count == 0)
    {
        return 0; /* reading 0 bytes is valid, returns 0 */
    }

    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return -1;
    }

    OVERLAPPED overlapped;
    ZeroMemory(&overlapped, sizeof(OVERLAPPED));

    LARGE_INTEGER li;
    li.QuadPart = offset;
    overlapped.Offset = li.LowPart;
    overlapped.OffsetHigh = li.HighPart;

    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (overlapped.hEvent == NULL)
    {
        errno = GetLastError();
        return -1;
    }

    DWORD bytes_read = 0;
    BOOL result = ReadFile(h, buf, (DWORD)count, &bytes_read, &overlapped);

    if (!result)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            if (!GetOverlappedResult(h, &overlapped, &bytes_read, TRUE))
            {
                CloseHandle(overlapped.hEvent);
                errno = GetLastError();
                return -1;
            }
        }
        else
        {
            CloseHandle(overlapped.hEvent);
            errno = err;
            return -1;
        }
    }

    CloseHandle(overlapped.hEvent);
    return (ssize_t)bytes_read;
}

/*
 * pwrite
 * writes data to a file descriptor at a specific offset
 * @param fd the file descriptor to write to
 * @param buf the buffer to write from
 * @param count the number of bytes to write
 * @param offset the offset to write at
 * @return the number of bytes written, or -1 on error
 */
static inline ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (count == 0)
    {
        return 0; /* writing 0 bytes is valid, returns 0 */
    }

    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return -1;
    }

    OVERLAPPED overlapped;
    ZeroMemory(&overlapped, sizeof(OVERLAPPED));

    LARGE_INTEGER li;
    li.QuadPart = offset;
    overlapped.Offset = li.LowPart;
    overlapped.OffsetHigh = li.HighPart;

    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (overlapped.hEvent == NULL)
    {
        errno = GetLastError();
        return -1;
    }

    DWORD bytes_written = 0;
    BOOL result = WriteFile(h, buf, (DWORD)count, &bytes_written, &overlapped);

    if (!result)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            if (!GetOverlappedResult(h, &overlapped, &bytes_written, TRUE))
            {
                CloseHandle(overlapped.hEvent);
                errno = GetLastError();
                return -1;
            }
        }
        else
        {
            CloseHandle(overlapped.hEvent);
            errno = err;
            return -1;
        }
    }

    CloseHandle(overlapped.hEvent);
    return (ssize_t)bytes_written;
}
#endif /* _MSC_VER */

/* fileno for all Windows (MSVC and MinGW) */
/*
 * tdb_fileno
 * portable file descriptor extraction from FILE*
 * @param stream the FILE* to get descriptor from
 * @return file descriptor, or -1 on failure
 */
static inline int tdb_fileno(FILE *stream)
{
    if (!stream) return -1;
    return _fileno(stream);
}

#if defined(__MINGW32__) || defined(__MINGW64__)
/* fopen for MinGW (uses standard fopen, not fopen_s) */
/*
 * tdb_fopen
 * portable file opening wrapper
 * @param filename the filename to open
 * @param mode the mode to open the file in
 * @return a pointer to the opened file, or NULL on failure
 */
static inline FILE *tdb_fopen(const char *filename, const char *mode)
{
    return fopen(filename, mode);
}
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
/* mingw provides semaphore.h for POSIX semaphores */
#include <semaphore.h>

/* mingw doesn't provide pread/pwrite/fdatasync, so we implement them */
/*
 * pread
 * reads data from a file descriptor at a specific offset
 * @param fd the file descriptor to read from
 * @param buf the buffer to read into
 * @param count the number of bytes to read
 * @param offset the offset to read from
 * @return the number of bytes read, or -1 on error
 */
static inline ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    if (count == 0)
    {
        return 0; /* reading 0 bytes is valid, returns 0 */
    }

    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return -1;
    }

    OVERLAPPED overlapped = {0};
    LARGE_INTEGER li;
    li.QuadPart = offset;
    overlapped.Offset = li.LowPart;
    overlapped.OffsetHigh = li.HighPart;

    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (overlapped.hEvent == NULL)
    {
        errno = GetLastError();
        return -1;
    }

    DWORD bytes_read = 0;
    BOOL result = ReadFile(h, buf, (DWORD)count, &bytes_read, &overlapped);

    if (!result)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            if (!GetOverlappedResult(h, &overlapped, &bytes_read, TRUE))
            {
                CloseHandle(overlapped.hEvent);
                errno = GetLastError();
                return -1;
            }
        }
        else
        {
            CloseHandle(overlapped.hEvent);
            errno = err;
            return -1;
        }
    }

    CloseHandle(overlapped.hEvent);
    return (ssize_t)bytes_read;
}

/*
 * pwrite
 * writes data to a file descriptor at a specific offset
 * @param fd the file descriptor to write to
 * @param buf the buffer to write from
 * @param count the number of bytes to write
 * @param offset the offset to write at
 * @return the number of bytes written, or -1 on error
 */
static inline ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (count == 0)
    {
        return 0; /* writing 0 bytes is valid, returns 0 */
    }

    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return -1;
    }

    OVERLAPPED overlapped = {0};
    LARGE_INTEGER li;
    li.QuadPart = offset;
    overlapped.Offset = li.LowPart;
    overlapped.OffsetHigh = li.HighPart;

    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (overlapped.hEvent == NULL)
    {
        errno = GetLastError();
        return -1;
    }

    DWORD bytes_written = 0;
    BOOL result = WriteFile(h, buf, (DWORD)count, &bytes_written, &overlapped);

    if (!result)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            if (!GetOverlappedResult(h, &overlapped, &bytes_written, TRUE))
            {
                CloseHandle(overlapped.hEvent);
                errno = GetLastError();
                return -1;
            }
        }
        else
        {
            CloseHandle(overlapped.hEvent);
            errno = err;
            return -1;
        }
    }

    CloseHandle(overlapped.hEvent);
    return (ssize_t)bytes_written;
}

/*
 * fsync
 * synchronizes file data to disk
 * @param fd the file descriptor to synchronize
 * @return 0 if successful, -1 otherwise
 */
static inline int fsync(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    return FlushFileBuffers(h) ? 0 : -1;
}

/*
 * fdatasync
 * synchronizes file data to disk
 * @param fd the file descriptor to synchronize
 * @return 0 if successful, -1 otherwise
 */
static inline int fdatasync(int fd)
{
    return fsync(fd);
}
#endif /* __MINGW32__ || __MINGW64__ */

#elif defined(__APPLE__)
#include <dirent.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

/* Grand Central Dispatch (dispatch/dispatch.h) is only available on macOS 10.6+
 * For older macOS versions (e.g., 10.5 PPC64), use POSIX semaphores instead */
#include <AvailabilityMacros.h>
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
#define TDB_USE_DISPATCH_SEMAPHORE 1
#include <dispatch/dispatch.h>
#else
#define TDB_USE_DISPATCH_SEMAPHORE 0
#include <semaphore.h>
#endif

/* pread and pwrite are available natively on macOS via unistd.h */
/* no additional implementation needed using system pread/pwrite */

/**
 * tdb_fopen
 * portable file opening wrapper
 * @param filename the filename to open
 * @param mode the mode to open the file in
 * @return a pointer to the opened file, or NULL on failure
 */
static inline FILE *tdb_fopen(const char *filename, const char *mode)
{
    return fopen(filename, mode);
}

/**
 * tdb_fileno
 * portable file descriptor extraction from FILE*
 * @param stream the FILE* to get descriptor from
 * @return file descriptor, or -1 on failure
 */
static inline int tdb_fileno(FILE *stream)
{
    if (!stream) return -1;
    return fileno(stream);
}

/*
 * fdatasync
 * synchronizes file data to disk
 * @param fd the file descriptor to synchronize
 * @return 0 if successful, -1 otherwise
 */
static inline int fdatasync(int fd)
{
#ifdef F_FULLFSYNC
    /* macOS requires F_FULLFSYNC to actually flush to disk */
    if (fcntl(fd, F_FULLFSYNC) == -1)
    {
        /* fall back to fsync if F_FULLFSYNC fails */
        return fsync(fd);
    }
    return 0;
#else
    /* fall back to fsync if F_FULLFSYNC not available */
    return fsync(fd);
#endif
}

#if TDB_USE_DISPATCH_SEMAPHORE
/* semaphore compatibility for macOS 10.6+ using Grand Central Dispatch
 * macOS deprecated POSIX semaphores (sem_init, sem_destroy, etc.)
 * use dispatch_semaphore instead */
typedef dispatch_semaphore_t sem_t;

/*
 * sem_init
 * initializes a semaphore
 * @param sem the semaphore to initialize
 * @param pshared whether the semaphore is shared between processes
 * @param value the initial value of the semaphore
 * @return 0 if successful, -1 otherwise
 */
static inline int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    (void)pshared; /* unused on macOS */
    *sem = dispatch_semaphore_create(value);
    return (*sem == NULL) ? -1 : 0;
}

/*
 * sem_destroy
 * destroys a semaphore
 * @param sem the semaphore to destroy
 * @return 0 if successful, -1 otherwise
 */
static inline int sem_destroy(sem_t *sem)
{
    if (*sem)
    {
        dispatch_release(*sem);
        *sem = NULL;
    }
    return 0;
}

/*
 * sem_wait
 * waits on a semaphore
 * @param sem the semaphore to wait on
 * @return 0 if successful, -1 otherwise
 */
static inline int sem_wait(sem_t *sem)
{
    return (dispatch_semaphore_wait(*sem, DISPATCH_TIME_FOREVER) == 0) ? 0 : -1;
}

/*
 * sem_post
 * posts a semaphore
 * @param sem the semaphore to post
 * @return 0 if successful, -1 otherwise
 */
static inline int sem_post(sem_t *sem)
{
    dispatch_semaphore_signal(*sem);
    return 0;
}
#else
/* for macOS < 10.6 (e.g., 10.5 PPC64), use POSIX semaphores
 * note-- POSIX semaphores are deprecated on modern macOS but work on older versions */
/* sem_t, sem_init, sem_destroy, sem_wait, sem_post are provided by semaphore.h */
#endif

#else /* posix systems */
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * tdb_fopen
 * @param filename the filename to open
 * @param mode the mode to open the file in
 * @return a pointer to the opened file, or NULL on failure
 */
static inline FILE *tdb_fopen(const char *filename, const char *mode)
{
    return fopen(filename, mode);
}

/**
 * tdb_fileno
 * portable file descriptor extraction from FILE*
 * @param stream the FILE* to get descriptor from
 * @return file descriptor, or -1 on failure
 */
static inline int tdb_fileno(FILE *stream)
{
    if (!stream) return -1;
    return fileno(stream);
}

/* sysinfo is Linux-specific, BSD uses sysctl */
#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <uvm/uvm_extern.h>
#endif

/* pread, pwrite, and fdatasync are available natively on POSIX systems via unistd.h */
/* no additional implementation needed using system pread/pwrite/fdatasync */

typedef pthread_t thread_t;
typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t cond_t;
typedef pthread_mutex_t crit_section_t;
typedef pthread_rwlock_t rwlock_t;
#endif

/* cross-platform thread naming
 * Linux                -- prctl(PR_SET_NAME)               -- 16 char limit including null
 * macOS                -- pthread_setname_np(name)         -- only current thread, 1 arg
 * FreeBSD/DragonFly    -- pthread_setname_np(thread, name) -- 2 args
 * NetBSD               -- pthread_setname_np(thread, fmt, arg) -- 3 args, printf-style
 * OpenBSD              -- pthread_set_name_np(thread, name)
 * Windows MSVC         -- SetThreadDescription (Win10 1607+)
 * Windows MinGW        -- no-op fallback */
#if defined(__linux__)
#include <sys/prctl.h>
#endif
static inline void tdb_set_thread_name(const char *name)
{
    if (!name) return;
#if defined(__linux__)
    prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void *)name);
#elif defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(_MSC_VER)
    /* SetThreadDescription requires wide string */
    wchar_t wname[64];
    size_t i;
    for (i = 0; i < 63 && name[i]; i++) wname[i] = (wchar_t)name[i];
    wname[i] = L'\0';
    SetThreadDescription(GetCurrentThread(), wname);
#else
    (void)name; /* no-op fallback */
#endif
}

/* O_DSYNC/O_SYNC for synchronous writes (must be after all platform includes)
 * POSIX -- O_DSYNC syncs data only, O_SYNC syncs data + metadata
 * windows -- no direct equivalent at open() time, use fdatasync() per-write
 * some BSDs (DragonFlyBSD, older FreeBSD) may not define O_DSYNC */
#ifndef O_DSYNC
#ifdef _WIN32
#define O_DSYNC 0 /* no O_DSYNC, will use fdatasync() fallback */
#elif defined(__APPLE__)
#define O_DSYNC 0x400000 /* macOS -- O_DSYNC = 0x400000 */
#else
#define O_DSYNC 0 /* fallback for BSDs and others without O_DSYNC */
#endif
#endif

/* cross-platform pwritev for scatter-gather I/O
 * Linux and modern BSDs have native pwritev in <sys/uio.h>
 * macOS added pwritev in 10.16/11.0 (Big Sur)
 * older macOS and Windows fall back to sequential pwrite calls */
#ifdef _WIN32
struct iovec
{
    void *iov_base;
    size_t iov_len;
};
#define TDB_NEED_PWRITEV_FALLBACK 1
#else
#include <sys/uio.h>
/* macOS < 11.0 does not have pwritev. MAC_OS_X_VERSION_10_16 == 101600 == Big Sur.
 * check for the availability macro; if it does not exist, assume the platform is old enough
 * to lack pwritev. */
#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#if !defined(MAC_OS_X_VERSION_10_16) || MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_16
#define TDB_NEED_PWRITEV_FALLBACK 1
#endif
#endif
#endif

#ifdef TDB_NEED_PWRITEV_FALLBACK
/*
 * pwritev
 * scatter-gather write at offset (fallback using sequential pwrite)
 * @param fd the file descriptor
 * @param iov array of iovec buffers
 * @param iovcnt number of iovec entries
 * @param offset the file offset to write at
 * @return total bytes written, or -1 on error
 */
static inline ssize_t tdb_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++)
    {
        ssize_t n = pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset);
        if (n != (ssize_t)iov[i].iov_len) return (total > 0) ? total : -1;
        total += n;
        offset += n;
    }
    return total;
}
#define pwritev tdb_pwritev
#endif

/**
 * tdb_pwritev_safe
 * wrapper around pwritev that blocks SIGALRM/SIGVTALRM/SIGPROF for the duration
 * of the syscall. prevents EINTR from leaving a zero-filled hole in the file when
 * the atomic offset reservation has already been committed.
 * @param fd the file descriptor
 * @param iov array of iovec buffers
 * @param iovcnt number of iovec entries
 * @param offset the file offset to write at
 * @return total bytes written, or -1 on error
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static ssize_t
tdb_pwritev_safe(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
#ifndef _WIN32
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGALRM);
    sigaddset(&block_set, SIGVTALRM);
    sigaddset(&block_set, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);
    const ssize_t written = pwritev(fd, iov, iovcnt, offset);
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
    return written;
#else
    return pwritev(fd, iov, iovcnt, offset);
#endif
}

/* atomic compare exchange for pointers (all platforms with C11 atomics) */
#if !defined(_MSC_VER) || _MSC_VER >= 1930
/*
 * atomic_compare_exchange_strong_ptr
 * @param ptr pointer to atomic pointer
 * @param expected pointer to expected value
 * @param desired new value to store
 * @return 1 if successful, 0 if failed
 */
static inline int atomic_compare_exchange_strong_ptr(_Atomic(void *) *ptr, void **expected,
                                                     void *desired)
{
    return atomic_compare_exchange_strong(ptr, expected, desired);
}
#endif

/*
 * get_available_memory
 * gets available system memory in bytes
 * @return available memory in bytes, or 0 on failure
 */
static inline size_t get_available_memory(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
    {
        return (size_t)status.ullAvailPhys;
    }
    return 0;
#elif defined(__APPLE__)
    vm_size_t page_size;
    mach_port_t mach_port;
    mach_msg_type_number_t count;

    mach_port = mach_host_self();

    /* 32-bit vm statistics on PPC regardless of OS version.
     * host_statistics64 is not available on 10.5 and for PPC 32-bit even on 10.6 */
#if defined(__ppc__) || (MAC_OS_X_VERSION_MIN_REQUIRED < 1060)
    /* PPC always uses 32-bit vm statistics */
    vm_statistics_data_t vm_stats;
    count = HOST_VM_INFO_COUNT;
    if (host_page_size(mach_port, &page_size) == KERN_SUCCESS &&
        host_statistics(mach_port, HOST_VM_INFO, (host_info_t)&vm_stats, &count) == KERN_SUCCESS)
    {
        return (size_t)((vm_stats.free_count + vm_stats.inactive_count + vm_stats.purgeable_count) *
                        page_size);
    }
#else
    /* try 64-bit first (macOS 10.6+ on x86/x86_64/ARM), fall back to 32-bit */
    vm_statistics64_data_t vm_stats64;
    count = sizeof(vm_stats64) / sizeof(natural_t);
    if (host_page_size(mach_port, &page_size) == KERN_SUCCESS &&
        host_statistics64(mach_port, HOST_VM_INFO, (host_info64_t)&vm_stats64, &count) ==
            KERN_SUCCESS)
    {
        return (size_t)((vm_stats64.free_count + vm_stats64.inactive_count +
                         vm_stats64.purgeable_count) *
                        page_size);
    }
    else
    {
        /* fallback to 32-bit for older systems or Rosetta edge cases */
        vm_statistics_data_t vm_stats;
        count = HOST_VM_INFO_COUNT;
        if (host_page_size(mach_port, &page_size) == KERN_SUCCESS &&
            host_statistics(mach_port, HOST_VM_INFO, (host_info_t)&vm_stats, &count) ==
                KERN_SUCCESS)
        {
            return (
                size_t)((vm_stats.free_count + vm_stats.inactive_count + vm_stats.purgeable_count) *
                        page_size);
        }
    }
#endif
    return 0;
#elif defined(__linux__)
    /* prefer /proc/meminfo MemAvailable -- the kernel's own estimate of memory
     * available for new allocations without swapping (includes free + reclaimable
     * buffers/cache + reclaimable slab). sysinfo.freeram only reports truly free
     * pages which is typically very low on a busy system and triggers false
     * critical memory pressure */
    {
        FILE *f = fopen("/proc/meminfo", "r");
        if (f)
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                unsigned long long val;
                if (sscanf(line, "MemAvailable: %llu kB", &val) == 1)
                {
                    fclose(f);
                    return (size_t)(val * 1024ULL);
                }
            }
            fclose(f);
        }
    }
    /* fallback to sysinfo.freeram if /proc/meminfo is unavailable */
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0)
        {
            return (size_t)si.freeram * (size_t)si.mem_unit;
        }
    }
    return 0;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    /* BSD systems use sysctl.. */
    unsigned long free_pages = 0;
    unsigned long page_size = 0;
    size_t len = sizeof(free_pages);

#if defined(__FreeBSD__) || defined(__DragonFly__)
    if (sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &len, NULL, 0) == 0)
    {
        len = sizeof(page_size);
        if (sysctlbyname("vm.stats.vm.v_page_size", &page_size, &len, NULL, 0) == 0)
        {
            return (size_t)(free_pages * page_size);
        }
    }
#elif defined(__OpenBSD__) || defined(__NetBSD__)
    int mib[2];
    struct uvmexp uvmexp;
    len = sizeof(uvmexp);

    mib[0] = CTL_VM;
    mib[1] = VM_UVMEXP;
    if (sysctl(mib, 2, &uvmexp, &len, NULL, 0) == 0)
    {
        return (size_t)((uint64_t)uvmexp.free * (uint64_t)uvmexp.pagesize);
    }
#endif
    return 0;
#else
    /* illumos/solaris and other POSIX systems
     * note -- on 32-bit systems, multiplying pages * page_size can overflow
     * so we cast to 64-bit before multiplication */
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0)
    {
        return (size_t)((uint64_t)pages * (uint64_t)page_size);
    }
    return 0;
#endif
}

/*
 * get_total_memory
 * gets total system memory in bytes
 * @return total memory in bytes, or 0 on failure
 */
static inline size_t get_total_memory(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
    {
        return (size_t)status.ullTotalPhys;
    }
    return 0;
#elif defined(__APPLE__)
    int mib[2];
    int64_t physical_memory;
    size_t length;

    mib[0] = CTL_HW;
    mib[1] = HW_MEMSIZE;
    length = sizeof(int64_t);
    if (sysctl(mib, 2, &physical_memory, &length, NULL, 0) == 0)
    {
        return (size_t)physical_memory;
    }
    return 0;
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
    {
        return (size_t)si.totalram * (size_t)si.mem_unit;
    }
    return 0;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    int mib[2];
    size_t physical_memory;
    size_t len;

    mib[0] = CTL_HW;
#if defined(__OpenBSD__) || defined(__NetBSD__)
    /* OpenBSD and NetBSD support HW_PHYSMEM64 for 64-bit physical memory */
    mib[1] = HW_PHYSMEM64;
    int64_t physmem64;
    len = sizeof(physmem64);
    if (sysctl(mib, 2, &physmem64, &len, NULL, 0) == 0)
    {
        return (size_t)physmem64;
    }
#else
    /* FreeBSD and DragonFlyBSD use HW_PHYSMEM which returns size_t */
    mib[1] = HW_PHYSMEM;
    len = sizeof(physical_memory);
    if (sysctl(mib, 2, &physical_memory, &len, NULL, 0) == 0)
    {
        return physical_memory;
    }
#endif
    return 0;
#else
    /* illumos/solaris and other POSIX systems
     * note -- on 32-bit systems, multiplying pages * page_size can overflow
     * so we cast to 64-bit before multiplication */
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0)
    {
        return (size_t)((uint64_t)pages * (uint64_t)page_size);
    }
    return 0;
#endif
}

/*
 * get_file_mod_time
 * gets the modified time of a file
 * @param path the path of the file
 * @return the modified time of the file, or -1 on failure
 */
static inline time_t get_file_mod_time(const char *path)
{
    struct STAT_STRUCT file_stat;

    if (STAT_FUNC(path, &file_stat) != 0)
    {
        return -1;
    }

    return (time_t)file_stat.st_mtime;
}

/* cross-platform little-endian serialization functions */

/*
 * encode_uint16_le_compat
 * encodes a uint16_t value in little-endian format
 * @param buf buffer to store encoded value
 * @param val value to encode
 */
static inline void encode_uint16_le_compat(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

/*
 * decode_uint16_le_compat
 * decodes a uint16_t value in little-endian format
 * @param buf buffer containing encoded value
 * @return decoded value
 */
static inline uint16_t decode_uint16_le_compat(const uint8_t *buf)
{
    return ((uint16_t)buf[0]) | ((uint16_t)buf[1] << 8);
}

/*
 * encode_uint32_le_compat
 * encodes a uint32_t value in little-endian format
 * @param buf buffer to store encoded value
 * @param val value to encode
 */
static inline void encode_uint32_le_compat(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

/*
 * decode_uint32_le_compat
 * decodes a uint32_t value in little-endian format
 * @param buf buffer containing encoded value
 * @return decoded value
 */
static inline uint32_t decode_uint32_le_compat(const uint8_t *buf)
{
    return ((uint32_t)buf[0]) | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

/*
 * encode_uint64_le_compat
 * encodes a uint64_t value in little-endian format
 * @param buf buffer to store encoded value
 * @param val value to encode
 */
static inline void encode_uint64_le_compat(uint8_t *buf, uint64_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
    buf[4] = (uint8_t)((val >> 32) & 0xFF);
    buf[5] = (uint8_t)((val >> 40) & 0xFF);
    buf[6] = (uint8_t)((val >> 48) & 0xFF);
    buf[7] = (uint8_t)((val >> 56) & 0xFF);
}

/*
 * encode_uint32_le
 * encodes a uint32_t value in little-endian format
 * @param buf buffer to store encoded value
 * @param val value to encode
 */
static inline void encode_uint32_le(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

/*
 * decode_uint32_le
 * decodes a uint32_t value in little-endian format
 * @param buf buffer containing encoded value
 * @return decoded value
 */
static inline uint32_t decode_uint32_le(const uint8_t *buf)
{
    return ((uint32_t)buf[0]) | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

/*
 * encode_int64_le
 * encodes an int64_t value in little-endian format
 * @param buf buffer to store encoded value
 * @param val value to encode
 */
static inline void encode_int64_le(uint8_t *buf, int64_t val)
{
    const uint64_t uval = (uint64_t)val;
    buf[0] = (uint8_t)(uval & 0xFF);
    buf[1] = (uint8_t)((uval >> 8) & 0xFF);
    buf[2] = (uint8_t)((uval >> 16) & 0xFF);
    buf[3] = (uint8_t)((uval >> 24) & 0xFF);
    buf[4] = (uint8_t)((uval >> 32) & 0xFF);
    buf[5] = (uint8_t)((uval >> 40) & 0xFF);
    buf[6] = (uint8_t)((uval >> 48) & 0xFF);
    buf[7] = (uint8_t)((uval >> 56) & 0xFF);
}

/*
 * decode_int64_le
 * decodes an int64_t value in little-endian format
 * @param buf buffer containing encoded value
 * @return decoded value
 */
static inline int64_t decode_int64_le(const uint8_t *buf)
{
    const uint64_t uval = ((uint64_t)buf[0]) | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) |
                          ((uint64_t)buf[3] << 24) | ((uint64_t)buf[4] << 32) |
                          ((uint64_t)buf[5] << 40) | ((uint64_t)buf[6] << 48) |
                          ((uint64_t)buf[7] << 56);
    return (int64_t)uval;
}

/*
 * encode_uint64_le
 * encodes a uint64_t value in little-endian format
 * @param buf buffer to store encoded value
 * @param val value to encode
 */
static inline void encode_uint64_le(uint8_t *buf, uint64_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
    buf[4] = (uint8_t)((val >> 32) & 0xFF);
    buf[5] = (uint8_t)((val >> 40) & 0xFF);
    buf[6] = (uint8_t)((val >> 48) & 0xFF);
    buf[7] = (uint8_t)((val >> 56) & 0xFF);
}

/*
 * decode_uint64_le
 * decodes a uint64_t value in little-endian format
 * @param buf buffer containing encoded value
 * @return decoded value
 */
static inline uint64_t decode_uint64_le(const uint8_t *buf)
{
    return ((uint64_t)buf[0]) | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) |
           ((uint64_t)buf[3] << 24) | ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
           ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
}

/*
 * decode_fixed_32
 * decodes a uint32_t value in little-endian format
 * @param data buffer containing encoded value
 * @return decoded value
 */
static inline uint32_t decode_fixed_32(const char *data)
{
    return ((uint32_t)(uint8_t)data[0]) | ((uint32_t)(uint8_t)data[1] << 8) |
           ((uint32_t)(uint8_t)data[2] << 16) | ((uint32_t)(uint8_t)data[3] << 24);
}

/*
 * decode_uint64_le_compat
 * decodes a uint64_t value in little-endian format
 * @param buf buffer containing encoded value
 * @return decoded value
 */
static inline uint64_t decode_uint64_le_compat(const uint8_t *buf)
{
    return ((uint64_t)buf[0]) | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) |
           ((uint64_t)buf[3] << 24) | ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
           ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
}

/**
 * encode_int64_le_compat
 * encodes a int64_t value in little-endian format
 * @param buf output buffer (must be at least 8 bytes)
 * @param val value to encode
 */
static inline void encode_int64_le_compat(uint8_t *buf, int64_t val)
{
    uint64_t uval = (uint64_t)val;
    buf[0] = (uint8_t)(uval);
    buf[1] = (uint8_t)(uval >> 8);
    buf[2] = (uint8_t)(uval >> 16);
    buf[3] = (uint8_t)(uval >> 24);
    buf[4] = (uint8_t)(uval >> 32);
    buf[5] = (uint8_t)(uval >> 40);
    buf[6] = (uint8_t)(uval >> 48);
    buf[7] = (uint8_t)(uval >> 56);
}

/**
 * decode_int64_le_compat
 * decodes a int64_t value in little-endian format
 * @param buf buffer containing encoded value
 * @return decoded value
 */
static inline int64_t decode_int64_le_compat(const uint8_t *buf)
{
    uint64_t uval = ((uint64_t)buf[0]) | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) |
                    ((uint64_t)buf[3] << 24) | ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
                    ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
    return (int64_t)uval;
}

/* varint encoding/decoding for compact serialization */
static inline uint8_t *encode_varint32(uint8_t *ptr, uint32_t value)
{
    while (value >= 0x80)
    {
        *ptr++ = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    *ptr++ = (uint8_t)value;
    return ptr;
}

static inline uint8_t *encode_varint64(uint8_t *ptr, uint64_t value)
{
    while (value >= 0x80)
    {
        *ptr++ = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    *ptr++ = (uint8_t)value;
    return ptr;
}

static inline const uint8_t *decode_varint32(const uint8_t *ptr, uint32_t *value)
{
    uint32_t result = 0;
    int shift = 0;
    while (*ptr & 0x80)
    {
        /* prevent shift overflow on corrupted data */
        if (shift >= 32)
        {
            *value = 0;
            return ptr;
        }
        result |= (uint32_t)(*ptr & 0x7F) << shift;
        shift += 7;
        ptr++;
    }
    /* final byte check */
    if (shift >= 32)
    {
        *value = 0;
        return ptr;
    }
    result |= (uint32_t)(*ptr) << shift;
    *value = result;
    return ptr + 1;
}

static inline const uint8_t *decode_varint64(const uint8_t *ptr, uint64_t *value)
{
    uint64_t result = 0;
    int shift = 0;
    while (*ptr & 0x80)
    {
        /* prevent shift overflow on corrupted data */
        if (shift >= 64)
        {
            *value = 0;
            return ptr;
        }
        result |= (uint64_t)(*ptr & 0x7F) << shift;
        shift += 7;
        ptr++;
    }
    /* final byte check */
    if (shift >= 64)
    {
        *value = 0;
        return ptr;
    }
    result |= (uint64_t)(*ptr) << shift;
    *value = result;
    return ptr + 1;
}

/* length-prefixed KV serialization helpers */

/*
 * serialize_kv_varint
 * serialize key-value pair with varint length prefixes
 * format-- varint(key_size) + key + varint(value_size) + value
 * @param ptr output buffer (must have enough space)
 * @param key key data
 * @param key_size key size
 * @param value value data (can be NULL if value_size is 0)
 * @param value_size value size
 * @return pointer to end of written data
 */
static inline uint8_t *serialize_kv_varint(uint8_t *ptr, const uint8_t *key, uint32_t key_size,
                                           const uint8_t *value, uint32_t value_size)
{
    /* write key size and key */
    ptr = encode_varint32(ptr, key_size);
    memcpy(ptr, key, key_size);
    ptr += key_size;

    /* write value size and value */
    ptr = encode_varint32(ptr, value_size);
    if (value_size > 0 && value)
    {
        memcpy(ptr, value, value_size);
        ptr += value_size;
    }

    return ptr;
}

/*
 * serialize_kv_varint_ex
 * serialize key-value pair with flags and varint length prefixes (for sstables)
 * format is flags(1) + varint(key_size) + key + varint(value_size) + value + varint(ttl)
 * @param ptr output buffer (must have enough space)
 * @param flags flags byte (e.g., tombstone marker)
 * @param key key data
 * @param key_size key size
 * @param value value data (can be NULL if value_size is 0)
 * @param value_size value size
 * @param ttl time-to-live (0 = no expiration)
 * @return pointer to end of written data
 */
static inline uint8_t *serialize_kv_varint_ex(uint8_t *ptr, uint8_t flags, const uint8_t *key,
                                              uint32_t key_size, const uint8_t *value,
                                              uint32_t value_size, int64_t ttl)
{
    /* write flags */
    *ptr++ = flags;

    /* write key size and key */
    ptr = encode_varint32(ptr, key_size);
    memcpy(ptr, key, key_size);
    ptr += key_size;

    /* write value size and value */
    ptr = encode_varint32(ptr, value_size);
    if (value_size > 0 && value)
    {
        memcpy(ptr, value, value_size);
        ptr += value_size;
    }

    /* write ttl */
    ptr = encode_varint64(ptr, (uint64_t)ttl);

    return ptr;
}

/*
 * serialize_kv_varint_full
 * serialize key-value pair with all metadata (for WAL)
 * format-- flags(1) + varint(key_size) + key + varint(value_size) + value + varint(ttl) +
 * varint(seq)
 * @param ptr output buffer (must have enough space)
 * @param flags flags byte
 * @param key key data
 * @param key_size key size
 * @param value value data (can be NULL if value_size is 0)
 * @param value_size value size
 * @param ttl time-to-live
 * @param seq sequence number
 * @return pointer to end of written data
 */
static inline uint8_t *serialize_kv_varint_full(uint8_t *ptr, uint8_t flags, const uint8_t *key,
                                                uint32_t key_size, const uint8_t *value,
                                                uint32_t value_size, int64_t ttl, uint64_t seq)
{
    /* write flags */
    *ptr++ = flags;

    /* write key size and key */
    ptr = encode_varint32(ptr, key_size);
    memcpy(ptr, key, key_size);
    ptr += key_size;

    /* write value size and value */
    ptr = encode_varint32(ptr, value_size);
    if (value_size > 0 && value)
    {
        memcpy(ptr, value, value_size);
        ptr += value_size;
    }

    /* write ttl and seq */
    ptr = encode_varint64(ptr, (uint64_t)ttl);
    ptr = encode_varint64(ptr, seq);

    return ptr;
}

/*
 * deserialize_kv_varint
 * deserialize key-value pair with varint length prefixes
 * @param ptr input buffer
 * @param end end of input buffer (for bounds checking)
 * @param key_size output key size
 * @param value_size output value size
 * @param key_out output pointer to key data (points into input buffer)
 * @param value_out output pointer to value data (points into input buffer)
 * @return pointer to next entry, or NULL on error
 */
static inline const uint8_t *deserialize_kv_varint(const uint8_t *ptr, const uint8_t *end,
                                                   uint32_t *key_size, uint32_t *value_size,
                                                   const uint8_t **key_out,
                                                   const uint8_t **value_out)
{
    /* read key size */
    if (ptr >= end) return NULL;
    ptr = decode_varint32(ptr, key_size);
    if (ptr + *key_size > end) return NULL;

    /* read key */
    *key_out = ptr;
    ptr += *key_size;

    /* read value size */
    if (ptr >= end) return NULL;
    ptr = decode_varint32(ptr, value_size);
    if (ptr + *value_size > end) return NULL;

    /* read value */
    *value_out = ptr;
    ptr += *value_size;

    return ptr;
}

/*
 * deserialize_kv_varint_ex
 * deserialize key-value pair with flags and varint length prefixes (for sstables)
 * @param ptr input buffer
 * @param end end of input buffer (for bounds checking)
 * @param flags output flags byte
 * @param key_size output key size
 * @param value_size output value size
 * @param key_out output pointer to key data (points into input buffer)
 * @param value_out output pointer to value data (points into input buffer)
 * @param ttl output time-to-live
 * @return pointer to next entry, or NULL on error
 */
static inline const uint8_t *deserialize_kv_varint_ex(const uint8_t *ptr, const uint8_t *end,
                                                      uint8_t *flags, uint32_t *key_size,
                                                      uint32_t *value_size, const uint8_t **key_out,
                                                      const uint8_t **value_out, int64_t *ttl)
{
    /* read flags */
    if (ptr >= end) return NULL;
    *flags = *ptr++;

    /* read key size */
    if (ptr >= end) return NULL;
    ptr = decode_varint32(ptr, key_size);
    if (ptr + *key_size > end) return NULL;

    /* read key */
    *key_out = ptr;
    ptr += *key_size;

    /* read value size */
    if (ptr >= end) return NULL;
    ptr = decode_varint32(ptr, value_size);
    if (ptr + *value_size > end) return NULL;

    /* read value */
    *value_out = ptr;
    ptr += *value_size;

    /* read ttl */
    if (ptr >= end) return NULL;
    uint64_t ttl_u64;
    ptr = decode_varint64(ptr, &ttl_u64);
    *ttl = (int64_t)ttl_u64;

    return ptr;
}

/*
 * deserialize_kv_varint_full
 * deserialize key-value pair with all metadata (for WAL)
 * @param ptr input buffer
 * @param end end of input buffer (for bounds checking)
 * @param flags output flags byte
 * @param key_size output key size
 * @param value_size output value size
 * @param key_out output pointer to key data (points into input buffer)
 * @param value_out output pointer to value data (points into input buffer)
 * @param ttl output time-to-live
 * @param seq output sequence number
 * @return pointer to next entry, or NULL on error
 */
static inline const uint8_t *deserialize_kv_varint_full(const uint8_t *ptr, const uint8_t *end,
                                                        uint8_t *flags, uint32_t *key_size,
                                                        uint32_t *value_size,
                                                        const uint8_t **key_out,
                                                        const uint8_t **value_out, int64_t *ttl,
                                                        uint64_t *seq)
{
    /* read flags */
    if (ptr >= end) return NULL;
    *flags = *ptr++;

    /* read key size */
    if (ptr >= end) return NULL;
    ptr = decode_varint32(ptr, key_size);
    if (ptr + *key_size > end) return NULL;

    /* read key */
    *key_out = ptr;
    ptr += *key_size;

    /* read value size */
    if (ptr >= end) return NULL;
    ptr = decode_varint32(ptr, value_size);
    if (ptr + *value_size > end) return NULL;

    /* read value */
    *value_out = ptr;
    ptr += *value_size;

    /* read ttl and seq */
    if (ptr >= end) return NULL;
    uint64_t ttl_u64;
    ptr = decode_varint64(ptr, &ttl_u64);
    *ttl = (int64_t)ttl_u64;

    if (ptr >= end) return NULL;
    ptr = decode_varint64(ptr, seq);

    return ptr;
}

/*
 * tdb_preallocate_extent
 * extends the logical file size and reserves on-disk blocks for the new region
 * ahead of writes, so that subsequent pwrites within the preallocated extent do
 * not take the kernel's "write extends file" fast path. on Linux ext4 this
 * avoids the per-inode i_rwsem write lock; equivalent locks exist on macOS APFS
 * (vnode write lock) and Windows NTFS (file-extension lock).
 *
 * critical detail the logical EOF (i_size) MUST advance, not just the on-disk
 * extent allocation. on Linux, fallocate(KEEP_SIZE) reserves blocks but leaves
 * i_size unchanged, and the kernel still treats writes past i_size as extending
 * writes -- delivering no speedup. mode 0 advances i_size and initializes the
 * extents so subsequent pwrites are fully in-place.
 *
 * the trailing region is zero-filled. the caller must ftruncate back to the
 * actual data extent on clean close so next-open validation isn't confused by
 * trailing zeros. crash recovery should tolerate trailing zeros as preallocation
 * tail (size_field == 0 marks the boundary between data and preallocated region).
 *
 * platform behavior:
 *   linux           fallocate(fd, 0, off, len) -- advances i_size, initializes extents
 *   macos           fcntl(F_PREALLOCATE) reserves, then ftruncate advances logical EOF
 *   windows         SetFileInformationByHandle(FileAllocationInfo) reserves, then
 *                   FileEndOfFileInfo advances EOF
 *   other posix     posix_fallocate -- already advances EOF
 *   fallback        returns -1, caller falls back to extending writes
 *
 * @param fd the file descriptor
 * @param offset start of the region to preallocate (typically current EOF)
 * @param len    number of bytes to preallocate
 * @return 0 on success, -1 on failure (non-fatal -- caller can continue)
 */
static inline int tdb_preallocate_extent(int fd, off_t offset, off_t len)
{
#if defined(__linux__)
    return fallocate(fd, 0, offset, len);
#elif defined(__APPLE__)
    /* reserve blocks past current EOF (offset param is implicit on macOS) */
    (void)offset;
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG | F_ALLOCATEALL;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = len;
    fst.fst_bytesalloc = 0;
    if (fcntl(fd, F_PREALLOCATE, &fst) == -1)
    {
        /* contiguous request failed, retry allowing fragmentation */
        fst.fst_flags = F_ALLOCATEALL;
        if (fcntl(fd, F_PREALLOCATE, &fst) == -1) return -1;
    }
    /* advance logical EOF so writes within the new region don't take the
     * extending-write lock */
    return ftruncate(fd, offset + len);
#elif defined(_WIN32)
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    FILE_ALLOCATION_INFO fai;
    fai.AllocationSize.QuadPart = (LONGLONG)(offset + len);
    if (!SetFileInformationByHandle(h, FileAllocationInfo, &fai, sizeof(fai))) return -1;
    /* advance logical EOF -- otherwise NTFS still treats writes past EOF as extending */
    FILE_END_OF_FILE_INFO eofi;
    eofi.EndOfFile.QuadPart = (LONGLONG)(offset + len);
    return SetFileInformationByHandle(h, FileEndOfFileInfo, &eofi, sizeof(eofi)) ? 0 : -1;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    int rc = posix_fallocate(fd, offset, len);
    return rc == 0 ? 0 : -1;
#else
    (void)fd;
    (void)offset;
    (void)len;
    return -1;
#endif
}

/*
 * set_file_sequential_hint
 * hints to the OS that file access will be sequential for read-ahead optimization
 * @param fd the file descriptor
 * @return 0 on success, -1 on failure (non-critical, can be ignored)
 */
static inline int set_file_sequential_hint(int fd)
{
#ifdef __linux__
    return posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#elif defined(__APPLE__)
    return fcntl(fd, F_RDAHEAD, 1);
#elif defined(_WIN32)
    /* _O_SEQUENTIAL flag set at open time via compat.h wrapper */
    (void)fd; /* unused on Windows */
    return 0;
#else
    (void)fd; /* unused on other platforms */
    return 0;
#endif
}

/*
 * set_file_random_hint
 * hints to the OS that file access will be random (disables read-ahead)
 * useful for point lookups where sequential read-ahead wastes I/O
 * @param fd the file descriptor
 * @return 0 on success, -1 on failure (non-critical, can be ignored)
 */
static inline int set_file_random_hint(int fd)
{
#ifdef __linux__
    return posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
#elif defined(__APPLE__)
    return fcntl(fd, F_RDAHEAD, 0);
#elif defined(_WIN32)
    /* _O_RANDOM flag would need to be set at open time
     * for existing fd, we cant change this, so no-op */
    (void)fd;
    return 0;
#else
    (void)fd;
    return 0;
#endif
}

/*
 * prefetch_file_region
 * initiates non-blocking read of specified region into page cache
 * useful when you know you'll need data soon (e.g., before decompression)
 * @param fd the file descriptor
 * @param offset starting offset to prefetch
 * @param len number of bytes to prefetch (0 = until end of file)
 * @return 0 on success, -1 on failure (non-critical, can be ignored)
 */
static inline int prefetch_file_region(int fd, off_t offset, off_t len)
{
#ifdef __linux__
    return posix_fadvise(fd, offset, len, POSIX_FADV_WILLNEED);
#elif defined(__APPLE__)
    /* on macos we utilize F_RDADVISE for read-ahead hint */
    struct radvisory ra;
    ra.ra_offset = offset;
    ra.ra_count = (int)(len > 0 ? len : (1024 * 1024)); /* default 1MB if len=0 */
    return fcntl(fd, F_RDADVISE, &ra);
#elif defined(_WIN32)
    /* windows PrefetchVirtualMemory requires mapped memory
     * for file-based prefetch, we do a small read to trigger caching on the system */
    (void)fd;
    (void)offset;
    (void)len;
    return 0;
#else
    (void)fd;
    (void)offset;
    (void)len;
    return 0;
#endif
}

/*
 * evict_file_region
 * hints to OS that specified region is no longer needed and can be evicted from cache
 * useful after streaming reads (e.g., compaction) to prevent cache pollution
 * call fsync/fdatasync first if dirty pages need to be written
 * @param fd the file descriptor
 * @param offset starting offset to evict
 * @param len number of bytes to evict (0 = until end of file)
 * @return 0 on success, -1 on failure (non-critical, can be ignored)
 */
static inline int evict_file_region(int fd, off_t offset, off_t len)
{
#ifdef __linux__
    return posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED);
#elif defined(__APPLE__)
    /* on macos F_NOCACHE disables caching for future I/O but doesn't evict
     * theres no direct equivalent to POSIX_FADV_DONTNEED
     * msync with MS_INVALIDATE on mmap'd regions is closest but requires mmap */
    (void)fd;
    (void)offset;
    (void)len;
    return 0;
#elif defined(_WIN32)
    /* no direct equivalent without memory mapping
     * FILE_FLAG_NO_BUFFERING at open time is closest but requires alignment */
    (void)fd;
    (void)offset;
    (void)len;
    return 0;
#else
    (void)fd;
    (void)offset;
    (void)len;
    return 0;
#endif
}

/*
 * set_file_noreuse_hint
 * hints that specified region will be accessed only once (streaming)
 * kernel page replacement can deprioritize these pages
 * effective on Linux 6.3+ (was no-op from 2.6.18 to 6.2)
 * @param fd the file descriptor
 * @param offset starting offset
 * @param len number of bytes (0 = until end of file)
 * @return 0 on success, -1 on failure (non-critical, can be ignored)
 */
static inline int set_file_noreuse_hint(int fd, off_t offset, off_t len)
{
#ifdef __linux__
    return posix_fadvise(fd, offset, len, POSIX_FADV_NOREUSE);
#elif defined(__APPLE__)
    /* F_NOCACHE is similar -- tells system not to cache I/O
     * this affects all future I/O on this fd, not just a region */
    (void)offset;
    (void)len;
    return fcntl(fd, F_NOCACHE, 1);
#elif defined(_WIN32)
    /** FILE_FLAG_SEQUENTIAL_SCAN at open time is closest
     * for existing fd, no equivalent */
    (void)fd;
    (void)offset;
    (void)len;
    return 0;
#else
    (void)fd;
    (void)offset;
    (void)len;
    return 0;
#endif
}

/**
 * tdb_get_available_disk_space
 * get available disk space for a given path
 * @param path the path to check
 * @param available pointer to store available bytes
 * @return 0 on success, -1 on failure
 */
static inline int tdb_get_available_disk_space(const char *path, uint64_t *available)
{
    if (!path || !available) return -1;

#if defined(_WIN32)
    ULARGE_INTEGER free_bytes;
    if (GetDiskFreeSpaceExA(path, &free_bytes, NULL, NULL))
    {
        *available = (uint64_t)free_bytes.QuadPart;
        return 0;
    }
    return -1;
#else
    struct statvfs stat;
    if (statvfs(path, &stat) == 0)
    {
        *available = (uint64_t)stat.f_bavail * (uint64_t)stat.f_frsize;
        return 0;
    }
    return -1;
#endif
}

/* cpu pause for spin-wait loops */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#ifdef _MSC_VER
#include <intrin.h>
#define cpu_pause() _mm_pause()
#else
#define cpu_pause() __builtin_ia32_pause()
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#ifdef _MSC_VER
#include <intrin.h>
#define cpu_pause() __yield()
#else
#define cpu_pause() __asm__ __volatile__("yield" ::: "memory")
#endif
#elif defined(__arm__) || defined(_M_ARM)
#ifdef _MSC_VER
#include <intrin.h>
#define cpu_pause() __yield()
#else
#define cpu_pause() __asm__ __volatile__("yield" ::: "memory")
#endif
#else
#define cpu_pause() ((void)0)
#endif

/* cpu yield for longer waits -- gives up time slice to scheduler */
#ifdef _WIN32
#include <windows.h>
#define cpu_yield() SwitchToThread()
#else
#include <sched.h>
#define cpu_yield() sched_yield()
#endif

/*
 * tdb_hardlink
 * portable hard link creation
 * @param src existing file path
 * @param dst new hard link path
 * @return 0 on success, -1 on failure
 */
static inline int tdb_hardlink(const char *src, const char *dst)
{
    if (!src || !dst) return -1;
#ifdef _WIN32
    return CreateHardLinkA(dst, src, NULL) ? 0 : -1;
#else
    return link(src, dst);
#endif
}

/*
 * tdb_unlink
 * portable file deletion
 * @param path the file path to delete
 * @return 0 on success, -1 on failure
 */
static inline int tdb_unlink(const char *path)
{
    if (!path) return -1;
#ifdef _WIN32
    /* clear read-only attribute that might prevent deletion */
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return _unlink(path);
#else
    return unlink(path);
#endif
}

/**
 * is_directory_empty
 * checks if a directory is empty (contains only . and ..)
 * @param path the directory path to check
 * @return 1 if empty, 0 if not empty or error
 */
static inline int is_directory_empty(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return 0;

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        count++;
        break; /* found at least one entry */
    }

    closedir(dir);
    return count == 0;
}

/**
 * remove_directory_once
 * single pass of recursive directory removal
 * @param path the directory path to remove
 * @return 0 on success, -1 on failure
 */
static inline int remove_directory_once(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return -1;

    struct dirent *entry;
    int result = 0;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        size_t len = strlen(path) + strlen(PATH_SEPARATOR) + strlen(entry->d_name) + 1;
        char *full_path = malloc(len);
        if (!full_path)
        {
            result = -1;
            continue;
        }

        snprintf(full_path, len, "%s%s%s", path, PATH_SEPARATOR, entry->d_name);

        struct STAT_STRUCT st;
        if (STAT_FUNC(full_path, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
            {
                /* recursive call for subdirectory */
                if (remove_directory_once(full_path) != 0) result = -1;
            }
            else
            {
#ifdef _WIN32
                /* clear read-only and other attributes that might prevent deletion */
                SetFileAttributesA(full_path, FILE_ATTRIBUTE_NORMAL);
                if (_unlink(full_path) != 0) result = -1;
#else
                if (unlink(full_path) != 0) result = -1;
#endif
            }
        }

        free(full_path);
    }

    closedir(dir);

    /* we try to remove the directory itself */
#ifdef _WIN32
    if (_rmdir(path) != 0) result = -1;
#else
    if (rmdir(path) != 0) result = -1;
#endif

    return result;
}

/**
 * remove_directory
 * recursively removes a directory and all its contents with retry logic
 * retries if directory is not empty after deletion attempt (handles file locking)
 * @param path the directory path to remove
 * @return 0 on success, -1 on failure
 */
static inline int remove_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return 0; /* already gone, success */
    closedir(dir);

    /* try up to 16 times with fixed 128ms delay */
    for (int attempt = 0; attempt < 16; attempt++)
    {
        /* attempt removal */
        (void)remove_directory_once(path);

        /* check if directory is gone or empty */
        dir = opendir(path);
        if (!dir)
        {
            /* directory successfully removed */
            return 0;
        }

        /* directory still exists, check if empty */
        if (is_directory_empty(path))
        {
            closedir(dir);
            /* empty but not removed, try rmdir directly */
#ifdef _WIN32
            if (_rmdir(path) == 0) return 0;
#else
            if (rmdir(path) == 0) return 0;
#endif
        }
        else
        {
            closedir(dir);
        }

        /* directory not empty or removal failed, wait and retry */
        if (attempt < 15)
        {
#ifdef _WIN32
            Sleep(128);
#else
            usleep(128000);
#endif
        }
    }

    dir = opendir(path);
    if (!dir) return 0; /* success */
    closedir(dir);
    return -1; /* failed after all retries */
}

/**
 * tdb_sync_directory
 * syncs a directory to ensure directory entries (new files/subdirs) are persisted
 * on POSIX systems, directory entries must be explicitly synced after mkdir/file creation
 * on Windows, directory entries are immediately durable, so this is a no-op
 * @param dir_path path to the directory to sync
 * @return 0 on success, -1 on error (errors are non-fatal, just logged)
 */
static inline int tdb_sync_directory(const char *dir_path)
{
#ifdef _WIN32
    /* Windows -- directory entries are immediately durable, no sync needed */
    (void)dir_path;
    return 0;
#else
    /* POSIX -- must fsync directory to persist directory entries */
    const int fd = open(dir_path, O_RDONLY);
    if (fd < 0)
    {
        /* non-fatal -- directory might not support fsync (e.g., some network filesystems) */
        return -1;
    }
    const int result = fsync(fd);
    close(fd);
    return result;
#endif
}

/**
 * atomic_rename_file
 * atomically renames a file from old_path to new_path
 * on POSIX systems, rename() is atomic and replaces existing files
 * on windows, rename() fails if target exists, so we remove it first
 * @param old_path the current path of the file
 * @param new_path the new path for the file
 * @return 0 on success, -1 on failure
 */
static inline int atomic_rename_file(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return -1;

#ifdef _WIN32
    /* MoveFileEx with MOVEFILE_REPLACE_EXISTING for atomic rename on Windows
     * this is truly atomic and replaces the target file if it exists */
    if (!MoveFileEx(old_path, new_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        errno = GetLastError();
        return -1;
    }

    /* flush parent directory to ensure rename is durable
     * extract directory from new_path */
    char dir_path[4096];
    const char *last_sep = strrchr(new_path, '\\');
    if (!last_sep) last_sep = strrchr(new_path, '/');
    if (last_sep && (size_t)(last_sep - new_path) < sizeof(dir_path) - 1)
    {
        size_t dir_len = last_sep - new_path;
        memcpy(dir_path, new_path, dir_len);
        dir_path[dir_len] = '\0';

        /* open directory and flush */
        HANDLE dir_handle = CreateFile(dir_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (dir_handle != INVALID_HANDLE_VALUE)
        {
            FlushFileBuffers(dir_handle);
            CloseHandle(dir_handle);
        }
    }

    return 0;
#else
    /* POSIX rename() is atomic and replaces existing files */
    if (rename(old_path, new_path) != 0)
    {
        return -1;
    }

    /* we sync parent directory to ensure rename metadata is durable
     * this is critical for crash safety on non-journaling filesystems
     * https://groups.google.com/g/comp.unix.programmer/c/AM2V83RCOVE?pli=1
     * https://man7.org/linux/man-pages/man2/rename.2.html
     */
    char dir_path[4096];
    const char *last_sep = strrchr(new_path, '/');
    if (last_sep && (size_t)(last_sep - new_path) < sizeof(dir_path) - 1)
    {
        size_t dir_len = last_sep - new_path;
        memcpy(dir_path, new_path, dir_len);
        dir_path[dir_len] = '\0';

        const int dir_fd = open(dir_path, O_RDONLY);
        if (dir_fd >= 0)
        {
            fsync(dir_fd);
            close(dir_fd);
        }
    }

    return 0;
#endif
}

/**
 * atomic_rename_dir
 * renames a directory from old_path to new_path
 * on POSIX systems, rename() works for directories
 * on Windows, rename() fails if target exists, so we use MoveFileEx
 * NOTE: This does not replace existing directories -- caller must ensure target doesn't exist
 * @param old_path the current path of the directory
 * @param new_path the new path for the directory
 * @return 0 on success, -1 on failure
 */
static inline int atomic_rename_dir(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return -1;

#ifdef _WIN32
    /* MoveFileEx works for directories on Windows
     * Note -- MOVEFILE_REPLACE_EXISTING does not work for non-empty directories,
     * so we don't use it here. Caller must ensure target doesn't exist. */
    if (!MoveFileEx(old_path, new_path, MOVEFILE_WRITE_THROUGH))
    {
        errno = GetLastError();
        return -1;
    }

    return 0;
#else
    /* POSIX rename() works for directories */
    if (rename(old_path, new_path) != 0)
    {
        return -1;
    }

    /* sync parent directory for durability */
    char dir_path[4096];
    const char *last_sep = strrchr(new_path, '/');
    if (last_sep && (size_t)(last_sep - new_path) < sizeof(dir_path) - 1)
    {
        size_t dir_len = last_sep - new_path;
        memcpy(dir_path, new_path, dir_len);
        dir_path[dir_len] = '\0';

        const int dir_fd = open(dir_path, O_RDONLY);
        if (dir_fd >= 0)
        {
            fsync(dir_fd);
            close(dir_fd);
        }
    }

    return 0;
#endif
}

/**
 * tdb_get_cpu_count
 * gets the number of available CPU cores
 * @return number of CPU cores, or 4 as fallback
 */
static inline int tdb_get_cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (int)sysinfo.dwNumberOfProcessors;
#elif defined(__APPLE__)
    int count;
    size_t count_len = sizeof(count);
    if (sysctlbyname("hw.logicalcpu", &count, &count_len, NULL, 0) == 0)
    {
        return count;
    }
    return 4; /* fallback */
#else
    /* POSIX systems (Linux, BSD, etc.) */
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count > 0)
    {
        return (int)count;
    }
    return 4; /* fallback */
#endif
}

/**
 * tdb_get_cpu_id
 * gets the current CPU core ID the calling thread is running on
 * used for NUMA-aware partition routing
 * @return current CPU ID, or 0 as fallback
 */
static inline int tdb_get_cpu_id(void)
{
#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
    /* sched_getcpu() is a fast vDSO call (~5ns) on modern Linux */
    extern int sched_getcpu(void);
    int cpu = sched_getcpu();
    return cpu >= 0 ? cpu : 0;
#elif defined(_WIN32)
    return (int)GetCurrentProcessorNumber();
#else
    return 0; /* fallback -- no CPU detection */
#endif
}

/*
 * tdb_get_current_time
 * cross-platform function to get current Unix timestamp in seconds
 * @return current Unix timestamp in seconds
 */
static inline time_t tdb_get_current_time(void)
{
#if defined(_WIN32)
    SYSTEMTIME st;
    FILETIME ft;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER ui;
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return (time_t)((ui.QuadPart - 116444736000000000ULL) / 10000000ULL);
#else
    return time(NULL);
#endif
}

/**
 * tdb_gmtime_r
 * cross-platform thread-safe gmtime
 * @param timep pointer to time_t value
 * @param result pointer to struct tm to fill
 * @return pointer to result on success, NULL on failure
 */
static inline struct tm *tdb_gmtime_r(const time_t *timep, struct tm *result)
{
#if defined(_WIN32)
    return (gmtime_s(result, timep) == 0) ? result : NULL;
#else
    return gmtime_r(timep, result);
#endif
}

/**
 * tdb_fmemopen
 * cross-platform fmemopen
 * opens a memory buffer as a FILE stream for reading
 * @param buf pointer to memory buffer
 * @param size size of buffer in bytes
 * @param mode fopen mode string (e.g. "rb")
 * @return FILE pointer or NULL on failure
 */
static inline FILE *tdb_fmemopen(void *buf, size_t size, const char *mode)
{
#if defined(_WIN32)
    /* windows has no fmemopen -- we write to a temp file and reopen */
    (void)mode;
    char temp_path[MAX_PATH];
    char temp_file[MAX_PATH];
    if (GetTempPathA(MAX_PATH, temp_path) == 0) return NULL;
    if (GetTempFileNameA(temp_path, "tdb", 0, temp_file) == 0) return NULL;

    FILE *fp = fopen(temp_file, "wb");
    if (!fp) return NULL;

    if (size > 0 && buf)
    {
        if (fwrite(buf, 1, size, fp) != size)
        {
            fclose(fp);
            DeleteFileA(temp_file);
            return NULL;
        }
    }
    fclose(fp);

    fp = fopen(temp_file, "rb");
    DeleteFileA(temp_file); /* the file stays open until fclose */
    return fp;
#else
    return fmemopen(buf, size, mode);
#endif
}

#ifndef _WIN32
#include <sys/resource.h> /* getrlimit / RLIMIT_NOFILE for tdb_max_open_files */
#endif

/* fallback open-file ceilings used when the OS limit cannot be queried */
#define TDB_FALLBACK_MAX_OPEN_FILES_POSIX 1024 /* POSIX-typical default RLIMIT_NOFILE soft cap */
#define TDB_FALLBACK_MAX_OPEN_FILES_WIN \
    2048 /* conservative floor for the Windows CRT low-IO layer */

/**
 * tdb_max_open_files
 * report the process's maximum number of simultaneously open file descriptors, so callers can
 * size their fd budgets (e.g. max_open_sstables) to fit the OS limit. returns a conservative
 * fallback when the limit cannot be determined or is unlimited.
 * @return the open-file ceiling as a long
 */
static inline long tdb_max_open_files(void)
{
#if defined(_WIN32)
    /* windows has no RLIMIT_NOFILE. the CRT low-IO layer permits a large but not directly
     * queryable number of _open handles; _getmaxstdio reports the (smaller) stdio stream cap.
     * use the larger of that and a conservative floor so we neither over- nor under-budget. */
    const int stdio_cap = _getmaxstdio();
    const long win_floor = TDB_FALLBACK_MAX_OPEN_FILES_WIN;
    return (stdio_cap > win_floor) ? (long)stdio_cap : win_floor;
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0)
        return (long)rl.rlim_cur;
    return TDB_FALLBACK_MAX_OPEN_FILES_POSIX;
#endif
}

/**
 * tdb_raise_max_open_files
 * raise THIS process's open-file ceiling toward `desired` descriptors and return the ceiling in
 * effect afterwards. POSIX raises the RLIMIT_NOFILE soft limit toward the hard limit (never
 * lowering it, clamped to the hard limit); Windows raises the CRT stdio cap via _setmaxstdio
 * (clamped to its 8192 maximum). an explicit, opt-in action -- tidesdb never raises the limit on
 * its own. a partial or failed raise is non-fatal: the prior ceiling simply stands.
 * @param desired target descriptor count; <= 0 just reports the current ceiling without raising
 * @return the open-file ceiling after the attempt
 */
static inline long tdb_raise_max_open_files(long desired)
{
    if (desired <= 0) return tdb_max_open_files();
#if defined(_WIN32)
    if (desired > 8192) desired = 8192; /* _setmaxstdio hard maximum */
    if (desired > _getmaxstdio()) _setmaxstdio((int)desired);
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        rlim_t target = (rlim_t)desired;
        if (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max) target = rl.rlim_max;
        /* macOS (and some BSDs) reject a soft limit above a kernel per-process cap even when the
         * hard limit reads higher/unlimited, so back off and retry rather than giving up -- this
         * lands the soft limit near the real ceiling instead of leaving it at the low default. */
        const rlim_t floor = rl.rlim_cur;
        while (target > rl.rlim_cur)
        {
            struct rlimit attempt = rl;
            attempt.rlim_cur = target;
            if (setrlimit(RLIMIT_NOFILE, &attempt) == 0)
            {
                rl.rlim_cur = target;
                break;
            }
            if (target <= floor + 1) break; /* even the smallest raise was refused */
            target = floor + (target - floor) / 2;
        }
    }
#endif
    return tdb_max_open_files();
}

#endif /* __COMPAT_H__ */
