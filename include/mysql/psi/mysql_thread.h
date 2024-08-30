/* Copyright (c) 2008, 2023, Oracle and/or its affiliates.
  Copyright (c) 2020, 2021, MariaDB Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef MYSQL_THREAD_H
#define MYSQL_THREAD_H

/**
  @file mysql/psi/mysql_thread.h
  Instrumentation helpers for mysys threads, mutexes,
  read write locks and conditions.
  This header file provides the necessary declarations
  to use the mysys thread API with the performance schema instrumentation.
  In some compilers (SunStudio), 'static inline' functions, when declared
  but not used, are not optimized away (because they are unused) by default,
  so that including a static inline function from a header file does
  create unwanted dependencies, causing unresolved symbols at link time.
  Other compilers, like gcc, optimize these dependencies by default.

  Since the instrumented APIs declared here are wrapper on top
  of my_pthread / safemutex / etc APIs,
  including mysql/psi/mysql_thread.h assumes that
  the dependency on my_pthread and safemutex already exists.
*/
/*
  Note: there are several orthogonal dimensions here.

  Dimension 1: Instrumentation
  HAVE_PSI_INTERFACE is defined when the instrumentation is compiled in.
  This may happen both in debug or production builds.

  Dimension 2: Debug
  SAFE_MUTEX is defined when debug is compiled in.
  This may happen both with and without instrumentation.

  Dimension 3: Platform
  Mutexes are implemented with one of:
  - the pthread library
  - fast mutexes
  - window apis
  This is implemented by various macro definitions in my_pthread.h

  This causes complexity with '#ifdef'-ery that can't be avoided.
*/

#include "mysql/psi/psi.h"
#ifdef MYSQL_SERVER
#ifndef MYSQL_DYNAMIC_PLUGIN
#include "pfs_thread_provider.h"
#endif
#endif

#ifndef PSI_MUTEX_CALL
#define PSI_MUTEX_CALL(M) PSI_DYNAMIC_CALL(M)
#endif

#ifndef PSI_RWLOCK_CALL
#define PSI_RWLOCK_CALL(M) PSI_DYNAMIC_CALL(M)
#endif

#ifndef PSI_COND_CALL
#define PSI_COND_CALL(M) PSI_DYNAMIC_CALL(M)
#endif

#ifndef PSI_THREAD_CALL
#define PSI_THREAD_CALL(M) PSI_DYNAMIC_CALL(M)
#endif

/**
  @defgroup Thread_instrumentation Thread Instrumentation
  @ingroup Instrumentation_interface
  @{
*/

#ifdef HAVE_PSI_THREAD_INTERFACE
#define PSI_CALL_delete_current_thread    PSI_THREAD_CALL(delete_current_thread)
#define PSI_CALL_get_thread               PSI_THREAD_CALL(get_thread)
#define PSI_CALL_new_thread               PSI_THREAD_CALL(new_thread)
#define PSI_CALL_register_thread          PSI_THREAD_CALL(register_thread)
#define PSI_CALL_set_thread               PSI_THREAD_CALL(set_thread)
#define PSI_CALL_set_thread_THD           PSI_THREAD_CALL(set_thread_THD)
#define PSI_CALL_set_thread_connect_attrs PSI_THREAD_CALL(set_thread_connect_attrs)
#define PSI_CALL_set_thread_db            PSI_THREAD_CALL(set_thread_db)
#define PSI_CALL_set_thread_id            PSI_THREAD_CALL(set_thread_id)
#define PSI_CALL_set_thread_os_id         PSI_THREAD_CALL(set_thread_os_id)
#define PSI_CALL_set_thread_info          PSI_THREAD_CALL(set_thread_info)
#define PSI_CALL_set_thread_start_time    PSI_THREAD_CALL(set_thread_start_time)
#define PSI_CALL_set_thread_account       PSI_THREAD_CALL(set_thread_account)
#define PSI_CALL_spawn_thread             PSI_THREAD_CALL(spawn_thread)
#define PSI_CALL_set_connection_type      PSI_THREAD_CALL(set_connection_type)
#else
#define PSI_CALL_delete_current_thread()                do { } while(0)
#define PSI_CALL_get_thread()                           NULL
#define PSI_CALL_new_thread(A1,A2,A3)                   NULL
#define PSI_CALL_register_thread(A1,A2,A3)              do { } while(0)
#define PSI_CALL_set_thread(A1)                         do { } while(0)
#define PSI_CALL_set_thread_THD(A1,A2)                  do { } while(0)
#define PSI_CALL_set_thread_connect_attrs(A1,A2,A3)     0
#define PSI_CALL_set_thread_db(A1,A2)                   do { } while(0)
#define PSI_CALL_set_thread_id(A1,A2)                   do { } while(0)
#define PSI_CALL_set_thread_os_id(A1)                   do { } while(0)
#define PSI_CALL_set_thread_info(A1, A2)                do { } while(0)
#define PSI_CALL_set_thread_start_time(A1)              do { } while(0)
#define PSI_CALL_set_thread_account(A1, A2, A3, A4)     do { } while(0)
#define PSI_CALL_spawn_thread(A1, A2, A3, A4, A5)       0
#define PSI_CALL_set_connection_type(A)                 do { } while(0)
#endif


