/* Copyright (c) 2000-2002, 2004-2008 MySQL AB
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

/* Update current record in heap-database */

#include "heapdef.h"

int heap_update(HP_INFO *info, const uchar *old, const uchar *heap_new)
{
  HP_KEYDEF *keydef, *end, *p_lastinx;
  uchar *pos, *recovery_ptr;
  struct st_hp_hash_info *recovery_hash_ptr;
  my_bool auto_key_changed= 0, key_changed= 0, lastinx_changed= 0;
  HP_SHARE *share= info->s;
  DBUG_ENTER("heap_update");

  test_active(info);
  pos=info->current_ptr;

  if (info->opt_flag & READ_CHECK_USED && hp_rectest(info,old))
    DBUG_RETURN(my_errno);				/* Record changed */
  if (--(share->records) < share->blength >> 1) share->blength>>= 1;
  share->changed=1;

  // Save the cursor position to recover if insert fails.
  recovery_ptr= info->current_ptr;
  recovery_hash_ptr= info->current_hash_ptr;

  p_lastinx= share->keydef + info->lastinx;
  /* Re-index the record on every key whose value differs between old and new */
  for (keydef= share->keydef, end= keydef + share->keys; keydef < end; keydef++)
  {
    /* Skip keys that are unchanged by this update */
    if (hp_rec_key_cmp(keydef, old, heap_new))
    {
      /* Remove the old key entry, then insert the new one */
      if ((*keydef->delete_key)(info, keydef, old, pos, keydef == p_lastinx) ||
          (*keydef->write_key)(info, keydef, heap_new, pos))
        goto err;
      key_changed= 1;
      /*
        p_lastinx is the index the caller is currently scanning (info->lastinx),
        the one holding the live cursor. Detect when its key moved, so we can fix
        up the scan state below.
      */
      if (keydef == p_lastinx)
        lastinx_changed= 1;
      if (share->auto_key == (uint) (keydef - share->keydef + 1))
        auto_key_changed= 1;
    }
  }

  memcpy(pos,heap_new,(size_t) share->reclength);
  if (++(share->records) == share->blength) share->blength+= share->blength;

#if !defined(DBUG_OFF) && defined(EXTRA_HEAP_DEBUG)
  DBUG_EXECUTE("check_heap",heap_check_heap(info, 0););
#endif
  if (auto_key_changed)
    heap_update_auto_increment(info, heap_new);
  if (key_changed)
    share->key_version++;
  if (lastinx_changed && (info->update & HA_STATE_NEXT_FOUND))
  {
    /*
      The scanned-index key of the current record changed in place (e.g. a
      versioned DELETE bumping row_end while scanning row_end), so the record
      left its position mid-scan. Clear HA_STATE_NEXT_FOUND, otherwise the next
      heap_rnext() hits the "!current_ptr && HA_STATE_NEXT_FOUND" guard in its
      hash (non-BTREE) branch, reports a false end-of-file and leaves a row
      behind. This is the same scan-state contract heap_delete() already
      maintains for heap_rnext() (it sets info->update there too); an
      in-place update that moves the scanned key must keep it consistent
      for the same reason. We only get here on an index scan; rnd scans
      (heap_scan()/heap_rrnd()) leave lastinx == -1, so despite also setting
      HA_STATE_NEXT_FOUND they never reach this. A plain single-row UPDATE has
      no preceding heap_rnext(), so the bit is unset and this is a no-op there.

      See also: mi_extra.c/ma_extra.c HA_EXTRA_RESTORE_POS (MDEV-41050).
    */
    info->update&= ~HA_STATE_NEXT_FOUND;
  }
  DBUG_RETURN(0);

 err:
  if (my_errno == HA_ERR_FOUND_DUPP_KEY)
  {
    info->errkey = (int) (keydef - share->keydef);
    if (keydef->algorithm == HA_KEY_ALG_BTREE)
    {
      /* we don't need to delete non-inserted key from rb-tree */
      if ((*keydef->write_key)(info, keydef, old, pos))
      {
        if (++(share->records) == share->blength)
	  share->blength+= share->blength;
        DBUG_RETURN(my_errno);
      }
      keydef--;
    }
    while (keydef >= share->keydef)
    {
      if (hp_rec_key_cmp(keydef, old, heap_new))
      {
	if ((*keydef->delete_key)(info, keydef, heap_new, pos, 0) ||
	    (*keydef->write_key)(info, keydef, old, pos))
	  break;
      }
      keydef--;
    }
    info->current_ptr= recovery_ptr;
    info->current_hash_ptr= recovery_hash_ptr;
  }
  if (++(share->records) == share->blength)
    share->blength+= share->blength;
  DBUG_RETURN(my_errno);
} /* heap_update */
