#include "my_global.h"
#include "log_cache.h"
#include "handler.h"
#include "my_sys.h"
#include "mysql/psi/mysql_file.h"
#include "mysql/service_wsrep.h"

const char *BINLOG_CACHE_DIR= "#binlog_cache_files";
char binlog_cache_dir[FN_REFLEN];
extern uint32 binlog_cache_reserved_size();

bool binlog_cache_data::init_file_reserved_bytes()
{
  // Session's cache file is not created, so created here.
  if (cache_log.file == -1)
  {
    char name[FN_REFLEN];

    /* Cache file is named with PREFIX + binlog_cache_data object's address */
    snprintf(name, FN_REFLEN, "%s/%s_%llu", cache_log.dir, cache_log.prefix,
             (ulonglong) this);

    if ((cache_log.file=
             mysql_file_open(0, name, O_CREAT | O_RDWR, MYF(MY_WME))) < 0)
    {
      sql_print_error("Failed to open binlog cache temporary file %s", name);
      cache_log.error= -1;
      return true;
    }
  }

#ifdef WITH_WSREP
  /*
    WSREP code accesses cache_log directly, so don't reserve space if WSREP is
    on.
  */
  if (unlikely(wsrep_on(current_thd)))
    return false;
#endif

  m_file_reserved_bytes= binlog_cache_reserved_size();
  cache_log.pos_in_file= m_file_reserved_bytes;
  cache_log.seek_not_done= 1;
  return false;
}

void binlog_cache_data::detach_temp_file()
{
  mysql_file_close(cache_log.file, MYF(0));
  cache_log.file= -1;
  reset();
}

extern void ignore_db_dirs_append(const char *dirname_arg);

bool init_binlog_cache_dir()
{
  size_t length;
  uint max_tmp_file_name_len=
      2 /* prefix */ + 10 /* max len of thread_id */ + 1 /* underline */;

  /*
    Even if the binary log is disabled (and thereby we wouldn't use the binlog
    cache), we need to try to build the directory name, so if it exists while
    the binlog is off (e.g. due to a previous run of mariadbd, or an SST), we
    can delete it.
  */
  dirname_part(binlog_cache_dir,
               opt_bin_log ? log_bin_basename : opt_log_basename, &length);
  /*
    Must ensure the full name of the tmp file is shorter than FN_REFLEN, to
    avoid overflowing the name buffer in write and commit.
  */
  if (length + strlen(BINLOG_CACHE_DIR) + max_tmp_file_name_len >= FN_REFLEN)
  {
    sql_print_error("Could not create binlog cache dir %s%s. It is too long.",
                    binlog_cache_dir, BINLOG_CACHE_DIR);
    return true;
  }

  memcpy(binlog_cache_dir + length, BINLOG_CACHE_DIR,
         strlen(BINLOG_CACHE_DIR));
  binlog_cache_dir[length + strlen(BINLOG_CACHE_DIR)]= 0;

  MY_DIR *dir_info= my_dir(binlog_cache_dir, MYF(0));

  /*
    If the binlog cache dir exists, yet binlogging is disabled, delete the
    directory and skip the initialization logic.
  */
  if (!opt_bin_log)
  {
    if (dir_info)
    {
      sql_print_information(
          "Found binlog cache dir '%s', yet binary logging is "
          "disabled. Deleting directory.",
          binlog_cache_dir);
      my_dirend(dir_info);
      my_rmtree(binlog_cache_dir, MYF(0));
    }
    memset(binlog_cache_dir, 0, sizeof(binlog_cache_dir));
    return false;
  }

  ignore_db_dirs_append(BINLOG_CACHE_DIR);

  if (!dir_info)
  {
    /* Make a dir for binlog cache temp files if not exist. */
    if (my_mkdir(binlog_cache_dir, 0777, MYF(0)) < 0)
    {
      sql_print_error("Could not create binlog cache dir %s.",
                      binlog_cache_dir);
      return true;
    }
    return false;
  }

  /* Try to delete all cache files in the directory. */
  for (uint i= 0; i < dir_info->number_of_files; i++)
  {
    FILEINFO *file= dir_info->dir_entry + i;

    if (strncmp(file->name, LOG_PREFIX, strlen(LOG_PREFIX)))
    {
      sql_print_warning("%s is in %s/, but it is not a binlog cache file",
                        file->name, BINLOG_CACHE_DIR);
      continue;
    }

    char file_path[FN_REFLEN];
    fn_format(file_path, file->name, binlog_cache_dir, "",
              MYF(MY_REPLACE_DIR));

    my_delete(file_path, MYF(0));
  }

  my_dirend(dir_info);
  return false;
}