/**
  An instrumented mutex structure.
  @sa mysql_mutex_t
*/
struct st_mysql_mutex
{
  /** The real mutex. */
#ifdef SAFE_MUTEX
  safe_mutex_t m_mutex;
#else
  pthread_mutex_t m_mutex;
#endif
  /**
    The instrumentation hook.
    Note that this hook is not conditionally defined,
    for binary compatibility of the @c mysql_mutex_t interface.
  */
  struct PSI_mutex *m_psi;
};

/**
  Type of an instrumented mutex.
  @c mysql_mutex_t is a drop-in replacement for @c pthread_mutex_t.
  @sa mysql_mutex_assert_owner
  @sa mysql_mutex_assert_not_owner
  @sa mysql_mutex_init
  @sa mysql_mutex_lock
  @sa mysql_mutex_unlock
  @sa mysql_mutex_destroy
*/
typedef struct st_mysql_mutex mysql_mutex_t;

/**
  An instrumented rwlock structure.
  @sa mysql_rwlock_t
*/
struct st_mysql_rwlock
{
  /** The real rwlock */
  rw_lock_t m_rwlock;
  /**
    The instrumentation hook.
    Note that this hook is not conditionally defined,
    for binary compatibility of the @c mysql_rwlock_t interface.
  */
  struct PSI_rwlock *m_psi;
};

/**
  An instrumented prlock structure.
  @sa mysql_prlock_t
*/
struct st_mysql_prlock
{
  /** The real prlock */
  rw_pr_lock_t m_prlock;
  /**
    The instrumentation hook.
    Note that this hook is not conditionally defined,
    for binary compatibility of the @c mysql_rwlock_t interface.
  */
  struct PSI_rwlock *m_psi;
};

/**
  Type of an instrumented rwlock.
  @c mysql_rwlock_t is a drop-in replacement for @c pthread_rwlock_t.
  @sa mysql_rwlock_init
  @sa mysql_rwlock_rdlock
  @sa mysql_rwlock_tryrdlock
  @sa mysql_rwlock_wrlock
  @sa mysql_rwlock_trywrlock
  @sa mysql_rwlock_unlock
  @sa mysql_rwlock_destroy
*/
typedef struct st_mysql_rwlock mysql_rwlock_t;

/**
  Type of an instrumented prlock.
  A prlock is a read write lock that 'prefers readers' (pr).
  @c mysql_prlock_t is a drop-in replacement for @c rw_pr_lock_t.
  @sa mysql_prlock_init
  @sa mysql_prlock_rdlock
  @sa mysql_prlock_wrlock
  @sa mysql_prlock_unlock
  @sa mysql_prlock_destroy
*/
typedef struct st_mysql_prlock mysql_prlock_t;

/**
  An instrumented cond structure.
  @sa mysql_cond_t
*/
struct st_mysql_cond
{
  /** The real condition */
  pthread_cond_t m_cond;
  /**
    The instrumentation hook.
    Note that this hook is not conditionally defined,
    for binary compatibility of the @c mysql_cond_t interface.
  */
  struct PSI_cond *m_psi;
};

/**
  Type of an instrumented condition.
  @c mysql_cond_t is a drop-in replacement for @c pthread_cond_t.
  @sa mysql_cond_init
  @sa mysql_cond_wait
  @sa mysql_cond_timedwait
  @sa mysql_cond_signal
  @sa mysql_cond_broadcast
  @sa mysql_cond_destroy
*/
typedef struct st_mysql_cond mysql_cond_t;

/*
  Consider the following code:
    static inline void foo() { bar(); }
  when foo() is never called.

  With gcc, foo() is a local static function, so the dependencies
  are optimized away at compile time, and there is no dependency on bar().
  With other compilers (HP, Sun Studio), the function foo() implementation
  is compiled, and bar() needs to be present to link.

  Due to the existing header dependencies in MySQL code, this header file
  is sometime used when it is not needed, which in turn cause link failures
  on some platforms.
  The proper fix would be to cut these extra dependencies in the calling code.
  DISABLE_MYSQL_THREAD_H is a work around to limit dependencies.
  DISABLE_MYSQL_PRLOCK_H is similar, and is used to disable specifically
  the prlock wrappers.
*/
#ifndef DISABLE_MYSQL_THREAD_H

