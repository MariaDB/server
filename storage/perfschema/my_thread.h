#ifndef STORAGE_PERFSCHEMA_MY_THREAD_INCLUDED
#define STORAGE_PERFSCHEMA_MY_THREAD_INCLUDED

#include <my_pthread.h>
#include <m_string.h>
#include "pfs_config.h"

#ifdef HAVE_SYS_GETTID
#include <sys/types.h>
#include <sys/syscall.h>
#endif

#ifdef HAVE_PTHREAD_GETTHREADID_NP
#include <pthread_np.h>
#endif

#if defined(HAVE_INTEGER_PTHREAD_SELF)
#include <cstdint>
#endif

typedef pthread_key_t thread_local_key_t;
typedef pthread_t my_thread_handle;
typedef pthread_attr_t my_thread_attr_t;
#if defined(HAVE_PTHREAD_THREADID_NP) || defined(HAVE_GETTID) || defined(HAVE_SYS_GETTID) || defined(HAVE_GETTHRID)
typedef pid_t my_thread_os_id_t;
#elif defined(_WIN32)
typedef uint32 my_thread_os_id_t;
#elif defined(HAVE_PTHREAD_GETTHREADID_NP)
typedef int my_thread_os_id_t;
#elif defined(HAVE_INTEGER_PTHREAD_SELF)
typedef uintptr_t my_thread_os_id_t;
#else
typedef unsigned long long my_thread_os_id_t;
#endif

#define LOCK_plugin_delete LOCK_plugin

static inline int my_create_thread_local_key(thread_local_key_t *key, void (*destructor)(void*))
{ return pthread_key_create(key, destructor); }

static inline int my_delete_thread_local_key(thread_local_key_t key)
{ return pthread_key_delete(key); }

static inline void *my_get_thread_local(thread_local_key_t key)
{ return pthread_getspecific(key); }

static inline int my_set_thread_local(thread_local_key_t key, const void *ptr)
{ return pthread_setspecific(key, ptr); }

static inline int my_thread_create(my_thread_handle *thread,
        const my_thread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{ return pthread_create(thread, attr, start_routine, arg); }

static inline my_thread_os_id_t my_thread_os_id()
{
#ifdef HAVE_PTHREAD_THREADID_NP
  /*
    macOS.

    Be careful to use this version first, and to not use SYS_gettid on macOS,
    as SYS_gettid has a different meaning compared to linux gettid().
  */
  uint64_t tid64;
  pthread_threadid_np(nullptr, &tid64);
  return (pid_t)tid64;
#else
#ifdef HAVE_GETTID
  /* Linux glibc-2.30+ */
  return gettid();
#else
#ifdef HAVE_SYS_GETTID
  /*
    Linux before glibc-2.30
    See man gettid
  */
  return syscall(SYS_gettid);
#else
#ifdef _WIN32
  /* Windows */
  return GetCurrentThreadId();
#else
#ifdef HAVE_PTHREAD_GETTHREADID_NP
  /* FreeBSD 10.2 */
  return pthread_getthreadid_np();
#else
#ifdef HAVE_GETTHRID
  /* OpenBSD */
  return getthrid();
#else
#ifdef HAVE_INTEGER_PTHREAD_SELF
  /* NetBSD, and perhaps something else, fallback. */
  return (my_thread_os_id_t) pthread_self();
#else
  /* Feature not available. */
  return 0;
#endif /* HAVE_INTEGER_PTHREAD_SELF */
#endif /* HAVE_GETTHRID */
#endif /* HAVE_PTHREAD_GETTHREADID_NP */
#endif /* _WIN32 */
#endif /* HAVE_SYS_GETTID */
#endif /* HAVE_GETTID */
#endif /* HAVE_PTHREAD_THREADID_NP */
}

#define CHANNEL_NAME_LENGTH MAX_CONNECTION_NAME

enum enum_mysql_show_scope
{
  SHOW_SCOPE_UNDEF,
  SHOW_SCOPE_GLOBAL,
  SHOW_SCOPE_SESSION,
  SHOW_SCOPE_ALL
};
typedef enum enum_mysql_show_scope SHOW_SCOPE;

#define SHOW_VAR_MAX_NAME_LEN NAME_LEN

static inline char *my_stpnmov(char *dst, const char *src, size_t n)
{ return strnmov(dst, src, n); }

static inline size_t bin_to_hex_str(char *to, size_t to_len,
                                    const char *from, size_t from_len)
{
  if (to_len < from_len * 2 + 1)
    return 0 ;
  for (size_t i=0; i < from_len; i++, from++)
  {
    *to++=_dig_vec_upper[((unsigned char) *from) >> 4];
    *to++=_dig_vec_upper[((unsigned char) *from) & 0xF];
  }
  *to= '\0';
  return from_len * 2 + 1;
}

#define thd_get_psi(X) ((X)->get_psi())

#endif
