/* Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.

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

/* remove current record in heap-database */

#include "heapdef.h"

/*
  Push a contiguous block of deleted records onto the free list.
  'first' is the lowest address; the block spans count records
  at stride share->block.recbuffer.
*/

void hp_push_free_block(HP_SHARE *share, uchar *first, uint16 count)
{
  uint recbuffer= share->block.recbuffer;
  uint visible= share->visible;
  uchar *last= first + (uint32)(count - 1) * recbuffer;

  DBUG_ASSERT(count >= 2);

  /* First record: block-start with chain to next free-list entry */
  *((uchar**) first)= share->del_link;
  first[HP_DEL_FLAG_OFFSET]= HP_DEL_BLOCK_START;
  int2store(first + HP_DEL_COUNT_OFFSET, count);
  first[visible]= 0;

  /* Dark records: clear metadata bytes at stride recbuffer */
  hp_clear_dark_records(first + recbuffer, last, recbuffer, visible);

  /* Last record: block-end with back-pointer to first */
  *((uchar**) last)= first;
  last[HP_DEL_FLAG_OFFSET]= HP_DEL_BLOCK_END;
  last[visible]= 0;

  share->del_link= last;
  share->deleted+= count;
  share->total_records-= count;
}


/*
  Push a contiguous block (count >= 2) or single record (count == 1)
  onto the free list, coalescing with the head entry if adjacent.
  Normalizes the head by treating a single record as a block of count 1,
  then checks adjacency in two directions (above/below).
  Falls back to hp_push_free_block/hp_push_free_record when no adjacency.
*/

void hp_push_free_block_coalesce(HP_SHARE *share, uchar *first,
                                 uint16 count)
{
  uchar *head= share->del_link;
  uint recbuffer= share->block.recbuffer;
  uchar *last= first + (uint32)(count - 1) * recbuffer;

  DBUG_ASSERT(count >= 1);

  if (head)
  {
    /* Normalize head: treat single record as a block of count 1 */
    uchar *head_first;
    uint16 head_count;
    uint32 combined;

    if (hp_is_free_block_end(head))
    {
      head_first= hp_free_block_first(head);
      head_count= hp_free_block_start_count(head_first);
    }
    else
    {
      head_first= head;
      head_count= 1;
    }
    combined= (uint32) head_count + count;

    if (combined <= UINT_MAX16)
    {
      if (head == first - recbuffer)
      {
        /* Head immediately below new block: extend upward */
        hp_clear_dark_records((head_count > 1 ? head : first), last,
                              recbuffer, share->visible);

        *((uchar**) last)= head_first;
        last[HP_DEL_FLAG_OFFSET]= HP_DEL_BLOCK_END;
        last[share->visible]= 0;

        head_first[HP_DEL_FLAG_OFFSET]= HP_DEL_BLOCK_START;
        int2store(head_first + HP_DEL_COUNT_OFFSET, (uint16) combined);

        share->del_link= last;
        share->deleted+= count;
        share->total_records-= count;
        return;
      }

      if (head_first == last + recbuffer)
      {
        /* Head immediately above new block: extend downward */
        uchar *next_entry= *((uchar**) head_first);

        hp_clear_dark_records(first + recbuffer, head_first + recbuffer,
                              recbuffer, share->visible);

        *((uchar**) first)= next_entry;
        first[HP_DEL_FLAG_OFFSET]= HP_DEL_BLOCK_START;
        int2store(first + HP_DEL_COUNT_OFFSET, (uint16) combined);
        first[share->visible]= 0;

        *((uchar**) head)= first;
        head[HP_DEL_FLAG_OFFSET]= HP_DEL_BLOCK_END;

        share->deleted+= count;
        share->total_records-= count;
        return;
      }
    }
  }

  if (count == 1)
    hp_push_free_record(share, first);
  else
    hp_push_free_block(share, first, count);
}


