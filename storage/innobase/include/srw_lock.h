/*****************************************************************************

Copyright (c) 2020, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

#pragma once
#include "univ.i"
#include "rw_lock.h"

#if defined __linux__
/* futex(2): FUTEX_WAIT_PRIVATE, FUTEX_WAKE_PRIVATE */
#elif defined __OpenBSD__ || defined __FreeBSD__ || defined __DragonFly__
/* system calls similar to Linux futex(2) */
#elif defined _WIN32
/* SRWLOCK as well as WaitOnAddress(), WakeByAddressSingle() */
#else
# define SUX_LOCK_GENERIC /* fall back to generic synchronization primitives */
#endif

#if !defined SUX_LOCK_GENERIC && 0 /* defined SAFE_MUTEX */
# define SUX_LOCK_GENERIC /* Use dummy implementation for debugging purposes */
#endif

#ifdef SUX_LOCK_GENERIC
/** An exclusive-only variant of srw_lock */
template<bool spinloop>
class pthread_mutex_wrapper final
{
  pthread_mutex_t lock;
public:
  void init()
  {
    if (spinloop)
      pthread_mutex_init(&lock, MY_MUTEX_INIT_FAST);
    else
      pthread_mutex_init(&lock, nullptr);
  }
  void destroy() { pthread_mutex_destroy(&lock); }
# ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
  void wr_lock() { pthread_mutex_lock(&lock); }
# else
private:
  void wr_wait();
public:
  inline void wr_lock();
# endif
  void wr_unlock() { pthread_mutex_unlock(&lock); }
  bool wr_lock_try() { return !pthread_mutex_trylock(&lock); }
};

# ifndef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
template<> void pthread_mutex_wrapper<true>::wr_wait();
template<>
inline void pthread_mutex_wrapper<false>::wr_lock()
{ pthread_mutex_lock(&lock); }
template<>
inline void pthread_mutex_wrapper<true>::wr_lock()
{ if (!wr_lock_try()) wr_wait(); }
# endif
#endif

/** Futex-based mutex */
template<bool spinloop>
class srw_mutex_impl final
{
  /** The lock word, containing HOLDER + 1 if the lock is being held,
  plus the number of waiters */
  std::atomic<uint32_t> lock;
  /** Identifies that the lock is being held */
  static constexpr uint32_t HOLDER= 1U << 31;

#ifdef SUX_LOCK_GENERIC
public:
  /** The mutex for the condition variables. */
  pthread_mutex_t mutex;
private:
  /** Condition variable for the lock word. Used with mutex. */
  pthread_cond_t cond;
#endif

  /** Wait until the mutex has been acquired */
  void wait_and_lock();
  /** Wait for lock!=lk */
  inline void wait(uint32_t lk);
  /** Wake up one wait() thread */
  void wake();
public:
  /** @return whether the mutex is being held or waited for */
  bool is_locked_or_waiting() const
  { return lock.load(std::memory_order_acquire) != 0; }
  /** @return whether the mutex is being held by any thread */
  bool is_locked() const
  { return (lock.load(std::memory_order_acquire) & HOLDER) != 0; }

  void init()
  {
    DBUG_ASSERT(!is_locked_or_waiting());
#ifdef SUX_LOCK_GENERIC
    pthread_mutex_init(&mutex, nullptr);
    pthread_cond_init(&cond, nullptr);
#endif
  }
  void destroy()
  {
    DBUG_ASSERT(!is_locked_or_waiting());
#ifdef SUX_LOCK_GENERIC
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
#endif
  }

  /** @return whether the mutex was acquired */
  bool wr_lock_try()
  {
    uint32_t lk= 0;
    return lock.compare_exchange_strong(lk, HOLDER + 1,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed);
  }

  void wr_lock() { if (!wr_lock_try()) wait_and_lock(); }
  void wr_unlock()
  {
    const uint32_t lk= lock.fetch_sub(HOLDER + 1, std::memory_order_release);
    if (lk != HOLDER + 1)
    {
      DBUG_ASSERT(lk & HOLDER);
      wake();
    }
  }
};

#ifdef SUX_LOCK_GENERIC
typedef pthread_mutex_wrapper<true> srw_spin_mutex;
typedef pthread_mutex_wrapper<false> srw_mutex;
#else
typedef srw_mutex_impl<true> srw_spin_mutex;
typedef srw_mutex_impl<false> srw_mutex;
#endif

