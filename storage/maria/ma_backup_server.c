/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "maria_def.h"
#include "ma_backup_server.h"
#include "mysqld_error.h"
#include <my_dir.h>

#if 1 /* can't #include "sql/table.h" because it is C++ */
# define tmp_file_prefix "#sql"
# define tmp_file_prefix_length 4
#endif

/*
  Predicate for checking if a file must be included in the backup.
  @param file_name   file base name to check
  @param len         strlen(file_name)
*/
typedef int (*name_predicate)(const char *file_name, size_t len);

/**
   Determine if a non-Aria file may be backed up.
   @param file_name   candidate file name
   @param len         strlen(file_name)
   @retval FALSE   if the file must be excluded
   @retval TRUE    if the file may be included
*/
static int is_db_file(const char *file_name, size_t len)
{
  uint32_t suffix;
  assert(len >= 4);
  memcpy(&suffix, file_name + len - 4, 4);
  switch (suffix) {
#ifdef WORDS_BIGENDIAN
  case 0x2e41524d: /* .ARM ENGINE=ARCHIVE metadata */
  case 0x2e41525a: /* .ARZ ENGINE=ARCHIVE compressed data */
  case 0x2e43534d: /* .CSM ENGINE=CSV metadata */
  case 0x2e435356: /* .CSV ENGINE=CSV data ("comma separated values") */
  case 0x2e4d5247: /* .MRG ENGINE=MRG_MyISAM */
  case 0x2e4d5944: /* .MYD ENGINE=MyISAM data heap */
  case 0x2e4d5949: /* .MYI ENGINE=MyISAM indexes */
  case 0x2e545247: /* .TRG trigger definition */
  case 0x2e54524e: /* .TRN trigger name */
  case 0x2e66726d: /* .frm form (SHOW CREATE TABLE) */
  case 0x2e706172: /* .par PARTITION metadata */
#else
  case 0x4d52412e: /* .ARM ENGINE=ARCHIVE metadata */
  case 0x5a52412e: /* .ARZ ENGINE=ARCHIVE compressed data */
  case 0x4d53432e: /* .CSM ENGINE=CSV metadata */
  case 0x5653432e: /* .CSV ENGINE=CSV data ("comma separated values") */
  case 0x47524d2e: /* .MRG ENGINE=MRG_MyISAM */
  case 0x44594d2e: /* .MYD ENGINE=MyISAM data heap */
  case 0x49594d2e: /* .MYI ENGINE=MyISAM indexes */
  case 0x4752542e: /* .TRG trigger definition */
  case 0x4e52542e: /* .TRN trigger name */
  case 0x6d72662e: /* .frm form (SHOW CREATE TABLE) */
  case 0x7261702e: /* .par PARTITION metadata */
#endif
    return TRUE;
  }
  return len == 6 && !memcmp(file_name, C_STRING_WITH_LEN("db.opt"));
}

/**
   Determine if a file is an ENGINE=Aria file.
   @param file_name   candidate file name
   @param len         strlen(file_name)
   @retval FALSE   if the file must be excluded
   @retval TRUE    if the file may be included
*/
static int is_ma_file(const char *file_name, size_t len)
{
  uint32_t suffix;
  assert(len >= 4);
  memcpy(&suffix, file_name + len - 4, 4);
  switch (suffix) {
#ifdef WORDS_BIGENDIAN
  case 0x2e4d4144: /* .MAD ENGINE=Aria data heap */
  case 0x2e4d4149: /* .MAI ENGINE=Aria indexes */
#else
  case 0x44414d2e: /* .MAD ENGINE=Aria data heap */
  case 0x49414d2e: /* .MAI ENGINE=Aria indexes */
#endif
    return TRUE;
  }
  return FALSE;
}

/** Backup status */
enum Aria_backup_status {
  /** No error yet */
  BACKUP_OK,
  /** A failure has occurred */
  BACKUP_FAIL
};

/** Backup state */
struct Aria_backup
{
  /** The data directory, or the Aria log directory in the last phase */
  MY_NO_CACHE_DIR *dir;
  /** The database directory that we are reading, or NULL */
  MY_NO_CACHE_DIR *subdir;
  /** status */
  enum Aria_backup_status status;
  /** mutex protecting dir, subdir, db_name and status */
  pthread_mutex_t mutex;
  /** name of the database directory that subdir was opened for */
  char db_name[FN_REFLEN];
  /** directory in the backup that the files of db_name are copied to */
  char dest_dir[FN_REFLEN];
};