int heap_delete(HP_INFO *info, const uchar *record)
{
  uchar *pos;
  HP_SHARE *share=info->s;
  HP_KEYDEF *keydef, *end, *p_lastinx;
  DBUG_ENTER("heap_delete");
  DBUG_PRINT("enter",("info: %p  record: %p", info, record));

  test_active(info);
  hp_flush_pending_blob_free(info);

  if (info->opt_flag & READ_CHECK_USED && hp_rectest(info,record))
    DBUG_RETURN(my_errno);			/* Record changed */
  share->changed=1;

  if ( --(share->records) < share->blength >> 1) share->blength>>=1;
  pos=info->current_ptr;

  p_lastinx = share->keydef + info->lastinx;
  for (keydef = share->keydef, end = keydef + share->keys; keydef < end; 
       keydef++)
  {
    if ((*keydef->delete_key)(info, keydef, record, pos, keydef == p_lastinx))
      goto err;
  }

  if (share->blob_count && hp_has_cont(pos, share->visible))
  {
    if (share->internal)
    {
      /*
        Internal temporary tables are never binlogged, so blob chains
        can be freed immediately.
      */
      hp_free_blobs(share, pos);
    }
    else
    {
      /*
        Defer blob chain free: save chain pointers for later cleanup.

        The handler layer calls binlog_log_row() AFTER delete_row()
        returns, reading blob data from the record buffer via zero-copy
        pointers into HP_BLOCK chain records.  Freeing chains here would
        overwrite those records with del_link pointers, making the
        zero-copy pointers dangle.

        Save the chain head pointers and free them on the next mutating
        operation (write/update/delete) or on reset/close.
      */
      HP_BLOB_DESC *desc;
      uint i;
      for (i= 0, desc= share->blob_descs; i < share->blob_count; i++, desc++)
      {
        if (hp_blob_length(desc, pos) == 0)
        {
          info->pending_blob_chains[i]= NULL;
          continue;
        }
        memcpy(&info->pending_blob_chains[i],
               pos + desc->offset + desc->packlength, sizeof(uchar*));
      }
      info->has_pending_blob_free= TRUE;
    }
  }
  info->update=HA_STATE_DELETED;
  hp_push_free_record_coalesce(share, pos);
  share->key_version++;
#if !defined(DBUG_OFF) && defined(EXTRA_HEAP_DEBUG)
  DBUG_EXECUTE("check_heap",heap_check_heap(info, 0););
#endif

  DBUG_RETURN(0);
err:
  if (++(share->records) == share->blength)
    share->blength+= share->blength;
  DBUG_RETURN(my_errno);
}


/*
  Remove one key from rb-tree
*/

int hp_rb_delete_key(HP_INFO *info, register HP_KEYDEF *keyinfo,
		   const uchar *record, uchar *recpos, int flag)
{
  heap_rb_param custom_arg;
  size_t old_allocated;
  int res;

  if (flag) 
    info->last_pos= NULL; /* For heap_rnext/heap_rprev */

  custom_arg.keyseg= keyinfo->seg;
  custom_arg.key_length= hp_rb_make_key(keyinfo, info->recbuf, record, recpos);
  custom_arg.search_flag= SEARCH_SAME;
  old_allocated= keyinfo->rb_tree.allocated;
  res= tree_delete(&keyinfo->rb_tree, info->recbuf, custom_arg.key_length,
                   &custom_arg);
  info->s->index_length-= (old_allocated - keyinfo->rb_tree.allocated);
  return res;
}


/*
  Remove one key from hash-table

  SYNPOSIS
    hp_delete_key()
    info		Hash handler
    keyinfo		key definition of key that we want to delete
    record		row data to be deleted
    recpos		Pointer to heap record in memory
    flag		Is set if we want's to correct info->current_ptr

  RETURN
    0      Ok
    other  Error code
*/

