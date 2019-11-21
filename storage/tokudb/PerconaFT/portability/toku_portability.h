/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#pragma once

#include "toku_config.h"

// Percona portability layer

#if defined(__clang__)
#  define constexpr_static_assert(a, b)
#else
#  define constexpr_static_assert(a, b) static_assert(a, b)
#endif

#if defined(_MSC_VER)
#  error "Windows is not supported."
#endif

#define DEV_NULL_FILE "/dev/null"

// include here, before they get deprecated
#include <toku_atomic.h>

#if defined(__GNUC__)
// GCC linux

#define DO_GCC_PRAGMA(x) _Pragma (#x)

#include <toku_stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdio.h>

#if defined(__FreeBSD__)
#include <stdarg.h>
#endif

#if defined(HAVE_ALLOCA_H)
# include <alloca.h>
#endif

#if defined(__cplusplus)
# include <type_traits>
#endif

#if defined(__cplusplus)
# define cast_to_typeof(v) (decltype(v))
#else
# define cast_to_typeof(v) (__typeof__(v))
#endif

#else // __GNUC__ was not defined, so...
#  error "Must use a GNUC-compatible compiler."
#endif

// Define some constants for Yama in case the build-machine's software is too old.
#if !defined(HAVE_PR_SET_PTRACER)
/*
 * Set specific pid that is allowed to ptrace the current task.
 * A value of 0 mean "no process".
 */
// Well defined ("Yama" in ascii)
#define PR_SET_PTRACER 0x59616d61
#endif
#if !defined(HAVE_PR_SET_PTRACER_ANY)
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

#if defined(__cplusplus)
// decltype() here gives a reference-to-pointer instead of just a pointer,
// just use __typeof__
# define CAST_FROM_VOIDP(name, value) name = static_cast<__typeof__(name)>(value)
#else
# define CAST_FROM_VOIDP(name, value) name = cast_to_typeof(name) (value)
#endif

#ifndef TOKU_OFF_T_DEFINED
#define TOKU_OFF_T_DEFINED
typedef int64_t toku_off_t;
#endif

#include "toku_os.h"
#include "toku_htod.h"
#include "toku_assert.h"
#include "toku_crash.h"
#include "toku_debug_sync.h"

#define UU(x) x __attribute__((__unused__))

// Branch prediction macros.
// If supported by the compiler, will hint in inctruction caching for likely
// branching. Should only be used where there is a very good idea of the correct
// branch heuristics as determined by profiling. Mostly copied from InnoDB.
// Use:
//   "if (FT_LIKELY(x))" where the chances of "x" evaluating true are higher
//   "if (FT_UNLIKELY(x))" where the chances of "x" evaluating false are higher
#if defined(__GNUC__) && (__GNUC__ > 2) && !defined(__INTEL_COMPILER)

// Tell the compiler that 'expr' probably evaluates to 'constant'.
#define FT_EXPECT(expr, constant) __builtin_expect(expr, constant)

#else

#warning "No FT branch prediction operations in use!"
#define FT_EXPECT(expr, constant) (expr)

#endif  // defined(__GNUC__) && (__GNUC__ > 2) && ! defined(__INTEL_COMPILER)

// Tell the compiler that cond is likely to hold
#define FT_LIKELY(cond) FT_EXPECT(bool(cond), true)

// Tell the compiler that cond is unlikely to hold
#define FT_UNLIKELY(cond) FT_EXPECT(bool(cond), false)

#include "toku_instrumentation.h"

