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
/** Simple read-update-write lock based on std::atomic */
#else
/** Simple read-write lock based on std::atomic */
#endif
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
#ifdef SUX_LOCK_GENERIC
  /** Flag to indicate that an update lock exists */
  static constexpr uint32_t UPDATER= 1U << 29;
#endif /* SUX_LOCK_GENERIC */

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
  /** Start waiting for an exclusive lock.
  @return current value of the lock word */
  uint32_t write_lock_wait_start_read()
  { return lock.fetch_or(WRITER_WAITING, std::memory_order_relaxed); }
  /** Wait for an exclusive lock.
  @param l the value of the lock word
  @return whether the exclusive lock was acquired */
  bool write_lock_wait_try(uint32_t &l)
  {
    return lock.compare_exchange_strong(l, WRITER, std::memory_order_acquire,
                                        std::memory_order_relaxed);
  }
  /** Try to acquire a shared lock.
  @tparam prioritize_updater   whether to ignore WRITER_WAITING for UPDATER
  @param l the value of the lock word
  @return whether the lock was acquired */
#ifdef SUX_LOCK_GENERIC
  template<bool prioritize_updater= false>
#endif /* SUX_LOCK_GENERIC */
  bool read_trylock(uint32_t &l)
  {
    l= UNLOCKED;
    while (!lock.compare_exchange_strong(l, l + 1, std::memory_order_acquire,
                                         std::memory_order_relaxed))
    {
      DBUG_ASSERT(!(WRITER & l) || !(~WRITER_PENDING & l));
#ifdef SUX_LOCK_GENERIC
      DBUG_ASSERT((~(WRITER_PENDING | UPDATER) & l) < UPDATER);
      if (prioritize_updater
          ? (WRITER & l) || ((WRITER_WAITING | UPDATER) & l) == WRITER_WAITING
          : (WRITER_PENDING & l))
#else /* SUX_LOCK_GENERIC */
      if (l & WRITER_PENDING)
#endif /* SUX_LOCK_GENERIC */
        return false;
    }
    return true;
  }
#ifdef SUX_LOCK_GENERIC
  /** Try to acquire an update lock.
  @param l the value of the lock word
  @return whether the lock was acquired */
  bool update_trylock(uint32_t &l)
  {
    l= UNLOCKED;
    while (!lock.compare_exchange_strong(l, l | UPDATER,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed))
    {
      DBUG_ASSERT(!(WRITER & l) || !(~WRITER_PENDING & l));
      DBUG_ASSERT((~(WRITER_PENDING | UPDATER) & l) < UPDATER);
      if ((WRITER_PENDING | UPDATER) & l)
        return false;
    }
    return true;
  }
  /** Try to upgrade an update lock to an exclusive lock.
  @return whether the update lock was upgraded to exclusive */
  bool upgrade_trylock()
  {
    auto l= UPDATER;
    while (!lock.compare_exchange_strong(l, WRITER,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed))
    {
      /* Either conflicting (read) locks have been granted, or
      the WRITER_WAITING flag was set by some thread that is waiting
      to become WRITER. */
      DBUG_ASSERT(((WRITER | UPDATER) & l) == UPDATER);
      if (~(WRITER_WAITING | UPDATER) & l)
        return false;
    }
    DBUG_ASSERT((l & ~WRITER_WAITING) == UPDATER);
    /* Any thread that had set WRITER_WAITING will eventually be woken
    up by ssux_lock_impl::x_unlock() or ssux_lock_impl::u_unlock()
    (not ssux_lock_impl::wr_u_downgrade() to keep the code simple). */
    return true;
  }
  /** Downgrade an exclusive lock to an update lock. */
  void downgrade()
  {
    IF_DBUG_ASSERT(auto l=,)
    lock.fetch_xor(WRITER | UPDATER, std::memory_order_relaxed);
    DBUG_ASSERT((l & ~WRITER_WAITING) == WRITER);
  }
#endif /* SUX_LOCK_GENERIC */

  /** Wait for an exclusive lock.
  @return whether the exclusive lock was acquired */
  bool write_lock_poll()
  {
    auto l= WRITER_WAITING;
    if (write_lock_wait_try(l))
      return true;
    if (!(l & WRITER_WAITING))
      /* write_lock() must have succeeded for another thread */
      write_lock_wait_start();
    return false;
  }
  /** @return the lock word value */
  uint32_t value() const { return lock.load(std::memory_order_acquire); }

public:
  /** Default constructor */
  rw_lock() : lock(UNLOCKED) {}

  /** Release a shared lock.
  @return whether any writers may have to be woken up */
  bool read_unlock()
  {
    auto l= lock.fetch_sub(1, std::memory_order_release);
    DBUG_ASSERT(!(l & WRITER)); /* no write lock must have existed */
#ifdef SUX_LOCK_GENERIC
    DBUG_ASSERT(~(WRITER_PENDING | UPDATER) & l); /* at least one read lock */
    return (~(WRITER_PENDING | UPDATER) & l) == 1;
#else /* SUX_LOCK_GENERIC */
    DBUG_ASSERT(~(WRITER_PENDING) & l); /* at least one read lock */
    return (~WRITER_PENDING & l) == 1;
#endif /* SUX_LOCK_GENERIC */
  }
#ifdef SUX_LOCK_GENERIC
  /** Release an update lock */
  void update_unlock()
  {
    IF_DBUG_ASSERT(auto l=,)
    lock.fetch_and(~UPDATER, std::memory_order_release);
    /* the update lock must have existed */
    DBUG_ASSERT((l & (WRITER | UPDATER)) == UPDATER);
  }
#endif /* SUX_LOCK_GENERIC */
  /** Release an exclusive lock */
  void write_unlock()
  {
    /* Below, we use fetch_sub(WRITER) instead of fetch_and(~WRITER).
    The reason is that on IA-32 and AMD64 it translates into the 80486
    instruction LOCK XADD, while fetch_and() translates into a loop
    around LOCK CMPXCHG. For other ISA either form should be fine. */
    static_assert(WRITER == 1U << 31, "compatibility");
    IF_DBUG_ASSERT(auto l=,) lock.fetch_sub(WRITER, std::memory_order_release);
    /* the write lock must have existed */
#ifdef SUX_LOCK_GENERIC
    DBUG_ASSERT((l & (WRITER | UPDATER)) == WRITER);
#else /* SUX_LOCK_GENERIC */
    DBUG_ASSERT(l & WRITER);
#endif /* SUX_LOCK_GENERIC */
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
  bool is_write_locked() const { return !!(value() & WRITER); }
#ifdef SUX_LOCK_GENERIC
  /** @return whether an update lock is being held by any thread */
  bool is_update_locked() const { return !!(value() & UPDATER); }
#endif /* SUX_LOCK_GENERIC */
  /** @return whether any lock is being held or waited for by any thread */
  bool is_locked_or_waiting() const { return value() != 0; }
  /** @return whether any lock is being held by any thread */
  bool is_locked() const { return (value() & ~WRITER_WAITING) != 0; }
};