/**
   Copy a file to the backup.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param src_path   name of the file to copy
   @param dest_path  name of the copy, when copying to a directory
   @param dest_name  name of the file in the backup, always with '/'
   @return error code
   @retval 0 on success
*/
static int aria_backup_file(const struct backup_target *target,
                            const struct backup_sink *sink,
                            const char *src_path, const char *dest_path,
                            const char *dest_name)
{
  File src;
  my_off_t length;
  int error;

  if (target->path)
    return my_copy(src_path, dest_path,
                   MYF(MY_WME | MY_DONT_OVERWRITE_FILE)) ? -1 : 0;

  /* Add the file to the backup stream */
  if ((src= my_open(src_path, O_RDONLY, MYF(MY_WME))) < 0)
    return -1;
  length= my_seek(src, 0L, MY_SEEK_END, MYF(0));
  error= (length == MY_FILEPOS_ERROR ||
          backup_stream_start(sink->stream, dest_name, 0644, length,
                              NULL, 0) ||
          backup_stream_append_plain(my_native_file_handle(src),
                                     sink->stream, 0, length) ||
          backup_stream_zeropad(sink->stream, (size_t) length));
  my_close(src, MYF(0));
  if (error)
  {
    my_error(ER_ERROR_ON_WRITE, MYF(0), dest_name, my_errno);
    return -1;
  }
  return 0;
}


/**
   Back up a data file.

   Only one thread at a time reads the directories, but the files are
   copied without holding the mutex, which allows several threads to
   copy files at the same time.

   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param include_p  predicate for files to include
   @retval 1 on success if some work remains
   @retval 0 on successful completion
   @retval -1 on error
*/
static int aria_backup_data(const struct backup_target *target,
                            const struct backup_sink *sink,
                            name_predicate include_p)
{
  struct Aria_backup *const backup= sink->ha_data;
  char name[FN_REFLEN], db_path[FN_REFLEN];
  char src_path[FN_REFLEN], dest_path[FN_REFLEN], dest_name[FN_REFLEN];
  int left= 1, found= 0;

  pthread_mutex_lock(&backup->mutex);

  if (backup->status != BACKUP_OK)
  {
    assert(backup->status == BACKUP_FAIL);
    pthread_mutex_unlock(&backup->mutex);
    my_error(ER_UNKNOWN_ERROR, MYF(0));         /* Another thread failed */
    return -1;
  }

  while (!found)
  {
    size_t length;

    if (!backup->subdir)
    {
      /* Open the next database directory */
      switch (my_dir_read_next(backup->dir, name, sizeof(name), NULL,
                               MYF(0))) {
      case MY_DIR_OK:
        break;
      case MY_DIR_EOF:
        left= 0;                                /* Nothing more to do */
        goto end;
      default:
        continue;                               /* Skip a too long name */
      }

      if (!fn_format(db_path, name, mysql_data_home, "", MYF(MY_SAFE_PATH)))
      {
        my_error(ER_TOO_LONG_IDENT, MYF(ME_WARNING), name);
        continue;                               /* Skip this database */
      }
      if (target->path)
      {
        /* Create the database directory in the backup */
        if (!fn_format(backup->dest_dir, name, target->path, "",
                       MYF(MY_SAFE_PATH)))
        {
          my_error(ER_TOO_LONG_IDENT, MYF(ME_WARNING), name);
          continue;
        }
        if (my_mkdir(backup->dest_dir, 0777, MYF(0)) && my_errno != EEXIST)
        {
          my_error(ER_CANT_CREATE_FILE, MYF(0), backup->dest_dir, my_errno);
          goto err;
        }
      }
      if (!(backup->subdir= my_dir_open(db_path, NULL,
                                        MYF(MY_DIR_ONLY_FILES | MY_WME |
                                            ME_WARNING))))
        goto err;
      strmake(backup->db_name, name, sizeof(backup->db_name) - 1);
      continue;
    }

    switch (my_dir_read_next(backup->subdir, name, sizeof(name), NULL,
                             MYF(0))) {
    case MY_DIR_OK:
      break;
    case MY_DIR_EOF:
      my_dir_close(backup->subdir);
      backup->subdir= NULL;
      continue;                                 /* Take the next database */
    default:
      continue;                                 /* Skip a too long name */
    }

    if ((length= strlen(name)) < 4 ||
        /*
          As noted in MDEV-25854, file names that start with #sql
          must be excluded from the backup. For example, a call to
          MDL_context::upgrade_shared_lock() in
          mysql_inplace_alter_table() could time out, resulting in
          cleanup_table_after_inplace_alter() deleting a
          #sql-alter*.frm file before we get a chance to copy it.
        */
        !memcmp(name, tmp_file_prefix, tmp_file_prefix_length) ||
        !(*include_p)(name, length))
      continue;

    /* Consume a file name */
    if (!fn_format(src_path, name, backup->subdir->path.str, "",
                   MYF(MY_SAFE_PATH)) ||
        (target->path &&
         !fn_format(dest_path, name, backup->dest_dir, "",
                    MYF(MY_SAFE_PATH))))
    {
      my_error(ER_TOO_LONG_IDENT, MYF(ME_WARNING), name);
      continue;                                 /* Skip this file */
    }
    /* tar always uses '/' as the directory separator */
    if (strxnmov(dest_name, sizeof(dest_name) - 1, backup->db_name, "/",
                 name, NullS) == dest_name + sizeof(dest_name) - 1)
    {
      my_error(ER_TOO_LONG_IDENT, MYF(ME_WARNING), name);
      continue;
    }
    found= 1;
  }

end:
  pthread_mutex_unlock(&backup->mutex);
  if (found &&
      aria_backup_file(target, sink, src_path, dest_path, dest_name))
    return -1;
  return left;

err:
  backup->status= BACKUP_FAIL;
  pthread_mutex_unlock(&backup->mutex);
  return -1;
}


