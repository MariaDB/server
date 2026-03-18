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
    Blob update strategy: skip unchanged blobs, write-before-free for
    changed ones.

    Compare each blob column (length, then pointer, then memcmp) to
    detect changes.  Unchanged blobs keep their existing chains.
    Changed blobs get new chains written before old ones are freed.

    The bulk memcpy of heap_new into pos overwrites blob chain pointers
    with SQL-layer data pointers, so we save old chain pointers first
    and restore them for unchanged blobs afterward.
  */
  if (share->blob_count)
  {
    my_bool had_cont= hp_has_cont(pos, share->visible);
    uint alloc_size= share->blob_count * (sizeof(uchar*) + sizeof(my_bool));
    uchar **saved_chains= (uchar**) my_safe_alloca(alloc_size);
    my_bool *blob_changed= (my_bool*)(saved_chains + share->blob_count);
    my_bool any_changed= FALSE;
    my_bool has_blob_data= FALSE;
    uint i;

    /* Save old chain pointers and detect which blobs changed */
    for (i= 0; i < share->blob_count; i++)
    {
      HP_BLOB_DESC *desc= &share->blob_descs[i];
      uint32 old_len, new_len;

      saved_chains[i]= NULL;
      if (had_cont)
        memcpy(&saved_chains[i], pos + desc->offset + desc->packlength,
               sizeof(saved_chains[i]));

      old_len= hp_blob_length(desc, old);
      new_len= hp_blob_length(desc, heap_new);

      if (old_len != new_len)
        blob_changed[i]= TRUE;
      else if (old_len == 0)
        blob_changed[i]= FALSE;
      else
      {
        const uchar *old_data, *new_data;
        memcpy(&old_data, old + desc->offset + desc->packlength,
               sizeof(old_data));
        memcpy(&new_data, heap_new + desc->offset + desc->packlength,
               sizeof(new_data));
        blob_changed[i]= (old_data != new_data &&
                           memcmp(old_data, new_data, old_len) != 0);
      }
      if (blob_changed[i])
        any_changed= TRUE;
    }

    memcpy(pos, heap_new, (size_t) share->reclength);

    /* Write new chains for changed blobs, restore old pointers for unchanged */
    for (i= 0; i < share->blob_count; i++)
    {
      HP_BLOB_DESC *desc= &share->blob_descs[i];

      if (!blob_changed[i])
      {
        /* Restore old chain pointer that memcpy overwrote */
        if (saved_chains[i])
        {
          memcpy(pos + desc->offset + desc->packlength,
                 &saved_chains[i], sizeof(saved_chains[i]));
          has_blob_data= TRUE;
        }
        continue;
      }

      {
        uint32 new_len= hp_blob_length(desc, heap_new);
        if (new_len == 0)
        {
          uchar *null_ptr= NULL;
          memcpy(pos + desc->offset + desc->packlength,
                 &null_ptr, sizeof(null_ptr));
        }
        else
        {
          const uchar *data_ptr;
          uchar *first_run;

          has_blob_data= TRUE;
          memcpy(&data_ptr, heap_new + desc->offset + desc->packlength,
                 sizeof(data_ptr));

          if (hp_write_one_blob(share, data_ptr, new_len, &first_run))
          {
            /* Rollback: free new chains already written, restore old record */
            uint j;
            for (j= 0; j < i; j++)
              if (blob_changed[j])
              {
                uchar *chain;
                memcpy(&chain, pos + share->blob_descs[j].offset +
                       share->blob_descs[j].packlength, sizeof(chain));
                if (chain)
                  hp_free_run_chain(share, chain);
              }
            memcpy(pos, old, (size_t) share->reclength);
            if (had_cont)
            {
              for (j= 0; j < share->blob_count; j++)
                memcpy(pos + share->blob_descs[j].offset +
                       share->blob_descs[j].packlength,
                       &saved_chains[j], sizeof(saved_chains[j]));
              pos[share->visible]|= HP_ROW_HAS_CONT;
            }
            my_safe_afree(saved_chains, alloc_size);
            goto err;
          }
          memcpy(pos + desc->offset + desc->packlength,
                 &first_run, sizeof(first_run));
        }
      }
    }

    if (any_changed)
    {
      /* Set flags and free old chains for changed blobs */
      pos[share->visible]= has_blob_data ?
        (HP_ROW_ACTIVE | HP_ROW_HAS_CONT) : HP_ROW_ACTIVE;
      for (i= 0; i < share->blob_count; i++)
        if (blob_changed[i] && saved_chains[i])
          hp_free_run_chain(share, saved_chains[i]);
    }
    else if (had_cont)
      pos[share->visible]|= HP_ROW_HAS_CONT;

    /*
      Refresh blob pointers in the caller's record buffer.

      For changed blobs, pos has new chain pointers that heap_new
      doesn't know about yet.  Copy all chain pointers from pos into
      heap_new and call hp_read_blobs() to re-materialize.

      Without this, callers that reuse heap_new after update (e.g., the
      INTERSECT ALL unfold path in sql_union.cc) would follow dangling
      pointers into freed HP_BLOCK records.
    */
    if (any_changed || info->has_zerocopy_blobs)
    {
      uchar *new_rec= (uchar*) heap_new;
      for (i= 0; i < share->blob_count; i++)
      {
        HP_BLOB_DESC *desc= &share->blob_descs[i];
        uchar *chain;
        memcpy(&chain, pos + desc->offset + desc->packlength, sizeof(chain));
        memcpy(new_rec + desc->offset + desc->packlength, &chain,
               sizeof(chain));
      }
      hp_read_blobs(info, new_rec, pos);
    }

    my_safe_afree(saved_chains, alloc_size);
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
