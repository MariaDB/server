/*****************************************************************************

Copyright (c) 2020, 2021, MariaDB Corporation.

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

#ifdef SUX_LOCK_GENERIC
/** An exclusive-only variant of srw_lock */
class srw_mutex final
{
  pthread_mutex_t lock;
public:
  void init() { pthread_mutex_init(&lock, nullptr); }
  void destroy() { pthread_mutex_destroy(&lock); }
  void wr_lock() { pthread_mutex_lock(&lock); }
  void wr_unlock() { pthread_mutex_unlock(&lock); }
  bool wr_lock_try() { return !pthread_mutex_trylock(&lock); }
};
#else
/** Futex-based mutex */
class srw_mutex final
{
  /** The lock word, containing HOLDER + 1 if the lock is being held,
  plus the number of waiters */
  std::atomic<uint32_t> lock;
  /** Identifies that the lock is being held */
  static constexpr uint32_t HOLDER= 1U << 31;

  /** Wait until the mutex has been acquired */
  void wait_and_lock();
  /** Wait for lock!=lk */
  inline void wait(uint32_t lk);
  /** Wake up one wait() thread */
  void wake();
public:
  /** @return whether the mutex is being held or waited for */
  bool is_locked_or_waiting() const
  { return lock.load(std::memory_order_relaxed) != 0; }
  /** @return whether the mutex is being held by any thread */
  bool is_locked() const
  { return (lock.load(std::memory_order_relaxed) & HOLDER) != 0; }

  void init() { DBUG_ASSERT(!is_locked_or_waiting()); }
  void destroy() { DBUG_ASSERT(!is_locked_or_waiting()); }

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
#endif

/** Slim shared-update-exclusive lock with no recursion */
class ssux_lock_low final
#ifdef SUX_LOCK_GENERIC
  : private rw_lock
#endif
{
#ifdef UNIV_PFS_RWLOCK
  friend class ssux_lock;
# ifdef SUX_LOCK_GENERIC
# elif defined _WIN32
# else
  friend class srw_lock;
# endif
#endif
#ifdef SUX_LOCK_GENERIC
  pthread_mutex_t mutex;
  pthread_cond_t cond_shared;
  pthread_cond_t cond_exclusive;

  /** Wait for a read lock.
  @param l lock word from a failed read_trylock() */
  void read_lock(uint32_t l);
  /** Wait for an update lock.
  @param l lock word from a failed update_trylock() */
  void update_lock(uint32_t l);
  /** Wait for a write lock after a failed write_trylock() or upgrade_trylock()
  @param holding_u  whether we already hold u_lock() */
  void write_lock(bool holding_u);
  /** Wait for signal
  @param l lock word from a failed acquisition */
  inline void writer_wait(uint32_t l);
  /** Wait for signal
  @param l lock word from a failed acquisition */
  inline void readers_wait(uint32_t l);
  /** Wake waiters */
  inline void wake();
public:
  void init();
  void destroy();
  /** @return whether any writer is waiting */
  bool is_waiting() const { return (value() & WRITER_WAITING) != 0; }

  bool rd_lock_try() { uint32_t l; return read_trylock(l); }
  bool wr_lock_try() { return write_trylock(); }
  void rd_lock() { uint32_t l; if (!read_trylock(l)) read_lock(l); }
  void u_lock() { uint32_t l; if (!update_trylock(l)) update_lock(l); }
  bool u_lock_try() { uint32_t l; return update_trylock(l); }
  void u_wr_upgrade() { if (!upgrade_trylock()) write_lock(true); }
  void wr_u_downgrade() { downgrade(); }
  void wr_lock() { if (!write_trylock()) write_lock(false); }
  void rd_unlock();
  void u_unlock();
  void wr_unlock();
#else
  /** mutex for synchronization; held by U or X lock holders */
  srw_mutex writer;
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
  void init() { DBUG_ASSERT(is_vacant()); }
  void destroy() { DBUG_ASSERT(is_vacant()); }
  /** @return whether any writer is waiting */
  bool is_waiting() const
  { return (readers.load(std::memory_order_relaxed) & WRITER) != 0; }
# ifndef DBUG_OFF
  /** @return whether the lock is being held or waited for */
  bool is_vacant() const
  {
    return !readers.load(std::memory_order_relaxed) &&
      !writer.is_locked_or_waiting();
  }
# endif /* !DBUG_OFF */

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
    if (uint32_t lk= readers.fetch_or(WRITER, std::memory_order_acquire))
      wr_wait(lk);
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
    DBUG_ASSERT(readers.load(std::memory_order_relaxed) == WRITER);
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
    DBUG_ASSERT(readers.load(std::memory_order_relaxed) == WRITER);
    readers.store(0, std::memory_order_release);
    writer.wr_unlock();
  }
#endif
};

#ifdef _WIN32
/** Slim read-write lock */
class srw_lock_low
{
# ifdef UNIV_PFS_RWLOCK
  friend class srw_lock;
# endif
  SRWLOCK lock;
public:
  void init() {}
  void destroy() {}
  void rd_lock() { AcquireSRWLockShared(&lock); }
  bool rd_lock_try() { return TryAcquireSRWLockShared(&lock); }
  void rd_unlock() { ReleaseSRWLockShared(&lock); }
  void wr_lock() { AcquireSRWLockExclusive(&lock); }
  bool wr_lock_try() { return TryAcquireSRWLockExclusive(&lock); }
  void wr_unlock() { ReleaseSRWLockExclusive(&lock); }
};
#elif defined SUX_LOCK_GENERIC
/** Slim read-write lock */
class srw_lock_low
{
# ifdef UNIV_PFS_RWLOCK
  friend class srw_lock;
# endif
  rw_lock_t lock;
public:
  void init() { my_rwlock_init(&lock, nullptr); }
  void destroy() { rwlock_destroy(&lock); }
  void rd_lock() { rw_rdlock(&lock); }
  bool rd_lock_try() { return !rw_tryrdlock(&lock); }
  void rd_unlock() { rw_unlock(&lock); }
  void wr_lock() { rw_wrlock(&lock); }
  bool wr_lock_try() { return !rw_trywrlock(&lock); }
  void wr_unlock() { rw_unlock(&lock); }
};
#else
typedef ssux_lock_low srw_lock_low;
#endif

#ifndef UNIV_PFS_RWLOCK
# define SRW_LOCK_INIT(key) init()
# define SRW_LOCK_ARGS(file, line) /* nothing */
# define SRW_LOCK_CALL /* nothing */
typedef srw_lock_low srw_lock;
typedef ssux_lock_low ssux_lock;
#else
# define SRW_LOCK_INIT(key) init(key)
# define SRW_LOCK_ARGS(file, line) file, line
# define SRW_LOCK_CALL __FILE__, __LINE__

/** Slim shared-update-exclusive lock with PERFORMANCE_SCHEMA instrumentation */
class ssux_lock
{
  PSI_rwlock *pfs_psi;
  ssux_lock_low lock;

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
class srw_lock
{
  PSI_rwlock *pfs_psi;
  srw_lock_low lock;

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
};
#endif
