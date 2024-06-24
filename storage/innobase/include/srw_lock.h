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
class pthread_mutex_wrapper final
{
  pthread_mutex_t lock;
public:
  template<bool spinloop=false> void init()
  {
    if (spinloop)
      pthread_mutex_init(&lock, MY_MUTEX_INIT_FAST);
    else
      pthread_mutex_init(&lock, nullptr);
  }
  void destroy() { pthread_mutex_destroy(&lock); }
# ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
  template<bool spinloop=false> void wr_lock() { pthread_mutex_lock(&lock); }
# else
private:
  void wr_wait();
public:
  template<bool spinloop=false> inline void wr_lock();
# endif
  void wr_unlock() { pthread_mutex_unlock(&lock); }
  bool wr_lock_try() { return !pthread_mutex_trylock(&lock); }
};

# ifndef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
template<> inline void pthread_mutex_wrapper::wr_lock<false>()
{ pthread_mutex_lock(&lock); }
template<> inline void pthread_mutex_wrapper::wr_lock<true>()
{ if (!wr_lock_try()) wr_wait(); }
# endif
#endif

class ssux_lock_impl;

/** Futex-based mutex */
class srw_mutex_impl final
{
  friend ssux_lock_impl;
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
  template<bool spinloop>
  void wait_and_lock();
  /** Wait for lock!=lk */
  inline void wait(uint32_t lk);
  /** Wake up one wait() thread */
  void wake();
  /** Wake up all wait() threads */
  inline void wake_all();
public:
  /** @return whether the mutex is being held or waited for */
  bool is_locked_or_waiting() const
  { return lock.load(std::memory_order_acquire) != 0; }
  /** @return whether the mutex is being held by any thread */
  bool is_locked() const
  { return (lock.load(std::memory_order_acquire) & HOLDER) != 0; }

  template<bool spinloop=false> void init()
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

  template<bool spinloop=false>
  void wr_lock() { if (!wr_lock_try()) wait_and_lock<spinloop>(); }
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
typedef pthread_mutex_wrapper srw_mutex;
#else
typedef srw_mutex_impl srw_mutex;
#endif

class srw_lock_psi;

/** Slim shared-update-exclusive lock with no recursion */
class ssux_lock_impl
{
#ifdef UNIV_PFS_RWLOCK
  friend class ssux_lock;
# ifdef SUX_LOCK_GENERIC
# elif defined _WIN32
# else
  friend srw_lock_psi;
# endif
#endif
#ifdef SUX_LOCK_GENERIC
  /** mutex for synchronization; held by U or X lock holders */
  srw_mutex_impl writer;
  /** Condition variable for "readers"; used with writer.mutex. */
  pthread_cond_t readers_cond;
#else
  /** mutex for synchronization; held by U or X lock holders */
  srw_mutex_impl writer;
#endif
  /** S or U holders, and WRITER flag for X holder or waiter */
  std::atomic<uint32_t> readers;
  /** indicates an X request; readers=WRITER indicates granted X lock */
  static constexpr uint32_t WRITER= 1U << 31;

  /** Wait for readers!=lk */
  inline void wait(uint32_t lk);

  /** Wait for readers!=lk|WRITER */
  template<bool spinloop> void wr_wait(uint32_t lk);
  /** Wake up wait() on the last rd_unlock() */
  void wake();
  /** Acquire a read lock */
  template<bool spinloop> void rd_wait();
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
private:
  /** Try to acquire a shared latch.
  @return the lock word value if the latch was not acquired
  @retval 0  if the latch was acquired */
  uint32_t rd_lock_try_low()
  {
    uint32_t lk= 0;
    while (!readers.compare_exchange_weak(lk, lk + 1,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed))
      if (lk & WRITER)
        return lk;
    return 0;
  }
public:

  bool rd_lock_try() { return rd_lock_try_low() == 0; }

  bool u_lock_try() { return writer.wr_lock_try(); }

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

