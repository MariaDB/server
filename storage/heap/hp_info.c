/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/* Returns info about database status */

#include "heapdef.h"


uchar *heap_position(HP_INFO *info)
{
  return ((info->update & HA_STATE_AKTIV) ? info->current_ptr :
	  (HEAP_PTR) 0);
}


/* Note that heap_info does NOT return information about the
   current position anymore;  Use heap_position instead */

int heap_info(reg1 HP_INFO *info,reg2 HEAPINFO *x, int flag )
{
  DBUG_ENTER("heap_info");
  x->records         = info->s->records;
  x->deleted         = info->s->deleted;
  /*
    Free list entries, a coalesced block counting once.  A scan skips a
    whole block in one step, so this and not 'deleted' is what the free
    records cost it: freeing a row whose blob data spanned a thousand
    records adds one step, not a thousand.
  */
  x->deleted_entries = info->s->deleted_entries;
  x->reclength       = info->s->reclength;
  x->data_length     = info->s->data_length;
  /*
    The free space, in the bytes data_length counts.  A free record is a
    slot of the stored row width, which is the SQL row width only while
    nothing is stored out of line; multiplying the free record count by
    reclength once a wide VARCHAR is promoted reports more free space
    than the table has ever allocated.
  */
  x->delete_length   = (ulonglong) info->s->deleted *
                       info->s->block.recbuffer;
  x->index_length    = info->s->index_length;
  /*
    The ceiling data_length counts toward, in the bytes data_length is
    counted in.  Records are refused once data_length + index_length
    reaches max_table_size, so that bound holds whatever the rows look
    like -- and it is the only one that can, because a row with
    out-of-line columns occupies a data-dependent number of records.

    A row limit binds first where the rows it admits cannot reach that
    ceiling.  declared_reclength is the widest a row can be whatever its
    columns are stored as, so max_rows times it bounds the rows -- except
    where a declared blob leaves a row with no width at all, and then
    there is nothing for a row limit to bound bytes by.

    Deliberately not the expected record count times reclength: that
    count is record slots, and a slot holds a whole row only while
    nothing is stored out of line.  Multiplying the two once a wide
    VARCHAR is promoted reports a table's ceiling as its slot count
    times a row width it no longer stores, which overstates it by the
    promotion ratio.
  */
  x->max_data_length = info->s->max_table_size;
  if (info->s->max_rows != NO_LIMIT_ROWS && info->s->declared_reclength &&
      (ulonglong) info->s->max_rows <=
        ULONGLONG_MAX / info->s->declared_reclength)
    set_if_smaller(x->max_data_length,
                   (ulonglong) info->s->max_rows *
                   info->s->declared_reclength);
  x->errkey          = info->errkey;
  x->create_time     = info->s->create_time;
  if (flag & HA_STATUS_AUTO)
  {
    x->auto_increment= info->s->auto_increment+1;
    if (!x->auto_increment)			/* This shouldn't happen */
      x->auto_increment= ~(ulonglong) 0;
  }
  DBUG_RETURN(0);
} /* heap_info */