#define mysql_mutex_is_owner(M) safe_mutex_is_owner(&(M)->m_mutex)
/**
  @def mysql_mutex_assert_owner(M)
  Wrapper, to use safe_mutex_assert_owner with instrumented mutexes.
  @c mysql_mutex_assert_owner is a drop-in replacement
  for @c safe_mutex_assert_owner.
*/
#define mysql_mutex_assert_owner(M) \
  safe_mutex_assert_owner(&(M)->m_mutex)

/**
  @def mysql_mutex_assert_not_owner(M)
  Wrapper, to use safe_mutex_assert_not_owner with instrumented mutexes.
  @c mysql_mutex_assert_not_owner is a drop-in replacement
  for @c safe_mutex_assert_not_owner.
*/
#define mysql_mutex_assert_not_owner(M) \
  safe_mutex_assert_not_owner(&(M)->m_mutex)

#define mysql_mutex_setflags(M, F) \
  safe_mutex_setflags(&(M)->m_mutex, (F))

/**
  @def mysql_prlock_assert_write_owner(M)
  Drop-in replacement
  for @c rw_pr_lock_assert_write_owner.
*/
#define mysql_prlock_assert_write_owner(M) \
  rw_pr_lock_assert_write_owner(&(M)->m_prlock)

/**
  @def mysql_prlock_assert_not_write_owner(M)
  Drop-in replacement
  for @c rw_pr_lock_assert_not_write_owner.
*/
#define mysql_prlock_assert_not_write_owner(M) \
  rw_pr_lock_assert_not_write_owner(&(M)->m_prlock)

/**
  @def mysql_mutex_register(P1, P2, P3)
  Mutex registration.
*/
#define mysql_mutex_register(P1, P2, P3) \
  inline_mysql_mutex_register(P1, P2, P3)

/**
  @def mysql_mutex_init(K, M, A)
  Instrumented mutex_init.
  @c mysql_mutex_init is a replacement for @c pthread_mutex_init.
  @param K The PSI_mutex_key for this instrumented mutex
  @param M The mutex to initialize
  @param A Mutex attributes
*/