#if defined(__cplusplus)
extern "C" {
#endif

// Deprecated functions.
#if !defined(TOKU_ALLOW_DEPRECATED) && !defined(__clang__)
int      creat(const char *pathname, mode_t mode)   __attribute__((__deprecated__));
int      fstat(int fd, struct stat *buf)            __attribute__((__deprecated__));
int      stat(const char *path, struct stat *buf)   __attribute__((__deprecated__));
int      getpid(void)                               __attribute__((__deprecated__));
#    if defined(__FreeBSD__) || defined(__APPLE__)
int syscall(int __sysno, ...)             __attribute__((__deprecated__));
#    else
long int syscall(long int __sysno, ...)             __attribute__((__deprecated__));
#    endif
 long int sysconf(int)                   __attribute__((__deprecated__));
int      mkdir(const char *pathname, mode_t mode)   __attribute__((__deprecated__));
int      dup2(int fd, int fd2)                      __attribute__((__deprecated__));
int      _dup2(int fd, int fd2)                     __attribute__((__deprecated__));
// strdup is a macro in some libraries.
#undef strdup
#    if defined(__FreeBSD__)
char*    strdup(const char *)         __malloc_like __attribute__((__deprecated__));
#    elif defined(__APPLE__)
char*    strdup(const char *)         __attribute__((__deprecated__));
#    else
char*    strdup(const char *)         __THROW __attribute_malloc__ __nonnull ((1)) __attribute__((__deprecated__));
#    endif
#undef __strdup
char*    __strdup(const char *)         __attribute__((__deprecated__));
#    ifndef DONT_DEPRECATE_WRITES
ssize_t  write(int, const void *, size_t)           __attribute__((__deprecated__));
ssize_t  pwrite(int, const void *, size_t, off_t)   __attribute__((__deprecated__));
#endif
#    ifndef DONT_DEPRECATE_MALLOC
#     if defined(__FreeBSD__)
extern void *malloc(size_t)                    __malloc_like __attribute__((__deprecated__));
extern void free(void*)                        __attribute__((__deprecated__));
extern void *realloc(void*, size_t)            __malloc_like __attribute__((__deprecated__));
#     elif defined(__APPLE__)
extern void *malloc(size_t)                    __attribute__((__deprecated__));
extern void free(void*)                        __attribute__((__deprecated__));
extern void *realloc(void*, size_t)            __attribute__((__deprecated__));
#     else
extern void *malloc(size_t)                    __THROW __attribute__((__deprecated__));
extern void free(void*)                        __THROW __attribute__((__deprecated__));
extern void *realloc(void*, size_t)            __THROW __attribute__((__deprecated__));
#     endif
#    endif
#    ifndef DONT_DEPRECATE_ERRNO
//extern int errno __attribute__((__deprecated__));
#    endif
#if !defined(__APPLE__)
// Darwin headers use these types, we should not poison them
#undef TRUE
#undef FALSE
# pragma GCC poison u_int8_t
# pragma GCC poison u_int16_t
# pragma GCC poison u_int32_t
# pragma GCC poison u_int64_t
# pragma GCC poison BOOL
#if !defined(MYSQL_TOKUDB_ENGINE)
# pragma GCC poison FALSE
# pragma GCC poison TRUE
#endif // MYSQL_TOKUDB_ENGINE
#endif
#pragma GCC poison __sync_fetch_and_add
#pragma GCC poison __sync_fetch_and_sub
#pragma GCC poison __sync_fetch_and_or
#pragma GCC poison __sync_fetch_and_and
#pragma GCC poison __sync_fetch_and_xor
#pragma GCC poison __sync_fetch_and_nand
#pragma GCC poison __sync_add_and_fetch
#pragma GCC poison __sync_sub_and_fetch
#pragma GCC poison __sync_or_and_fetch
#pragma GCC poison __sync_and_and_fetch
#pragma GCC poison __sync_xor_and_fetch
#pragma GCC poison __sync_nand_and_fetch
#pragma GCC poison __sync_bool_compare_and_swap
#pragma GCC poison __sync_val_compare_and_swap
#pragma GCC poison __sync_synchronize
#pragma GCC poison __sync_lock_test_and_set
#pragma GCC poison __sync_release
#endif

#if defined(__cplusplus)
};
#endif

void *os_malloc(size_t) __attribute__((__visibility__("default")));
// Effect: See man malloc(2)

void *os_malloc_aligned(size_t /*alignment*/, size_t /*size*/) __attribute__((__visibility__("default")));
// Effect: Perform a malloc(size) with the additional property that the returned pointer is a multiple of ALIGNMENT.
// Requires: alignment is a power of two.


