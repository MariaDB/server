/* Copyright (c) 2023, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#pragma once

#include <atomic>
#include "my_global.h"
#include "mysql/psi/mysql_thread.h"

struct Workzone_debug_guard
{
#ifndef DBUG_OFF
  mysql_mutex_t *guard= NULL;
  Workzone_debug_guard(mysql_mutex_t *mutex) : guard(mutex) {}
#endif
  Workzone_debug_guard(){}
  void assert_protected() { mysql_mutex_assert_owner(guard); }
};

#ifndef WIN32
/**
  An idiom to protect coroutine-like (stateful) tasks that can process requests.
  Protects the coroutine (task) from skipping the signal while active and
  handles the resource reclamation problem.

  Both problems are going to be described in an iterative way.
  First, a lost signal problem will be addressed.

  Context:
  A task is executed in the thread pool and sometimes waits for data
  (user input, response from the remote server, etc). When it happens so, it is
  scheduled out, until the data is available.
  Typically:
    void Thread_pool::schedule_execution(Task *task) {
      get_queue()->put(task);
    }
  On the worker's side:
    Worker::execute(Task *task) {
      task->execute();
      // Enable the task back in the event poll.
      // When the data is ready, a task can be rescheduled.
      task->add_to_event_pool(
        [task](){ pool->schedule_execution(task); }
      );
    }
  Some other actors (threads, tasks) may make the requests to this
  task, for example to dump its state, or to change the state. But the
  task may sleep at that time. So the actor may want to wake it up.
    void send_request(Task *t, Request *r) {
      t->enqueue_request(r);
      t->wake_up_if_needed();
    }
  After such call, an actor would want to await on the response.

  Someone else (i.e. event pool or other actor) may also try to wake it up.
  How can we avoid double wake up and at the same time guarantee that a task
  will eventually process the messages?

  Three bool-returning functions are introduced for this:
  try_enter, try_leave, and notify.

  try_enter  Enters the execution context. Ensures the uniqueness of task
             presence in the execution pool. It simply sets the ENTER_BIT,
             which means entering critical section. If this bit wasn't set
             before, then task enters.
             @returns whether the task enters

  try_leave  ensures that the caller is aware of the pending events and leaves
             if none.
             First, it checks SIGNAL_BIT. If it's set, discards it and returns
             false. Else it discards ENTER_BIT and returns true.
             @returns true if task has left execution context.

  notify     sets the signal bit and exits. Returns true if the task
             should be explicitly woken up. It is determined by the presense of
             ENTER_BIT. If it was present, then it sohuld be handled by
             currently running task. If it's not, then the task
             should be woken up.
             @returns whether the task was active.

  try_leave usage protocol is as follows:
  void Task::leave() {
    while (!guard.try_leave())
      process_messages();
  }

  Now, a worker thread may discover this task finished and proceed to its
  deallocation. At this point, some other actor may access this task
  guard's state.

  Example:
  A task is typically added to a working queue with a method like this:
  void Thread_pool::schedule_execution(Task *task) {
    if (!task->guard.try_enter())
      return; // Do nothing as someone else executed this task.
    get_queue()->put(task);
  }

  Then on the Worker side:
  Worker::execute(Task *task) {
    assert_task_entered(task);
    task->execute();
    task->leave();

    // Add to the event pool (epoll/mitex queue/etc), where it can be
    // rescheduled. Normally, this normally be the last time when the task data
    // is accessed in this execution context, unless addition fails.
    bool success= task->add_to_event_pool(...);
    if (!success) {
      // Failure may mean a closed socket or file handle.
      delete task; // oops...
    }
  }

  It may go wrong by many ways!
  As one example, once the task executed leave(), any other actor may add it
  to the execution queue. Then, it may wrongly try to add the task to the event
  pool twice, and what's worse, end up in double free problem, or access to
  freed memory.

  To fix this, one may try to first disable the task in the event pool:
  void send_request(Task *t, Request *r) {
    assert_pointer_protected(t); // ensure nobody can delete this task
    t->enqueue_request(r);
    bool success;
    do {
      success = t->guard->notify() || t->remove_from_event_pool();
    } while (!success);
    release_pointer(t);
  }
  Few problems here:
  1. It's blocking.
  2. One also needs to protect from concurrent requesters. protect_pointer(t)
     could already do this, if it's for example a global mutex (or a chain of
     global -> local mutexes, like in MariaDB).
  3. It's not always possible to know whether we removed a task successfully
     or not. For example, this problem is present in epoll.


  The solution introduced here is a third bit representing ownership, and hence
  ownership passing.

  There's always the only owner. The ownership is first obtained with Task
  creation.
  An execution context is entered with try_enter_owner. If failed,
  then the ownership is atomically passed to the currently active execution
  context.
  On leaving the context, the ownership is checked. Once the owner
  leaves the context, no-one else can enter the context owned, until the
  ownership is passed again.
  An owner is responsible to free the resources.
  Only an owner can access resources without protection.

  So, one function, try_enter_owner is added, and try_leave function is updated
  with one new feature.

  try_enter_owner  enters the execution context by atomically setting
                   OWNER_BIt|ENTER_BIT. If ENTER_BIT was set in previous state
                   version, reports failure and passes the ownership by leaving
                   OWNER_BIT set.
                   @returns whether succeeds entering the execution context.

  try_leave        does either of the following:
                   * if SIGNAL_BIT is set, unsets it and reports failure
                   * otherwise leaves the execution context by unsetting
                     ENTER_BIT (and OWNER_BIT) and reports success
                   @returns enum with one of the following values:
                   SIGNAL     caller did not leave an execution context as there
                              was a signal. A false SIGNAL may be reported if
                              the ownership was passed in-between of try_leave's
                              work.
                   NOT_OWNER  caller has left an execution context is left and
                              by leaving he doesn't own the object
                   OWNER      execution context is left and the caller owns the
                              object


  Example:
    Worker::execute(Task *task) {
      assert_task_entered(task);
      task->execute();
      bool owner = task->leave();

      if (owner) {
        bool success= task->add_to_event_pool(...);
        if (!success) {
          // The task wasn't added to the event pool
(1)       if (task->try_enter_owner())
            delete task;
        }
      }
    }

    Now the task is only freed by the owner. Also, only owner adds the task back
    to the event pool. After that, event pool becomes the owner. Then, once the
    event is available, the task should be added with try_enter_owned:

    void Thread_pool::schedule_execution_owner(Task *task) {
      if (!task->guard.try_enter_owner())
        return; // Do nothing as someone else executed this task.
      get_queue()->put(task);
    }

    If try_enter_owner fails, i.e. condition on line (1) evaluates to false,
    then another (non-owner) context was active. Now it will become the owner
    and will be responsible for further resource deallocation.

    Third-party actors still require to ensure the protection on the pointer,
    as they do not own the task. The protection can be a mutex, or a one of
    memory reclamation schemes.
    The owner, in turn, should make precautions to make sure nobody uses this
    pointer. See for example THD::~THD().

    void send_request(Task *t) {
      assert_pointer_protected(t);
      t->enqueue_request(r);
      if (!t->guard->notify())
        pool->schedule_execution(t);
      release_pointer(t);
    }

    Note that a usual schedule_execution method is used here, which still enters
    context without ownership.

    Obviously, the request will be processed if either notify or enter succeed.

    If enter failed, then another context is currently active, but since notify
    failed, it wasn't active when request was enqueued. Therefore, it definitely
    will be processed.
 */
