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

static std::atomic<bool> critical;

ulong srv_n_spin_wait_rounds= 30;
uint srv_spin_wait_delay= 4;

constexpr unsigned N_THREADS= 30;
constexpr unsigned N_ROUNDS= 100;
constexpr unsigned M_ROUNDS= 100;

static srw_mutex m;

static void test_srw_mutex()
{
  for (auto i= N_ROUNDS * M_ROUNDS; i--; )
  {
    m.wr_lock();
    assert(!critical);
    critical= true;
    critical= false;
    m.wr_unlock();
  }
}

static srw_lock_low l;

static void test_srw_lock()
{
  for (auto i= N_ROUNDS; i--; )
  {
    l.wr_lock();
    assert(!critical);
    critical= true;
    critical= false;
    l.wr_unlock();

    for (auto j= M_ROUNDS; j--; )
    {
      l.rd_lock();
      assert(!critical);
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
    assert(!critical);
    critical= true;
    critical= false;
    ssux.wr_unlock();

    for (auto j= M_ROUNDS; j--; )
    {
      ssux.rd_lock();
      assert(!critical);
      ssux.rd_unlock();
    }

    for (auto j= M_ROUNDS; j--; )
    {
      ssux.u_lock();
      assert(!critical);
      ssux.u_wr_upgrade();
      assert(!critical);
      critical= true;
      critical= false;
      ssux.wr_u_downgrade();
      ssux.u_unlock();
    }
  }
}

static sux_lock<ssux_lock_impl<true>> sux;

static void test_sux_lock()
{
  for (auto i= N_ROUNDS; i--; )
  {
    sux.x_lock();
    assert(!critical);
    critical= true;
    for (auto j= M_ROUNDS; j--; )
      sux.x_lock();
    critical= false;
    for (auto j= M_ROUNDS + 1; j--; )
      sux.x_unlock();

    for (auto j= M_ROUNDS; j--; )
    {
      sux.s_lock();
      assert(!critical);
      sux.s_unlock();
    }

    for (auto j= M_ROUNDS / 2; j--; )
    {
      sux.u_lock();
      assert(!critical);
      sux.u_lock();
      sux.u_x_upgrade();
      assert(!critical);
      critical= true;
      sux.x_unlock();
      critical= false;
      sux.x_u_downgrade();
      sux.u_unlock();
    }
  }
}

int main(int argc __attribute__((unused)), char **argv)
{
  std::thread t[N_THREADS];

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
}
