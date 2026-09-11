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
  const uchar *pos= info->current_ptr;
  const HP_BLOB_DESC *desc, *desc_end;
  uint sql_pos= 0, shrink;
  DBUG_ENTER("hp_rectest");

  /*
    Walk the record, stopping at each column stored out of line.  What the
    two buffers hold there does not match and is not meant to: the stored
    record has a pointer to the continuation chain, while the record buffer
    has either the value itself, for a VARCHAR the engine promoted, or a
    pointer to wherever the read handed the value out, for a blob.  So each
    such column is compared through its length and then its data, and only
    the ranges between them are compared as bytes.

    A table with no column stored out of line runs the loop zero times and
    compares the whole record in one memcmp, which is what this did before
    out-of-line storage existed.
  */
  for (desc= share->blob_descs, desc_end= desc + share->blob_count;
       desc < desc_end; desc++)
  {
    /* The range up to and including the length prefix */
    uint gap= desc->offset + desc->packlength;
    uint32 length;
    const uchar *data, *stored;

    /*
      Compaction removes bytes only where a promoted column's payload was,
      so one distance describes the whole range from the previous column
      to this one, and the share already holds it for this column.
    */
    DBUG_ASSERT(gap >= sql_pos);        /* Descriptors ascend by offset */
    shrink= desc->offset - desc->store_offset;
    if (memcmp(pos + sql_pos - shrink, old + sql_pos,
               (size_t) (gap - sql_pos)))
      goto changed;
    sql_pos= gap;

    /* The prefixes just compared equal, so one length describes both */
    length= hp_blob_length(desc, old);

    if (desc->promoted)
    {
      data= old + sql_pos;
      sql_pos+= desc->length;
    }
    else
    {
      memcpy(&data, old + sql_pos, sizeof(data));
      sql_pos+= portable_sizeof_char_ptr;
    }

    if (length)
    {
      /*
        The same materialization the key code runs, which hands back a
        pointer into the chain itself when the run layout already has the
        bytes contiguous and only copies when it does not.
      */
      stored= hp_materialize_one_blob(info, hp_blob_get_chain(desc, pos),
                                      length);
      if (!stored)
        DBUG_RETURN((my_errno= HA_ERR_OUT_OF_MEM));
      if (memcmp(stored, data, length))
        goto changed;
    }
  }

  shrink= share->reclength - share->stored_reclength;
  if (memcmp(pos + sql_pos - shrink, old + sql_pos,
             (size_t) (share->reclength - sql_pos)))
    goto changed;
  DBUG_RETURN(0);

changed:
  DBUG_RETURN((my_errno= HA_ERR_RECORD_CHANGED)); /* Record have changed */
} /* _heap_rectest */