/**
   Back up an ENGINE=Aria log file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @retval 1 on success if some work remains
   @retval 0 on successful completion
   @retval -1 on error
*/
static int aria_backup_log(const struct backup_target *target,
                           const struct backup_sink *sink)
{
  struct Aria_backup *const backup= sink->ha_data;
  char name[FN_REFLEN], src_path[FN_REFLEN], dest_path[FN_REFLEN];
  int left= 1, found= 0;

  pthread_mutex_lock(&backup->mutex);

  if (backup->status != BACKUP_OK)
  {
    assert(backup->status == BACKUP_FAIL);
    pthread_mutex_unlock(&backup->mutex);
    my_error(ER_UNKNOWN_ERROR, MYF(0));         /* Another thread failed */
    return -1;
  }
  assert(backup->dir);
  assert(!backup->subdir);

  while (!found)
  {
    switch (my_dir_read_next(backup->dir, name, sizeof(name), NULL,
                             MYF(0))) {
    case MY_DIR_OK:
      break;
    case MY_DIR_EOF:
      left= 0;                                  /* Nothing more to do */
      goto end;
    default:
      continue;                                 /* Skip a too long name */
    }

    /* We want aria_log.00000001 ... and aria_log_control */
    if ((strncmp(name, C_STRING_WITH_LEN("aria_log.")) ||
         strlen(name) != sizeof("aria_log.00000001") - 1) &&
        strcmp(name, "aria_log_control"))
      continue;

    if (!fn_format(src_path, name, backup->dir->path.str, "",
                   MYF(MY_SAFE_PATH)) ||
        (target->path &&
         !fn_format(dest_path, name, target->path, "", MYF(MY_SAFE_PATH))))
    {
      my_error(ER_TOO_LONG_IDENT, MYF(ME_WARNING), name);
      continue;                                 /* Skip this file */
    }
    found= 1;
  }

end:
  pthread_mutex_unlock(&backup->mutex);
  if (found && aria_backup_file(target, sink, src_path, dest_path, name))
    return -1;
  return left;
}