#ifdef HAVE_PSI_MUTEX_INTERFACE
  #ifdef SAFE_MUTEX
    #define mysql_mutex_init(K, M, A) \
      inline_mysql_mutex_init(K, M, A, #M, __FILE__, __LINE__)
  #else
    #define mysql_mutex_init(K, M, A) \
      inline_mysql_mutex_init(K, M, A)
  #endif
#else
  #ifdef SAFE_MUTEX
    #define mysql_mutex_init(K, M, A) \
      inline_mysql_mutex_init(M, A, #M, __FILE__, __LINE__)
  #else
    #define mysql_mutex_init(K, M, A) \
      inline_mysql_mutex_init(M, A)
  #endif
#endif

/**
  @def mysql_mutex_destroy(M)
  Instrumented mutex_destroy.
  @c mysql_mutex_destroy is a drop-in replacement
  for @c pthread_mutex_destroy.
*/
#ifdef SAFE_MUTEX
  #define mysql_mutex_destroy(M) \
    inline_mysql_mutex_destroy(M, __FILE__, __LINE__)
#else
  #define mysql_mutex_destroy(M) \
    inline_mysql_mutex_destroy(M)
#endif

/**
  @def mysql_mutex_lock(M)
  Instrumented mutex_lock.
  @c mysql_mutex_lock is a drop-in replacement for @c pthread_mutex_lock.
  @param M The mutex to lock
*/

#if defined(SAFE_MUTEX) || defined (HAVE_PSI_MUTEX_INTERFACE)
  #define mysql_mutex_lock(M) \
    inline_mysql_mutex_lock(M, __FILE__, __LINE__)
#else
  #define mysql_mutex_lock(M) \
    inline_mysql_mutex_lock(M)
#endif

/**
  @def mysql_mutex_trylock(M)
  Instrumented mutex_lock.
  @c mysql_mutex_trylock is a drop-in replacement
  for @c pthread_mutex_trylock.
*/

#if defined(SAFE_MUTEX) || defined (HAVE_PSI_MUTEX_INTERFACE)
  #define mysql_mutex_trylock(M) \
    inline_mysql_mutex_trylock(M, __FILE__, __LINE__)
#else
  #define mysql_mutex_trylock(M) \
    inline_mysql_mutex_trylock(M)
#endif

/**
  @def mysql_mutex_unlock(M)
  Instrumented mutex_unlock.
  @c mysql_mutex_unlock is a drop-in replacement for @c pthread_mutex_unlock.
*/
#ifdef SAFE_MUTEX
  #define mysql_mutex_unlock(M) \
    inline_mysql_mutex_unlock(M, __FILE__, __LINE__)
#else
  #define mysql_mutex_unlock(M) \
    inline_mysql_mutex_unlock(M)
#endif

/**
  @def mysql_rwlock_register(P1, P2, P3)
  Rwlock registration.
*/
#define mysql_rwlock_register(P1, P2, P3) \
  inline_mysql_rwlock_register(P1, P2, P3)

/**
  @def mysql_rwlock_init(K, RW)
  Instrumented rwlock_init.
  @c mysql_rwlock_init is a replacement for @c pthread_rwlock_init.
  Note that pthread_rwlockattr_t is not supported in MySQL.
  @param K The PSI_rwlock_key for this instrumented rwlock
  @param RW The rwlock to initialize
*/
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  #define mysql_rwlock_init(K, RW) inline_mysql_rwlock_init(K, RW)
#else
  #define mysql_rwlock_init(K, RW) inline_mysql_rwlock_init(RW)
#endif

/**
  @def mysql_prlock_init(K, RW)
  Instrumented rw_pr_init.
  @c mysql_prlock_init is a replacement for @c rw_pr_init.
  @param K The PSI_rwlock_key for this instrumented prlock
  @param RW The prlock to initialize
*/
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  #define mysql_prlock_init(K, RW) inline_mysql_prlock_init(K, RW)
#else
  #define mysql_prlock_init(K, RW) inline_mysql_prlock_init(RW)
#endif

/**
  @def mysql_rwlock_destroy(RW)
  Instrumented rwlock_destroy.
  @c mysql_rwlock_destroy is a drop-in replacement
  for @c pthread_rwlock_destroy.
*/
#define mysql_rwlock_destroy(RW) inline_mysql_rwlock_destroy(RW)

/**
  @def mysql_prlock_destroy(RW)
  Instrumented rw_pr_destroy.
  @c mysql_prlock_destroy is a drop-in replacement
  for @c rw_pr_destroy.
*/
#define mysql_prlock_destroy(RW) inline_mysql_prlock_destroy(RW)

/**
  @def mysql_rwlock_rdlock(RW)
  Instrumented rwlock_rdlock.
  @c mysql_rwlock_rdlock is a drop-in replacement
  for @c pthread_rwlock_rdlock.
*/
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  #define mysql_rwlock_rdlock(RW) \
    inline_mysql_rwlock_rdlock(RW, __FILE__, __LINE__)
#else
  #define mysql_rwlock_rdlock(RW) \
    inline_mysql_rwlock_rdlock(RW)
#endif

/**
  @def mysql_prlock_rdlock(RW)
  Instrumented rw_pr_rdlock.
  @c mysql_prlock_rdlock is a drop-in replacement
  for @c rw_pr_rdlock.
*/
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  #define mysql_prlock_rdlock(RW) \
    inline_mysql_prlock_rdlock(RW, __FILE__, __LINE__)
#else
  #define mysql_prlock_rdlock(RW) \
    inline_mysql_prlock_rdlock(RW)
#endif

/**
  @def mysql_rwlock_wrlock(RW)
  Instrumented rwlock_wrlock.
  @c mysql_rwlock_wrlock is a drop-in replacement
  for @c pthread_rwlock_wrlock.
*/
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  #define mysql_rwlock_wrlock(RW) \
    inline_mysql_rwlock_wrlock(RW, __FILE__, __LINE__)
#else
  #define mysql_rwlock_wrlock(RW) \
    inline_mysql_rwlock_wrlock(RW)
#endif

/**
  @def mysql_prlock_wrlock(RW)
  Instrumented rw_pr_wrlock.
  @c mysql_prlock_wrlock is a drop-in replacement
  for @c rw_pr_wrlock.
*/
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  #define mysql_prlock_wrlock(RW) \
    inline_mysql_prlock_wrlock(RW, __FILE__, __LINE__)
#else
  #define mysql_prlock_wrlock(RW) \
    inline_mysql_prlock_wrlock(RW)
#endif

/**
  @def mysql_rwlock_tryrdlock(RW)
  Instrumented rwlock_tryrdlock.
  @c mysql_rwlock_tryrdlock is a drop-in replacement
  for @c pthread_rwlock_tryrdlock.
*/
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  #define mysql_rwlock_tryrdlock(RW) \
    inline_mysql_rwlock_tryrdlock(RW, __FILE__, __LINE__)
#else
  #define mysql_rwlock_tryrdlock(RW) \
    inline_mysql_rwlock_tryrdlock(RW)
#endif

/**
  @def mysql_rwlock_trywrlock(RW)
  Instrumented rwlock_trywrlock.
  @c mysql_rwlock_trywrlock is a drop-in replacement
  for @c pthread_rwlock_trywrlock.
*/
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  #define mysql_rwlock_trywrlock(RW) \
    inline_mysql_rwlock_trywrlock(RW, __FILE__, __LINE__)
#else
  #define mysql_rwlock_trywrlock(RW) \
    inline_mysql_rwlock_trywrlock(RW)
#endif

/**
  @def mysql_rwlock_unlock(RW)
  Instrumented rwlock_unlock.
  @c mysql_rwlock_unlock is a drop-in replacement
  for @c pthread_rwlock_unlock.
*/
#define mysql_rwlock_unlock(RW) inline_mysql_rwlock_unlock(RW)

/**
  @def mysql_prlock_unlock(RW)
  Instrumented rw_pr_unlock.
  @c mysql_prlock_unlock is a drop-in replacement
  for @c rw_pr_unlock.
*/
#define mysql_prlock_unlock(RW) inline_mysql_prlock_unlock(RW)

/**
  @def mysql_cond_register(P1, P2, P3)
  Cond registration.
*/
#define mysql_cond_register(P1, P2, P3) \
  inline_mysql_cond_register(P1, P2, P3)

/**
  @def mysql_cond_init(K, C, A)
  Instrumented cond_init.
  @c mysql_cond_init is a replacement for @c pthread_cond_init.
  @param C The cond to initialize
  @param K The PSI_cond_key for this instrumented cond
  @param A Condition attributes
*/
#ifdef HAVE_PSI_COND_INTERFACE
  #define mysql_cond_init(K, C, A) inline_mysql_cond_init(K, C, A)
#else
  #define mysql_cond_init(K, C, A) inline_mysql_cond_init(C, A)
#endif

/**
  @def mysql_cond_destroy(C)
  Instrumented cond_destroy.
  @c mysql_cond_destroy is a drop-in replacement for @c pthread_cond_destroy.
*/
#define mysql_cond_destroy(C) inline_mysql_cond_destroy(C)

/**
  @def mysql_cond_wait(C)
  Instrumented cond_wait.
  @c mysql_cond_wait is a drop-in replacement for @c pthread_cond_wait.
*/
#if defined(SAFE_MUTEX) || defined(HAVE_PSI_COND_INTERFACE)
  #define mysql_cond_wait(C, M) \
    inline_mysql_cond_wait(C, M, __FILE__, __LINE__)
#else
  #define mysql_cond_wait(C, M) \
    inline_mysql_cond_wait(C, M)
#endif

/**
  @def mysql_cond_timedwait(C, M, W)
  Instrumented cond_timedwait.
  @c mysql_cond_timedwait is a drop-in replacement
  for @c pthread_cond_timedwait.
*/
#if defined(SAFE_MUTEX) || defined(HAVE_PSI_COND_INTERFACE)
  #define mysql_cond_timedwait(C, M, W) \
    inline_mysql_cond_timedwait(C, M, W, __FILE__, __LINE__)
#else
  #define mysql_cond_timedwait(C, M, W) \
    inline_mysql_cond_timedwait(C, M, W)
#endif

/**
  @def mysql_cond_signal(C)
  Instrumented cond_signal.
  @c mysql_cond_signal is a drop-in replacement for @c pthread_cond_signal.
*/
#define mysql_cond_signal(C) inline_mysql_cond_signal(C)

/**
  @def mysql_cond_broadcast(C)
  Instrumented cond_broadcast.
  @c mysql_cond_broadcast is a drop-in replacement
  for @c pthread_cond_broadcast.
*/
#define mysql_cond_broadcast(C) inline_mysql_cond_broadcast(C)

/**
  @def mysql_thread_register(P1, P2, P3)
  Thread registration.
*/
#define mysql_thread_register(P1, P2, P3) \
  inline_mysql_thread_register(P1, P2, P3)

/**
  @def mysql_thread_create(K, P1, P2, P3, P4)
  Instrumented pthread_create.
  This function creates both the thread instrumentation and a thread.
  @c mysql_thread_create is a replacement for @c pthread_create.
  The parameter P4 (or, if it is NULL, P1) will be used as the
  instrumented thread "identity".
  Providing a P1 / P4 parameter with a different value for each call
  will on average improve performances, since this thread identity value
  is used internally to randomize access to data and prevent contention.
  This is optional, and the improvement is not guaranteed, only statistical.
  @param K The PSI_thread_key for this instrumented thread
  @param P1 pthread_create parameter 1
  @param P2 pthread_create parameter 2
  @param P3 pthread_create parameter 3
  @param P4 pthread_create parameter 4
*/
#ifdef HAVE_PSI_THREAD_INTERFACE
  #define mysql_thread_create(K, P1, P2, P3, P4) \
    inline_mysql_thread_create(K, P1, P2, P3, P4)
#else
  #define mysql_thread_create(K, P1, P2, P3, P4) \
    pthread_create(P1, P2, P3, P4)
#endif

/**
  @def mysql_thread_set_psi_id(I)
  Set the thread identifier for the instrumentation.
  @param I The thread identifier
*/
#ifdef HAVE_PSI_THREAD_INTERFACE
  #define mysql_thread_set_psi_id(I) inline_mysql_thread_set_psi_id(I)
#else
  #define mysql_thread_set_psi_id(I) do {} while (0)
#endif

/**
  @def mysql_thread_set_psi_THD(T)
  Set the thread sql session for the instrumentation.
  @param I The thread identifier
*/
#ifdef HAVE_PSI_THREAD_INTERFACE
  #define mysql_thread_set_psi_THD(T) inline_mysql_thread_set_psi_THD(T)
#else
  #define mysql_thread_set_psi_THD(T) do {} while (0)
#endif

static inline void inline_mysql_mutex_register(
#ifdef HAVE_PSI_MUTEX_INTERFACE
  const char *category,
  PSI_mutex_info *info,
  int count
#else
  const char *category __attribute__ ((unused)),
  void *info __attribute__ ((unused)),
  int count __attribute__ ((unused))
#endif
)
{
#ifdef HAVE_PSI_MUTEX_INTERFACE
  PSI_MUTEX_CALL(register_mutex)(category, info, count);
#endif
}

static inline int inline_mysql_mutex_init(
#ifdef HAVE_PSI_MUTEX_INTERFACE
  PSI_mutex_key key,
#endif
  mysql_mutex_t *that,
  const pthread_mutexattr_t *attr
#ifdef SAFE_MUTEX
  , const char *src_name, const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_MUTEX_INTERFACE
  that->m_psi= PSI_MUTEX_CALL(init_mutex)(key, &that->m_mutex);
#else
  that->m_psi= NULL;
#endif
#ifdef SAFE_MUTEX
  return safe_mutex_init(&that->m_mutex, attr, src_name, src_file, src_line);
#else
  return pthread_mutex_init(&that->m_mutex, attr);
#endif
}

static inline int inline_mysql_mutex_destroy(
  mysql_mutex_t *that
#ifdef SAFE_MUTEX
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_MUTEX_INTERFACE
  if (that->m_psi != NULL)
  {
    PSI_MUTEX_CALL(destroy_mutex)(that->m_psi);
    that->m_psi= NULL;
  }
#endif
#ifdef SAFE_MUTEX
  return safe_mutex_destroy(&that->m_mutex, src_file, src_line);
#else
  return pthread_mutex_destroy(&that->m_mutex);
#endif
}

#ifdef HAVE_PSI_MUTEX_INTERFACE
ATTRIBUTE_COLD int psi_mutex_lock(mysql_mutex_t *that,
                                  const char *file, uint line);
ATTRIBUTE_COLD int psi_mutex_trylock(mysql_mutex_t *that,
                                     const char *file, uint line);
#endif

static inline int inline_mysql_mutex_lock(
  mysql_mutex_t *that
#if defined(SAFE_MUTEX) || defined (HAVE_PSI_MUTEX_INTERFACE)
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_MUTEX_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_mutex_lock(that, src_file, src_line);
#endif
  /* Non instrumented code */
#ifdef SAFE_MUTEX
  return safe_mutex_lock(&that->m_mutex, FALSE, src_file, src_line);
#else
  return pthread_mutex_lock(&that->m_mutex);
#endif
}

static inline int inline_mysql_mutex_trylock(
  mysql_mutex_t *that
#if defined(SAFE_MUTEX) || defined (HAVE_PSI_MUTEX_INTERFACE)
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_MUTEX_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_mutex_trylock(that, src_file, src_line);
#endif
  /* Non instrumented code */
#ifdef SAFE_MUTEX
  return safe_mutex_lock(&that->m_mutex, TRUE, src_file, src_line);
#else
  return pthread_mutex_trylock(&that->m_mutex);
#endif
}

static inline int inline_mysql_mutex_unlock(
  mysql_mutex_t *that
#ifdef SAFE_MUTEX
  , const char *src_file, uint src_line
#endif
  )
{
  int result;

#ifdef HAVE_PSI_MUTEX_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    PSI_MUTEX_CALL(unlock_mutex)(that->m_psi);
#endif

#ifdef SAFE_MUTEX
  result= safe_mutex_unlock(&that->m_mutex, src_file, src_line);
#else
  result= pthread_mutex_unlock(&that->m_mutex);
#endif

  return result;
}

static inline void inline_mysql_rwlock_register(
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  const char *category,
  PSI_rwlock_info *info,
  int count
#else
  const char *category __attribute__ ((unused)),
  void *info __attribute__ ((unused)),
  int count __attribute__ ((unused))
#endif
)
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  PSI_RWLOCK_CALL(register_rwlock)(category, info, count);
#endif
}

static inline int inline_mysql_rwlock_init(
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  PSI_rwlock_key key,
#endif
  mysql_rwlock_t *that)
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  that->m_psi= PSI_RWLOCK_CALL(init_rwlock)(key, &that->m_rwlock);
#else
  that->m_psi= NULL;
#endif
  /*
    pthread_rwlockattr_t is not used in MySQL.
  */
  return my_rwlock_init(&that->m_rwlock, NULL);
}