  template<bool spinloop=false> void rd_lock()
  { if (!rd_lock_try()) rd_wait<spinloop>(); }
  template<bool spinloop> void u_lock() { writer.wr_lock<spinloop>(); }
  template<bool spin_writer,bool spin_readers> void wr_lock()
  {
    writer.wr_lock<spin_writer>();
#if defined __i386__||defined __x86_64__||defined _M_IX86||defined _M_X64
    /* On IA-32 and AMD64, this type of fetch_or() can only be implemented
    as a loop around LOCK CMPXCHG. In this particular case, setting the
    most significant bit using fetch_add() is equivalent, and is
    translated into a simple LOCK XADD. */
    static_assert(WRITER == 1U << 31, "compatibility");
    if (uint32_t lk= readers.fetch_add(WRITER, std::memory_order_acquire))
      wr_wait<spin_readers>(lk);
#else
    if (uint32_t lk= readers.fetch_or(WRITER, std::memory_order_acquire))
      wr_wait<spin_readers>(lk);
#endif
  }
  template<bool spinloop=false> void wr_lock()
  { wr_lock<spinloop,spinloop>(); }

  template<bool spinloop> void u_wr_upgrade()
  {
    DBUG_ASSERT(writer.is_locked());
    uint32_t lk= readers.fetch_add(WRITER, std::memory_order_acquire);
    if (lk)
      wr_wait<spinloop>(lk);
  }
  void wr_u_downgrade()
  {
    DBUG_ASSERT(writer.is_locked());
    DBUG_ASSERT(is_write_locked());
    readers.store(0, std::memory_order_release);
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

  template<bool spinloop=false> void lock_shared() { rd_lock<spinloop>(); }
  void unlock_shared() { rd_unlock(); }
  template<bool spinloop=false> void lock() { wr_lock<spinloop>(); }
  void unlock() { wr_unlock(); }
};

#if defined _WIN32 || defined SUX_LOCK_GENERIC
/** Slim read-write lock */
class srw_lock_
{
# ifdef UNIV_PFS_RWLOCK
  friend srw_lock_psi;
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
  template<bool spinloop=false> inline void rd_lock();
  template<bool spinloop=false> inline void wr_lock();
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

  template<bool spinloop=false> void lock_shared() { rd_lock<spinloop>(); }
  void unlock_shared() { rd_unlock(); }
  template<bool spinloop=false> void lock() { wr_lock<spinloop>(); }
  void unlock() { wr_unlock(); }
#endif
};

template<> inline void srw_lock_::rd_lock<false>()
{ IF_WIN(AcquireSRWLockShared(&lk), rw_rdlock(&lk)); }
template<> inline void srw_lock_::wr_lock<false>()
{ IF_WIN(AcquireSRWLockExclusive(&lk), rw_wrlock(&lk)); }

template<>
inline void srw_lock_::rd_lock<true>() { if (!rd_lock_try()) rd_wait(); }
template<>
inline void srw_lock_::wr_lock<true>() { if (!wr_lock_try()) wr_wait(); }

typedef srw_lock_ srw_lock_low;
#else
typedef ssux_lock_impl srw_lock_low;
#endif

#ifndef UNIV_PFS_RWLOCK
# define SRW_LOCK_INIT(key) init()
# define SRW_LOCK_ARGS(file, line) /* nothing */
# define SRW_LOCK_CALL /* nothing */
typedef srw_lock_low srw_lock;
#else
# define SRW_LOCK_INIT(key) init(key)
# define SRW_LOCK_ARGS(file, line) file, line
# define SRW_LOCK_CALL __FILE__, __LINE__

/** Slim shared-update-exclusive lock with PERFORMANCE_SCHEMA instrumentation */
class ssux_lock
{
  PSI_rwlock *pfs_psi;
  ssux_lock_impl lock;

