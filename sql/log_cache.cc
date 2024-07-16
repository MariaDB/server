#include "my_global.h"
#include "log_cache.h"
#include "handler.h"
#include "my_sys.h"
#include "mysql/psi/mysql_file.h"

Binlog_cache_manager binlog_cache_manager;
const char *BINLOG_CACHE_DIR= "#binlog_cache_files";

extern void ignore_db_dirs_append(const char *dirname_arg);

void binlog_cache_data::init_file_reserved_bytes()
{
#ifdef WITH_WSREP
  if (unlikely(wsrep_on(current_thd)))
  {
    if (m_file_reserved_bytes > 0 && cache_log.file != -1)
    {
      unlink(my_filename(cache_log.file));
      mysql_file_close(cache_log.file, MYF(0));
      cache_log.file= -1;

      reinit_io_cache(&cache_log, WRITE_CACHE, 0, false, true);
      m_file_reserved_bytes= 0;
      cache_log.end_of_file= saved_max_binlog_cache_size;
    }
    return;
  }
#endif

  if (unlikely(m_file_reserved_bytes == 0))
  {
    if (cache_log.file != -1)
    {
      mysql_file_close(cache_log.file, MYF(0));
      cache_log.file= -1;
    }
    reinit_io_cache(&cache_log, WRITE_CACHE, IO_SIZE, false, true);
    m_file_reserved_bytes= IO_SIZE;
    cache_log.end_of_file= saved_max_binlog_cache_size;
  }
}

void binlog_cache_data::tmp_file_name(char name[FN_REFLEN])
{
  assert(current_thd);

  size_t length;
  dirname_part(name, log_bin_basename, &length);

  snprintf(name + length, FN_REFLEN - length, "%s/%s_%llu", BINLOG_CACHE_DIR,
           cache_log.prefix, (ulonglong) this);
}

bool binlog_cache_data::init_tmp_file()
{
  char name[FN_REFLEN];

  tmp_file_name(name);
  if ((cache_log.file=
           mysql_file_open(0, name, O_CREAT | O_RDWR, MYF(MY_WME))) < 0)
  {
    sql_print_error("Failed to open binlog cache temporary file %s", name);
    cache_log.error= -1;
    return true;
  }
  return false;
}

void binlog_cache_data::detach_temp_file()
{
  /*
    If there was a rollback_to_savepoint happened before, the real length of
    tmp file can be greater than the file_end_pos. Truncate the cache tmp
    file to file_end_pos of this cache.
  */
  my_chsize(cache_log.file, my_b_tell(&cache_log), 0, MYF(MY_WME));

  mysql_file_close(cache_log.file, MYF(0));
  cache_log.file= -1;
  reset();
}

void Binlog_cache_manager::init_binlog_cache_dir()
{
  char target_dir[FN_REFLEN];
  size_t length;

  uint max_tmp_file_name_len=
      2 /* prefix */ + 10 /* max len of thread_id */ + 1 /* underline */;

  dirname_part(target_dir, log_bin_basename, &length);

  ignore_db_dirs_append(BINLOG_CACHE_DIR);

  /*
    Must ensure the full name of the tmp file is shorter than FN_REFLEN, to
    avoid overflowing the name buffer in write and commit.
  */
  if (length + strlen(BINLOG_CACHE_DIR) + max_tmp_file_name_len >= FN_REFLEN)
  {
    sql_print_error("Could not create binlog cache dir %s%s. It is too long.",
                    target_dir, BINLOG_CACHE_DIR);
    return;
  }

  memcpy(target_dir + length, BINLOG_CACHE_DIR, strlen(BINLOG_CACHE_DIR));
  target_dir[length + strlen(BINLOG_CACHE_DIR)]= 0;

  MY_DIR *dir_info= my_dir(target_dir, MYF(0));

  m_dir_created= true;

  if (!dir_info)
  {
    /* Make a dir for binlog cache temp files if not exist. */
    if (my_mkdir(target_dir, 0777, MYF(0)) < 0)
    {
      sql_print_error("Could not create binlog cache dir %s.", target_dir);
      m_dir_created= false;
    }
    return;
  }

  if (dir_info->number_of_files > 2)
  {
    /* Delete all files in directory */
    for (uint i= 0; i < dir_info->number_of_files; i++)
    {
      FILEINFO *file= dir_info->dir_entry + i;

      /* Skip the names "." and ".." */
      if (!strcmp(file->name, ".") || !strcmp(file->name, ".."))
        continue;

      char file_path[FN_REFLEN];
      fn_format(file_path, file->name, target_dir, "", MYF(MY_REPLACE_DIR));

      my_delete(file_path, MYF(0));
    }
  }

  my_dirend(dir_info);
}
