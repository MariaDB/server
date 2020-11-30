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

#include "rw_lock.h"

/** Slim reader-writer lock with no recursion */
class srw_lock_low final : private rw_lock
{
#ifdef UNIV_PFS_RWLOCK
  friend class srw_lock;
#endif
#ifdef SRW_LOCK_DUMMY
  pthread_mutex_t mutex;
  pthread_cond_t cond;
#endif
  /** @return pointer to the lock word */
  rw_lock *word() { return static_cast<rw_lock*>(this); }
  /** Wait for a read lock.
  @param l lock word from a failed read_trylock() */
  void read_lock(uint32_t l);
  /** Wait for a write lock after a failed write_trylock() */
  void write_lock();
  /** Wait for signal
  @param l lock word from a failed acquisition */
  inline void wait(uint32_t l);
  /** Send signal to one waiter */
  inline void wake_one();
  /** Send signal to all waiters */
  inline void wake_all();
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
  void wr_lock() { if (!write_trylock()) write_lock(); }
  void rd_unlock();
  void wr_unlock();
};

#ifndef UNIV_PFS_RWLOCK
# define SRW_LOCK_INIT(key) init()
typedef srw_lock_low srw_lock;
#else
# define SRW_LOCK_INIT(key) init(key)

/** Slim reader-writer lock with PERFORMANCE_SCHEMA instrumentation */
class srw_lock
{
  srw_lock_low lock;
  PSI_rwlock *pfs_psi;

public:
  void init(mysql_pfs_key_t key)
  {
    lock.init();
    pfs_psi= PSI_RWLOCK_CALL(init_rwlock)(key, this);
  }
  void destroy()
  {
    if (pfs_psi)
    {
      PSI_RWLOCK_CALL(destroy_rwlock)(pfs_psi);
      pfs_psi= nullptr;
    }
    lock.destroy();
  }
  void rd_lock()
  {
    uint32_t l;
    if (lock.read_trylock(l))
      return;
    if (pfs_psi)
    {
      PSI_rwlock_locker_state state;
      PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_rdwait)
        (&state, pfs_psi, PSI_RWLOCK_READLOCK, __FILE__, __LINE__);
      lock.read_lock(l);
      if (locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
      return;
    }
    lock.read_lock(l);
  }
  void rd_unlock()
  {
    if (pfs_psi)
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.rd_unlock();
  }
  void wr_lock()
  {
    if (lock.write_trylock())
      return;
    if (pfs_psi)
    {
      PSI_rwlock_locker_state state;
      PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
        (&state, pfs_psi, PSI_RWLOCK_WRITELOCK, __FILE__, __LINE__);
      lock.write_lock();
      if (locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
      return;
    }
    lock.write_lock();
  }
  void wr_unlock()
  {
    if (pfs_psi)
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.wr_unlock();
  }
  bool rd_lock_try() { return lock.rd_lock_try(); }
  bool wr_lock_try() { return lock.wr_lock_try(); }
};
#endif
