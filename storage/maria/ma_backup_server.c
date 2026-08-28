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

#if 1 /* can't #include "sql/table.h" because it is C++ */
# define tmp_file_prefix "#sql"
# define tmp_file_prefix_length 4
#endif

#ifndef _WIN32
# include <sys/types.h>
# include <dirent.h>
#endif

/**
   Report that a directory cannot be read.
   @param name   directory name
*/
ATTRIBUTE_COLD ATTRIBUTE_NOINLINE static void dir_error(const char *name)
{
#ifdef _WIN32
  my_osmaperr(GetLastError());
#endif
  my_error(ER_CANT_READ_DIR, MYF(0), name, errno);
}

/**
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
enum Aria_backup_status { BACKUP_OK, BACKUP_FAIL, TRANSLOG_PURGE_DISABLED };

/** Backup state */
struct Aria_backup
{
#ifndef _WIN32
  /** directory stream */
  DIR *dir;
  /** the readdir(dir) result for which subdir was opened */
  const struct dirent *d;
  /** subdirectory stream, or NULL if iterating to next entry in dir */
  DIR *subdir;
#else
  /** directory iterator */
  HANDLE dir;
  /** subdirectory iterator, or INVALID_HANDLE_VALUE */
  HANDLE subdir;
#endif
  /** status */
  enum Aria_backup_status status;
  /** mutex protecting d, subdir, status in concurrent aria_backup_step() */
  pthread_mutex_t mutex;
#ifdef _WIN32
  /** FindFirstFileA()/FindNextFile() buffer for dir */
  WIN32_FIND_DATAA d;
  /** FindFirstFileA()/FindNextFile() buffer for subdir */
  WIN32_FIND_DATAA sd;
#endif
};

/**
   Create a subdirectory unless we are streaming or it pre-exists.
   @param target    BACKUP SERVER target
   @param name      base name of subdirectory to create
   @return error code (also errno will be set)
   @retval 0 on success (errno might not be touched)
*/
static int aria_backup_mkdir(const struct backup_target *target,
                             const char *name)
{
#ifdef _WIN32
  if (!target->path)
    return 0;
  char path[FN_REFLEN];
  if ((int) sizeof path <=
      snprintf(path, sizeof path, "%s/%s", target->path, name))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), name);
    return -1;
  }
  if (CreateDirectory(path, NULL))
    return 0;
  DWORD err= GetLastError();
  if (err == ERROR_ALREADY_EXISTS)
    return 0;
  my_osmaperr(err);
#else
  if (target->fd == -1 || likely(!mkdirat(target->fd, name, 0777)) ||
      errno == EEXIST)
    return 0;
#endif
  my_error(ER_CANT_CREATE_FILE, MYF(0), name, errno);
  return 1;
}

