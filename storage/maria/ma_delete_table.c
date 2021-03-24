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

#include "ma_fulltext.h"
#include "trnman_public.h"

/**
   @brief drops (deletes) a table

   @param  name             table's name

   @return Operation status
     @retval 0      ok
     @retval 1      error
*/

int maria_delete_table(const char *name)
{
  MARIA_HA *info;
  myf sync_dir;
  int got_error= 0, error;
  DBUG_ENTER("maria_delete_table");

#ifdef EXTRA_DEBUG
  _ma_check_table_is_closed(name,"delete");
#endif
  /** @todo LOCK take X-lock on table */
  /*
    We need to know if this table is transactional.
    Unfortunately it is necessary to open the table just to check this. We use
    'open_for_repair' to be able to open even a crashed table.
  */
  my_errno= 0;
  if (!(info= maria_open(name, O_RDONLY, HA_OPEN_FOR_REPAIR, 0)))
  {
    sync_dir= 0;
    /* Ignore not found errors and wrong symlink errors */
    if (my_errno != ENOENT && my_errno != HA_WRONG_CREATE_OPTION)
      got_error= my_errno;
  }
  else
  {
    sync_dir= (info->s->now_transactional && !info->s->temporary &&
               !maria_in_recovery) ?
      MY_SYNC_DIR : 0;
    /* Remove history for table */
    _ma_reset_state(info);
    maria_close(info);
  }

  if (sync_dir)
  {
    /*
      For this log record to be of any use for Recovery, we need the upper
      MySQL layer to be crash-safe in DDLs.
      For now this record can serve when we apply logs to a backup, so we sync
      it.
    */
    LSN lsn;
    LEX_CUSTRING log_array[TRANSLOG_INTERNAL_PARTS + 1];
    log_array[TRANSLOG_INTERNAL_PARTS + 0].str= (uchar*)name;
    log_array[TRANSLOG_INTERNAL_PARTS + 0].length= strlen(name) + 1;
    if (unlikely(translog_write_record(&lsn, LOGREC_REDO_DROP_TABLE,
                                       &dummy_transaction_object, NULL,
                                       (translog_size_t)
                                       log_array[TRANSLOG_INTERNAL_PARTS +
                                                 0].length,
                                       sizeof(log_array)/sizeof(log_array[0]),
                                       log_array, NULL, NULL) ||
                 translog_flush(lsn)))
      DBUG_RETURN(1);
  }

  if (!(error= maria_delete_table_files(name, 0, sync_dir | MY_WME)))
    error= got_error;
  DBUG_RETURN(error);
}

/**
   Delete all files related to a aria table
*/

int maria_delete_table_files(const char *name, my_bool temporary, myf flags)
{
  int error= 0;
  DBUG_ENTER("maria_delete_table_files");

  if (mysql_file_delete_with_symlink(key_file_kfile, name, MARIA_NAME_IEXT,
                                     flags))
    error= my_errno;
  if (mysql_file_delete_with_symlink(key_file_dfile, name, MARIA_NAME_DEXT,
                                     flags))
    error= my_errno;
  if (!temporary)
  {
    /* This is delete a possible temporary aria_chk file */
    mysql_file_delete_with_symlink(key_file_dfile, name, DATA_TMP_EXT, MYF(0));
#ifdef SUPPORT_ARIA_PACK
    /* This is delete a possible temporary aria_pack file */
    mysql_file_delete_with_symlink(key_file_dfile, name, ".OLD", MYF(0));
#endif
  }
  DBUG_RETURN(error);
}
