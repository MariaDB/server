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

#if 1 /* can't #include "sql/mysqld.h" because it is C++ */
extern char mysql_real_data_home[];
#endif

ATTRIBUTE_COLD ATTRIBUTE_NOINLINE static int dir_error(const char *name)
{
  my_error(ER_CANT_READ_DIR, MYF(0), name, my_errno);
  return 1;
}

/**
   Determine if a file may be backed up.
   @param file_name   candidate file name
   @retval FALSE   if the file must be excluded
   @retval TRUE    if the file may be included
*/
static int is_db_file(const char *file_name)
{
  size_t len= strlen(file_name);
  uint32_t suffix;
  if (len < 4)
    return FALSE;
  if (!memcmp(file_name, tmp_file_prefix, tmp_file_prefix_length))
    /*
      As noted in MDEV-25854, file names that start with #sql
      must be excluded from the backup. For example, a call to
      MDL_context::upgrade_shared_lock() in
      mysql_inplace_alter_table() could time out, resulting in
      cleanup_table_after_inplace_alter() deleting a
      #sql-alter*.frm file before we get a chance to copy it.
    */
    return FALSE;
  memcpy(&suffix, file_name + len - 4, 4);
  switch (suffix) {
  default:
    return len == 6 && !memcmp(file_name, C_STRING_WITH_LEN("db.opt"));
#ifdef WORDS_BIGENDIAN
  case 0x2e41524d: /* .ARM ENGINE=ARCHIVE metadata */
  case 0x2e41525a: /* .ARZ ENGINE=ARCHIVE compressed data */
  case 0x2e43534d: /* .CSM ENGINE=CSV metadata */
  case 0x2e435356: /* .CSV ENGINE=CSV data ("comma separated values") */
  case 0x2e4d4144: /* .MAD ENGINE=Aria data heap */
  case 0x2e4d4149: /* .MAI ENGINE=Aria indexes */
  case 0x2e4d5247: /* .MRG ENGINE=MRG_MyISAM */
  case 0x2e4d5944: /* .MYD ENGINE=MyISAM data heap */
  case 0x2e4d5949: /* .MYI ENGINE=MyISAM indexes */
  case 0x2e66726d: /* .frm form (SHOW CREATE TABLE) */
  case 0x2e706172: /* .par PARTITION metadata */
#else
  case 0x4d52412e: /* .ARM ENGINE=ARCHIVE metadata */
  case 0x5a52412e: /* .ARZ ENGINE=ARCHIVE compressed data */
  case 0x4d53432e: /* .CSM ENGINE=CSV metadata */
  case 0x5653432e: /* .CSV ENGINE=CSV data ("comma separated values") */
  case 0x44414d2e: /* .MAD ENGINE=Aria data heap */
  case 0x49414d2e: /* .MAI ENGINE=Aria indexes */
  case 0x47524d2e: /* .MRG ENGINE=MRG_MyISAM */
  case 0x44594d2e: /* .MYD ENGINE=MyISAM data heap */
  case 0x49594d2e: /* .MYI ENGINE=MyISAM indexes */
  case 0x6d72662e: /* .frm form (SHOW CREATE TABLE) */
  case 0x7261702e: /* .par PARTITION metadata */
#endif
    return TRUE;
  }
}

struct Aria_backup_entry
{
  /** directory name */
  const char *dir;
  /** file name relative to the directory */
  const char *name;
};

/** Backup state */
struct Aria_backup
{
  /** whether translog_disable_purge() is in effect */
  int translog_purge_disabled;
};

static void aria_backup_init(struct Aria_backup *ab)
{
  ab->translog_purge_disabled= TRUE;
  translog_disable_purge();
}

static void aria_backup_destroy(const struct Aria_backup *ab)
{
  if (ab && ab->translog_purge_disabled)
    translog_enable_purge();
}