template<bool spinloop> class srw_lock_impl;

/** Slim shared-update-exclusive lock with no recursion */
template<bool spinloop>
class ssux_lock_impl final
{
#ifdef UNIV_PFS_RWLOCK
  friend class ssux_lock;
# ifdef SUX_LOCK_GENERIC
# elif defined _WIN32
# else
  friend srw_lock_impl<spinloop>;
# endif
#endif
  /** mutex for synchronization; held by U or X lock holders */
  srw_mutex_impl<spinloop> writer;
#ifdef SUX_LOCK_GENERIC
  /** Condition variable for "readers"; used with writer.mutex. */
  pthread_cond_t readers_cond;
#endif
  /** S or U holders, and WRITER flag for X holder or waiter */
  std::atomic<uint32_t> readers;
  /** indicates an X request; readers=WRITER indicates granted X lock */
  static constexpr uint32_t WRITER= 1U << 31;

  /** Wait for readers!=lk */
  inline void wait(uint32_t lk);

  /** Wait for readers!=lk|WRITER */
  void wr_wait(uint32_t lk);
  /** Wake up wait() on the last rd_unlock() */
  void wake();
  /** Acquire a read lock */
  void rd_wait();
public:
  void init()
  {
    writer.init();
    DBUG_ASSERT(is_vacant());
#ifdef SUX_LOCK_GENERIC
    pthread_cond_init(&readers_cond, nullptr);
#endif
  }
  void destroy()
  {
    DBUG_ASSERT(is_vacant());
    writer.destroy();
#ifdef SUX_LOCK_GENERIC
    pthread_cond_destroy(&readers_cond);
#endif
  }
  /** @return whether any writer is waiting */
  bool is_waiting() const
  { return (readers.load(std::memory_order_relaxed) & WRITER) != 0; }
#ifndef DBUG_OFF
  /** @return whether the lock is being held or waited for */
  bool is_vacant() const { return !is_locked_or_waiting(); }
#endif /* !DBUG_OFF */

  bool rd_lock_try()
  {
    uint32_t lk= 0;
    while (!readers.compare_exchange_weak(lk, lk + 1,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed))
      if (lk & WRITER)
        return false;
    return true;
  }

  bool u_lock_try()
  {
    if (!writer.wr_lock_try())
      return false;
    IF_DBUG_ASSERT(uint32_t lk=,)
    readers.fetch_add(1, std::memory_order_acquire);
    DBUG_ASSERT(lk < WRITER - 1);
    return true;
  }

  bool wr_lock_try()
  {
    if (!writer.wr_lock_try())
      return false;
    uint32_t lk= 0;
    if (readers.compare_exchange_strong(lk, WRITER,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed))
      return true;
    writer.wr_unlock();
    return false;
  }

  void rd_lock() { if (!rd_lock_try()) rd_wait(); }
  void u_lock()
  {
    writer.wr_lock();
    IF_DBUG_ASSERT(uint32_t lk=,)
    readers.fetch_add(1, std::memory_order_acquire);
    DBUG_ASSERT(lk < WRITER - 1);
  }
  void wr_lock()
  {
    writer.wr_lock();
#if defined __i386__||defined __x86_64__||defined _M_IX86||defined _M_X64
    /* On IA-32 and AMD64, this type of fetch_or() can only be implemented
    as a loop around LOCK CMPXCHG. In this particular case, setting the
    most significant bit using fetch_add() is equivalent, and is
    translated into a simple LOCK XADD. */
    static_assert(WRITER == 1U << 31, "compatibility");
    if (uint32_t lk= readers.fetch_add(WRITER, std::memory_order_acquire))
      wr_wait(lk);
#else
    if (uint32_t lk= readers.fetch_or(WRITER, std::memory_order_acquire))
      wr_wait(lk);
#endif
  }

  void u_wr_upgrade()
  {
    DBUG_ASSERT(writer.is_locked());
    uint32_t lk= readers.fetch_add(WRITER - 1, std::memory_order_acquire);
    if (lk != 1)
      wr_wait(lk - 1);
  }
  void wr_u_downgrade()
  {
    DBUG_ASSERT(writer.is_locked());
    DBUG_ASSERT(is_write_locked());
    readers.store(1, std::memory_order_release);
    /* Note: Any pending rd_lock() will not be woken up until u_unlock() */
  }

