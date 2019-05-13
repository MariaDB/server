/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2017 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */

#ifdef WIN32
# ifdef __GNUC__
#  define __MINGW_MSVC_COMPAT_WARNINGS
# endif /* __GNUC__ */

# ifdef __GNUC__
#  include <w32api.h>
#  define GRN_MINIMUM_WINDOWS_VERSION WindowsVista
# else /* __GNUC__ */
#  define GRN_MINIMUM_WINDOWS_VERSION 0x0600 /* Vista */
# endif /* __GNUC__ */

# ifdef WINVER
#  undef WINVER
# endif /* WINVER */
# define WINVER GRN_MINIMUM_WINDOWS_VERSION
# ifdef _WIN32_WINNT
#  undef _WIN32_WINNT
# endif /* _WIN32_WINNT */
# define _WIN32_WINNT GRN_MINIMUM_WINDOWS_VERSION
# ifdef NTDDI_VERSION
#  undef NTDDI_VERSION
# endif /* NTDDI_VERSION */
# define NTDDI_VERSION GRN_MINIMUM_WINDOWS_VERSION

# ifdef WIN32_LEAN_AND_MEAN
#  undef WIN32_LEAN_AND_MEAN
# endif /* WIN32_LEAN_AND_MEAN */
#endif /* WIN32 */

#ifdef __cplusplus
# define __STDC_LIMIT_MACROS
#endif

#include <stdlib.h>
#include <stdint.h>

#include <sys/types.h>

#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif /* HAVE_SYS_PARAM_H */

#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif /* HAVE_SYS_MMAN_H */

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */

#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif /* HAVE_SYS_RESOURCE_H */

#ifdef WIN32
# define GRN_API __declspec(dllexport)
# ifdef GROONGA_MAIN
#  define GRN_VAR __declspec(dllimport)
# else
#  define GRN_VAR __declspec(dllexport) extern
# endif /* GROONGA_MAIN */
#else
# define GRN_API
# define GRN_VAR extern
#endif

#ifdef WIN32
# include <basetsd.h>
# include <process.h>
# include <winsock2.h>
# include <ws2tcpip.h>
# include <windows.h>
# include <stddef.h>
# include <windef.h>
# include <float.h>
# include <time.h>
# include <sys/types.h>

# ifndef __GNUC__
#  define PATH_MAX (MAX_PATH - 1)
#  ifndef __cplusplus
#   define inline _inline
#  endif
# endif

# ifndef __GNUC__
typedef SSIZE_T ssize_t;
typedef int pid_t;
typedef int64_t off64_t;
# endif

# undef MSG_WAITALL
# define MSG_WAITALL 0 /* before Vista, not supported... */
# define SHUT_RDWR SD_BOTH

typedef SOCKET grn_sock;
# define grn_sock_close(sock) closesocket(sock)

# define CALLBACK __stdcall

# ifndef __GNUC__
#  include <intrin.h>
#  include <sys/timeb.h>
#  include <errno.h>
# endif

#else /* WIN32 */

# define GROONGA_API

# ifdef HAVE_UNISTD_H
#  include <unistd.h>
# endif /* HAVE_UNISTD_H */

# ifndef __off64_t_defined
typedef off_t off64_t;
# endif

# ifndef PATH_MAX
#  if defined(MAXPATHLEN)
#   define PATH_MAX MAXPATHLEN
#  else /* MAXPATHLEN */
#   define PATH_MAX 1024
#  endif /* MAXPATHLEN */
# endif /* PATH_MAX */
# ifndef INT_LEAST8_MAX
typedef char int_least8_t;
# endif /* INT_LEAST8_MAX */
# ifndef UINT_LEAST8_MAX
typedef unsigned char uint_least8_t;
# endif /* UINT_LEAST8_MAX */
typedef int grn_sock;
# define grn_sock_close(sock) close(sock)
# define CALLBACK

#endif /* WIN32 */

#ifndef INT8_MAX
# define INT8_MAX (127)
#endif /* INT8_MAX */

#ifndef INT8_MIN
# define INT8_MIN (-128)
#endif /* INT8_MIN */