#ifndef DISABLE_MYSQL_PRLOCK_H
static inline int inline_mysql_prlock_init(
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  PSI_rwlock_key key,
#endif
  mysql_prlock_t *that)
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  that->m_psi= PSI_RWLOCK_CALL(init_rwlock)(key, &that->m_prlock);
#else
  that->m_psi= NULL;
#endif
  return rw_pr_init(&that->m_prlock);
}
#endif

static inline int inline_mysql_rwlock_destroy(
  mysql_rwlock_t *that)
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
  {
    PSI_RWLOCK_CALL(destroy_rwlock)(that->m_psi);
    that->m_psi= NULL;
  }
#endif
  return rwlock_destroy(&that->m_rwlock);
}

#ifndef DISABLE_MYSQL_PRLOCK_H
static inline int inline_mysql_prlock_destroy(
  mysql_prlock_t *that)
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
  {
    PSI_RWLOCK_CALL(destroy_rwlock)(that->m_psi);
    that->m_psi= NULL;
  }
#endif
  return rw_pr_destroy(&that->m_prlock);
}
#endif

#ifdef HAVE_PSI_RWLOCK_INTERFACE
ATTRIBUTE_COLD
int psi_rwlock_rdlock(mysql_rwlock_t *that, const char *file, uint line);
ATTRIBUTE_COLD
int psi_rwlock_tryrdlock(mysql_rwlock_t *that, const char *file, uint line);
ATTRIBUTE_COLD
int psi_rwlock_wrlock(mysql_rwlock_t *that, const char *file, uint line);
ATTRIBUTE_COLD
int psi_rwlock_trywrlock(mysql_rwlock_t *that, const char *file, uint line);
# ifndef DISABLE_MYSQL_PRLOCK_H
ATTRIBUTE_COLD
int psi_prlock_rdlock(mysql_prlock_t *that, const char *file, uint line);
ATTRIBUTE_COLD
int psi_prlock_wrlock(mysql_prlock_t *that, const char *file, uint line);
# endif
#endif