#ifndef _WIN32
/**
   Copy a file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param dirfd      source directory
   @param path       file name
   @return error code (also errno will be set)
   @retval 0 on success (errno might not be touched)
*/
static int aria_backup_file(const struct backup_target *target,
                            const struct backup_sink *sink,
                            int dirfd, const char *path)
{
  int ret= -1, src= openat(dirfd, path, O_RDONLY), dst= sink->stream;
  if (src < 0)
  {
    my_error(ER_CANT_OPEN_FILE, MYF(0), path, errno);
    return ret;
  }
  if (dst < 0)
  {
    dst= openat(target->fd, path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (dst < 0)
      my_error(ER_CANT_CREATE_FILE, MYF(0), path, errno);
    else
    {
      ret= copy_entire_file(src, dst) | close(dst);
      if (ret)
      write_error:
        my_error(ER_ERROR_ON_WRITE, MYF(0), path, errno);
    }
  }
  else
  {
    uint64_t end= (uint64_t) lseek(src, 0, SEEK_END);
    ret= backup_stream_start(dst, path, 0644, end, NULL, 0) ||
      backup_stream_append_plain(src, dst, 0, end) ||
      backup_stream_zeropad(dst, (size_t) end);
    if (ret)
      goto write_error;
  }
  close(src);
  return ret;
}
#else
/**
   Copy a file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param path       file name
   @param dir_prefix length of the path prefix to omit from the backup
   @return error code (also errno will be set)
   @retval 0 on success (errno might not be touched)
*/
static int aria_backup_file(const struct backup_target *target,
                            const struct backup_sink *sink,
                            const char *path, size_t dir_prefix)
{
  int ret= -1;
  if (sink->stream == INVALID_HANDLE_VALUE)
  {
    char dstpath[FN_REFLEN * 3 / 2];
    if ((int) sizeof dstpath <=
        snprintf(dstpath, sizeof dstpath, "%s/%s",
                 target->path, path + dir_prefix))
      my_error(ER_TOO_LONG_IDENT, MYF(0), path + dir_prefix);
    else if (!CopyFileEx(path, dstpath, NULL, NULL, NULL,
                         COPY_FILE_NO_BUFFERING))
    {
      my_osmaperr(GetLastError());
      my_error(ER_CANT_CREATE_FILE, MYF(0), dstpath, errno);
    }
    else
      return 0;
  }
  else
  {
    LARGE_INTEGER li;
    HANDLE src, dst= sink->stream;
    for (;;)
    {
      src= CreateFile(path, GENERIC_READ,
                      FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                      my_win_file_secattr(), OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, NULL);
      if (src != INVALID_HANDLE_VALUE)
        break;
      switch (GetLastError()) {
      case ERROR_SHARING_VIOLATION:
      case ERROR_LOCK_VIOLATION:
        my_sleep(1000000);
        continue;
      }

      my_osmaperr(GetLastError());
      my_error(ER_FILE_NOT_FOUND, MYF(ME_ERROR_LOG), path, errno);
      return ret;
    }

    ret= !GetFileSizeEx(src, &li) ||
      backup_stream_start(dst, path + dir_prefix, 0644, li.QuadPart,
                          NULL, 0) ||
      backup_stream_append_plain(src, dst, 0, li.QuadPart) ||
      backup_stream_zeropad(dst, (size_t) li.QuadPart);
    (void) CloseHandle(src);

    if (ret)
    {
      my_osmaperr(GetLastError());
      my_error(ER_ERROR_ON_WRITE, MYF(0), path + dir_prefix, errno);
    }
  }
  return ret;
}
#endif

/**
   Back up a data file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param include_p  predicate for files to include
   @return number directories or files left; negative on error
   @retval 0 on successful completion
*/
static int aria_backup_data(const struct backup_target *target,
                            const struct backup_sink *sink,
                            name_predicate include_p)
{
  const char *filename= NULL;
  char path[FN_REFLEN * 2 + 2];
  struct Aria_backup *const ab= sink->ha_data;
  int left= 0;
#ifndef _WIN32
  struct dirent *d;
  struct stat sb;
  assert(ab->dir);
#else
  assert(ab->dir != INVALID_HANDLE_VALUE);
#endif
  pthread_mutex_lock(&ab->mutex);

  if (ab->status != BACKUP_OK)
  {
    assert(ab->status == BACKUP_FAIL);
  err_exit:
    left= -1;
    ab->status= BACKUP_FAIL;
  }
#ifndef _WIN32
  else if (!ab->subdir)
  {
    DIR *const dir= ab->dir;
    while ((d= readdir(dir)) != NULL)
    {
      switch (d->d_type) {
      default:
        continue;
      case DT_DIR:
        break;
      case DT_UNKNOWN:
        if (fstatat(dirfd(dir), d->d_name, &sb, 0) ||
            (sb.st_mode & S_IFMT) != S_IFDIR)
          continue;
      }
      if (!aria_backup_mkdir(target, d->d_name))
      {
        int dfd= openat(dirfd(dir), d->d_name, O_DIRECTORY);
        ab->d= d;
        if (dfd >= 0)
        {
          if ((ab->subdir= fdopendir(dfd)))
            goto consume_subdir;
          close(dfd);
        }
        dir_error(d->d_name);
      }
      goto err_exit;
    }
  }
  else
  {
  consume_subdir:
    assert(ab->d);
    assert(ab->d->d_type == DT_DIR || ab->d->d_type == DT_UNKNOWN);
    while ((d= readdir(ab->subdir)) != NULL)
    {
      const char *const name= d->d_name;
      size_t len;
      switch (d->d_type) {
      default:
        continue;
      case DT_REG:
      case DT_LNK:
        break;
      case DT_UNKNOWN:
        if (fstatat(dirfd(ab->subdir), name, &sb, 0) ||
            (sb.st_mode & S_IFMT) != S_IFREG)
          continue;
      }
      if ((len= strlen(name)) < 4 ||
          /*
            As noted in MDEV-25854, file names that start with #sql
            must be excluded from the backup. For example, a call to
            MDL_context::upgrade_shared_lock() in
            mysql_inplace_alter_table() could time out, resulting in
            cleanup_table_after_inplace_alter() deleting a
            #sql-alter*.frm file before we get a chance to copy it.
          */
          !memcmp(name, tmp_file_prefix, tmp_file_prefix_length) ||
          !(*include_p)(name, len))
        continue;

      /* Consume a file name */
      if ((int) sizeof path <=
          snprintf(path, sizeof path, "%s/%s", ab->d->d_name, name))
      {
        path[(sizeof path) - 1]= '\0';
        my_error(ER_TOO_LONG_IDENT, MYF(0), path);
        goto err_exit;
      }
      filename= path;
      break;
    }

    left= 1;
    if (!d)
    {
      closedir(ab->subdir);
      ab->d= NULL;
      ab->subdir= NULL;
    }
  }
#else
  else if (ab->subdir == INVALID_HANDLE_VALUE)
  {
    do
    {
      if (!(ab->d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        continue;

      if ((int) sizeof path <=
          snprintf(path, sizeof path, "%s/*.*", ab->d.cFileName))
      {
      name_too_long:
        path[(sizeof path) - 1]= '\0';
        my_error(ER_TOO_LONG_IDENT, MYF(0), path);
      }
      else if (aria_backup_mkdir(target, ab->d.cFileName));
      else if ((ab->subdir= FindFirstFileA(path, &ab->sd)) !=
               INVALID_HANDLE_VALUE)
        goto consume_subdir;
      else
        dir_error(path);
      goto err_exit;
    }
    while (FindNextFile(ab->dir, &ab->d));
  }
  else
  {
  consume_subdir:
    do
    {
      const char *const name= ab->sd.cFileName;
      size_t len;
      if (ab->sd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        continue;
      if ((len= strlen(name)) < 4 ||
          /*
            As noted in MDEV-25854, file names that start with #sql
            must be excluded from the backup. For example, a call to
            MDL_context::upgrade_shared_lock() in
            mysql_inplace_alter_table() could time out, resulting in
            cleanup_table_after_inplace_alter() deleting a
            #sql-alter*.frm file before we get a chance to copy it.
          */
          !memcmp(name, tmp_file_prefix, tmp_file_prefix_length) ||
          !(*include_p)(name, len))
        continue;

      /* Consume a file name */
      if ((int) sizeof path <=
          snprintf(path, sizeof path, "%s/%s", ab->d.cFileName, name));
        goto name_too_long;

      filename= path;
    }
    while ((left= FindNextFile(ab->subdir, &ab->sd)) && !filename);

    if (!left)
    {
      FindClose(ab->subdir);
      ab->subdir= INVALID_HANDLE_VALUE;
      left= FindNextFile(ab->dir, &ab->d);
    }
  }
#endif

  pthread_mutex_unlock(&ab->mutex);
  if (filename &&
#ifndef _WIN32
      aria_backup_file(target, sink, dirfd(ab->dir), filename) &&
#else
      aria_backup_file(target, sink, filename, 0) &&
#endif
      TRUE)
    return -1;
  return left;
}

/**
   Back up an ENGINE=Aria log file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @return number directories or files left; negative on error
   @retval 0 on successful completion
*/
static int aria_backup_log(const struct backup_target *target,
                           const struct backup_sink *sink)
{
  struct Aria_backup *const ab= sink->ha_data;
  int left;
  const char *filename= NULL;
#ifdef _WIN32
  char path[FN_REFLEN * 2 + 2];
#endif
  pthread_mutex_lock(&ab->mutex);
#ifndef _WIN32
  assert(!ab->d);
  assert(ab->dir);
  assert(!ab->subdir);
#else
  assert(ab->dir != INVALID_HANDLE_VALUE);
  assert(ab->subdir == INVALID_HANDLE_VALUE);
#endif

  if (ab->status != TRANSLOG_PURGE_DISABLED)
  {
    assert(ab->status == BACKUP_FAIL);
#ifdef _WIN32
  err_exit:
#endif
    left= -1;
    ab->status= BACKUP_FAIL;
  }
  else
  {
#ifndef _WIN32
    struct dirent *d;
    while ((d= readdir(ab->dir)) != NULL)
    {
      struct stat sb;
      switch (d->d_type) {
      default:
        continue;
      case DT_REG:
      case DT_LNK:
        break;
      case DT_UNKNOWN:
        if (fstatat(dirfd(ab->dir), d->d_name, &sb, 0) ||
            (sb.st_mode & S_IFMT) != S_IFREG)
          continue;
      }
      if (strncmp(d->d_name, C_STRING_WITH_LEN("aria_log.")) &&
          strcmp(d->d_name, "aria_log_control"))
        continue;
      filename= d->d_name;
      break;
    }
    left= d != NULL;
#else
    do
    {
      const char *const name= ab->d.cFileName;
      if (ab->d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        continue;
      if (strncmp(name, C_STRING_WITH_LEN("aria_log.")) &&
          strcmp(name, "aria_log_control"))
        continue;
      else if ((int) sizeof path <=
               snprintf(path, sizeof path, "%s/%s", maria_data_root, name))
      {
        path[(sizeof path) - 1]= '\0';
        my_error(ER_TOO_LONG_IDENT, MYF(0), path);
        goto err_exit;
      }
      filename= path;
    }
    while ((left= FindNextFile(ab->dir, &ab->d)) && !filename);
#endif
  }

  pthread_mutex_unlock(&ab->mutex);
  if (filename &&
#ifndef _WIN32
      aria_backup_file(target, sink, dirfd(ab->dir), filename) &&
#else
      aria_backup_file(target, sink, filename,
                       strlen(maria_data_root) + 1) &&
#endif
      TRUE)
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
    struct Aria_backup* ab;
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
    if (!(ab= calloc(1, sizeof *ab)))
      return (void*) -1;
    pthread_mutex_init(&ab->mutex, NULL);
#ifdef _WIN32
    ab->subdir= INVALID_HANDLE_VALUE;
    ab->dir= FindFirstFileA("*.*", &ab->d);
    if (ab->dir != INVALID_HANDLE_VALUE)
      return ab;
#else
    {
      int dfd= open(mysql_data_home, O_DIRECTORY);
      if (dfd >= 0)
      {
        if ((ab->dir= fdopendir(dfd)))
          return ab;
        close(dfd);
      }
    }
#endif
    dir_error(mysql_data_home);
    pthread_mutex_destroy(&ab->mutex);
    free(ab);
    return (void*) -1;
  case BACKUP_PHASE_NO_COMMIT:
    translog_flush(translog_get_horizon());
#ifndef NDEBUG
    ab= sink->ha_data;
#endif
#ifndef _WIN32
    assert(ab->dir);
    assert(!ab->subdir);
    assert(!ab->d);
#else
    assert(ab->dir != INVALID_HANDLE_VALUE);
    assert(ab->subdir == INVALID_HANDLE_VALUE);
#endif
    break;
  case BACKUP_PHASE_FINISH:
    if (!sink)
      break;
    ab= sink->ha_data;
    if (!ab)
      break;
    if (ab->status != BACKUP_OK)
    {
      assert(ab->status == BACKUP_FAIL);
      break;
    }
#ifndef _WIN32
    assert(!ab->dir);
    assert(!ab->subdir);
    assert(!ab->d);
    {
      int dfd= open(maria_data_root, O_DIRECTORY);
      if (dfd >= 0)
      {
        if ((ab->dir= fdopendir(dfd)))
        {
          ab->status= TRANSLOG_PURGE_DISABLED;
          translog_disable_purge();
          break;
        }
        close(dfd);
      }
    }
#else
    assert(ab->dir == INVALID_HANDLE_VALUE);
    assert(ab->subdir == INVALID_HANDLE_VALUE);
    {
      char path[FN_REFLEN * 2 + 2];
      if ((int) sizeof path >
          snprintf(path, sizeof path, "%s/aria_log*", maria_data_root) &&
          (ab->dir= FindFirstFileA(path, &ab->d)) !=
          INVALID_HANDLE_VALUE)
      {
        ab->status= TRANSLOG_PURGE_DISABLED;
        translog_disable_purge();
        break;
      }
    }
#endif
    dir_error(maria_data_root);
    ab->status= BACKUP_FAIL;
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
   @return number directories or files left; negative on error
   @retval 0 on completion
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
    struct Aria_backup* ab;
  case BACKUP_PHASE_PREPARE_START:
    assert(!sink);
    break;
  case BACKUP_PHASE_START:
  case BACKUP_PHASE_NO_BEGIN_NON_TRANS:
  case BACKUP_PHASE_NO_DML_NON_TRANS:
    assert(!sink->ha_data);
    break;
  case BACKUP_PHASE_NO_DDL:
    ab= sink->ha_data;
    if (!ab)
      break;
    /* Rewind the directory for BACKUP_PHASE_NO_COMMIT */
#ifndef _WIN32
    assert(!ab->d);
    assert(!ab->subdir);
    assert(ab->dir);
    rewinddir(ab->dir);
#else
    assert(ab->subdir == INVALID_HANDLE_VALUE);
    assert(ab->dir != INVALID_HANDLE_VALUE);
    FindClose(ab->dir);
    ab->dir= FindFirstFileA("*.*", &ab->d);
    if (ab->dir == INVALID_HANDLE_VALUE)
    {
      dir_error(mysql_data_home);
      return -1;
    }
#endif
    break;
  case BACKUP_PHASE_NO_COMMIT:
    ab= sink->ha_data;
    if (!ab)
      break;
#ifndef _WIN32
    assert(ab->dir);
    assert(!ab->subdir);
    assert(!ab->d);
    closedir(ab->dir);
    ab->dir= NULL;
#else
    assert(ab->dir != INVALID_HANDLE_VALUE);
    assert(ab->subdir == INVALID_HANDLE_VALUE);
    FindClose(ab->dir);
    ab->dir= INVALID_HANDLE_VALUE;
#endif
    break;
  case BACKUP_PHASE_ABORT:
    break;
  case BACKUP_PHASE_FINISH:
    ab= sink->ha_data;
    if (!ab)
      break;
    if (ab->status == TRANSLOG_PURGE_DISABLED)
      translog_enable_purge();
#ifndef _WIN32
    if (ab->dir)
      closedir(ab->dir);
    if (ab->subdir)
      closedir(ab->subdir);
#else
    if (ab->dir != INVALID_HANDLE_VALUE)
      FindClose(ab->dir);
    if (ab->subdir != INVALID_HANDLE_VALUE)
      FindClose(ab->subdir);
#endif
    pthread_mutex_destroy(&ab->mutex);
    free(ab);
  }

  return 0;
}