int hp_delete_key(HP_INFO *info, register HP_KEYDEF *keyinfo,
		  const uchar *record, uchar *recpos, int flag)
{
  ulong blength, pos2, pos_hashnr, lastpos_hashnr, key_pos, rec_hash;
  HASH_INFO *lastpos,*gpos,*pos,*pos3,*empty,*last_ptr;
  HP_SHARE *share=info->s;
  DBUG_ENTER("hp_delete_key");

  blength=share->blength;
  if (share->records+1 == blength)
    blength+= blength;
  lastpos=hp_find_hash(&keyinfo->block,share->records);
  last_ptr=0;

  /* Search after record with key */
  rec_hash= hp_rec_hashnr(0, keyinfo, record);
  key_pos= hp_mask(rec_hash, blength, share->records + 1);
  pos= hp_find_hash(&keyinfo->block, key_pos);

  gpos = pos3 = 0;

  while (pos->ptr_to_rec != recpos)
  {
    /*
      Hash pre-check avoids expensive blob materialization
      for non-matching entries.
    */
    if (flag && pos->hash_of_key == rec_hash &&
        !hp_rec_key_cmp(keyinfo, record, pos->ptr_to_rec, info))
      last_ptr=pos;				/* Previous same key */
    gpos=pos;
    if (!(pos=pos->next_key))
    {
      DBUG_RETURN(my_errno=HA_ERR_CRASHED);	/* This shouldn't happend */
    }
  }

  /* Remove link to record */

  if (flag)
  {
    /* Save for heap_rnext/heap_rprev */
    info->current_hash_ptr=last_ptr;
    info->current_ptr = last_ptr ? last_ptr->ptr_to_rec : 0;
    DBUG_PRINT("info",("Corrected current_ptr to point at: %p",
		       info->current_ptr));
  }
  empty=pos;
  if (gpos)
    gpos->next_key=pos->next_key;	/* unlink current ptr */
  else if (pos->next_key)
  {
    empty=pos->next_key;
    pos->ptr_to_rec=  empty->ptr_to_rec;
    pos->next_key=    empty->next_key;
    pos->hash_of_key= empty->hash_of_key;
  }
  else
    keyinfo->hash_buckets--;

  if (empty == lastpos)			/* deleted last hash key */
    DBUG_RETURN (0);

  /* Move the last key (lastpos) */
  lastpos_hashnr= lastpos->hash_of_key;
  /* pos is where lastpos should be */
  pos=hp_find_hash(&keyinfo->block, hp_mask(lastpos_hashnr, share->blength,
					    share->records));
  if (pos == empty)			/* Move to empty position. */
  {
    empty[0]=lastpos[0];
    DBUG_RETURN(0);
  }
  pos_hashnr= pos->hash_of_key;
  /* pos3 is where the pos should be */
  pos3= hp_find_hash(&keyinfo->block,
		     hp_mask(pos_hashnr, share->blength, share->records));
  if (pos != pos3)
  {					/* pos is on wrong posit */
    empty[0]=pos[0];			/* Save it here */
    pos[0]=lastpos[0];			/* This shold be here */
    hp_movelink(pos, pos3, empty);	/* Fix link to pos */
    DBUG_RETURN(0);
  }
  pos2= hp_mask(lastpos_hashnr, blength, share->records + 1);
  if (pos2 == hp_mask(pos_hashnr, blength, share->records + 1))
  {
    /* lastpos and the row in the main bucket entry (pos) has the same hash */ 
    if (pos2 != share->records)
    {
      /*
        The bucket entry was not deleted. Copy lastpos over the
        deleted entry and update previous link to point to it.
      */
      empty[0]= lastpos[0];
      hp_movelink(lastpos, pos, empty);
      if (last_ptr == lastpos)
      {
        /*
          We moved the row that info->current_hash_ptr points to.
          Update info->current_hash_ptr to point to the new position.
        */
        info->current_hash_ptr= empty;
      }
      DBUG_RETURN(0);
    }
    /*
      Shrinking the hash table deleted the main bucket entry for this hash.
      In this case the last entry was the first key in the key chain.
      We move things around so that we keep the original key order to ensure
      that heap_rnext() works.
      
      - Move the row at the main bucket entry to the empty spot.
      - Move the last entry first in the new chain.
      - Link in the first element of the hash.
    */
    empty[0]= pos[0];
    pos[0]= lastpos[0];
    hp_movelink(pos, pos, empty);

    /* Update current_hash_ptr if the entry moved */
    if (last_ptr == lastpos)
      info->current_hash_ptr= pos;
    else if (last_ptr == pos)
      info->current_hash_ptr= empty;
    DBUG_RETURN(0);
  }

  pos3= 0;				/* Different positions merge */
  keyinfo->hash_buckets--;
  empty[0]=lastpos[0];
  hp_movelink(pos3, empty, pos->next_key);
  pos->next_key=empty;
  DBUG_RETURN(0);
}
