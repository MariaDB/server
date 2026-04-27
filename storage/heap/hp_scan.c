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

/* Scan through all rows */

#include "heapdef.h"

/*
	   Returns one of following values:
	   0 = Ok.
	   HA_ERR_RECORD_DELETED = Record is deleted.
	   HA_ERR_END_OF_FILE = EOF.
*/

int heap_scan_init(register HP_INFO *info)
{
  DBUG_ENTER("heap_scan_init");
  info->lastinx= -1;
  info->current_record= (ulong) ~0L;		/* No current record */
  info->update=0;
  info->next_block=0;
  info->key_version= info->s->key_version;
  info->file_version= info->s->file_version;
  DBUG_RETURN(0);
}

int heap_scan(register HP_INFO *info, uchar *record)
{
  HP_SHARE *share=info->s;
  ulong pos;
  DBUG_ENTER("heap_scan");

  /*
    Scan boundary: total_records + deleted == block.last_allocated.

    Every slot in the HP_BLOCK data area is either a live record (counted in
    total_records) or a deleted/free slot (counted in deleted).  This
    includes blob continuation records allocated by hp_alloc_from_tail()
    and freed by hp_free_run_chain(), both of which maintain the invariant
    total_records + deleted == block.last_allocated.

    next_block is a cached upper bound for the current HP_BLOCK segment:
    within one segment, current_ptr can be advanced by recbuffer without
    calling hp_find_record().  It MUST satisfy
        next_block <= total_records + deleted
    at all times, otherwise the scan will walk past the last allocated
    slot into unmapped memory.

    The else branch below recomputes next_block and caps it.  Any code
    that manipulates next_block externally (e.g. restart_rnd_next) must
    also enforce this cap.
  */
  pos= ++info->current_record;
  if (pos < info->next_block)
  {
    info->current_ptr+=share->block.recbuffer;
  }
  else
  {
    /* Advance next_block to the next records_in_block boundary */
    ulong rem= info->next_block % share->block.records_in_block;
    info->next_block+=share->block.records_in_block - rem;
    /*
      Cap next_block at the scan end (total_records + deleted).  This is
      essential: rows may have been deleted since next_block was last set
      (e.g. remove_dup_with_compare deletes duplicates mid-scan), and
      block boundaries can extend well past the last allocated slot.
    */
    if (info->next_block >= share->total_records+share->deleted)
    {
      info->next_block= share->total_records+share->deleted;
      if (pos >= info->next_block)
      {
	info->update= 0;
	DBUG_RETURN(my_errno= HA_ERR_END_OF_FILE);
      }
    }
    hp_find_record(info, pos);
  }
  if (!info->current_ptr[share->visible])
  {
    DBUG_PRINT("warning",("Found deleted record"));
    info->update= HA_STATE_PREV_FOUND | HA_STATE_NEXT_FOUND;
    DBUG_RETURN(my_errno=HA_ERR_RECORD_DELETED);
  }
  /*
    Skip blob continuation runs.  Rec 0 of each run has the flags byte
    with HP_ROW_IS_CONT set; inner records (rec 1..N-1) have no flags
    byte.  Read run_rec_count from the header and skip the entire run.
  */
  if (hp_is_cont(info->current_ptr, share->visible))
  {
    /*
      Case A (HP_BLOB_CASE_A_SINGLE_REC): single record, no header — skip
      just this one record.
      Case B/C: read run_rec_count from header and skip the entire run.
    */
    if (hp_blob_run_format(info->current_ptr, share->visible)
        != HP_BLOB_CASE_A_SINGLE_REC)
    {
      uint16 run_rec_count= hp_cont_rec_count(info->current_ptr);
      if (run_rec_count > 1)
      {
        uint skip= run_rec_count - 1;
        info->current_record+= skip;
        info->current_ptr+= skip * share->block.recbuffer;
      }
    }
    info->update= HA_STATE_PREV_FOUND | HA_STATE_NEXT_FOUND;
    DBUG_RETURN(my_errno=HA_ERR_RECORD_DELETED);
  }
  info->update= HA_STATE_PREV_FOUND | HA_STATE_NEXT_FOUND | HA_STATE_AKTIV;
  memcpy(record,info->current_ptr,(size_t) share->reclength);
  if (share->blob_count && hp_read_blobs(info, record, info->current_ptr))
    DBUG_RETURN(my_errno);
  info->current_hash_ptr=0;			/* Can't use read_next */
  DBUG_RETURN(0);
} /* heap_scan */