static inline int inline_mysql_rwlock_rdlock(
  mysql_rwlock_t *that
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_rwlock_rdlock(that, src_file, src_line);
#endif
  return rw_rdlock(&that->m_rwlock);
}

#ifndef DISABLE_MYSQL_PRLOCK_H
static inline int inline_mysql_prlock_rdlock(
  mysql_prlock_t *that
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_prlock_rdlock(that, src_file, src_line);
#endif
  return rw_pr_rdlock(&that->m_prlock);
}
#endif

static inline int inline_mysql_rwlock_wrlock(
  mysql_rwlock_t *that
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_rwlock_wrlock(that, src_file, src_line);
#endif
  return rw_wrlock(&that->m_rwlock);
}

#ifndef DISABLE_MYSQL_PRLOCK_H
static inline int inline_mysql_prlock_wrlock(
  mysql_prlock_t *that
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_prlock_wrlock(that, src_file, src_line);
#endif
  return rw_pr_wrlock(&that->m_prlock);
}
#endif

static inline int inline_mysql_rwlock_tryrdlock(
  mysql_rwlock_t *that
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_rwlock_tryrdlock(that, src_file, src_line);
#endif
  return rw_tryrdlock(&that->m_rwlock);
}

static inline int inline_mysql_rwlock_trywrlock(
  mysql_rwlock_t *that
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_rwlock_trywrlock(that, src_file, src_line);
#endif
  return rw_trywrlock(&that->m_rwlock);
}

