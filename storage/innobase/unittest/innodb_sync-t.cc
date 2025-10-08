/* Copyright (c) 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <thread>
#include "tap.h"
#include "my_sys.h"
#include "sux_lock.h"

static std::atomic<bool>* critical= nullptr;

ulong srv_n_spin_wait_rounds= 30;
uint srv_spin_wait_delay= 4;

unsigned N_THREADS= 30;
unsigned N_ROUNDS= 100;
unsigned M_ROUNDS= 100;

size_t n_critical= 1;

#define ASSERT_CRITICAL(value) \
  for (size_t i= 0; i < n_critical; ++i) \
    assert(critical[i] == value)

static inline void set_critical(const bool value)
{
  for (size_t i= 0; i < n_critical; ++i)
    critical[i]= value;
}

static srw_mutex m;

static void test_srw_mutex()
{
  for (auto i= N_ROUNDS * M_ROUNDS; i--; )
  {
    m.wr_lock();
    ASSERT_CRITICAL(false);
    set_critical(true);
    set_critical(false);
    m.wr_unlock();
  }
}

static srw_lock_low l;

static void test_srw_lock()
{
  for (auto i= N_ROUNDS; i--; )
  {
    l.wr_lock();
    ASSERT_CRITICAL(false);
    set_critical(true);
    set_critical(false);
    l.wr_unlock();

    for (auto j= M_ROUNDS; j--; )
    {
      l.rd_lock();
      ASSERT_CRITICAL(false);
      l.rd_unlock();
    }
  }
}

static ssux_lock_impl<false> ssux;

static void test_ssux_lock()
{
  for (auto i= N_ROUNDS; i--; )
  {
    ssux.wr_lock();
    ASSERT_CRITICAL(false);
    set_critical(true);
    set_critical(false);
    ssux.wr_unlock();

    for (auto j= M_ROUNDS; j--; )
    {
      ssux.rd_lock();
      ASSERT_CRITICAL(false);
      ssux.rd_unlock();
    }

    for (auto j= M_ROUNDS; j--; )
    {
      ssux.u_lock();
      ASSERT_CRITICAL(false);
      ssux.u_wr_upgrade();
      ASSERT_CRITICAL(false);
      set_critical(true);
      set_critical(false);
      ssux.wr_u_downgrade();
      ssux.u_unlock();
    }

    for (auto j= M_ROUNDS; j--; )
    {
      ssux.rd_lock();
      ASSERT_CRITICAL(false);
      if (ssux.rd_u_upgrade_try())
      {
        ASSERT_CRITICAL(false);
        ssux.rd_unlock();
        ssux.u_wr_upgrade();
        ASSERT_CRITICAL(false);
        set_critical(true);
        set_critical(false);
        ssux.wr_u_downgrade();
        ssux.u_rd_downgrade();
      }
      ASSERT_CRITICAL(false);
      ssux.rd_unlock();
    }
  }
}

static sux_lock<ssux_lock_impl<true>> sux;

static void test_sux_lock()
{
  for (auto i= N_ROUNDS; i--; )
  {
    sux.x_lock();
    ASSERT_CRITICAL(false);
    set_critical(true);
    for (auto j= M_ROUNDS; j--; )
      sux.x_lock();
    set_critical(false);
    for (auto j= M_ROUNDS + 1; j--; )
      sux.x_unlock();

    for (auto j= M_ROUNDS; j--; )
    {
      sux.s_lock();
      ASSERT_CRITICAL(false);
      sux.s_unlock();
    }

    for (auto j= M_ROUNDS / 2; j--; )
    {
      sux.u_lock();
      ASSERT_CRITICAL(false);
      sux.u_lock();
      sux.u_x_upgrade();
      ASSERT_CRITICAL(false);
      set_critical(true);
      sux.x_unlock();
      set_critical(false);
      sux.x_u_downgrade();
      sux.u_unlock();
      sux.s_lock();
      std::ignore= sux.s_x_upgrade();
      ASSERT_CRITICAL(false);
      sux.x_lock();
      set_critical(true);
      sux.x_unlock();
      set_critical(false);
      sux.x_unlock();
    }
  }
}

int main(int argc, char **argv)
{
  if (argc > 1)
    srv_n_spin_wait_rounds= atoi(argv[1]);
  if (argc > 2)
    srv_spin_wait_delay= atoi(argv[2]);
  if (argc > 3)
    N_THREADS= atoi(argv[3]);
  if (argc > 4)
    N_ROUNDS= atoi(argv[4]);
  if (argc > 5)
    M_ROUNDS= atoi(argv[5]);
  if (argc > 6)
    n_critical= atoi(argv[6]);

  if (argc > 1)
  {
    printf("Parameters: srv_n_spin_wait_rounds=%lu srv_spin_wait_delay=%u "
           "N_THREADS=%u N_ROUNDS=%u M_ROUNDS=%u n_critical=%zu\n",
           srv_n_spin_wait_rounds, srv_spin_wait_delay,
           N_THREADS, N_ROUNDS, M_ROUNDS, n_critical);
  }

  std::thread* t= nullptr;
  if (N_THREADS > 0)
  {
    t= new std::thread[N_THREADS];
    if (!t)
    {
      printf("Failed to allocate memory for %u threads\n", N_THREADS);
      return 1;
    }
  }

  if (n_critical > 0)
  {
    critical= new std::atomic<bool>[n_critical];
    if (!critical)
    {
      printf("Failed to allocate memory for %zu criticals\n", n_critical);
      free(t);
      t= nullptr;
      return 1;
    }
    set_critical(false);
  }

  MY_INIT(argv[0]);

  plan(4);

  m.init();
  for (auto i= N_THREADS; i--; )
    t[i]= std::thread(test_srw_mutex);

  for (auto i= N_THREADS; i--; )
    t[i].join();

  m.destroy();
  ok(true, "srw_mutex");

  l.init();

  for (auto i= N_THREADS; i--; )
    t[i]= std::thread(test_srw_lock);

  for (auto i= N_THREADS; i--; )
    t[i].join();

  ok(true, "srw_lock");

  l.destroy();

  ssux.init();
  for (auto i= N_THREADS; i--; )
    t[i]= std::thread(test_ssux_lock);

  for (auto i= N_THREADS; i--; )
    t[i].join();

  ok(true, "ssux_lock");
  ssux.destroy();

  sux.init();
  for (auto i= N_THREADS; i--; )
    t[i]= std::thread(test_sux_lock);

  for (auto i= N_THREADS; i--; )
    t[i].join();

  ok(true, "sux_lock");
  sux.free();

  delete[] t;
  t= nullptr;

  delete[] critical;
  critical= nullptr;

  my_end(MY_CHECK_ERROR);
  return exit_status();
}
