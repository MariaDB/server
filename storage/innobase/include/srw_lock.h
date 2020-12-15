/*****************************************************************************

Copyright (c) 2020, MariaDB Corporation.

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

#if !(defined __linux__ || defined _WIN32 || defined __OpenBSD__)
# define SRW_LOCK_DUMMY
#elif 0 // defined SAFE_MUTEX
# define SRW_LOCK_DUMMY /* Use dummy implementation for debugging purposes */
#endif

#ifdef SRW_LOCK_DUMMY
/** An exclusive-only variant of srw_lock */
class srw_mutex
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
# define srw_mutex srw_lock_low
#endif

#include "rw_lock.h"

/** Slim shared-update-exclusive lock with no recursion */
class ssux_lock_low final : private rw_lock
{
#ifdef UNIV_PFS_RWLOCK
  friend class ssux_lock;
# if defined SRW_LOCK_DUMMY || defined _WIN32
# else
  friend class srw_lock;
# endif
#endif
#ifdef SRW_LOCK_DUMMY
  pthread_mutex_t mutex;
  pthread_cond_t cond_shared;
  pthread_cond_t cond_exclusive;
#endif
  /** @return pointer to the lock word */
  rw_lock *word() { return static_cast<rw_lock*>(this); }
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
  /** Send signal to one waiter */
  inline void writer_wake();
  /** Send signal to all waiters */
  inline void readers_wake();
public:
#ifdef SRW_LOCK_DUMMY
  void init();
  void destroy();
#else
  void init() { DBUG_ASSERT(!is_locked_or_waiting()); }
  void destroy() { DBUG_ASSERT(!is_locked_or_waiting()); }
#endif
  bool rd_lock_try() { uint32_t l; return read_trylock(l); }
  bool wr_lock_try() { return write_trylock(); }
  void rd_lock() { uint32_t l; if (!read_trylock(l)) read_lock(l); }
  void u_lock() { uint32_t l; if (!update_trylock(l)) update_lock(l); }
  bool u_lock_try() { uint32_t l; return update_trylock(l); }
  void u_wr_upgrade() { if (!upgrade_trylock()) write_lock(true); }
  void wr_lock() { if (!write_trylock()) write_lock(false); }
  void rd_unlock();
  void u_unlock();
  void wr_unlock();
  /** @return whether any writer is waiting */
  bool is_waiting() const { return value() & WRITER_WAITING; }
};

#if defined SRW_LOCK_DUMMY || defined _WIN32
/** Slim read-write lock */
class srw_lock_low
{
# ifdef UNIV_PFS_RWLOCK
  friend class srw_lock;
# endif
# ifdef _WIN32
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
# else
  rw_lock_t lock;
  void init() { my_rwlock_init(&lock, nullptr); }
  void destroy() { rwlock_destroy(&lock); }
  void rd_lock() { rw_rdlock(&lock); }
  bool rd_lock_try() { return rw_tryrdlock(&lock); }
  void rd_unlock() { rw_unlock(&lock); }
  void wr_lock() { rw_wrlock(&lock); }
  bool wr_lock_try() { return rw_trywrlock(&lock); }
  void wr_unlock() { rw_unlock(&lock); }
# endif
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
