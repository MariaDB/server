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
#include "srw_lock.h"
#include "my_atomic_wrapper.h"
#ifdef UNIV_DEBUG
# include <unordered_set>
#endif

/** A "fat" rw-lock that supports
S (shared), U (update, or shared-exclusive), and X (exclusive) modes
as well as recursive U and X latch acquisition
@tparam ssux ssux_lock_impl or ssux_lock */
template<typename ssux>
class sux_lock final
{
  /** The underlying non-recursive lock */
  ssux lock;
  /** Numbers of U and X locks. Protected by lock. */
  uint32_t recursive;
  /** The owner of the U or X lock (0 if none); protected by lock */
  std::atomic<pthread_t> writer;
  /** Special writer!=0 value to indicate that the lock is non-recursive
  and will be released by an I/O thread */
#if defined __linux__ || defined _WIN32
  static constexpr pthread_t FOR_IO= pthread_t(~0UL);
#else
# define FOR_IO ((pthread_t) ~0UL) /* it could be a pointer */
#endif
#ifdef UNIV_DEBUG
  /** Protects readers */
  mutable srw_mutex readers_lock;
  /** Threads that hold the lock in shared mode */
  std::atomic<std::unordered_multiset<pthread_t>*> readers;
#endif

  /** The multiplier in recursive for X locks */
  static constexpr uint32_t RECURSIVE_X= 1U;
  /** The multiplier in recursive for U locks */
  static constexpr uint32_t RECURSIVE_U= 1U << 16;
  /** The maximum allowed level of recursion */
  static constexpr uint32_t RECURSIVE_MAX= RECURSIVE_U - 1;

public:
#ifdef UNIV_PFS_RWLOCK
  inline void init();
#endif
  void SRW_LOCK_INIT(mysql_pfs_key_t key)
  {
    lock.SRW_LOCK_INIT(key);
    ut_ad(!writer.load(std::memory_order_relaxed));
    ut_ad(!recursive);
    ut_d(readers_lock.init());
#ifdef UNIV_DEBUG
    if (auto r= readers.load(std::memory_order_relaxed))
      ut_ad(r->empty());
#endif
  }

  /** Free the rw-lock after init() */
  void free()
  {
    ut_ad(!writer.load(std::memory_order_relaxed));
    ut_ad(!recursive);
#ifdef UNIV_DEBUG
    readers_lock.destroy();
    if (auto r= readers.load(std::memory_order_relaxed))
    {
      ut_ad(r->empty());
      delete r;
      readers.store(nullptr, std::memory_order_relaxed);
    }
#endif
    lock.destroy();
  }

  /** needed for dict_index_t::clone() */
  inline void operator=(const sux_lock&);

#ifdef UNIV_DEBUG
  /** @return whether no recursive locks are being held */
  bool not_recursive() const
  {
    ut_ad(recursive);
    return recursive == RECURSIVE_X || recursive == RECURSIVE_U;
  }

  /** @return the number of X locks being held (by any thread) */
  unsigned x_lock_count() const { return recursive & RECURSIVE_MAX; }
#endif

  /** Acquire a recursive lock */
  template<bool allow_readers> void writer_recurse()
  {
    ut_ad(writer == pthread_self());
    ut_d(auto rec= (recursive / (allow_readers ? RECURSIVE_U : RECURSIVE_X)) &
         RECURSIVE_MAX);
    ut_ad(allow_readers ? recursive : rec);
    ut_ad(rec < RECURSIVE_MAX);
    recursive+= allow_readers ? RECURSIVE_U : RECURSIVE_X;
  }

private:
  /** Transfer the ownership of a write lock to another thread
  @param id the new owner of the U or X lock */
  void set_new_owner(pthread_t id)
  {
    IF_DBUG(DBUG_ASSERT(writer.exchange(id, std::memory_order_relaxed)),
            writer.store(id, std::memory_order_relaxed));
  }
  /** Assign the ownership of a write lock to a thread
  @param id the owner of the U or X lock */
  void set_first_owner(pthread_t id)
  {
    IF_DBUG(DBUG_ASSERT(!writer.exchange(id, std::memory_order_relaxed)),
            writer.store(id, std::memory_order_relaxed));
  }
#ifdef UNIV_DEBUG
  /** Register the current thread as a holder of a shared lock */
  void s_lock_register()
  {
    const pthread_t id= pthread_self();
    readers_lock.wr_lock();
    auto r= readers.load(std::memory_order_relaxed);
    if (!r)
    {
      r= new std::unordered_multiset<pthread_t>();
      readers.store(r, std::memory_order_relaxed);
    }
    r->emplace(id);
    readers_lock.wr_unlock();
  }
#endif

public:
  /** In crash recovery or the change buffer, claim the ownership
  of the exclusive block lock to the current thread */
  void claim_ownership() { set_new_owner(pthread_self()); }