#ifndef INT16_MAX
# define INT16_MAX (32767)
#endif /* INT16_MAX */

#ifndef INT16_MIN
# define INT16_MIN (-32768)
#endif /* INT16_MIN */

#ifndef INT32_MAX
# define INT32_MAX (2147483647)
#endif /* INT32_MAX */

#ifndef INT32_MIN
# define INT32_MIN (-2147483648)
#endif /* INT32_MIN */

#ifndef UINT32_MAX
# define UINT32_MAX (4294967295)
#endif /* UINT32_MAX */

#ifndef INT64_MAX
# define INT64_MAX (9223372036854775807)
#endif /* INT64_MAX */

#ifndef INT64_MIN
# define INT64_MIN (-9223372036854775808)
#endif /* INT64_MIN */


#ifdef WIN32
# define grn_lseek(fd, offset, whence) _lseeki64(fd, offset, whence)
#else /* WIN32 */
# define grn_lseek(fd, offset, whence) lseek(fd, offset, whence)
#endif /* WIN32 */


#ifdef HAVE_PTHREAD_H
# include <pthread.h>
typedef pthread_t grn_thread;
typedef void * grn_thread_func_result;
# define GRN_THREAD_FUNC_RETURN_VALUE NULL
# define THREAD_CREATE(thread,func,arg) \
  (pthread_create(&(thread), NULL, (func), (arg)))
# define THREAD_JOIN(thread) (pthread_join(thread, NULL))
typedef pthread_mutex_t grn_mutex;
# define MUTEX_INIT(m)       pthread_mutex_init(&m, NULL)
# define MUTEX_LOCK(m)       pthread_mutex_lock(&m)
# define MUTEX_LOCK_CHECK(m) (MUTEX_LOCK(m) == 0)
# define MUTEX_UNLOCK(m)     pthread_mutex_unlock(&m)
# define MUTEX_FIN(m)        pthread_mutex_destroy(&m)
# ifdef HAVE_PTHREAD_MUTEXATTR_SETPSHARED
#  define MUTEX_INIT_SHARED(m) do {\
  pthread_mutexattr_t mutexattr;\
  pthread_mutexattr_init(&mutexattr);\
  pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED);\
  pthread_mutex_init(&m, &mutexattr);\
} while (0)
# else
#  define MUTEX_INIT_SHARED MUTEX_INIT
# endif /* HAVE_PTHREAD_MUTEXATTR_SETPSHARED */

typedef pthread_mutex_t grn_critical_section;
# define CRITICAL_SECTION_INIT(cs)  pthread_mutex_init(&(cs), NULL)
# define CRITICAL_SECTION_ENTER(cs) pthread_mutex_lock(&(cs))
# define CRITICAL_SECTION_LEAVE(cs) pthread_mutex_unlock(&(cs))
# define CRITICAL_SECTION_FIN(cs)

typedef pthread_cond_t grn_cond;
# define COND_INIT(c)   pthread_cond_init(&c, NULL)
# define COND_SIGNAL(c) pthread_cond_signal(&c)
# define COND_WAIT(c,m) pthread_cond_wait(&c, &m)
# define COND_BROADCAST(c) pthread_cond_broadcast(&c)
# ifdef HAVE_PTHREAD_CONDATTR_SETPSHARED
#  define COND_INIT_SHARED(c) do {\
  pthread_condattr_t condattr;\
  pthread_condattr_init(&condattr);\
  pthread_condattr_setpshared(&condattr, PTHREAD_PROCESS_SHARED);\
  pthread_cond_init(&c, &condattr);\
} while (0)
# else
#  define COND_INIT_SHARED COND_INIT
# endif /* HAVE_PTHREAD_CONDATTR_SETPSHARE */
# define COND_FIN(c)    pthread_cond_destroy(&c)

typedef pthread_key_t grn_thread_key;
# define THREAD_KEY_CREATE(key, destr)  pthread_key_create(key, destr)
# define THREAD_KEY_DELETE(key)         pthread_key_delete(key)
# define THREAD_SETSPECIFIC(key, value) pthread_setspecific(key, value)
# define THREAD_GETSPECIFIC(key)        pthread_getspecific(key)

