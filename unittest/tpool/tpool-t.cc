/* Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
*/

#include <my_global.h>
#include <tap.h>
#include <tpool.h>

static std::mutex gate_mutex;
static std::condition_variable gate_cv;
static bool allow_init;
static bool allow_completion;
static unsigned int waiting_tasks;

static void worker_init()
{
  std::unique_lock<std::mutex> lk(gate_mutex);
  gate_cv.wait(lk, [] { return allow_init; });
}

static void count_task(void *arg)
{
  tpool::tpool_wait_begin();
  {
    std::unique_lock<std::mutex> lk(gate_mutex);
    waiting_tasks++;
    gate_cv.notify_all();
    gate_cv.wait(lk, [] { return allow_completion; });
  }
  tpool::tpool_wait_end();
  ++*static_cast<std::atomic<unsigned int> *>(arg);
}

int main()
{
  plan(18);

  constexpr unsigned int max_threads= 8;
  std::atomic<unsigned int> completed{0};
  tpool::task task(count_task, &completed);
  std::unique_ptr<tpool::thread_pool> pool(tpool::create_thread_pool_generic(
    0, max_threads, tpool::thread_pool_policy::no_maintenance));
  pool->set_thread_callbacks(worker_init, [] {});
  pool->set_concurrency(1);

  tpool::worker_stats stats{};
  ok(pool->get_worker_stats(&stats) == 0, "Read initial worker stats");
  ok(stats.used == 0, "Pool starts with no used workers");
  ok(stats.idle == 0, "Pool starts with no idle workers");
  ok(stats.queued == 0, "Pool starts with an empty queue");
  ok(stats.max_threads == max_threads, "Stats report the configured cap");

  // Keep all workers in their init callbacks during the burst.
  for (unsigned int i= 0; i < max_threads; i++)
    pool->submit_task(&task);
  ok(pool->get_worker_stats(&stats) == 0, "Read stats during worker startup");
  ok(stats.used == max_threads, "Pool reaches the cap during worker startup");
  ok(stats.idle == 0, "Starting workers do not count as idle");
  ok(stats.queued == max_threads, "Tasks stay queued during worker startup");

  pool->submit_task(&task);
  ok(pool->get_worker_stats(&stats) == 0, "Read stats after exceeding the cap");
  ok(stats.used == max_threads, "Extra submission does not create a worker");
  ok(stats.queued == max_threads + 1, "Extra submission is queued");

  {
    std::lock_guard<std::mutex> lk(gate_mutex);
    allow_init= true;
  }
  gate_cv.notify_all();

  bool workers_waiting;
  {
    std::unique_lock<std::mutex> lk(gate_mutex);
    workers_waiting= gate_cv.wait_for(lk, std::chrono::seconds(5), [] {
      return waiting_tasks == max_threads;
    });
  }
  ok(workers_waiting, "All workers reached the wait inside their tasks");
  ok(pool->get_worker_stats(&stats) == 0, "Read stats while tasks are waiting");
  ok(stats.used == max_threads, "Workers waiting in tasks count as used");
  ok(stats.idle == 0, "Workers waiting in tasks do not count as idle");
  ok(stats.queued == 1, "Only the extra submission remains queued");

  {
    std::lock_guard<std::mutex> lk(gate_mutex);
    allow_completion= true;
  }
  gate_cv.notify_all();
  pool.reset();
  ok(completed == max_threads + 1, "Shutdown drains the task queue");

  return exit_status();
}
