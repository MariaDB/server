/* Copyright (C) 2006 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* close a isam-database */
/*
  TODO:
   We need to have a separate mutex on the closed file to allow other threads
   to open other files during the time we flush the cache and close this file
*/

#include "ma_ftdefs.h"
#include "ma_crypt.h"
#ifdef WITH_S3_STORAGE_ENGINE
#include "s3_func.h"
#endif /* WITH_S3_STORAGE_ENGINE */

int maria_close(register MARIA_HA *info)
{
  int error=0,flag;
  MARIA_SHARE *share= info->s;
  my_bool internal_table= share->internal_table;
  DBUG_ENTER("maria_close");
  DBUG_PRINT("enter",("name: '%s'  base: %p reopen: %u  locks: %u",
                      share->open_file_name.str,
		      info, (uint) share->reopen,
                      (uint) share->tot_locks));

  /* Check that we have unlocked key delete-links properly */
  DBUG_ASSERT(info->key_del_used == 0);
  /* Check that file is not part of any uncommitted transactions */
  DBUG_ASSERT(info->trn == 0 || info->trn == &dummy_transaction_object);

  if (share->reopen == 1)
  {
    /*
      If we are going to close the file, flush page cache without
      a global mutex
    */
    if (flush_pagecache_blocks(share->pagecache, &share->kfile,
                       share->deleting ? FLUSH_IGNORE_CHANGED : FLUSH_RELEASE))
      error= my_errno;
  }

  /* Ensure no one can open this file while we are closing it */
  if (!internal_table)
    mysql_mutex_lock(&THR_LOCK_maria);
  if (info->lock_type == F_EXTRA_LCK)
    info->lock_type=F_UNLCK;			/* HA_EXTRA_NO_USER_CHANGE */

  if (info->lock_type != F_UNLCK)
  {
    if (maria_lock_database(info,F_UNLCK))
      error=my_errno;
  }
  if (!internal_table)
  {
    mysql_mutex_lock(&share->close_lock);
    mysql_mutex_lock(&share->intern_lock);
  }

  if (share->options & HA_OPTION_READ_ONLY_DATA)
  {
    share->r_locks--;
    share->tot_locks--;
  }
  if (info->opt_flag & (READ_CACHE_USED | WRITE_CACHE_USED))
  {
    if (end_io_cache(&info->rec_cache))
      error=my_errno;
    info->opt_flag&= ~(READ_CACHE_USED | WRITE_CACHE_USED);
  }
  flag= !--share->reopen;
  if (!internal_table)
  {
    maria_open_list=  list_delete(maria_open_list,&info->open_list);
    share->open_list= list_delete(share->open_list, &info->share_list);
  }

  maria_ftparser_call_deinitializer(info);
  my_free(info->rec_buff);
  (*share->end)(info);

  if (flag)
  {
    /* Last close of file */

    /*
      Check that we don't have any dangling open files
      We may still have some open transactions. In this case the share
      will be kept around until the transaction has closed
    */
    DBUG_ASSERT(share->open_list == 0);

    /* Flush everything */
    if (share->kfile.file >= 0)
    {
      my_bool save_global_changed= share->global_changed;

      /* Avoid _ma_mark_file_changed() when flushing pages */
      share->global_changed= 1;

      /* Flush page cache if BLOCK format */
      if ((*share->once_end)(share))
        error= my_errno;
      /*
        Extra flush, just in case someone opened and closed the file
        since the start of the function (very unlikely)
      */
      if (flush_pagecache_blocks(share->pagecache, &share->kfile,
                        share->deleting ? FLUSH_IGNORE_CHANGED : FLUSH_RELEASE))
        error= my_errno;
      unmap_file(info);
      if (!internal_table &&
          (((share->changed && share->base.born_transactional) ||
            maria_is_crashed(info) ||
            (share->temporary && !share->deleting))))
      {
        if (save_global_changed)
        {
          /*
            Reset effect of _ma_mark_file_changed(). Better to do it
            here than in _ma_decrement_open_count(), as
            _ma_state_info_write() will write the open_count.
           */
          save_global_changed= 0;
          share->state.open_count--;
        }        
        /*
          State must be written to file as it was not done at table's
          unlocking.
        */
        if (_ma_state_info_write(share, MA_STATE_INFO_WRITE_DONT_MOVE_OFFSET))
          error= my_errno;
      }
      DBUG_ASSERT(maria_is_crashed(info) || !share->base.born_transactional ||
                  share->state.open_count == 0 ||
                  share->open_count_not_zero_on_open);

      /* Ensure that open_count is zero on close */
      share->global_changed= save_global_changed;
      _ma_decrement_open_count(info, 0);

      /* Ensure that open_count really is zero */
      DBUG_ASSERT(maria_is_crashed(info) || share->temporary ||
                  share->state.open_count == 0 ||
                  share->open_count_not_zero_on_open);

      /*
        File must be synced as it is going out of the maria_open_list and so
        becoming unknown to future Checkpoints.
      */
      if (share->now_transactional &&
          mysql_file_sync(share->kfile.file, MYF(MY_WME)))
        error= my_errno;
      if (!share->s3_path && mysql_file_close(share->kfile.file, MYF(0)))
        error= my_errno;
    }
    thr_lock_delete(&share->lock);
    mysql_mutex_destroy(&share->key_del_lock);

    {
      int i,keys;
      keys = share->state.header.keys;
      mysql_rwlock_destroy(&share->mmap_lock);
      for(i=0; i<keys; i++) {
	mysql_rwlock_destroy(&share->keyinfo[i].root_lock);
      }
    }
    DBUG_ASSERT(share->now_transactional == share->base.born_transactional);
    /*
      We assign -1 because checkpoint does not need to flush (in case we
      have concurrent checkpoint if no then we do not need it here also)
    */
    share->kfile.file= -1;

    /*
      Remember share->history for future opens

      We have to unlock share->intern_lock then lock it after
      LOCK_trn_list (trnman_lock()) to avoid dead locks.
    */
    if (!internal_table)
      mysql_mutex_unlock(&share->intern_lock);
    _ma_remove_not_visible_states_with_lock(share, TRUE);
    if (!internal_table)
      mysql_mutex_lock(&share->intern_lock);

    if (share->in_checkpoint & MARIA_CHECKPOINT_LOOKS_AT_ME)
    {
      /* we cannot my_free() the share, Checkpoint would see a bad pointer */
      share->in_checkpoint|= MARIA_CHECKPOINT_SHOULD_FREE_ME;
    }

    if (share->state_history)
    {
      if (share->state_history->trid)           /* If not visible for all */
      {
        MARIA_STATE_HISTORY_CLOSED *history;
        DBUG_PRINT("info", ("Storing state history"));
        /*
          Here we ignore the unlikely case that we don't have memory
          to store the state. In the worst case what happens is that
          any transaction that tries to access this table will get a
          wrong status information.
        */
        if ((history= (MARIA_STATE_HISTORY_CLOSED *)
             my_malloc(PSI_INSTRUMENT_ME, sizeof(*history), MYF(MY_WME))))
        {
          history->create_rename_lsn= share->state.create_rename_lsn;
          history->state_history= share->state_history;
          if (my_hash_insert(&maria_stored_state, (uchar*) history))
            my_free(history);
        }
      }
      else
        my_free(share->state_history);
      /* Marker for concurrent checkpoint */
      share->state_history= 0;
    }
  }
  if (!internal_table)
  {
    mysql_mutex_unlock(&THR_LOCK_maria);
    mysql_mutex_unlock(&share->close_lock);
  }

  /* free_maria_share will free share->internal_lock */
  free_maria_share(share);

  my_free(info->ftparser_param);
  if (info->dfile.file >= 0 && ! info->s3)
  {
    /*
      This is outside of mutex so would confuse a concurrent
      Checkpoint. Fortunately in BLOCK_RECORD we close earlier under mutex.
    */
    if (mysql_file_close(info->dfile.file, MYF(0)))
      error= my_errno;
  }

  delete_dynamic(&info->pinned_pages);
#ifdef WITH_S3_STORAGE_ENGINE
  if (info->s3)
    ms3_deinit(info->s3);
#endif /* WITH_S3_STORAGE_ENGINE */
  my_free(info);

  if (error)
  {
    DBUG_PRINT("error", ("Got error on close: %d", my_errno));
    DBUG_RETURN(my_errno= error);
  }
  DBUG_RETURN(0);
} /* maria_close */


/**
  Free Aria table share

  Note that share will not be freed a long as there are active checkpoints
  or transactions pointing at the shared object
*/

void free_maria_share(MARIA_SHARE *share)
{
  if (!share->internal_table)
    mysql_mutex_assert_owner(&share->intern_lock);

  if (!share->reopen && !share->in_trans &&
      !(share->in_checkpoint & MARIA_CHECKPOINT_SHOULD_FREE_ME))
  {
    /* No one can access this share anymore, time to delete it ! */
    if (!share->internal_table)
      mysql_mutex_unlock(&share->intern_lock);
    ma_crypt_free(share);
    my_free(share->s3_path);
    (void) mysql_mutex_destroy(&share->intern_lock);
    (void) mysql_mutex_destroy(&share->close_lock);
    (void) mysql_cond_destroy(&share->key_del_cond);
    my_free(share);
    return;
  }
  if (!share->internal_table)
    mysql_mutex_unlock(&share->intern_lock);
  return;
}