void *os_realloc(void*,size_t) __attribute__((__visibility__("default")));
// Effect: See man realloc(2)

void *os_realloc_aligned(size_t/*alignment*/, void*,size_t) __attribute__((__visibility__("default")));
// Effect: Perform a realloc(p, size) with the additional property that the returned pointer is a multiple of ALIGNMENT.
// Requires: alignment is a power of two.

void os_free(void*) __attribute__((__visibility__("default")));
// Effect: See man free(2)

size_t os_malloc_usable_size(const void *p) __attribute__((__visibility__("default")));
// Effect: Return an estimate of the usable size inside a pointer.  If this function is not defined the memory.cc will
//  look for the jemalloc, libc, or darwin versions of the function for computing memory footprint.

// full_pwrite and full_write performs a pwrite, and checks errors.  It doesn't return unless all the data was written. */
void toku_os_full_pwrite (int fd, const void *buf, size_t len, toku_off_t off) __attribute__((__visibility__("default")));
void toku_os_full_write (int fd, const void *buf, size_t len) __attribute__((__visibility__("default")));

// os_write returns 0 on success, otherwise an errno.
ssize_t toku_os_pwrite (int fd, const void *buf, size_t len, toku_off_t off) __attribute__((__visibility__("default")));
int toku_os_write(int fd, const void *buf, size_t len)
    __attribute__((__visibility__("default")));

// wrappers around file system calls
void toku_os_recursive_delete(const char *path);

TOKU_FILE *toku_os_fdopen_with_source_location(int fildes,
                                               const char *mode,
                                               const char *filename,
                                               const toku_instr_key &instr_key,
                                               const char *src_file,
                                               uint src_line);
#define toku_os_fdopen(FD, M, FN, K) \
    toku_os_fdopen_with_source_location(FD, M, FN, K, __FILE__, __LINE__)

TOKU_FILE *toku_os_fopen_with_source_location(const char *filename,
                                              const char *mode,
                                              const toku_instr_key &instr_key,
                                              const char *src_file,
                                              uint src_line);
#define toku_os_fopen(F, M, K) \
    toku_os_fopen_with_source_location(F, M, K, __FILE__, __LINE__)

int toku_os_open_with_source_location(const char *path,
                                      int oflag,
                                      int mode,
                                      const toku_instr_key &instr_key,
                                      const char *src_file,
                                      uint src_line);
#define toku_os_open(FD, F, M, K) \
    toku_os_open_with_source_location(FD, F, M, K, __FILE__, __LINE__)

int toku_os_open_direct(const char *path,
                        int oflag,
                        int mode,
                        const toku_instr_key &instr_key);

int toku_os_delete_with_source_location(const char *name,
                                        const char *src_file,
                                        uint src_line);
#define toku_os_delete(FN) \
    toku_os_delete_with_source_location(FN, __FILE__, __LINE__)

int toku_os_rename_with_source_location(const char *old_name,
                                        const char *new_name,
                                        const char *src_file,
                                        uint src_line);
#define toku_os_rename(old_name, new_name) \
    toku_os_rename_with_source_location(old_name, new_name, __FILE__, __LINE__)

void toku_os_full_write_with_source_location(int fd,
                                             const void *buf,
                                             size_t len,
                                             const char *src_file,
                                             uint src_line);
#define toku_os_full_write(FD, B, L) \
    toku_os_full_write_with_source_location(FD, B, L, __FILE__, __LINE__)

int toku_os_write_with_source_location(int fd,
                                       const void *buf,
                                       size_t len,
                                       const char *src_file,
                                       uint src_line);
#define toku_os_write(FD, B, L) \
    toku_os_write_with_source_location(FD, B, L, __FILE__, __LINE__)

void toku_os_full_pwrite_with_source_location(int fd,
                                              const void *buf,
                                              size_t len,
                                              toku_off_t off,
                                              const char *src_file,
                                              uint src_line);
#define toku_os_full_pwrite(FD, B, L, O) \
    toku_os_full_pwrite_with_source_location(FD, B, L, O, __FILE__, __LINE__)