#if USE_UYIELD
  extern int grn_uyield_count;
  #define GRN_TEST_YIELD() do {\
    if (((++grn_uyield_count) & (0x20 - 1)) == 0) {\
      sched_yield();\
      if (grn_uyield_count > 0x1000) {\
        grn_uyield_count = (uint32_t)time(NULL) % 0x1000;\
      }\
    }\
  } while (0)

  #undef assert
  #define assert(assert_expr) do {\
    if (!(assert_expr)){\
      fprintf(stderr, "assertion failed: %s\n", #assert_expr);\
      abort();\
    }\
    GRN_TEST_YIELD();\
  } while (0)

  #define if (if_cond) \
    if ((((++grn_uyield_count) & (0x100 - 1)) != 0 || (sched_yield() * 0) == 0) && (if_cond))
  #define while(while_cond) \
    while ((((++grn_uyield_count) & (0x100 - 1)) != 0 || (sched_yield() * 0) == 0) && (while_cond))

  #if !defined(_POSIX_PRIORITY_SCHEDULING)
  #define sched_yield() grn_nanosleep(1000000 * 20)
  #endif
# else /* USE_UYIELD */
  #define GRN_TEST_YIELD() do {} while (0)
# endif /* USE_UYIELD */

#else /* HAVE_PTHREAD_H */

/* todo */
typedef int grn_thread_key;
# define THREAD_KEY_CREATE(key,destr)
# define THREAD_KEY_DELETE(key)
# define THREAD_SETSPECIFIC(key)
# define THREAD_GETSPECIFIC(key,value)

# ifdef WIN32
typedef uintptr_t grn_thread;
typedef unsigned int grn_thread_func_result;
#  define GRN_THREAD_FUNC_RETURN_VALUE 0
#  define THREAD_CREATE(thread,func,arg) \
  (((thread)=_beginthreadex(NULL, 0, (func), (arg), 0, NULL)) == (grn_thread)0)
#  define THREAD_JOIN(thread) \
  (WaitForSingleObject((HANDLE)(thread), INFINITE) == WAIT_FAILED)
typedef HANDLE grn_mutex;
#  define MUTEX_INIT(m)       ((m) = CreateMutex(0, FALSE, NULL))
#  define MUTEX_LOCK(m)       WaitForSingleObject((m), INFINITE)
#  define MUTEX_LOCK_CHECK(m) (MUTEX_LOCK(m) == WAIT_OBJECT_0)
#  define MUTEX_UNLOCK(m)     ReleaseMutex(m)
#  define MUTEX_FIN(m)        CloseHandle(m)
typedef CRITICAL_SECTION grn_critical_section;
#  define CRITICAL_SECTION_INIT(cs)  InitializeCriticalSection(&(cs))
#  define CRITICAL_SECTION_ENTER(cs) EnterCriticalSection(&(cs))
#  define CRITICAL_SECTION_LEAVE(cs) LeaveCriticalSection(&(cs))
#  define CRITICAL_SECTION_FIN(cs)   DeleteCriticalSection(&(cs))

typedef struct
{
  int waiters_count_;
  HANDLE waiters_count_lock_;
  HANDLE sema_;
  HANDLE waiters_done_;
  size_t was_broadcast_;
} grn_cond;

#  define COND_INIT(c) do { \
  (c).waiters_count_ = 0; \
  (c).sema_ = CreateSemaphore(NULL, 0, 0x7fffffff, NULL); \
  MUTEX_INIT((c).waiters_count_lock_); \
  (c).waiters_done_ = CreateEvent(NULL, FALSE, FALSE, NULL); \
} while (0)

#  define COND_SIGNAL(c) do { \
  MUTEX_LOCK((c).waiters_count_lock_); \
  { \
    int have_waiters = (c).waiters_count_ > 0; \
    MUTEX_UNLOCK((c).waiters_count_lock_); \
    if (have_waiters) { \
      ReleaseSemaphore((c).sema_, 1, 0); \
    } \
  } \
} while (0)

