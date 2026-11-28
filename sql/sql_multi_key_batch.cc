/*
   Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "mariadb.h"
#include "handler.h"
#include "sql_multi_key.h"
#include "sql_multi_key_batch.h"


Multi_key_batch::Multi_key_batch(Multi_key_params *params_arg)
  :params(params_arg)
{}


size_t Multi_key_batch::slot_length() const
{
  return ALIGN_SIZE(params->rec_length) + sizeof(Link);
}


Multi_key_batch::Link *Multi_key_batch::link(uchar *slot) const
{
  return (Link *) (slot + slot_length() - sizeof(Link));
}


size_t Multi_key_batch::space_per_tuple() const
{
  /* a slot, two list entries and a row_data entry; a refill of one tuple
     must also fit the row_data terminator and the slots' alignment */
  return slot_length() + 4 * sizeof(uchar *) + ALIGN_MAX_UNIT;
}


void Multi_key_batch::reset(HANDLER_BUFFER *buff)
{
  slots= slots_end= ALIGN_PTR(my_ptrdiff_t(buff->buffer), uchar);
  buff_end= buff->buffer_end;
}


void Multi_key_batch::add(uchar *pos)
{
  if (params->pack(slots_end))
    return;
  link(slots_end)->pos= pos;
  slots_end+= slot_length();
}


static int cmp_tuples(void *arg, const void *a, const void *b)
{
  const Multi_key_params *params= (const Multi_key_params *) arg;
  int res= params->cmp_key((const uchar *) a, (const uchar *) b);
  return res ? res : params->cmp((const uchar *) a, (const uchar *) b);
}


void Multi_key_batch::split()
{
  const size_t length= slot_length();
  const size_t tuples= (slots_end - slots) / length;
  uchar **list= (uchar **) slots_end;
  uchar **first_call;
  uchar *to= slots;
  uint rank= 0;

  calls= call= call_tuples= 0;
  if (!tuples)
    return;
  my_qsort2(slots, tuples, length, cmp_tuples, params);
  first_call= row_data= list + 2 * tuples;
  DBUG_ASSERT((uchar *) (row_data + tuples + 1) <= buff_end);
  for (uchar *from= slots; from < slots_end; to+= length)
  {
    uchar *tuple= from;
    uchar **tuple_positions= list;

    if (to > slots && !params->cmp_key(to - length, tuple))
      rank++;
    else
    {
      rank= 0;
      *first_call++= to;
    }
    set_if_bigger(calls, rank + 1);
    do
    {
      *list++= link(from)->pos;
      from+= length;
    } while (from < slots_end && !cmp_tuples(params, tuple, from));
    *list++= NULL;
    memmove(to, tuple, length);
    link(to)->positions= tuple_positions;
  }
  *first_call= NULL;
  call_tuples= (uint) (first_call - row_data);
  slots_end= to;
}


uchar **Multi_key_batch::next_call()
{
  if (call == calls)
    return NULL;
  if (call++)
  {
    const size_t length= slot_length();
    uchar **to= row_data;
    for (uchar **from= row_data; *from; from++)
    {
      uchar *next= *from + length;
      if (next < slots_end && !params->cmp_key(*from, next))
        *to++= next;
    }
    *to= NULL;
    call_tuples= (uint) (to - row_data);
  }
  params->move_to(row_data[0]);
  return row_data;
}


uchar **Multi_key_batch::positions(uint32 offset)
{
  DBUG_ASSERT(offset < call_tuples);
  return link(row_data[offset])->positions;
}