static inline int inline_mysql_rwlock_unlock(
  mysql_rwlock_t *that)
{
  int result;
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    PSI_RWLOCK_CALL(unlock_rwlock)(that->m_psi);
#endif
  result= rw_unlock(&that->m_rwlock);
  return result;
}

#ifndef DISABLE_MYSQL_PRLOCK_H
static inline int inline_mysql_prlock_unlock(
  mysql_prlock_t *that)
{
  int result;
#ifdef HAVE_PSI_RWLOCK_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    PSI_RWLOCK_CALL(unlock_rwlock)(that->m_psi);
#endif
  result= rw_pr_unlock(&that->m_prlock);
  return result;
}
#endif

static inline void inline_mysql_cond_register(
#ifdef HAVE_PSI_COND_INTERFACE
  const char *category,
  PSI_cond_info *info,
  int count
#else
  const char *category __attribute__ ((unused)),
  void *info __attribute__ ((unused)),
  int count __attribute__ ((unused))
#endif
)
{
#ifdef HAVE_PSI_COND_INTERFACE
  PSI_COND_CALL(register_cond)(category, info, count);
#endif
}

static inline int inline_mysql_cond_init(
#ifdef HAVE_PSI_COND_INTERFACE
  PSI_cond_key key,
#endif
  mysql_cond_t *that,
  const pthread_condattr_t *attr)
{
#ifdef HAVE_PSI_COND_INTERFACE
  that->m_psi= PSI_COND_CALL(init_cond)(key, &that->m_cond);
#else
  that->m_psi= NULL;
#endif
  return pthread_cond_init(&that->m_cond, attr);
}