#  define COND_BROADCAST(c) do { \
  MUTEX_LOCK((c).waiters_count_lock_); \
  { \
    int have_waiters = (c).waiters_count_ > 0; \
    if ((c).waiters_count_ > 0) { \
      (c).was_broadcast_ = 1; \
      have_waiters = 1; \
    } \
    if (have_waiters) { \
      ReleaseSemaphore((c).sema_, (c).waiters_count_, 0); \
      MUTEX_UNLOCK((c).waiters_count_lock_); \
      WaitForSingleObject((c).waiters_done_, INFINITE); \
      (c).was_broadcast_ = 0; \
    } \
    else { \
      MUTEX_UNLOCK((c).waiters_count_lock_); \
    } \
  } \
} while (0)

#  define COND_WAIT(c,m) do { \
  MUTEX_LOCK((c).waiters_count_lock_); \
  (c).waiters_count_++; \
  MUTEX_UNLOCK((c).waiters_count_lock_); \
  SignalObjectAndWait((m), (c).sema_, INFINITE, FALSE); \
  MUTEX_LOCK((c).waiters_count_lock_); \
  (c).waiters_count_--; \
  { \
    int last_waiter = (c).was_broadcast_ && (c).waiters_count_ == 0; \
    MUTEX_UNLOCK((c).waiters_count_lock_); \
    if (last_waiter)  { \
      SignalObjectAndWait((c).waiters_done_, (m), INFINITE, FALSE); \
    } \
    else { \
      WaitForSingleObject((m), FALSE); \
    } \
  } \
} while (0)

#  define COND_FIN(c) do { \
  CloseHandle((c).waiters_done_); \
  MUTEX_FIN((c).waiters_count_lock_); \
  CloseHandle((c).sema_); \
} while (0)

# else /* WIN32 */
/* todo */
typedef int grn_cond;
#  define COND_INIT(c)   ((c) = 0)
#  define COND_SIGNAL(c)
#  define COND_WAIT(c,m) do { \
  MUTEX_UNLOCK(m); \
  grn_nanosleep(1000000); \
  MUTEX_LOCK(m); \
} while (0)
#  define COND_FIN(c)
/* todo : must be enhanced! */

# endif /* WIN32 */

# define MUTEX_INIT_SHARED MUTEX_INIT
# define COND_INIT_SHARED COND_INIT

# define GRN_TEST_YIELD() do {} while (0)

#endif /* HAVE_PTHREAD_H */

#define MUTEX_LOCK_ENSURE(ctx_, mutex) do {     \
  grn_ctx *ctx__ = (ctx_);                      \
  do {                                          \
    grn_ctx *ctx = ctx__;                       \
    if (MUTEX_LOCK_CHECK(mutex)) {              \
      break;                                    \
    }                                           \
    if (ctx) {                                  \
      SERR("MUTEX_LOCK");                       \
    }                                           \
    grn_nanosleep(1000000);                     \
  } while (GRN_TRUE);                           \
} while (GRN_FALSE)

/* format string for printf */
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
# define GRN_FMT_INT32D PRId32
# define GRN_FMT_INT32U PRIu32
# define GRN_FMT_INT64D PRId64
# define GRN_FMT_INT64U PRIu64
#else /* HAVE_INTTYPES_H */
# ifdef WIN32
#  define GRN_FMT_INT32D "I32d"
#  define GRN_FMT_INT32U "I32u"
#  define GRN_FMT_INT64D "I64d"
#  define GRN_FMT_INT64U "I64u"
# else /* WIN32 */
#  define GRN_FMT_INT32D "d"
#  define GRN_FMT_INT32U "u"
#  ifdef __x86_64__
#   define GRN_FMT_INT64D "ld"
#   define GRN_FMT_INT64U "lu"
#  else /* __x86_64__ */
#   define GRN_FMT_INT64D "lld"
#   define GRN_FMT_INT64U "llu"
#  endif /* __x86_64__ */
# endif /* WIN32 */
#endif /* HAVE_INTTYPES_H */