  template<bool spinloop>
  ATTRIBUTE_NOINLINE void psi_rd_lock(const char *file, unsigned line);
  template<bool spin_writer,bool spin_readers>
  ATTRIBUTE_NOINLINE void psi_wr_lock(const char *file, unsigned line);
  template<bool spinloop>
  ATTRIBUTE_NOINLINE void psi_u_lock(const char *file, unsigned line);
  template<bool spinloop>
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
  template<bool spinloop>
  void rd_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_rd_lock<spinloop>(file, line);
    else
      lock.rd_lock<spinloop>();
  }
  void rd_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.rd_unlock();
  }
  template<bool spinloop>
  void u_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_u_lock<spinloop>(file, line);
    else
      lock.u_lock<spinloop>();
  }
  void u_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.u_unlock();
  }
  template<bool spin_writer,bool spin_readers>
  void wr_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_wr_lock<spin_writer,spin_readers>(file, line);
    else
      lock.wr_lock<spin_writer,spin_readers>();
  }
  void wr_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.wr_unlock();
  }
  template<bool spinloop>
  void u_wr_upgrade(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_u_wr_upgrade<spinloop>(file, line);
    else
      lock.u_wr_upgrade<spinloop>();
  }
  bool rd_lock_try() { return lock.rd_lock_try(); }
  bool u_lock_try() { return lock.u_lock_try(); }
  bool wr_lock_try() { return lock.wr_lock_try(); }
  bool is_waiting() const { return lock.is_waiting(); }
};

/** Slim reader-writer lock with PERFORMANCE_SCHEMA instrumentation */
class srw_lock_psi
{
  PSI_rwlock *pfs_psi;
# if defined _WIN32 || defined SUX_LOCK_GENERIC
  srw_lock_ lock;
# else
  ssux_lock_impl lock;
# endif

  template<bool spinloop>
  ATTRIBUTE_NOINLINE void psi_rd_lock(const char *file, unsigned line);
  template<bool spinloop>
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
  template<bool spinloop=false>
  void rd_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_rd_lock<spinloop>(file, line);
    else
      lock.rd_lock<spinloop>();
  }
  void rd_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.rd_unlock();
  }
  template<bool spinloop=false>
  void wr_lock(const char *file, unsigned line)
  {
    if (psi_likely(pfs_psi != nullptr))
      psi_wr_lock<spinloop>(file, line);
    else
      lock.wr_lock<spinloop>();
  }
  void wr_unlock()
  {
    if (psi_likely(pfs_psi != nullptr))
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.wr_unlock();
  }
  bool rd_lock_try() { return lock.rd_lock_try(); }
  bool wr_lock_try() { return lock.wr_lock_try(); }
  template<bool spinloop>
  void lock_shared() { return rd_lock<spinloop>(SRW_LOCK_CALL); }
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

typedef srw_lock_psi srw_lock;

#endif

#ifdef UNIV_DEBUG
# include <unordered_set>

class srw_lock_debug : private srw_lock
{
  /** The owner of the exclusive lock (0 if none) */
  std::atomic<pthread_t> writer;
  /** Protects readers */
  mutable srw_mutex readers_lock;
  /** Threads that hold the lock in shared mode */
  std::atomic<std::unordered_multiset<pthread_t>*> readers;

  /** Register a read lock. */
  void readers_register();

public:
  void SRW_LOCK_INIT(mysql_pfs_key_t key);
  void destroy();

#ifndef SUX_LOCK_GENERIC
  /** @return whether any lock may be held by any thread */
  bool is_locked_or_waiting() const noexcept
  { return srw_lock::is_locked_or_waiting(); }
  /** @return whether an exclusive lock may be held by any thread */
  bool is_write_locked() const noexcept { return srw_lock::is_write_locked(); }
#endif

  /** Acquire an exclusive lock */
  void wr_lock(SRW_LOCK_ARGS(const char *file, unsigned line));
  /** @return whether an exclusive lock was acquired */
  bool wr_lock_try();
  /** Release after wr_lock() */
  void wr_unlock();
  /** Acquire a shared lock */
  void rd_lock(SRW_LOCK_ARGS(const char *file, unsigned line));
  /** @return whether a shared lock was acquired */
  bool rd_lock_try();
  /** Release after rd_lock() */
  void rd_unlock();
  /** @return whether this thread is between rd_lock() and rd_unlock() */
  bool have_rd() const noexcept;
  /** @return whether this thread is between wr_lock() and wr_unlock() */
  bool have_wr() const noexcept;
  /** @return whether this thread is holding rd_lock() or wr_lock() */
  bool have_any() const noexcept;
};
#endif