  void rd_unlock()
  {
    uint32_t lk= readers.fetch_sub(1, std::memory_order_release);
    ut_ad(~WRITER & lk);
    if (lk == WRITER + 1)
      wake();
  }
  void u_unlock()
  {
    IF_DBUG_ASSERT(uint32_t lk=,)
    readers.fetch_sub(1, std::memory_order_release);
    DBUG_ASSERT(lk);
    DBUG_ASSERT(lk < WRITER);
    writer.wr_unlock();
  }
  void wr_unlock()
  {
    DBUG_ASSERT(is_write_locked());
    readers.store(0, std::memory_order_release);
    writer.wr_unlock();
  }
  /** @return whether an exclusive lock may be held by any thread */
  bool is_write_locked() const noexcept
  { return readers.load(std::memory_order_acquire) == WRITER; }
  /** @return whether any lock may be held by any thread */
  bool is_locked() const noexcept
  { return readers.load(std::memory_order_acquire) != 0; }
  /** @return whether any lock may be held by any thread */
  bool is_locked_or_waiting() const noexcept
  { return is_locked() || writer.is_locked_or_waiting(); }

  void lock_shared() { rd_lock(); }
  void unlock_shared() { rd_unlock(); }
  void lock() { wr_lock(); }
  void unlock() { wr_unlock(); }
};

#if defined _WIN32 || defined SUX_LOCK_GENERIC
/** Slim read-write lock */
template<bool spinloop>
class srw_lock_
{
# ifdef UNIV_PFS_RWLOCK
  friend srw_lock_impl<spinloop>;
# endif
# ifdef _WIN32
  SRWLOCK lk;
# else
  rw_lock_t lk;
# endif

  void rd_wait();
  void wr_wait();
public:
  void init() { IF_WIN(,my_rwlock_init(&lk, nullptr)); }
  void destroy() { IF_WIN(,rwlock_destroy(&lk)); }
  inline void rd_lock();
  inline void wr_lock();
  bool rd_lock_try()
  { return IF_WIN(TryAcquireSRWLockShared(&lk), !rw_tryrdlock(&lk)); }
  void rd_unlock()
  { IF_WIN(ReleaseSRWLockShared(&lk), rw_unlock(&lk)); }
  bool wr_lock_try()
  { return IF_WIN(TryAcquireSRWLockExclusive(&lk), !rw_trywrlock(&lk)); }
  void wr_unlock()
  { IF_WIN(ReleaseSRWLockExclusive(&lk), rw_unlock(&lk)); }
#ifdef _WIN32
  /** @return whether any lock may be held by any thread */
  bool is_locked_or_waiting() const noexcept { return (size_t&)(lk) != 0; }
  /** @return whether any lock may be held by any thread */
  bool is_locked() const noexcept { return is_locked_or_waiting(); }
  /** @return whether an exclusive lock may be held by any thread */
  bool is_write_locked() const noexcept
  {
    // FIXME: this returns false positives for shared locks
    return is_locked();
  }

  void lock_shared() { rd_lock(); }
  void unlock_shared() { rd_unlock(); }
  void lock() { wr_lock(); }
  void unlock() { wr_unlock(); }
#endif
};

template<> void srw_lock_<true>::rd_wait();
template<> void srw_lock_<true>::wr_wait();

template<>
inline void srw_lock_<false>::rd_lock()
{ IF_WIN(AcquireSRWLockShared(&lk), rw_rdlock(&lk)); }
template<>
inline void srw_lock_<false>::wr_lock()
{ IF_WIN(AcquireSRWLockExclusive(&lk), rw_wrlock(&lk)); }

template<>
inline void srw_lock_<true>::rd_lock() { if (!rd_lock_try()) rd_wait(); }
template<>
inline void srw_lock_<true>::wr_lock() { if (!wr_lock_try()) wr_wait(); }

typedef srw_lock_<false> srw_lock_low;
typedef srw_lock_<true> srw_spin_lock_low;
#else
typedef ssux_lock_impl<false> srw_lock_low;
typedef ssux_lock_impl<true> srw_spin_lock_low;
#endif

