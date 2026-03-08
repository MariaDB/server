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
  my_bool auto_key_changed= 0, key_changed= 0;
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
  for (keydef= share->keydef, end= keydef + share->keys; keydef < end; keydef++)
  {
    if (hp_rec_key_cmp(keydef, old, heap_new, NULL))
    {
      if ((*keydef->delete_key)(info, keydef, old, pos, keydef == p_lastinx) ||
          (*keydef->write_key)(info, keydef, heap_new, pos))
        goto err;
      if (share->auto_key == (uint) (keydef - share->keydef + 1))
        auto_key_changed= 1;
    }
  }

  /*
    Blob update strategy: write new chains before freeing old ones.

    We must not free old blob chains before the new ones are successfully
    written, because hp_write_blobs() can fail (e.g. table full) and then
    the old data would be unrecoverable.  Instead:
    1. Save old chain head pointers (from pos) before memcpy overwrites them
    2. memcpy new record data into pos
    3. Write new blob chains (hp_write_blobs)
    4. On success: free old chains via saved pointers
       On failure: restore old record from 'old' buffer, restore saved
       chain pointers, re-set HP_ROW_HAS_CONT flag
  */
  if (share->blob_count)
  {
    my_bool had_cont= hp_has_cont(pos, share->visible);
    uchar **saved_chains= NULL;

    if (had_cont)
    {
      saved_chains= (uchar**) my_safe_alloca(
        share->blob_count * sizeof(uchar*));
      for (uint i= 0; i < share->blob_count; i++)
      {
        HP_BLOB_DESC *desc= &share->blob_descs[i];
        memcpy(&saved_chains[i], pos + desc->offset + desc->packlength,
               sizeof(saved_chains[i]));
      }
    }
    memcpy(pos, heap_new, (size_t) share->reclength);
    if (hp_write_blobs(info, heap_new, pos))
    {
      /* New blobs cleaned up by hp_write_blobs rollback. Restore old record. */
      memcpy(pos, old, (size_t) share->reclength);
      if (had_cont)
      {
        for (uint i= 0; i < share->blob_count; i++)
        {
          HP_BLOB_DESC *desc= &share->blob_descs[i];
          memcpy(pos + desc->offset + desc->packlength,
                 &saved_chains[i], sizeof(saved_chains[i]));
        }
        pos[share->visible]|= HP_ROW_HAS_CONT;
      }
      my_safe_afree(saved_chains,
                    share->blob_count * sizeof(uchar*));
      goto err;
    }
    /* New blobs written — now safe to free old chains */
    if (had_cont)
    {
      for (uint i= 0; i < share->blob_count; i++)
        hp_free_run_chain(share, saved_chains[i]);
      my_safe_afree(saved_chains,
                    share->blob_count * sizeof(uchar*));
    }
    /*
      Refresh blob pointers in the caller's record buffer when zero-copy
      pointers were used.

      hp_write_blobs() stored new chain head pointers in pos, but
      heap_new may still have zero-copy pointers from the caller's last
      hp_read_blobs() — those point into old chains that were just freed.
      Copy new chain pointers from pos into heap_new, then call
      hp_read_blobs() to replace them with materialized data pointers.

      Without this, callers that reuse heap_new after update (e.g., the
      INTERSECT ALL unfold path in sql_union.cc) would follow dangling
      pointers into freed HP_BLOCK records.

      Non-zero-copy blobs (Case C) have pointers into blob_buff which
      is not affected by the chain free, so no refresh is needed.
    */
    if (info->has_zerocopy_blobs)
    {
      uchar *new_rec= (uchar*) heap_new;
      for (uint i= 0; i < share->blob_count; i++)
      {
        HP_BLOB_DESC *desc= &share->blob_descs[i];
        {
          uchar *chain;
          memcpy(&chain, pos + desc->offset + desc->packlength, sizeof(chain));
          memcpy(new_rec + desc->offset + desc->packlength, &chain,
                 sizeof(chain));
        }
      }
      hp_read_blobs(info, new_rec, pos);
    }
  }
  else
  {
    memcpy(pos, heap_new, (size_t) share->reclength);
  }
  if (++(share->records) == share->blength) share->blength+= share->blength;

#if !defined(DBUG_OFF) && defined(EXTRA_HEAP_DEBUG)
  DBUG_EXECUTE("check_heap",heap_check_heap(info, 0););
#endif
  if (auto_key_changed)
    heap_update_auto_increment(info, heap_new);
  if (key_changed)
    share->key_version++;
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
      if (hp_rec_key_cmp(keydef, old, heap_new, NULL))
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
