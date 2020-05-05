/* Copyright (c) 2020, MariaDB Corporation

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
#include <queues.h>
#include <my_rnd.h>
#include "tap.h"

int cmp(void *arg __attribute__((unused)), uchar *a, uchar *b)
{
  return *a < *b ? -1 : *a > *b;
}

#define rnd(R) ((uint)(my_rnd(R) * INT_MAX32))

#define el(Q,I) ((uint)*queue_element(Q, I))

my_bool verbose;

my_bool check_queue(QUEUE *queue)
{
  char b[1024]={0}, *s, *e=b+sizeof(b)-2;
  my_bool ok=1;
  uint i;

  s= b + my_snprintf(b, e-b, "%x", el(queue, 1));
  for (i=2; i <= queue->elements; i++)
  {
    s+= my_snprintf(s, e-s, ", %x", el(queue, i));
    ok &= el(queue, i) <= el(queue, i>>1);
  }
  if (!ok || verbose)
    diag("%s", b);
  return ok;
}

int main(int argc __attribute__((unused)), char *argv[])
{
  QUEUE q, *queue=&q;
  MY_INIT(argv[0]);
  plan(19);

  verbose=1;

  init_queue(queue, 256, 0, 1, cmp, NULL, 0, 0);
  queue_insert(queue, (uchar*)"\x99");
  queue_insert(queue, (uchar*)"\x19");
  queue_insert(queue, (uchar*)"\x36");
  queue_insert(queue, (uchar*)"\x17");
  queue_insert(queue, (uchar*)"\x12");
  queue_insert(queue, (uchar*)"\x05");
  queue_insert(queue, (uchar*)"\x25");
  queue_insert(queue, (uchar*)"\x09");
  queue_insert(queue, (uchar*)"\x15");
  queue_insert(queue, (uchar*)"\x06");
  queue_insert(queue, (uchar*)"\x11");
  queue_insert(queue, (uchar*)"\x01");
  queue_insert(queue, (uchar*)"\x04");
  queue_insert(queue, (uchar*)"\x13");
  queue_insert(queue, (uchar*)"\x24");
  ok(check_queue(queue), "after insert");
  queue_remove(queue, 5);
  ok(check_queue(queue), "after remove 5th");

  queue_element(queue, 1) = (uchar*)"\x01";
  queue_element(queue, 2) = (uchar*)"\x10";
  queue_element(queue, 3) = (uchar*)"\x04";
  queue_element(queue, 4) = (uchar*)"\x09";
  queue_element(queue, 5) = (uchar*)"\x13";
  queue_element(queue, 6) = (uchar*)"\x03";
  queue_element(queue, 7) = (uchar*)"\x08";
  queue_element(queue, 8) = (uchar*)"\x07";
  queue_element(queue, 9) = (uchar*)"\x06";
  queue_element(queue,10) = (uchar*)"\x12";
  queue_element(queue,11) = (uchar*)"\x05";
  queue_element(queue,12) = (uchar*)"\x02";
  queue_element(queue,13) = (uchar*)"\x11";
  queue->elements= 13;
  ok(!check_queue(queue), "manually filled (queue property violated)");

  queue_fix(queue);
  ok(check_queue(queue), "fixed");

  ok(*queue_remove_top(queue) == 0x13, "remove top 13");
  ok(*queue_remove_top(queue) == 0x12, "remove top 12");
  ok(*queue_remove_top(queue) == 0x11, "remove top 11");
  ok(*queue_remove_top(queue) == 0x10, "remove top 10");
  ok(*queue_remove_top(queue) == 0x09, "remove top 9");
  ok(*queue_remove_top(queue) == 0x08, "remove top 8");
  ok(*queue_remove_top(queue) == 0x07, "remove top 7");
  ok(*queue_remove_top(queue) == 0x06, "remove top 6");
  ok(*queue_remove_top(queue) == 0x05, "remove top 5");
  ok(*queue_remove_top(queue) == 0x04, "remove top 4");
  ok(*queue_remove_top(queue) == 0x03, "remove top 3");
  ok(*queue_remove_top(queue) == 0x02, "remove top 2");
  ok(*queue_remove_top(queue) == 0x01, "remove top 1");

  /* random test */
  {
    int i, res;
    struct my_rnd_struct rand;
    my_rnd_init(&rand, (ulong)(intptr)&i, (ulong)(intptr)argv);
    verbose=0;

    for (res= i=1; i <= 250; i++)
    {
      uchar *s=alloca(2);
      *s= rnd(&rand) % 251;
      queue_insert(queue, s);
      res &= check_queue(queue);
    }
    ok(res, "inserted 250");

    while (queue->elements)
    {
      queue_remove(queue, (rnd(&rand) % queue->elements) + 1);
      res &= check_queue(queue);
    }
    ok(res, "removed 250");
  }

  delete_queue(queue);
  my_end(0);
  return exit_status();
}