static inline int inline_mysql_cond_destroy(
  mysql_cond_t *that)
{
#ifdef HAVE_PSI_COND_INTERFACE
  if (psi_likely(that->m_psi != NULL))
  {
    PSI_COND_CALL(destroy_cond)(that->m_psi);
    that->m_psi= NULL;
  }
#endif
  return pthread_cond_destroy(&that->m_cond);
}

#ifdef HAVE_PSI_COND_INTERFACE
ATTRIBUTE_COLD int psi_cond_wait(mysql_cond_t *that, mysql_mutex_t *mutex,
                                 const char *file, uint line);
ATTRIBUTE_COLD int psi_cond_timedwait(mysql_cond_t *that, mysql_mutex_t *mutex,
                                      const struct timespec *abstime,
                                      const char *file, uint line);
#endif

static inline int inline_mysql_cond_wait(
  mysql_cond_t *that,
  mysql_mutex_t *mutex
#if defined(SAFE_MUTEX) || defined(HAVE_PSI_COND_INTERFACE)
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_COND_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_cond_wait(that, mutex, src_file, src_line);
#endif
  return my_cond_wait(&that->m_cond, &mutex->m_mutex);
}

static inline int inline_mysql_cond_timedwait(
  mysql_cond_t *that,
  mysql_mutex_t *mutex,
  const struct timespec *abstime
#if defined(SAFE_MUTEX) || defined(HAVE_PSI_COND_INTERFACE)
  , const char *src_file, uint src_line
#endif
  )
{
#ifdef HAVE_PSI_COND_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    return psi_cond_timedwait(that, mutex, abstime, src_file, src_line);
#endif
  return my_cond_timedwait(&that->m_cond, &mutex->m_mutex, abstime);
}

static inline int inline_mysql_cond_signal(
  mysql_cond_t *that)
{
  int result;
#ifdef HAVE_PSI_COND_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    PSI_COND_CALL(signal_cond)(that->m_psi);
#endif
  result= pthread_cond_signal(&that->m_cond);
  return result;
}

static inline int inline_mysql_cond_broadcast(
  mysql_cond_t *that)
{
  int result;
#ifdef HAVE_PSI_COND_INTERFACE
  if (psi_likely(that->m_psi != NULL))
    PSI_COND_CALL(broadcast_cond)(that->m_psi);
#endif
  result= pthread_cond_broadcast(&that->m_cond);
  return result;
}

static inline void inline_mysql_thread_register(
#ifdef HAVE_PSI_THREAD_INTERFACE
  const char *category,
  PSI_thread_info *info,
  int count
#else
  const char *category __attribute__ ((unused)),
  void *info __attribute__ ((unused)),
  int count __attribute__ ((unused))
#endif
)
{
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(register_thread)(category, info, count);
#endif
}

#ifdef HAVE_PSI_THREAD_INTERFACE
static inline int inline_mysql_thread_create(
  PSI_thread_key key,
  pthread_t *thread, const pthread_attr_t *attr,
  void *(*start_routine)(void*), void *arg)
{
  int result;
  result= PSI_THREAD_CALL(spawn_thread)(key, thread, attr, start_routine, arg);
  return result;
}

static inline void inline_mysql_thread_set_psi_id(my_thread_id id)
{
  struct PSI_thread *psi= PSI_THREAD_CALL(get_thread)();
  PSI_THREAD_CALL(set_thread_id)(psi, id);
}

#ifdef __cplusplus
class THD;
static inline void inline_mysql_thread_set_psi_THD(THD *thd)
{
  struct PSI_thread *psi= PSI_THREAD_CALL(get_thread)();
  PSI_THREAD_CALL(set_thread_THD)(psi, thd);
}
#endif /* __cplusplus */

static inline void mysql_thread_set_peer_port(uint port __attribute__ ((unused))) {
#ifdef HAVE_PSI_THREAD_INTERFACE
  struct PSI_thread *psi = PSI_THREAD_CALL(get_thread)();
  PSI_THREAD_CALL(set_thread_peer_port)(psi, port);
#endif
}

#endif

#endif /* DISABLE_MYSQL_THREAD_H */

/** @} (end of group Thread_instrumentation) */

#endif

