/* Copyright (c) 2000-2002, 2005-2007 MySQL AB
   Use is subject to license terms

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

/* Test if a record has changed since last read */
/* In heap this is only used when debugging */

#include "heapdef.h"

int hp_rectest(register HP_INFO *info, register const uchar *old)
{
  HP_SHARE *share= info->s;
  const HP_COPY_SPAN *span, *span_end;
  DBUG_ENTER("hp_rectest");

  /*
    Compare the ranges the stored record and the record buffer share.
    A promoted column's payload is not in the stored record at all, so
    there is nothing to compare it against.  Everything else, native blob
    descriptors included, is covered exactly as before.
  */
  for (span= share->copy_spans, span_end= span + share->copy_span_count;
       span < span_end; span++)
  {
    if (memcmp(info->current_ptr + span->store_offset, old + span->offset,
               (size_t) span->length))
    {
      DBUG_RETURN((my_errno=HA_ERR_RECORD_CHANGED)); /* Record have changed */
    }
  }
  DBUG_RETURN(0);
} /* _heap_rectest */