ssize_t toku_os_pwrite_with_source_location(int fd,
                                            const void *buf,
                                            size_t len,
                                            toku_off_t off,
                                            const char *src_file,
                                            uint src_line);

#define toku_os_pwrite(FD, B, L, O) \
    toku_os_pwrite_with_source_location(FD, B, L, O, __FILE__, __LINE__)

int toku_os_fwrite_with_source_location(const void *ptr,
                                        size_t size,
                                        size_t nmemb,
                                        TOKU_FILE *stream,
                                        const char *src_file,
                                        uint src_line);

#define toku_os_fwrite(P, S, N, FS) \
    toku_os_fwrite_with_source_location(P, S, N, FS, __FILE__, __LINE__)

int toku_os_fread_with_source_location(void *ptr,
                                       size_t size,
                                       size_t nmemb,
                                       TOKU_FILE *stream,
                                       const char *src_file,
                                       uint src_line);
#define toku_os_fread(P, S, N, FS) \
    toku_os_fread_with_source_location(P, S, N, FS, __FILE__, __LINE__)

TOKU_FILE *toku_os_fopen_with_source_location(const char *filename,
                                              const char *mode,
                                              const toku_instr_key &instr_key,
                                              const char *src_file,
                                              uint src_line);

int toku_os_fclose_with_source_location(TOKU_FILE *stream,
                                        const char *src_file,
                                        uint src_line);

#define toku_os_fclose(FS) \
    toku_os_fclose_with_source_location(FS, __FILE__, __LINE__)

int toku_os_close_with_source_location(int fd,
                                       const char *src_file,
                                       uint src_line);
#define toku_os_close(FD) \
    toku_os_close_with_source_location(FD, __FILE__, __LINE__)

ssize_t toku_os_read_with_source_location(int fd,
                                          void *buf,
                                          size_t count,
                                          const char *src_file,
                                          uint src_line);

#define toku_os_read(FD, B, C) \
    toku_os_read_with_source_location(FD, B, C, __FILE__, __LINE__);

ssize_t inline_toku_os_pread_with_source_location(int fd,
                                                  void *buf,
                                                  size_t count,
                                                  off_t offset,
                                                  const char *src_file,
                                                  uint src_line);
#define toku_os_pread(FD, B, C, O) \
    inline_toku_os_pread_with_source_location(FD, B, C, O, __FILE__, __LINE__);

void file_fsync_internal_with_source_location(int fd,
                                              const char *src_file,
                                              uint src_line);

#define file_fsync_internal(FD) \
    file_fsync_internal_with_source_location(FD, __FILE__, __LINE__);

int toku_os_get_file_size_with_source_location(int fildes,
                                               int64_t *fsize,
                                               const char *src_file,
                                               uint src_line);

#define toku_os_get_file_size(D, S) \
    toku_os_get_file_size_with_source_location(D, S, __FILE__, __LINE__)

// TODO: should this prototype be moved to toku_os.h?
int toku_stat_with_source_location(const char *name,
                                   toku_struct_stat *buf,
                                   const toku_instr_key &instr_key,
                                   const char *src_file,
                                   uint src_line)
    __attribute__((__visibility__("default")));

#define toku_stat(N, B, K) \
    toku_stat_with_source_location(N, B, K, __FILE__, __LINE__)

int toku_os_fstat_with_source_location(int fd,
                                       toku_struct_stat *buf,
                                       const char *src_file,
                                       uint src_line)
    __attribute__((__visibility__("default")));

#define toku_os_fstat(FD, B) \
    toku_os_fstat_with_source_location(FD, B, __FILE__, __LINE__)

#ifdef HAVE_PSI_FILE_INTERFACE2
int inline_toku_os_close(int fd, const char *src_file, uint src_line);
int inline_toku_os_fclose(TOKU_FILE *stream,
                          const char *src_file,
                          uint src_line);
ssize_t inline_toku_os_read(int fd,
                            void *buf,
                            size_t count,
                            const char *src_file,
                            uint src_line);