/**
   Start of a BACKUP SERVER phase,
   when no aria_backup_step() or aria_backup_end() is pending.
   @param thd     current session
   @param target  backup target
   @param phase   BACKUP_PHASE_START, ... (not BACKUP_PHASE_ABORT)
   @param sink    worker context
   @return backup context object to be attached to backup_target
   @retval NULL   if no context needs to be created
   @retval -1     on failure
*/
void *aria_backup_start(THD *thd, const struct backup_target *target,
                        enum backup_phase phase,
                        const struct backup_sink *sink)
{
  switch (phase) {
    struct Aria_backup* backup;
  case BACKUP_PHASE_PREPARE_START:
    assert(!sink);
    return NULL;
  case BACKUP_PHASE_START:
  case BACKUP_PHASE_NO_BEGIN_NON_TRANS:
  case BACKUP_PHASE_NO_DML_NON_TRANS:
    assert(!sink->ha_data);
    break;
  case BACKUP_PHASE_NO_DDL:
    assert(!sink->ha_data);
    if (!(backup= my_malloc(PSI_INSTRUMENT_ME, sizeof(*backup),
                            MYF(MY_WME | MY_ZEROFILL))))
      return (void*) -1;
    pthread_mutex_init(&backup->mutex, NULL);
    if ((backup->dir= my_dir_open(mysql_data_home, NULL,
                                  MYF(MY_DIR_ONLY_DIRS | MY_WME |
                                      ME_WARNING))))
      return backup;
    pthread_mutex_destroy(&backup->mutex);
    my_free(backup);
    return (void*) -1;
  case BACKUP_PHASE_NO_COMMIT:
#ifndef NDEBUG
    backup= sink->ha_data;
#endif
    assert(backup->dir);
    assert(!backup->subdir);
    assert(backup->status == BACKUP_OK);
    translog_flush(translog_get_horizon());
    break;
  case BACKUP_PHASE_FINISH:
    if (!sink)
      break;
    backup= sink->ha_data;
    if (!backup)
      break;
    if (backup->status != BACKUP_OK)
    {
      assert(backup->status == BACKUP_FAIL);
      break;
    }
    assert(!backup->dir);
    assert(!backup->subdir);
    if ((backup->dir= my_dir_open(maria_data_root, "aria_log*",
                                  MYF(MY_DIR_ONLY_FILES | MY_WME |
                                      ME_WARNING))))
      break;
    backup->status= BACKUP_FAIL;
    return (void*) -1;
  case BACKUP_PHASE_ABORT:
    break;
  }
  return sink->ha_data;
}


/**
   Process a file that was collected in aria_backup_start().
   @param thd   current session
   @param target  backup target
   @param phase   last phase on which backup_start() was successfully invoked
   @param sink    worker context
   @retval 1 on success if some work remains
   @retval 0 on completion
   @retval -1 on error
*/
int aria_backup_step(THD *thd, const struct backup_target *target,
                     enum backup_phase phase, const struct backup_sink *sink)
{
  switch (phase) {
  case BACKUP_PHASE_PREPARE_START:
    assert(!sink);
    break;
  case BACKUP_PHASE_START:
  case BACKUP_PHASE_NO_BEGIN_NON_TRANS:
  case BACKUP_PHASE_NO_DML_NON_TRANS:
    assert(!sink->ha_data);
    break;
  case BACKUP_PHASE_NO_DDL:
    return aria_backup_data(target, sink, is_db_file);
  case BACKUP_PHASE_NO_COMMIT:
    return aria_backup_data(target, sink, is_ma_file);
  case BACKUP_PHASE_FINISH:
    return aria_backup_log(target, sink);
  case BACKUP_PHASE_ABORT:
    break;
  }
  return 0;
}


/**
   Finish a phase, once all calls for the current phase are completed.
   @param thd   current session
   @param target  backup target
   @param phase   last phase on which backup_start() was successfully invoked,
   or BACKUP_PHASE_ABORT or BACKUP_PHASE_FINISH
   @param sink    worker context
   @return error code
   @retval 0 on success
*/
int aria_backup_end(THD *thd, const struct backup_target *target,
                    enum backup_phase phase, const struct backup_sink *sink)
{
  switch (phase) {
    struct Aria_backup* backup;
  case BACKUP_PHASE_PREPARE_START:
    assert(!sink);
    break;
  case BACKUP_PHASE_START:
  case BACKUP_PHASE_NO_BEGIN_NON_TRANS:
  case BACKUP_PHASE_NO_DML_NON_TRANS:
    assert(!sink->ha_data);
    break;
  case BACKUP_PHASE_NO_DDL:
    backup= sink->ha_data;
    if (!backup)
      break;
    /* Rewind the directory for BACKUP_PHASE_NO_COMMIT */
    assert(backup->dir);
    assert(!backup->subdir);
    if (my_dir_rewind(backup->dir, MYF(MY_WME)))
      return -1;
    break;
  case BACKUP_PHASE_NO_COMMIT:
    backup= sink->ha_data;
    if (!backup)
      break;
    assert(backup->status == BACKUP_OK);
    assert(backup->dir);
    assert(!backup->subdir);
    my_dir_close(backup->dir);
    backup->dir= NULL;
    break;
  case BACKUP_PHASE_ABORT:
    break;
  case BACKUP_PHASE_FINISH:
    backup= sink->ha_data;
    if (!backup)
      break;
    if (backup->dir)
      my_dir_close(backup->dir);
    if (backup->subdir)
      my_dir_close(backup->subdir);
    pthread_mutex_destroy(&backup->mutex);
    my_free(backup);
  }

  return 0;
}
