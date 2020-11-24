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

#if 0 // defined SAFE_MUTEX
# define SRW_LOCK_DUMMY /* Use mysql_rwlock_t for debugging purposes */
#endif

#if defined SRW_LOCK_DUMMY || (!defined _WIN32 && !defined __linux__)
#else
# ifdef _WIN32
#  include <windows.h>
# else
#  include "rw_lock.h"
# endif
#endif

class srw_lock final
#if defined __linux__ && !defined SRW_LOCK_DUMMY
  : protected rw_lock
#endif
{
#if defined SRW_LOCK_DUMMY || (!defined _WIN32 && !defined __linux__)
  mysql_rwlock_t lock;
public:
  void init(mysql_pfs_key_t key) { mysql_rwlock_init(key, &lock); }
  void destroy() { mysql_rwlock_destroy(&lock); }
  void rd_lock() { mysql_rwlock_rdlock(&lock); }
  void rd_unlock() { mysql_rwlock_unlock(&lock); }
  void wr_lock() { mysql_rwlock_wrlock(&lock); }
  void wr_unlock() { mysql_rwlock_unlock(&lock); }
#else
# ifdef UNIV_PFS_RWLOCK
  PSI_rwlock *pfs_psi;
# endif
# ifdef _WIN32
  SRWLOCK lock;
  bool read_trylock() { return TryAcquireSRWLockShared(&lock); }
  bool write_trylock() { return TryAcquireSRWLockExclusive(&lock); }
  void read_lock() { AcquireSRWLockShared(&lock); }
  void write_lock() { AcquireSRWLockExclusive(&lock); }
# else
  /** @return pointer to the lock word */
  rw_lock *word() { return static_cast<rw_lock*>(this); }
  /** Wait for a read lock.
  @param l lock word from a failed read_trylock() */
  void read_lock(uint32_t l);
  /** Wait for a write lock after a failed write_trylock() */
  void write_lock();
# endif

public:
  void init(mysql_pfs_key_t key)
  {
# ifdef UNIV_PFS_RWLOCK
    pfs_psi= PSI_RWLOCK_CALL(init_rwlock)(key, this);
# endif
    IF_WIN(lock= SRWLOCK_INIT, static_assert(4 == sizeof(rw_lock), "ABI"));
  }
  void destroy()
  {
# ifdef UNIV_PFS_RWLOCK
    if (pfs_psi)
    {
      PSI_RWLOCK_CALL(destroy_rwlock)(pfs_psi);
      pfs_psi= nullptr;
    }
# endif
    DBUG_ASSERT(!is_locked_or_waiting());
  }
  void rd_lock()
  {
    IF_WIN(, uint32_t l);
# ifdef UNIV_PFS_RWLOCK
    if (read_trylock(IF_WIN(, l)))
      return;
    if (pfs_psi)
    {
      PSI_rwlock_locker_state state;
      PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_rdwait)
        (&state, pfs_psi, PSI_RWLOCK_READLOCK, __FILE__, __LINE__);
      read_lock(IF_WIN(, l));
      if (locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
      return;
    }
# endif /* UNIV_PFS_RWLOCK */
    IF_WIN(read_lock(), if (!read_trylock(l)) read_lock(l));
  }
  void wr_lock()
  {
# ifdef UNIV_PFS_RWLOCK
    if (write_trylock())
      return;
    if (pfs_psi)
    {
      PSI_rwlock_locker_state state;
      PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
        (&state, pfs_psi, PSI_RWLOCK_WRITELOCK, __FILE__, __LINE__);
      write_lock();
      if (locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
      return;
    }
# endif /* UNIV_PFS_RWLOCK */
    IF_WIN(, if (!write_trylock())) write_lock();
  }
#ifdef _WIN32
  void rd_unlock()
  {
#ifdef UNIV_PFS_RWLOCK
    if (pfs_psi) PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
#endif
    ReleaseSRWLockShared(&lock);
  }
  void wr_unlock()
  {
#ifdef UNIV_PFS_RWLOCK
    if (pfs_psi) PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
#endif
    ReleaseSRWLockExclusive(&lock);
  }
#else
  void rd_unlock();
  void wr_unlock();
#endif
#endif
};
