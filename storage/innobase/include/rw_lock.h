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
#include <atomic>
#include "my_dbug.h"

/** Simple read-write lock based on std::atomic */
class rw_lock
{
  /** The lock word */
  std::atomic<uint32_t> lock;

protected:
  /** Available lock */
  static constexpr uint32_t UNLOCKED= 0;
  /** Flag to indicate that write_lock() is being held */
  static constexpr uint32_t WRITER= 1U << 31;
  /** Flag to indicate that write_lock_wait() is pending */
  static constexpr uint32_t WRITER_WAITING= 1U << 30;
  /** Flag to indicate that write_lock() or write_lock_wait() is pending */
  static constexpr uint32_t WRITER_PENDING= WRITER | WRITER_WAITING;

  /** Start waiting for an exclusive lock. */
  void write_lock_wait_start()
  {
#if defined __GNUC__ && (defined __i386__ || defined __x86_64__)
    static_assert(WRITER_WAITING == 1U << 30, "compatibility");
    __asm__ __volatile__("lock btsl $30, %0" : "+m" (lock));
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
    static_assert(WRITER_WAITING == 1U << 30, "compatibility");
    _interlockedbittestandset(reinterpret_cast<volatile long*>(&lock), 30);
#else
    lock.fetch_or(WRITER_WAITING, std::memory_order_relaxed);
#endif
  }
  /** Try to acquire a shared lock.
  @param l the value of the lock word
  @return whether the lock was acquired */
  bool read_trylock(uint32_t &l)
  {
    l= UNLOCKED;
    while (!lock.compare_exchange_strong(l, l + 1, std::memory_order_acquire,
                                         std::memory_order_relaxed))
    {
      DBUG_ASSERT(!(WRITER & l) || !(~WRITER_PENDING & l));
      if (l & WRITER_PENDING)
        return false;
    }
    return true;
  }
  /** Wait for an exclusive lock.
  @return whether the exclusive lock was acquired */
  bool write_lock_poll()
  {
    auto l= WRITER_WAITING;
    if (lock.compare_exchange_strong(l, WRITER, std::memory_order_acquire,
                                     std::memory_order_relaxed))
      return true;
    if (!(l & WRITER_WAITING))
      /* write_lock() must have succeeded for another thread */
      write_lock_wait_start();
    return false;
  }

public:
  /** Default constructor */
  rw_lock() : lock(UNLOCKED) {}

  /** Release a shared lock */
  void read_unlock()
  {
    IF_DBUG_ASSERT(auto l=,) lock.fetch_sub(1, std::memory_order_release);
    DBUG_ASSERT(l & ~WRITER_PENDING); /* at least one read lock */
    DBUG_ASSERT(!(l & WRITER)); /* no write lock must have existed */
  }
  /** Release an exclusive lock */
  void write_unlock()
  {
    IF_DBUG_ASSERT(auto l=,) lock.fetch_sub(WRITER, std::memory_order_release);
    DBUG_ASSERT(l & WRITER); /* the write lock must have existed */
  }
  /** Try to acquire a shared lock.
  @return whether the lock was acquired */
  bool read_trylock() { uint32_t l; return read_trylock(l); }
  /** Try to acquire an exclusive lock.
  @return whether the lock was acquired */
  bool write_trylock()
  {
    auto l= UNLOCKED;
    return lock.compare_exchange_strong(l, WRITER, std::memory_order_acquire,
                                        std::memory_order_relaxed);
  }

  /** @return whether an exclusive lock is being held by any thread */
  bool is_write_locked() const
  { return !!(lock.load(std::memory_order_relaxed) & WRITER); }
  /** @return whether a shared lock is being held by any thread */
  bool is_read_locked() const
  {
    auto l= lock.load(std::memory_order_relaxed);
    return (l & ~WRITER_PENDING) && !(l & WRITER);
  }
  /** @return whether any lock is being held by any thread */
  bool is_locked() const
  { return (lock.load(std::memory_order_relaxed) & ~WRITER_WAITING) != 0; }
};