/*
  Create directory in the target directory if it does not exist.
  Return 0 on success, non-0 on failure. Set errno in case of failure
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

static int aria_backup_file(const struct backup_target *target,
                            const struct backup_sink *sink,
                            const char *dir, const char *name,
                            size_t dir_prefix, int if_exists)
{
#ifndef _WIN32
  int src, dst= sink->stream;
#endif
  int ret= -1;
  char path[FN_REFLEN * 3 / 2];
  if ((int) sizeof path <= snprintf(path, sizeof path, "%s/%s", dir, name))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), name);
    return -1;
  }
#ifndef _WIN32
  src= open(path, O_RDONLY);
  if (src < 0)
  {
    my_error(ER_CANT_OPEN_FILE, MYF(0), path, errno);
    return ret;
  }
  if (dst < 0)
  {
    dst= openat(target->fd, path + dir_prefix,
                O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (dst < 0)
      my_error(ER_CANT_CREATE_FILE, MYF(0), path, errno);
    else
    {
      ret= copy_entire_file(src, dst) | close(dst);
      if (ret)
      write_error:
        my_error(ER_ERROR_ON_WRITE, MYF(0), path + dir_prefix, errno);
    }
  }
  else
  {
    uint64_t end= (uint64_t) lseek(src, 0, SEEK_END);
    ret= backup_stream_start(dst, path + dir_prefix, 0644, end, NULL, 0) ||
      backup_stream_append_plain(src, dst, 0, end) ||
      backup_stream_zeropad(dst, (size_t) end);
    if (ret)
      goto write_error;
  }
  close(src);
  return ret;
#else
  if (sink->stream == INVALID_HANDLE_VALUE)
  {
    char dstpath[FN_REFLEN * 3 / 2];
    if ((int) sizeof dstpath <=
        snprintf(dstpath, sizeof dstpath, "%s/%s/%s",
                 target->path, path + dir_prefix, name))
      my_error(ER_TOO_LONG_IDENT, MYF(0), name);
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
#endif
}

static int aria_backup_dir(const struct backup_target *target,
                           const struct backup_sink *sink,
                           const char *dir_name, size_t prefix)
{
  int fail= 0;
  char path[FN_REFLEN];
  if ((int) sizeof path <=
      snprintf(path, sizeof path, "%s/%s", mysql_real_data_home, dir_name))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), dir_name);
    return -1;
  }
  else if ((fail= aria_backup_mkdir(target, path + prefix)) != 0)
    return fail;
  else
  {
    MY_DIR *dir= my_dir(path, MYF(MY_WANT_STAT));
    if (!dir)
      return dir_error(path);
    else
    {
      const struct fileinfo *fi= dir->dir_entry;
      const struct fileinfo *const end= fi + dir->number_of_files;
      for (; fi < end; fi++)
        if (is_db_file(fi->name))
          if ((fail= aria_backup_file(target, sink, path, fi->name,
                                      prefix, 0)) != 0)
            break;
      my_dirend(dir);
      return fail;
    }
  }
}

static int aria_backup_scan(const struct backup_target *target,
                            const struct backup_sink *sink)
{
  int fail= 0;
  size_t prefix= strlen(mysql_real_data_home) + 1;
  /* Scan the server data directory for data files. */
  MY_DIR *dir= my_dir(mysql_real_data_home, MYF(MY_WANT_STAT));
  if (!dir)
    return dir_error(mysql_real_data_home);
  else
  {
    const struct fileinfo *fi= dir->dir_entry;
    const struct fileinfo *const end= fi + dir->number_of_files;
    for (; fi < end; fi++)
    {
      if ((fi->mystat->st_mode & S_IFMT) == S_IFDIR)
        if ((fail= aria_backup_dir(target, sink, fi->name, prefix)) != 0)
          break;
    }
    my_dirend(dir);
  }
  if (fail)
    return fail;
  /* Process the Aria logs. */
  prefix= strlen(maria_data_root) + 1;
  fail= aria_backup_file(target, sink, maria_data_root, "aria_log_control",
                         prefix, 1);
  if (fail)
    return fail;
  translog_flush(translog_get_horizon());
  dir= my_dir(maria_data_root, MYF(MY_WANT_STAT));
  if (!dir)
    return dir_error(maria_data_root);
  else
  {
    const struct fileinfo *fi= dir->dir_entry;
    const struct fileinfo *const end= fi + dir->number_of_files;
    for (; fi < end; fi++)
      if ((fail=
           !strncmp(fi->name, C_STRING_WITH_LEN("aria_log.")) &&
           aria_backup_file(target, sink, maria_data_root, fi->name,
                            prefix, 0)) != 0)
        break;
    my_dirend(dir);
  }
  return fail;
}

void *aria_backup_start(THD *thd, const struct backup_target *target,
                        enum backup_phase phase,
                        const struct backup_sink *sink)
{
  switch (phase) {
    struct Aria_backup *aria_backup;
  case BACKUP_PHASE_PREPARE_START:
    return 0;
  default:
    return sink->ha_data;
  case BACKUP_PHASE_NO_COMMIT:
    assert(!sink->ha_data);
    aria_backup= calloc(1, sizeof *aria_backup);
    if (!aria_backup)
      return (void*) -1;
    aria_backup_init(aria_backup);
    return aria_backup;
  }
}

#if 0 // FIXME: implement this
int aria_backup_step(THD *, const struct backup_target*, enum backup_phase,
                     const struct backup_sink*)
{
  return 0;
}
#endif

int aria_backup_end(THD *thd, const struct backup_target *target,
                    enum backup_phase phase, const struct backup_sink *sink)
{
  struct Aria_backup *aria_backup= sink->ha_data;
  int ret= 0;
  switch (phase) {
    extern void purge_tables(void);
  case BACKUP_PHASE_NO_COMMIT:
    assert(aria_backup);
    assert(aria_backup->translog_purge_disabled);
    aria_backup->translog_purge_disabled= FALSE;
    purge_tables(); // TODO: do not close transactional tables
    ret= aria_backup_scan(target, sink);
    translog_enable_purge();
    break;
  case BACKUP_PHASE_FINISH:
    aria_backup_destroy(aria_backup);
    free(aria_backup);
    break;
  default:
    break;
  }
  return ret;
}