#ifndef UNIV_PFS_RWLOCK
# define SRW_LOCK_INIT(key) init()
# define SRW_LOCK_ARGS(file, line) /* nothing */
# define SRW_LOCK_CALL /* nothing */
typedef srw_lock_low srw_lock;
typedef srw_spin_lock_low srw_spin_lock;
#else
# define SRW_LOCK_INIT(key) init(key)
# define SRW_LOCK_ARGS(file, line) file, line
# define SRW_LOCK_CALL __FILE__, __LINE__

/** Slim shared-update-exclusive lock with PERFORMANCE_SCHEMA instrumentation */
class ssux_lock
{
  PSI_rwlock *pfs_psi;
  ssux_lock_impl<false> lock;

  ATTRIBUTE_NOINLINE void psi_rd_lock(const char *file, unsigned line);
  ATTRIBUTE_NOINLINE void psi_wr_lock(const char *file, unsigned line);
  ATTRIBUTE_NOINLINE void psi_u_lock(const char *file, unsigned line);
  ATTRIBUTE_NOINLINE void psi_u_wr_upgrade(const char *file, unsigned line);
public:
  void init(mysql_pfs_key_t key)
  {
    pfs_psi= PSI_RWLOCK_CALL(init_rwlock)(key, this);
    lock.init();
  }
  void destroy()
  {
    if (psi_likely(pfs_psi != nullptr))
    {
      PSI_RWLOCK_CALL(destroy_rwlock)(pfs_psi);
      pfs_psi= nullptr;
    }
    lock.destroy();
  }
  void rd_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_rd_lock(file, line);
    else
      lock.rd_lock();
  }
  void rd_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.rd_unlock();
  }
  void u_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_u_lock(file, line);
    else
      lock.u_lock();
  }
  void u_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.u_unlock();
  }
  void wr_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_wr_lock(file, line);
    else
      lock.wr_lock();
  }
  void wr_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.wr_unlock();
  }
  void u_wr_upgrade(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_u_wr_upgrade(file, line);
    else
      lock.u_wr_upgrade();
  }
  bool rd_lock_try() { return lock.rd_lock_try(); }
  bool u_lock_try() { return lock.u_lock_try(); }
  bool wr_lock_try() { return lock.wr_lock_try(); }
  bool is_waiting() const { return lock.is_waiting(); }
};

/** Slim reader-writer lock with PERFORMANCE_SCHEMA instrumentation */
template<bool spinloop>
class srw_lock_impl
{
  PSI_rwlock *pfs_psi;
# if defined _WIN32 || defined SUX_LOCK_GENERIC
  srw_lock_<spinloop> lock;
# else
  ssux_lock_impl<spinloop> lock;
# endif

  ATTRIBUTE_NOINLINE void psi_rd_lock(const char *file, unsigned line);
  ATTRIBUTE_NOINLINE void psi_wr_lock(const char *file, unsigned line);
public:
  void init(mysql_pfs_key_t key)
  {
    pfs_psi= PSI_RWLOCK_CALL(init_rwlock)(key, this);
    lock.init();
  }
  void destroy()
  {
    if (psi_likely(pfs_psi != nullptr))
    {
      PSI_RWLOCK_CALL(destroy_rwlock)(pfs_psi);
      pfs_psi= nullptr;
    }
    lock.destroy();
  }
  void rd_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_rd_lock(file, line);
    else
      lock.rd_lock();
  }
  void rd_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.rd_unlock();
  }
  void wr_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_wr_lock(file, line);
    else
      lock.wr_lock();
  }
  void wr_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.wr_unlock();
  }
  bool rd_lock_try() { return lock.rd_lock_try(); }
  bool wr_lock_try() { return lock.wr_lock_try(); }
  void lock_shared() { return rd_lock(SRW_LOCK_CALL); }
  void unlock_shared() { return rd_unlock(); }
#ifndef SUX_LOCK_GENERIC
  /** @return whether any lock may be held by any thread */
  bool is_locked_or_waiting() const noexcept
  { return lock.is_locked_or_waiting(); }
  /** @return whether an exclusive lock may be held by any thread */
  bool is_locked() const noexcept { return lock.is_locked(); }
  /** @return whether an exclusive lock may be held by any thread */
  bool is_write_locked() const noexcept { return lock.is_write_locked(); }
#endif
};

typedef srw_lock_impl<false> srw_lock;
typedef srw_lock_impl<true> srw_spin_lock;

#endif
