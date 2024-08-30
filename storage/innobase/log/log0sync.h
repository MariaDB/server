/*****************************************************************************
Copyright (c) 2020 MariaDB Corporation.

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

#include <atomic>
#include <thread>
#include <log0types.h>
#include <vector>

struct group_commit_waiter_t;
struct completion_callback
{
  void (*m_callback)(void*);
  void* m_param;
};

/**
Special synchronization primitive, which is helpful for
performing group commit.

It has a state consisting of
 - locked (bool)
 - current value (number). This value is always increasing.
 - pending value (number). current value can soon become this number
   This is only used for optimization, does not have to be exact

Operations supported on this semaphore

1.acquire(num, callback):
- waits until current value exceeds num, or until lock is granted.
  if running synchronously (callback is nullptr)

- returns EXPIRED if current_value >= num,
  or ACQUIRED, if current_value < num and lock is granted,
  or CALLBACK_QUEUED, if callback was not nullptr, and function
  would otherwise have to wait

2.release(num)
- releases lock
- sets new current value to max(num,current_value)
- releases some threads waiting in acquire()
- executes some callbacks
- might return some lsn, meaning there are some pending
  callbacks left, and there is no new group commit lead
  (i.e caller must do something to flush those pending callbacks)

3. value()
- read current value

4. pending_value()
- read pending value

5. set_pending_value()
*/
class group_commit_lock
{
  using value_type = lsn_t;
#ifndef DBUG_OFF
  std::thread::id m_owner_id{};
#endif
  std::mutex m_mtx;
  std::atomic<value_type> m_value;
  std::atomic<value_type> m_pending_value;
  bool m_lock;
  group_commit_waiter_t* m_waiters_list;

  typedef std::pair<value_type, completion_callback> pending_cb;
  std::vector<pending_cb> m_pending_callbacks;

public:
  group_commit_lock();
  enum lock_return_code
  {
    ACQUIRED,
    EXPIRED,
    CALLBACK_QUEUED
  };
  lock_return_code acquire(value_type num, const completion_callback *cb);
  value_type release(value_type num);
  value_type value() const;
  value_type pending() const;
  void set_pending(value_type num);
#ifndef DBUG_OFF
  bool is_owner();
#endif
};