#ifdef WIN32
# define GRN_FMT_LLD "I64d"
# define GRN_FMT_LLU "I64u"
# define GRN_FMT_SIZE  "Iu"
# define GRN_FMT_SSIZE "Id"
# ifdef WIN64
#  define GRN_FMT_SOCKET GRN_FMT_INT64U
#  define GRN_FMT_DWORD  "lu"
# else /* WIN64 */
#  define GRN_FMT_SOCKET GRN_FMT_INT32U
#  define GRN_FMT_DWORD  "u"
# endif /* WIN64 */
# define GRN_FMT_OFF64_T GRN_FMT_LLD
#else /* WIN32 */
# define GRN_FMT_LLD  "lld"
# define GRN_FMT_LLU  "llu"
# define GRN_FMT_SIZE  "zu"
# define GRN_FMT_SSIZE "zd"
# define GRN_FMT_SOCKET "d"
# define GRN_FMT_OFF64_T "jd"
#endif /* WIN32 */

#ifdef __GNUC__
# if (defined(__i386__) || defined(__x86_64__)) /* ATOMIC ADD */
/*
 * GRN_ATOMIC_ADD_EX() performs { r = *p; *p += i; } atomically.
 */
#  define GRN_ATOMIC_ADD_EX(p, i, r) \
  __asm__ __volatile__ ("lock xaddl %0, %1" : "=r"(r), "+m"(*p) : "0"(i))
/*
 * GRN_BIT_SCAN_REV() finds the most significant 1 bit of `v'. Then, `r' is set
 * to the index of the found bit. Note that `v' must not be 0.
 */
#  define GRN_BIT_SCAN_REV(v, r) \
  __asm__ __volatile__ ("bsrl %1, %%eax; movl %%eax, %0" : "=r"(r) : "r"(v) : "%eax")
/*
 * GRN_BIT_SCAN_REV0() is similar to GRN_BIT_SCAN_REV() but if `v' is 0, `r' is
 * set to 0.
 */
#  define GRN_BIT_SCAN_REV0(v, r) \
  __asm__ __volatile__ ("bsrl %1, %%eax; cmovzl %1, %%eax; movl %%eax, %0" : "=r"(r) : "r"(v) : "%eax", "cc")
# elif (defined(__PPC__) || defined(__ppc__)) /* ATOMIC ADD */
#  define GRN_ATOMIC_ADD_EX(p,i,r) \
  __asm__ __volatile__ ("\n1:\n\tlwarx %0, 0, %1\n\tadd %0, %0, %2\n\tstwcx. %0, 0, %1\n\tbne- 1b\n\tsub %0, %0, %2" : "=&r" (r) : "r" (p), "r" (i) : "cc", "memory")
/* todo */
#  define GRN_BIT_SCAN_REV(v,r)  for (r = 31; r && !((1 << r) & v); r--)
#  define GRN_BIT_SCAN_REV0(v,r) GRN_BIT_SCAN_REV(v,r)
# elif (defined(__sun) && defined(__SVR4)) /* ATOMIC ADD */
#  include <atomic.h>
#  define GRN_ATOMIC_ADD_EX(p,i,r) \
  (r = atomic_add_32_nv(p, i) - i)
/* todo */
#  define GRN_BIT_SCAN_REV(v,r)  for (r = 31; r && !((1 << r) & v); r--)
#  define GRN_BIT_SCAN_REV0(v,r) GRN_BIT_SCAN_REV(v,r)
# elif defined(__ATOMIC_SEQ_CST) /* GCC atomic builtins */
#  define GRN_ATOMIC_ADD_EX(p,i,r) \
  (r = __atomic_fetch_add(p, i, __ATOMIC_SEQ_CST))
#  define GRN_BIT_SCAN_REV(v,r)  for (r = 31; r && !((1 << r) & v); r--)
#  define GRN_BIT_SCAN_REV0(v,r) GRN_BIT_SCAN_REV(v,r)
# else /* ATOMIC ADD */
/* todo */
#  define GRN_BIT_SCAN_REV(v,r)  for (r = 31; r && !((1 << r) & v); r--)
#  define GRN_BIT_SCAN_REV0(v,r) GRN_BIT_SCAN_REV(v,r)
# endif /* ATOMIC ADD */

