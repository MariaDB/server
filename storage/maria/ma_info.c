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

/* Return useful base information for an open table */

#include "maria_def.h"
#include <mysys_err.h>
#ifdef	_WIN32
#include <sys/stat.h>
#endif

uint maria_max_key_length()
{
  uint tmp= (_ma_max_key_length() - 8 - HA_MAX_KEY_SEG*3);
  return MY_MIN(MARIA_MAX_KEY_LENGTH, tmp);
}

/* Get information about the table */
/* if flag == 2 one get current info (no sync from database */

int maria_status(MARIA_HA *info, register MARIA_INFO *x, uint flag)
{
  MY_STAT state;
  MARIA_SHARE *share= info->s;
  DBUG_ENTER("maria_status");
  DBUG_PRINT("info", ("records: %lld", info->state->records));

  x->recpos= info->cur_row.lastpos;
  if (flag == HA_STATUS_POS)
    DBUG_RETURN(0);				/* Compatible with ISAM */
  if (!(flag & HA_STATUS_NO_LOCK))
  {
    mysql_mutex_lock(&share->intern_lock);
    _ma_readinfo(info,F_RDLCK,0);
    fast_ma_writeinfo(info);
    mysql_mutex_unlock(&share->intern_lock);
  }
  if (flag & HA_STATUS_VARIABLE)
  {
    /* If table is locked, give versioned number otherwise last commited */
    if (info->lock_type == F_UNLCK)
      x->records         = share->state.state.records;
    else
      x->records         = info->state->records;
    x->deleted	 	= share->state.state.del;
    x->delete_length	= share->state.state.empty;
    x->data_file_length	= share->state.state.data_file_length;
    x->index_file_length= share->state.state.key_file_length;

    x->keys	 	= share->state.header.keys;
    x->check_time	= share->state.check_time;
    x->mean_reclength	= x->records ?
      (ulong) ((x->data_file_length - x->delete_length) /x->records) :
      (ulong) share->min_pack_length;
  }
  if (flag & HA_STATUS_ERRKEY)
  {
    x->errkey=       info->errkey;
    x->dup_key_pos=  info->dup_key_pos;
  }
  if (flag & HA_STATUS_CONST)
  {
    x->reclength	= share->base.reclength;
    x->max_data_file_length=share->base.max_data_file_length;
    x->max_index_file_length=info->s->base.max_key_file_length;
    x->filenr	 = info->dfile.file;
    x->options	 = share->options;
    x->create_time=share->state.create_time;
    x->reflength= share->base.rec_reflength;
    x->record_offset= (info->s->data_file_type == STATIC_RECORD ?
                       share->base.pack_reclength: 0);
    x->sortkey= -1;				/* No clustering */
    x->rec_per_key	= share->state.rec_per_key_part;
    x->key_map	 	= share->state.key_map;
    x->data_file_name   = share->data_file_name.str;
    x->index_file_name  = share->index_file_name.str;
    x->data_file_type   = share->data_file_type;
  }
  if ((flag & HA_STATUS_TIME) && !my_fstat(info->dfile.file, &state, MYF(0)))
    x->update_time=state.st_mtime;
  else
    x->update_time=0;
  if (flag & HA_STATUS_AUTO)
  {
    x->auto_increment= share->state.auto_increment+1;
    if (!x->auto_increment)			/* This shouldn't happen */
      x->auto_increment= ~(ulonglong) 0;
  }
  DBUG_RETURN(0);
}


/*
  Write a message to the user or the error log.

  SYNOPSIS
    _ma_report_error()
    file_name                   Name of table file (e.g. index_file_name).
    errcode                     Error number.
    flags                       Flags to my_error

  DESCRIPTION
    This function supplies my_error() with a table name. Most error
    messages need one. Since string arguments in error messages are limited
    to 64 characters by convention, we ensure that in case of truncation,
    that the end of the index file path is in the message. This contains
    the most valuable information (the table name and the database name).

  RETURN
    void
*/

void _ma_report_error(int errcode, const LEX_STRING *name, myf flags)
{
  size_t length;
  const char *file_name= name->str;
  DBUG_ENTER("_ma_report_error");
  DBUG_PRINT("enter",("error: %d  table: '%s'", errcode, file_name));

  if ((length= name->length) > 64)
  {
    /* we first remove the directory */
    size_t dir_length= dirname_length(file_name);
    file_name+= dir_length;
    if ((length-= dir_length) > 64)
    {
      /* still too long, chop start of table name */
      file_name+= length - 64;
    }
  }
  my_printf_error(errcode, "Got error '%iE' for '%s'",
                  flags, errcode, file_name);
  DBUG_VOID_RETURN;
}


/**
   If standalone report all errors to the user
   If run trough the Aria handler, only report first error to the user
   to not spam him

   @param info         Aria Handler
   @param error        Error code
   @apram write_to_log If set to 1, print the error to the log. This is only set
                       when a table was found to be crashed the first time
*/

void _ma_print_error(MARIA_HA *info, int error, my_bool write_to_log)
{
  DBUG_ENTER("_ma_print_error");
  DBUG_PRINT("error", ("error: %d  log: %d", error, write_to_log));
  if (!info->error_count++ || !maria_in_ha_maria || write_to_log)
  {
    MARIA_SHARE *share= info->s;
    _ma_report_error(error,
                     (share->index_file_name.length ?
                      &share->index_file_name :
                      &share->unique_file_name),
                     MYF(write_to_log ? ME_ERROR_LOG : 0));
  }
  DBUG_VOID_RETURN;
}


/*
  Handle a fatal error

  - Mark the table as crashed
  - Print an error message, if we had not issued an error message before
    that the table had been crashed.
  - set my_errno to error
  - If 'maria_assert_if_crashed_table is set, then assert.
*/

void _ma_set_fatal_error(MARIA_HA *info, int error)
{
  MARIA_SHARE *share= info->s;
  _ma_print_error(info, error,
                  (share->state.changed & STATE_CRASHED_PRINTED) == 0);
  maria_mark_crashed_share(share);
  share->state.changed|= STATE_CRASHED_PRINTED;
  my_errno= error;
  DBUG_ASSERT(!maria_assert_if_crashed_table);
}


/*
  Similar to the above, but only used from maria_open() where we don't have
  an active handler object. Here we don't set a fatal error as we may
  still want to do an automatic repair on the table
*/

void _ma_set_fatal_error_with_share(MARIA_SHARE *share, int error)
{
  DBUG_PRINT("error", ("error: %d", error));

  if (!(share->state.changed & STATE_CRASHED_PRINTED))
  {
    _ma_report_error(error,
                     (share->index_file_name.length ?
                      &share->index_file_name :
                      &share->unique_file_name),
                     MYF(ME_WARNING | ME_ERROR_LOG));
  }
  maria_mark_crashed_share(share);
  share->state.changed|= STATE_CRASHED_PRINTED;
  DBUG_ASSERT(!maria_assert_if_crashed_table);
}

/*
  Check quotas for internal temporary files
*/

int _ma_update_tmp_file_size(struct tmp_file_tracking *track,
                             ulonglong file_size)
{
  int err;
  if (track->file_size != file_size)
  {
    track->file_size= file_size;
    if ((err= update_tmp_file_size(track, 0)))
    {
      my_errno= HA_ERR_LOCAL_TMP_SPACE_FULL + (err - EE_LOCAL_TMP_SPACE_FULL);
      return 1;
    }
  }
  return 0;
}