  /** @return whether the current thread is holding X or U latch */
  bool have_u_or_x() const
  {
    if (pthread_self() != writer.load(std::memory_order_relaxed))
      return false;
    ut_ad(recursive);
    return true;
  }
  /** @return whether the current thread is holding U but not X latch */
  bool have_u_not_x() const
  { return have_u_or_x() && !((recursive / RECURSIVE_X) & RECURSIVE_MAX); }
  /** @return whether the current thread is holding X latch */
  bool have_x() const
  { return have_u_or_x() && ((recursive / RECURSIVE_X) & RECURSIVE_MAX); }
#ifdef UNIV_DEBUG
  /** @return whether the current thread is holding S latch */
  bool have_s() const
  {
    if (auto r= readers.load(std::memory_order_relaxed))
    {
      readers_lock.wr_lock();
      bool found= r->find(pthread_self()) != r->end();
      readers_lock.wr_unlock();
      return found;
    }
    return false;
  }
  /** @return whether the current thread is holding the latch */
  bool have_any() const { return have_u_or_x() || have_s(); }
#endif

  /** Acquire a shared lock */
  inline void s_lock();
  inline void s_lock(const char *file, unsigned line);
  /** Acquire an update lock */
  inline void u_lock();
  inline void u_lock(const char *file, unsigned line);
  /** Acquire an exclusive lock */
  inline void x_lock(bool for_io= false);
  inline void x_lock(const char *file, unsigned line);
  /** Acquire a recursive exclusive lock */
  void x_lock_recursive() { writer_recurse<false>(); }
  /** Upgrade an update lock */
  inline void u_x_upgrade();
  inline void u_x_upgrade(const char *file, unsigned line);
  /** Downgrade a single exclusive lock to an update lock */
  void x_u_downgrade()
  {
    ut_ad(have_u_or_x());
    ut_ad(recursive <= RECURSIVE_MAX);
    recursive*= RECURSIVE_U;
    lock.wr_u_downgrade();
  }

  /** Acquire an exclusive lock or upgrade an update lock
  @return whether U locks were upgraded to X */
  inline bool x_lock_upgraded();

  /** @return whether a shared lock was acquired */
  bool s_lock_try()
  {
    bool acquired= lock.rd_lock_try();
    ut_d(if (acquired) s_lock_register());
    return acquired;
  }

  /** Try to acquire an update lock
  @param for_io  whether the lock will be released by another thread
  @return whether the update lock was acquired */
  inline bool u_lock_try(bool for_io);

  /** Try to acquire an exclusive lock
  @return whether an exclusive lock was acquired */
  inline bool x_lock_try();

  /** Release a shared lock */
  void s_unlock()
  {
#ifdef UNIV_DEBUG
    const pthread_t id= pthread_self();
    auto r= readers.load(std::memory_order_relaxed);
    ut_ad(r);
    readers_lock.wr_lock();
    auto i= r->find(id);
    ut_ad(i != r->end());
    r->erase(i);
    readers_lock.wr_unlock();
#endif
    lock.rd_unlock();
  }
  /** Release an update or exclusive lock
  @param allow_readers    whether we are releasing a U lock
  @param claim_ownership  whether the lock was acquired by another thread */
  void u_or_x_unlock(bool allow_readers, bool claim_ownership= false)
  {
    ut_d(auto owner= writer.load(std::memory_order_relaxed));
    ut_ad(owner == pthread_self() ||
          (owner == FOR_IO && claim_ownership &&
           recursive == (allow_readers ? RECURSIVE_U : RECURSIVE_X)));
    ut_d(auto rec= (recursive / (allow_readers ? RECURSIVE_U : RECURSIVE_X)) &
         RECURSIVE_MAX);
    ut_ad(rec);
    if (!(recursive-= allow_readers ? RECURSIVE_U : RECURSIVE_X))
    {
      set_new_owner(0);
      if (allow_readers)
        lock.u_unlock();
      else
        lock.wr_unlock();
    }
  }
  /** Release an update lock */
  void u_unlock(bool claim_ownership= false)
  { u_or_x_unlock(true, claim_ownership); }
  /** Release an exclusive lock */
  void x_unlock(bool claim_ownership= false)
  { u_or_x_unlock(false, claim_ownership); }

  /** @return whether any writer is waiting */
  bool is_waiting() const { return lock.is_waiting(); }

  bool is_write_locked() const { return lock.is_write_locked(); }

  bool is_locked_or_waiting() const { return lock.is_locked_or_waiting(); }

  inline void lock_shared();
  inline void unlock_shared();
};

typedef sux_lock<ssux_lock_impl<true>> block_lock;

#ifndef UNIV_PFS_RWLOCK
typedef sux_lock<ssux_lock_impl<false>> index_lock;
#else
typedef sux_lock<ssux_lock> index_lock;