# ifdef __i386__ /* ATOMIC 64BIT SET */
#  define GRN_SET_64BIT(p,v) \
  __asm__ __volatile__ ("\txchgl %%esi, %%ebx\n1:\n\tmovl (%0), %%eax\n\tmovl 4(%0), %%edx\n\tlock; cmpxchg8b (%0)\n\tjnz 1b\n\txchgl %%ebx, %%esi" : : "D"(p), "S"(*(((uint32_t *)&(v))+0)), "c"(*(((uint32_t *)&(v))+1)) : "ax", "dx", "memory")
# elif defined(__x86_64__) /* ATOMIC 64BIT SET */
#  define GRN_SET_64BIT(p,v) \
  (*(p) = (v))
# elif (defined(__sun) && defined(__SVR4)) /* ATOMIC 64BIT SET */
/* todo */
#  define GRN_SET_64BIT(p,v) \
  (void)atomic_swap_64(p, v)
# elif defined(__ATOMIC_SEQ_CST) /* GCC atomic builtins */
#  define GRN_SET_64BIT(p,v) \
   __atomic_store_n(p, v, __ATOMIC_SEQ_CST)
# else
#  warning Need atomic 64bit operation support. The current implementation may break data.
#  define GRN_SET_64BIT(p,v) \
  (*(p) = (v))
# endif /* ATOMIC 64BIT SET */

#elif (defined(WIN32) || defined (_WIN64)) /* __GNUC__ */

# define GRN_ATOMIC_ADD_EX(p,i,r) \
  ((r) = InterlockedExchangeAdd((p), (i)))
# if defined(_WIN64) /* ATOMIC 64BIT SET */
#  define GRN_SET_64BIT(p,v) \
  (*(p) = (v))
# else /* ATOMIC 64BIT SET */
#  define GRN_SET_64BIT(p,v) do {\
  uint32_t v1, v2; \
  uint64_t *p2= (p); \
  v1 = *(((uint32_t *)&(v))+0);\
  v2 = *(((uint32_t *)&(v))+1);\
  __asm  _set_loop: \
  __asm  mov esi, p2 \
  __asm  mov ebx, v1 \
  __asm  mov ecx, v2 \
  __asm  mov eax, dword ptr [esi] \
  __asm  mov edx, dword ptr [esi + 4] \
  __asm  lock cmpxchg8b qword ptr [esi] \
  __asm  jnz  _set_loop \
} while (0)
/* TODO: use _InterlockedCompareExchange64 or inline asm */
# endif /* ATOMIC 64BIT SET */

/* todo */
# define GRN_BIT_SCAN_REV(v,r)  for (r = 31; r && !((1 << r) & v); r--)
# define GRN_BIT_SCAN_REV0(v,r) GRN_BIT_SCAN_REV(v,r)

#else /* __GNUC__ */

# if (defined(__sun) && defined(__SVR4)) /* ATOMIC ADD */
#  define __FUNCTION__ ""
#  include <atomic.h>
#  define GRN_ATOMIC_ADD_EX(p,i,r) \
  (r = atomic_add_32_nv(p, i) - i)
/* todo */
#  define GRN_SET_64BIT(p,v) \
  (void)atomic_swap_64(p, v)
# endif /* ATOMIC ADD */
/* todo */
# define GRN_BIT_SCAN_REV(v,r)  for (r = 31; r && !((1 << r) & v); r--)
# define GRN_BIT_SCAN_REV0(v,r) GRN_BIT_SCAN_REV(v,r)

#endif /* __GNUC__ */

typedef uint8_t byte;

#define GRN_ID_WIDTH 30

#ifdef __GNUC__
inline static int
grn_str_greater(const uint8_t *ap, uint32_t as, const uint8_t *bp, uint32_t bs)
{
  for (;; ap++, bp++, as--, bs--) {
    if (!as) { return 0; }
    if (!bs) { return 1; }
    if (*ap < *bp) { return 0; }
    if (*ap > *bp) { return 1; }
  }
}
#else /* __GNUC__ */
# define grn_str_greater(ap,as,bp,bs)\
  (((as) > (bs)) ? (memcmp((ap), (bp), (bs)) >= 0) : (memcmp((ap), (bp), (as)) > 0))
#endif /* __GNUC__ */

