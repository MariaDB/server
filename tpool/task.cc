/* Copyright (C) 2019, 2021, MariaDB Corporation.

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

namespace tpool
{
  task::task(callback_func func, void* arg, task_group* group) :
    m_func(func), m_arg(arg), m_group(group) {}

  void task::execute()
  {
    if (m_group)
    {
      /* Executing in a group (limiting concurrency).*/
      m_group->execute(this);
    }
    else
    {
      /* Execute directly. */
      m_func(m_arg);
      release();
    }
  }

  /* Task that provide wait() operation. */
  waitable_task::waitable_task(callback_func func, void* arg, task_group* group) :
    task(func,arg, group),m_mtx(),m_cv(),m_ref_count(),m_waiter_count(),m_original_func(){}

  void waitable_task::add_ref()
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    m_ref_count++;
  }

  void waitable_task::release()
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    m_ref_count--;
    if (!m_ref_count && m_waiter_count)
      m_cv.notify_all();
  }
  void waitable_task::wait(std::unique_lock<std::mutex>& lk)
  {
    m_waiter_count++;
    while (m_ref_count)
      m_cv.wait(lk);
    m_waiter_count--;
  }
  void waitable_task::wait()
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    wait(lk);
  }

  static void noop(void*)
  {
  }
  void waitable_task::disable()
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    if (m_func == noop)
     return;
    wait(lk);
    m_original_func = m_func;
    m_func = noop;
  }
  void waitable_task::enable()
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    if(m_func != noop)
      return;
    wait(lk);
    m_func = m_original_func;
  }
}