class Notifiable_work_zone
{
  static constexpr ulong ENTER_BIT = 4;
  static constexpr ulong OWNER_BIT = 2;
  static constexpr ulong SIGNAL_BIT = 1;
  std::atomic<ulong> state{0};
  Workzone_debug_guard guard;
public:

  bool try_enter_owner()
  {
    const ulong ENTER_OWNER= ENTER_BIT|OWNER_BIT;
    ulong old_state= state.fetch_or(ENTER_OWNER);
    // We can't have an active owner in parallel.
    DBUG_ASSERT((old_state & ENTER_OWNER) != (ENTER_OWNER));
    return !(old_state & ENTER_BIT);
  }

  bool try_enter()
  {
    guard.assert_protected();
    ulong old_state= state.fetch_or(ENTER_BIT);
    return !(old_state & ENTER_BIT);
  }

  enum Leave_result
  {
    SIGNAL= 0,
    NOT_OWNER= 1,
    OWNER= 2,
  };
  static_assert(OWNER == OWNER_BIT, "OWNER_BIT changed?");

  Leave_result try_leave()
  {
    ulong old_state= state.load();
    DBUG_ASSERT(old_state & ENTER_BIT);

    if (unlikely(old_state & SIGNAL_BIT))
    {
      state.fetch_and(~SIGNAL_BIT);
      return SIGNAL; // one can reveal ownership only after leave
    }

    bool success= state.compare_exchange_weak(old_state, 0);
    return success
           // Transforms: OWNER_BIT == 2  ->  2 == SUCCESS_OWNER
           //                          0  ->  1 == SUCCESS_NOT_OWNER
           ? Leave_result(((old_state & OWNER_BIT) >> 1) + 1)
           : SIGNAL;

  }
  bool notify()
  {
    ulong old_state= state.fetch_or(SIGNAL_BIT);
    return old_state & ENTER_BIT; // true if there was someone to notify
  }

  void assert_entered()
  {
    DBUG_ASSERT(state.load(std::memory_order_relaxed) & ENTER_BIT);
  }

  void init_guard(Workzone_debug_guard g) { guard= g; }
};
#else
class Notifiable_work_zone
{
public:
  bool try_enter_owner() const { return true; }
  void assert_entered(){}
  void init_guard(Workzone_debug_guard) {}
};
#endif // !WIN32
