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
  template<bool update=false> void wr_lock() { pthread_mutex_lock(&lock); }
  void wr_unlock() { pthread_mutex_unlock(&lock); }
  bool wr_lock_try() { return !pthread_mutex_trylock(&lock); }
};
#else
# define srw_mutex srw_lock_low
#endif

#include "rw_lock.h"

/** Slim reader-writer lock with no recursion */
class srw_lock_low final : private rw_lock
{
#ifdef SRW_LOCK_DUMMY
  pthread_mutex_t mutex;
  pthread_cond_t cond;
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
  template<bool update=false>
  void rd_lock() { uint32_t l; if (!read_trylock(l)) read_lock(l); }
  void u_lock() { uint32_t l; if (!update_trylock(l)) update_lock(l); }
  bool u_lock_try() { uint32_t l; return update_trylock(l); }
  void u_wr_upgrade() { if (!upgrade_trylock()) write_lock(true); }
  template<bool update=false>
  void wr_lock() { if (!write_trylock()) write_lock(false); }
  void rd_unlock();
  void u_unlock();
  void wr_unlock();
};

#ifndef UNIV_PFS_RWLOCK
# define SRW_LOCK_INIT(key) init()
# define SRW_LOCK_ARGS(file, line) /* nothing */
# define SRW_LOCK_CALL /* nothing */
typedef srw_lock_low srw_lock;
#else
# define SRW_LOCK_INIT(key) init(key)
# define SRW_LOCK_ARGS(file, line) file, line
# define SRW_LOCK_CALL __FILE__, __LINE__

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
  template<bool update= false>
  void rd_lock(const char *file, unsigned line)
  {
    if (pfs_psi)
    {
      PSI_rwlock_locker_state state;
      PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_rdwait)
        (&state, pfs_psi, update ? PSI_RWLOCK_SHAREDLOCK : PSI_RWLOCK_READLOCK,
         file, line);
      lock.rd_lock();
      if (locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
      return;
    }
    lock.rd_lock();
  }
  void rd_unlock()
  {
    if (pfs_psi)
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.rd_unlock();
  }
  void u_lock(const char *file, unsigned line)
  {
    if (pfs_psi)
    {
      PSI_rwlock_locker_state state;
      PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
        (&state, pfs_psi, PSI_RWLOCK_SHAREDEXCLUSIVELOCK, file, line);
      lock.u_lock();
      if (locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
      return;
    }
    lock.u_lock();
  }
  void u_unlock()
  {
    if (pfs_psi)
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.u_unlock();
  }
  template<bool update= false>
  void wr_lock(const char *file, unsigned line)
  {
    if (pfs_psi)
    {
      PSI_rwlock_locker_state state;
      PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
        (&state, pfs_psi,
         update ? PSI_RWLOCK_EXCLUSIVELOCK : PSI_RWLOCK_WRITELOCK,
         file, line);
      lock.wr_lock();
      if (locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
      return;
    }
    lock.wr_lock();
  }
  void wr_unlock()
  {
    if (pfs_psi)
      PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
    lock.wr_unlock();
  }
  void u_wr_upgrade(const char *file, unsigned line)
  {
    if (pfs_psi)
    {
      PSI_rwlock_locker_state state;
      PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
        (&state, pfs_psi, PSI_RWLOCK_WRITELOCK, file, line);
      lock.u_wr_upgrade();
      if (locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
      return;
    }
    lock.u_wr_upgrade();
  }
  bool rd_lock_try() { return lock.rd_lock_try(); }
  bool u_lock_try() { return lock.u_lock_try(); }
  bool wr_lock_try() { return lock.wr_lock_try(); }
};
#endif