#ifdef WORDS_BIGENDIAN
# define grn_hton(buf,key,size) do {\
  uint32_t size_ = (uint32_t)size;\
  uint8_t *buf_ = (uint8_t *)buf;\
  uint8_t *key_ = (uint8_t *)key;\
  while (size_--) { *buf_++ = *key_++; }\
} while (0)
# define grn_ntohi(buf,key,size) do {\
  uint32_t size_ = (uint32_t)size;\
  uint8_t *buf_ = (uint8_t *)buf;\
  uint8_t *key_ = (uint8_t *)key;\
  if (size_) { *buf_++ = 0x80 ^ *key_++; size_--; }\
  while (size_) { *buf_++ = *key_++; size_--; }\
} while (0)
#else /* WORDS_BIGENDIAN */
# define grn_hton(buf,key,size) do {\
  uint32_t size_ = (uint32_t)size;\
  uint8_t *buf_ = (uint8_t *)buf;\
  uint8_t *key_ = (uint8_t *)key + size;\
  while (size_--) { *buf_++ = *(--key_); }\
} while (0)
# define grn_ntohi(buf,key,size) do {\
  uint32_t size_ = (uint32_t)size;\
  uint8_t *buf_ = (uint8_t *)buf;\
  uint8_t *key_ = (uint8_t *)key + size;\
  while (size_ > 1) { *buf_++ = *(--key_); size_--; }\
  if (size_) { *buf_ = 0x80 ^ *(--key_); } \
} while (0)
#endif /* WORDS_BIGENDIAN */
#define grn_ntoh(buf,key,size) grn_hton(buf,key,size)

#ifndef __GNUC_PREREQ
# if defined(__GNUC__) && defined(__GNUC_MINOR__)
#  define __GNUC_PREREQ(maj, min) \
   ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
# else
#  define __GNUC_PREREQ(maj, min) 0
# endif /* defined(__GNUC__) && defined(__GNUC_MINOR__) */
#endif /* __GNUC_PREREQ */

#ifdef _MSC_VER
# define grn_bswap_uint64(in, out) ((out) = _byteswap_uint64(in))
#else /* _MSC_VER */
# if defined(__GNUC__) && __GNUC_PREREQ(4, 3)
#  define grn_bswap_uint64(in, out) ((out) = __builtin_bswap64(in))
# else /* defined(__GNUC__) && __GNUC_PREREQ(4, 3) */
#  define grn_bswap_uint64(in, out) do {\
  uint64_t temp_ = (in);\
  (out) = (temp_ << 56) |\
          ((temp_ & (0xFFULL << 8)) << 40) |\
          ((temp_ & (0xFFULL << 16)) << 24) |\
          ((temp_ & (0xFFULL << 24)) << 8) |\
          ((temp_ & (0xFFULL << 32)) >> 8) |\
          ((temp_ & (0xFFULL << 40)) >> 24) |\
          ((temp_ & (0xFFULL << 48)) >> 40) |\
          (temp_ >> 56);\
} while (0)
# endif /* __GNUC__ */
#endif /* _MSC_VER */

#ifdef WORDS_BIGENDIAN
# define grn_hton_uint64(in, out) ((out) = (in))
#else /* WORDS_BIGENDIAN */
# define grn_hton_uint64(in, out) grn_bswap_uint64(in, out)
#endif /* WORDS_BIGENDIAN */
#define grn_ntoh_uint64(in, out) grn_hton_uint64(in, out)

#define grn_gton(keybuf,key,size) do {\
  const grn_geo_point *point_ = (const grn_geo_point *)key;\
  uint64_t la_ = (uint32_t)point_->latitude;\
  uint64_t lo_ = (uint32_t)point_->longitude;\
  uint64_t result_;\
  la_ = (la_ | (la_ << 16)) & 0x0000FFFF0000FFFFULL;\
  la_ = (la_ | (la_ <<  8)) & 0x00FF00FF00FF00FFULL;\
  la_ = (la_ | (la_ <<  4)) & 0x0F0F0F0F0F0F0F0FULL;\
  la_ = (la_ | (la_ <<  2)) & 0x3333333333333333ULL;\
  la_ = (la_ | (la_ <<  1)) & 0x5555555555555555ULL;\
  lo_ = (lo_ | (lo_ << 16)) & 0x0000FFFF0000FFFFULL;\
  lo_ = (lo_ | (lo_ <<  8)) & 0x00FF00FF00FF00FFULL;\
  lo_ = (lo_ | (lo_ <<  4)) & 0x0F0F0F0F0F0F0F0FULL;\
  lo_ = (lo_ | (lo_ <<  2)) & 0x3333333333333333ULL;\
  lo_ = (lo_ | (lo_ <<  1)) & 0x5555555555555555ULL;\
  result_ = (la_ << 1) | lo_;\
  grn_hton_uint64(result_, result_);\
  grn_memcpy(keybuf, &result_, sizeof(result_));\
} while (0)

