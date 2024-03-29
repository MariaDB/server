/* Copyright(C) 2019 MariaDB Corporation.

This program is free software; you can redistribute itand /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111 - 1301 USA*/

#include <tpool.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <tpool_structs.h>
#include <thread>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h> // usleep
#endif
namespace tpool
{

  /**
    Task_group constructor

     @param max_threads - maximum number of threads allowed to execute
     tasks from the group at the same time.

     @param enable_task_release - if true (default), task::release() will be
     called after task execution.'false' should only be used in rare cases
     when accessing memory, pointed by task structures, would be unsafe after.
     the callback. Also 'false' is only possible ,if task::release() is a trivial function
  */
  task_group::task_group(unsigned int max_concurrency,
                       bool enable_task_release)
    :
    m_queue(8),
    m_mtx(),
    m_total_tasks(0),
    m_total_enqueues(0),
    m_tasks_running(),
    m_max_concurrent_tasks(max_concurrency),
    m_enable_task_release(enable_task_release)
  {};

  void task_group::set_max_tasks(unsigned int max_concurrency)
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    m_max_concurrent_tasks = max_concurrency;
  }
  void task_group::execute(task* t)
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    if (m_tasks_running == m_max_concurrent_tasks)
    {
      /* Queue for later execution by another thread.*/
      m_queue.push(t);
      m_total_enqueues++;
      return;
    }
    m_tasks_running++;
    for (;;)
    {
      lk.unlock();
      if (t)
      {
        t->m_func(t->m_arg);
        if (m_enable_task_release)
          t->release();
      }
      lk.lock();
      m_total_tasks++;
      if (m_queue.empty())
        break;
      t = m_queue.front();
      m_queue.pop();
    }
    m_tasks_running--;
  }

  void task_group::cancel_pending(task* t)
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    if (!t)
      m_queue.clear();
    for (auto it = m_queue.begin(); it != m_queue.end(); it++)
    {
      if (*it == t)
      {
        (*it)->release();
        (*it) = nullptr;
      }
    }
  }

  void task_group::get_stats(group_stats *stats)
  {
    std::lock_guard<std::mutex> lk(m_mtx);
    stats->tasks_running= m_tasks_running;
    stats->queue_size= m_queue.size();
    stats->total_tasks_executed= m_total_tasks;
    stats->total_tasks_enqueued= m_total_enqueues;
  }

  task_group::~task_group()
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    assert(m_queue.empty());

    while (m_tasks_running)
    {
      lk.unlock();
#ifndef _WIN32
      usleep(1000);
#else
      Sleep(1);
#endif
      lk.lock();
    }
  }
}
