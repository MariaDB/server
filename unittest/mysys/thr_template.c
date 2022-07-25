/* Copyright (c) 2006-2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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

#include <my_global.h>
#include <my_sys.h>
#include <my_atomic.h>
#include <tap.h>

volatile uint32 bad;
pthread_mutex_t mutex;

void do_tests();

void test_concurrently(const char *test, pthread_handler handler, int n, int m)
{
  pthread_t *threads= malloc(n * sizeof(pthread_t));
  int i;
  ulonglong now= my_interval_timer();

  assert(threads);
  bad= 0;

  diag("Testing %s with %d threads, %d iterations... ", test, n, m);
  for (i= 0; i < n; i++)
  {
    if (pthread_create(&threads[i], 0, handler, &m) != 0)
    {
      diag("Could not create thread");
      abort();
    }
  }

  for (i= 0; i < n; i++)
    pthread_join(threads[i], 0);

  now= my_interval_timer() - now;
  free(threads);
  ok(!bad, "tested %s in %g secs (%d)", test, ((double)now)/1e9, bad);
}

int main(int argc __attribute__((unused)), char **argv)
{
  MY_INIT(argv[0]);

  if (argv[1] && *argv[1])
    DBUG_SET_INITIAL(argv[1]);

  pthread_mutex_init(&mutex, 0);

#define CYCLES 30000
#define THREADS 30

  diag("N CPUs: %d, atomic ops: %s", my_getncpus(), MY_ATOMIC_MODE);

  do_tests();

  pthread_mutex_destroy(&mutex);
  my_end(0);
  return exit_status();
}

