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

  /** Start waiting for an exclusive lock.
  @return current value of the lock word */
  uint32_t write_lock_wait_start()
  { return lock.fetch_or(WRITER_WAITING, std::memory_order_relaxed); }
  /** Wait for an exclusive lock.
  @param l the value of the lock word
  @return whether the exclusive lock was acquired */
  bool write_lock_wait_try(uint32_t &l)
  {
    l= WRITER_WAITING;
    return lock.compare_exchange_strong(l, WRITER, std::memory_order_acquire,
                                        std::memory_order_relaxed);
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
    uint32_t l;
    if (write_lock_wait_try(l))
      return true;
    if (!(l & WRITER_WAITING))
      /* write_lock() must have succeeded for another thread */
      write_lock_wait_start();
    return false;
  }

public:
  /** Default constructor */
  rw_lock() : lock(UNLOCKED) {}

  /** Release a shared lock.
  @return whether any writers may have to be woken up */
  bool read_unlock()
  {
    auto l= lock.fetch_sub(1, std::memory_order_release);
    DBUG_ASSERT(~WRITER_PENDING & l); /* at least one read lock */
    DBUG_ASSERT(!(l & WRITER)); /* no write lock must have existed */
    return (~WRITER_PENDING & l) == 1;
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
  /** @return whether any lock is being held or waited for by any thread */
  bool is_locked_or_waiting() const
  { return lock.load(std::memory_order_relaxed) != 0; }
  /** @return whether any lock is being held by any thread */
  bool is_locked() const
  { return (lock.load(std::memory_order_relaxed) & ~WRITER_WAITING) != 0; }
};