template<> inline void sux_lock<ssux_lock_impl<true>>::init()
{
  lock.init();
  ut_ad(!writer.load(std::memory_order_relaxed));
  ut_ad(!recursive);
  ut_d(readers_lock.init());
#ifdef UNIV_DEBUG
  if (auto r= readers.load(std::memory_order_relaxed))
    ut_ad(r->empty());
#endif
}

template<>
inline void sux_lock<ssux_lock>::s_lock(const char *file, unsigned line)
{
  ut_ad(!have_x());
  ut_ad(!have_s());
  lock.rd_lock(file, line);
  ut_d(s_lock_register());
}

template<>
inline void sux_lock<ssux_lock>::u_lock(const char *file, unsigned line)
{
  pthread_t id= pthread_self();
  if (writer.load(std::memory_order_relaxed) == id)
    writer_recurse<true>();
  else
  {
    lock.u_lock(file, line);
    ut_ad(!recursive);
    recursive= RECURSIVE_U;
    set_first_owner(id);
  }
}

template<>
inline void sux_lock<ssux_lock>::x_lock(const char *file, unsigned line)
{
  pthread_t id= pthread_self();
  if (writer.load(std::memory_order_relaxed) == id)
    writer_recurse<false>();
  else
  {
    lock.wr_lock(file, line);
    ut_ad(!recursive);
    recursive= RECURSIVE_X;
    set_first_owner(id);
  }
}

template<>
inline void sux_lock<ssux_lock>::u_x_upgrade(const char *file, unsigned line)
{
  ut_ad(have_u_not_x());
  lock.u_wr_upgrade(file, line);
  recursive/= RECURSIVE_U;
}
#endif

/** needed for dict_index_t::clone() */
template<> inline void index_lock::operator=(const sux_lock&)
{
  memset((void*) this, 0, sizeof *this);
}

template<typename ssux> inline void sux_lock<ssux>::s_lock()
{
  ut_ad(!have_x());
  ut_ad(!have_s());
  lock.rd_lock();
  ut_d(s_lock_register());
}

template<typename ssux>
inline void sux_lock<ssux>::lock_shared() { s_lock(); }
template<typename ssux>
inline void sux_lock<ssux>::unlock_shared() { s_unlock(); }

template<typename ssux> inline void sux_lock<ssux>::u_lock()
{
  pthread_t id= pthread_self();
  if (writer.load(std::memory_order_relaxed) == id)
    writer_recurse<true>();
  else
  {
    lock.u_lock();
    ut_ad(!recursive);
    recursive= RECURSIVE_U;
    set_first_owner(id);
  }
}

template<typename ssux> inline void sux_lock<ssux>::x_lock(bool for_io)
{
  pthread_t id= pthread_self();
  if (writer.load(std::memory_order_relaxed) == id)
  {
    ut_ad(!for_io);
    writer_recurse<false>();
  }
  else
  {
    lock.wr_lock();
    ut_ad(!recursive);
    recursive= RECURSIVE_X;
    set_first_owner(for_io ? FOR_IO : id);
  }
}

template<typename ssux> inline void sux_lock<ssux>::u_x_upgrade()
{
  ut_ad(have_u_not_x());
  lock.u_wr_upgrade();
  recursive/= RECURSIVE_U;
}

template<typename ssux> inline bool sux_lock<ssux>::x_lock_upgraded()
{
  pthread_t id= pthread_self();
  if (writer.load(std::memory_order_relaxed) == id)
  {
    ut_ad(recursive);
    static_assert(RECURSIVE_X == 1, "compatibility");
    if (recursive & RECURSIVE_MAX)
    {
      writer_recurse<false>();
      return false;
    }
    /* Upgrade the lock. */
    lock.u_wr_upgrade();
    recursive/= RECURSIVE_U;
    return true;
  }
  else
  {
    lock.wr_lock();
    ut_ad(!recursive);
    recursive= RECURSIVE_X;
    set_first_owner(id);
    return false;
  }
}

template<typename ssux> inline bool sux_lock<ssux>::u_lock_try(bool for_io)
{
  pthread_t id= pthread_self();
  if (writer.load(std::memory_order_relaxed) == id)
  {
    if (for_io)
      return false;
    writer_recurse<true>();
    return true;
  }
  if (lock.u_lock_try())
  {
    ut_ad(!recursive);
    recursive= RECURSIVE_U;
    set_first_owner(for_io ? FOR_IO : id);
    return true;
  }
  return false;
}

template<typename ssux> inline bool sux_lock<ssux>::x_lock_try()
{
  pthread_t id= pthread_self();
  if (writer.load(std::memory_order_relaxed) == id)
  {
    writer_recurse<false>();
    return true;
  }
  if (lock.wr_lock_try())
  {
    ut_ad(!recursive);
    recursive= RECURSIVE_X;
    set_first_owner(id);
    return true;
  }
  return false;
}