#define grn_ntog(keybuf,key,size) do {\
  grn_geo_point *point_ = (grn_geo_point *)keybuf;\
  uint64_t key_ = *(const uint64_t *)key;\
  uint64_t la_, lo_;\
  grn_ntoh_uint64(key_, key_);\
  la_ = (key_ >> 1) & 0x5555555555555555ULL;\
  lo_ = key_ & 0x5555555555555555ULL;\
  la_ = (la_ | (la_ >>  1)) & 0x3333333333333333ULL;\
  la_ = (la_ | (la_ >>  2)) & 0x0F0F0F0F0F0F0F0FULL;\
  la_ = (la_ | (la_ >>  4)) & 0x00FF00FF00FF00FFULL;\
  la_ = (la_ | (la_ >>  8)) & 0x0000FFFF0000FFFFULL;\
  la_ = (la_ | (la_ >> 16)) & 0x00000000FFFFFFFFULL;\
  lo_ = (lo_ | (lo_ >>  1)) & 0x3333333333333333ULL;\
  lo_ = (lo_ | (lo_ >>  2)) & 0x0F0F0F0F0F0F0F0FULL;\
  lo_ = (lo_ | (lo_ >>  4)) & 0x00FF00FF00FF00FFULL;\
  lo_ = (lo_ | (lo_ >>  8)) & 0x0000FFFF0000FFFFULL;\
  lo_ = (lo_ | (lo_ >> 16)) & 0x00000000FFFFFFFFULL;\
  point_->latitude = la_;\
  point_->longitude = lo_;\
} while (0)

#ifdef HAVE__STRTOUI64
# define strtoull(nptr,endptr,base) _strtoui64(nptr,endptr,base)
#endif /* HAVE__STRTOUI64 */

#ifdef USE_FUTEX
# include <linux/futex.h>
# include <sys/syscall.h>

# define GRN_FUTEX_WAIT(p) do {\
  int err;\
  struct timespec timeout = {1, 0};\
  while (1) {\
    if (!(err = syscall(SYS_futex, p, FUTEX_WAIT, *p, &timeout))) {\
      break;\
    }\
    if (err == ETIMEDOUT) {\
      GRN_LOG(ctx, GRN_LOG_CRIT, "timeout in GRN_FUTEX_WAIT(%p)", p);\
      break;\
    } else if (err != EWOULDBLOCK) {\
      GRN_LOG(ctx, GRN_LOG_CRIT, "error %d in GRN_FUTEX_WAIT(%p)", err);\
      break;\
    }\
  }\
} while(0)

# define GRN_FUTEX_WAKE(p) syscall(SYS_futex, p, FUTEX_WAKE, 1)
#else /* USE_FUTEX */
# define GRN_FUTEX_WAIT(p) grn_nanosleep(1000000)
# define GRN_FUTEX_WAKE(p)
#endif /* USE_FUTEX */

#ifndef HOST_NAME_MAX
# ifdef _POSIX_HOST_NAME_MAX
#  define HOST_NAME_MAX _POSIX_HOST_NAME_MAX
# else /* POSIX_HOST_NAME_MAX */
#  define HOST_NAME_MAX 128
# endif /* POSIX_HOST_NAME_MAX */
#endif /* HOST_NAME_MAX */

#define GRN_NEXT_ADDR(p) (((byte *)(p)) + sizeof(*(p)))

GRN_API void grn_sleep(uint32_t seconds);
GRN_API void grn_nanosleep(uint64_t nanoseconds);

#include <groonga.h>