ssize_t inline_toku_os_pread(int fd,
                             void *buf,
                             size_t count,
                             off_t offset,
                             const char *src_file,
                             uint src_line);
int inline_toku_os_fwrite(const void *ptr,
                          size_t size,
                          size_t nmemb,
                          TOKU_FILE *stream,
                          const char *src_file,
                          uint src_line);
int inline_toku_os_fread(void *ptr,
                         size_t size,
                         size_t nmemb,
                         TOKU_FILE *stream,
                         const char *src_file,
                         uint src_line);
int inline_toku_os_write(int fd,
                         const void *buf,
                         size_t len,
                         const char *src_file,
                         uint src_line);
ssize_t inline_toku_os_pwrite(int fd,
                              const void *buf,
                              size_t len,
                              toku_off_t off,
                              const char *src_file,
                              uint src_line);
void inline_toku_os_full_write(int fd,
                               const void *buf,
                               size_t len,
                               const char *src_file,
                               uint src_line);
void inline_toku_os_full_pwrite(int fd,
                                const void *buf,
                                size_t len,
                                toku_off_t off,
                                const char *src_file,
                                uint src_line);
int inline_toku_os_delete(const char *name,
                          const char *srv_file,
                          uint src_line);
//#else
int inline_toku_os_close(int fd);
int inline_toku_os_fclose(TOKU_FILE *stream);
ssize_t inline_toku_os_read(int fd, void *buf, size_t count);
ssize_t inline_toku_os_pread(int fd, void *buf, size_t count, off_t offset);
int inline_toku_os_fwrite(const void *ptr,
                          size_t size,
                          size_t nmemb,
                          TOKU_FILE *stream);
int inline_toku_os_fread(void *ptr,
                         size_t size,
                         size_t nmemb,
                         TOKU_FILE *stream);
int inline_toku_os_write(int fd, const void *buf, size_t len);
ssize_t inline_toku_os_pwrite(int fd,
                              const void *buf,
                              size_t len,
                              toku_off_t off);
void inline_toku_os_full_write(int fd, const void *buf, size_t len);
void inline_toku_os_full_pwrite(int fd,
                                const void *buf,
                                size_t len,
                                toku_off_t off);
int inline_toku_os_delete(const char *name);
#endif

// wrapper around fsync
void toku_file_fsync(int fd);
int toku_fsync_directory(const char *fname);
void toku_file_fsync_without_accounting(int fd);

// get the number of fsync calls and the fsync times (total)
void toku_get_fsync_times(uint64_t *fsync_count,
                          uint64_t *fsync_time,
                          uint64_t *long_fsync_threshold,
                          uint64_t *long_fsync_count,
                          uint64_t *long_fsync_time);

void toku_set_func_fsync (int (*fsync_function)(int));
void toku_set_func_pwrite (ssize_t (*)(int, const void *, size_t, toku_off_t));
void toku_set_func_full_pwrite (ssize_t (*)(int, const void *, size_t, toku_off_t));
void toku_set_func_write (ssize_t (*)(int, const void *, size_t));
void toku_set_func_full_write (ssize_t (*)(int, const void *, size_t));
void toku_set_func_fdopen (FILE * (*)(int, const char *));
void toku_set_func_fopen (FILE * (*)(const char *, const char *));
void toku_set_func_open (int (*)(const char *, int, int));
void toku_set_func_fclose(int (*)(FILE *));
void toku_set_func_read(ssize_t (*)(int, void *, size_t));
void toku_set_func_pread(ssize_t (*)(int, void *, size_t, off_t));
void toku_set_func_fwrite(
    size_t (*fwrite_fun)(const void *, size_t, size_t, FILE *));

int toku_portability_init(void);
void toku_portability_destroy(void);

// Effect: Return X, where X the smallest multiple of ALIGNMENT such that X>=V.
// Requires: ALIGNMENT is a power of two
static inline uint64_t roundup_to_multiple(uint64_t alignment, uint64_t v) {
    return (v + alignment - 1) & ~(alignment - 1);
}
