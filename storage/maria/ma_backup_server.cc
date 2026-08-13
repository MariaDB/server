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
#include "table.h"
#include <aria_backup.h>
#include <mysqld_error.h>
#include <atomic>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "span.h"

#ifndef DBUG_OFF
# include "sql_table.h"
#endif

/*
  Implementation of functions declared in ma_backup.h:
  BACKUP SERVER support for Aria engine
*/

namespace
{
  /* Utility class to implement the "backup step" interface when
  processing several lists. It implements the logic where an item
  is processed (copied) from the first list which has available
  items, and a "remaining" counter accumulates the number of
  items remaining to be processed on all lists, regardless of
  whether an item from that list was processed or not. */
  class Copy_from_list
  {
    size_t m_remaining {0};
    bool m_copy_done;
  public:
    Copy_from_list(bool copy_done= false) noexcept
    : m_copy_done(copy_done)
    {
    }

    int remaining() const noexcept
    {
      /* In theory the list of files/tables to be processed may be larger
      than the maximum value of signed int, which is the type defined by the
      API. Rather than impose artificial limits, we recognize that in extreme
      cases the exact number returned doesn't matter as much as whether there
      is more processing to be done or not. For these cases it's acceptable to
      return the max value of int. */
      return static_cast<int>(m_remaining <= std::numeric_limits<int>::max()
                              ? m_remaining : std::numeric_limits<int>::max());
    }

    /* perform copy_action on the next list element if no copy was done
       previously by this instance, in each case accumulate the count
       of remaining elements to be copied. Note that because the counter
       is incremented atomically by multiple threads, it may go over the
       list size as the copying of the list is completed.
       Returns true on failure, false on success. */
    template<typename T, typename Fn>
    bool operator()(const T &list, std::atomic<size_t> &copied_counter,
                    Fn copy_action) noexcept
    {
      if (!m_copy_done)
      {
        size_t idx= copied_counter.fetch_add(1, std::memory_order_relaxed);
        if (idx < list.size())
        {
          if (copy_action(list[idx]) != 0)
            return true;
          m_copy_done= true;
          m_remaining+= list.size() - idx - 1U;
        }
      }
      else
      {
        size_t current_copied= copied_counter.load(std::memory_order_relaxed);
        if (current_copied < list.size())
          m_remaining+= list.size() - current_copied;
      }
      return false;
    }
  };

  class Aria_backup
  {
  public:
    Aria_backup()= default;
    ~Aria_backup()
    {
#ifndef _WIN32
      if (logdir_fd >= 0)
        close(logdir_fd);
#endif
      if (translog_purge_disabled)
        translog_enable_purge();
    }

    bool initialize() noexcept
    {
#ifndef _WIN32
      /* Aria table files live under the server data directory
      (mysql_real_data_home), while the transaction logs and control file
      live under aria_log_dir_path (maria_data_root). These differ when
      aria_log_dir_path is set, so open and scan them separately. */
      datadir_fd= get_datadir_fd();
      if (datadir_fd < 0)
        return true;
      logdir_fd= open(maria_data_root, O_DIRECTORY);
      if (logdir_fd < 0)
      {
        my_error(ER_CANT_READ_DIR, MYF(0), maria_data_root, errno);
        return true;
      }
#endif // _WIN32
      assert(!translog_purge_disabled);
      translog_purge_disabled= true;
      translog_disable_purge();
      return false;
    }

    bool start_copy_no_ddl(const backup_target *target,
                           const backup_sink *sink) noexcept
    {
      assert(translog_purge_disabled);
      if (scan_dbdirs())
        return true;
      build_table_lists();
      if (sink->stream == sink->NO_STREAM)
        return ensure_target_dirs(target);
      return false;
    }

    bool start_copy_no_commit() noexcept
    {
      if (scan_logs())
        return true;
      return false;
    }

    /* Copy an Aria table that is safe to be copied in BACKUP_PHASE_NO_DDL.
      These are files for Aria user tables: Writes to non-transactional user
      tables are blocked in this phase, while transactional tables can be
      recovered using write-ahead logs. */
    int no_ddl_copy_step(const backup_target *target,
                         const backup_sink *sink) noexcept
    {
      Copy_from_list copy_from_list;
      if (copy_from_list(user_tables, user_tables_copied,
                         [this, target, sink](const table_ref &table) noexcept
                         {
                           return copy_table(target, sink, table);
                         }))
        return -1;
      return copy_from_list.remaining();
    }

    /* Copy an entity (Aria table or log file) that is only safe to copy in
       BACKUP_PHASE_NO_COMMIT. System tables fall in this category. */
    int no_commit_copy_step(const backup_target *target,
                            const backup_sink *sink) noexcept
    {
      bool control_file_copied_now= false;
      if (have_control_file)
      {
        bool already_copied= control_file_copied.exchange(true);
        if (!already_copied)
        {
          if (copy_control_file(target, sink) != 0)
            return -1;
          control_file_copied_now= true;
        }
      }

      Copy_from_list copy_from_list(control_file_copied_now);

      if (copy_from_list(system_tables, system_tables_copied,
                         [this, target, sink](const table_ref &table) noexcept
                         {
                           return copy_table(target, sink, table);
                         }))
        return -1;

      if (copy_from_list(log_files, log_files_copied,
                         [this, target, sink](const std::string &path) noexcept
                         {
                           return copy_log_file(target, sink, path.c_str());
                         }))
        return -1;

      return copy_from_list.remaining();
    }

    int end() noexcept
    {
      assert(translog_purge_disabled);
      translog_purge_disabled= false;
      translog_enable_purge();
      return 0;
    }
  private:
#ifndef _WIN32
    /** The server data directory */
    int datadir_fd{-1};
    /** The Aria log directory aria_log_dir_path (logs, control file) */
    int logdir_fd{-1};
#endif
    /** whether the Aria translog_disable_purge() is in effect */
    bool translog_purge_disabled{false};

    /* File extensions are 4 characters long (dot and 3 letter extension) */
    static constexpr size_t ext_len= 4;
    static constexpr const char* data_ext {MARIA_NAME_DEXT};
    static constexpr const char* index_ext {MARIA_NAME_IEXT};
    static constexpr LEX_CSTRING log_file_prefix {C_STRING_WITH_LEN("aria_log.")};
    static constexpr LEX_CSTRING tmp_prefix {C_STRING_WITH_LEN(tmp_file_prefix)};
    static constexpr LEX_CSTRING control_file_name {C_STRING_WITH_LEN("aria_log_control")};
    using dir_name = std::string;
    using dir_contents = std::vector<std::string>;
    using database_dir = std::pair<dir_name, dir_contents>;
    using database_dirs = std::vector<database_dir>;
    /* Collection of tables to be backed up. */
    database_dirs tables;
    /* Aria log files */
    std::vector<std::string> log_files;

    bool have_control_file = false;

    /* Refer to a string stored elsewhere */
    using dir_ref= std::string_view;
    using tablename_ref= std::string_view;
    using table_ref= std::pair<dir_ref, tablename_ref>;
    using table_list= std::vector<table_ref>;

    table_list user_tables;
    table_list system_tables;
    std::atomic<size_t> user_tables_copied {0};
    std::atomic<size_t> system_tables_copied {0};
    std::atomic<size_t> log_files_copied {0};
    std::atomic<bool> control_file_copied {false};

    ATTRIBUTE_COLD ATTRIBUTE_NOINLINE
      static int dir_error(const char *name) noexcept
    {
      my_error(ER_CANT_READ_DIR, MYF(0), name, my_errno);
      return 1;
    }

    int scan_dbdirs() noexcept
    {
      MY_DIR *data_dir= my_dir(mysql_real_data_home, MYF(MY_WANT_STAT));
      if (!data_dir)
        return dir_error(mysql_real_data_home);
      int fail= 0;
      for (const fileinfo &fi :
             st_::span<const fileinfo>{data_dir->dir_entry,
                                       data_dir->number_of_files})
        if ((fi.mystat->st_mode & S_IFMT) == S_IFDIR)
        {
          fail= scan_database_dir(fi.name);
          if (fail != 0)
            goto func_exit;
        }
    func_exit:
      my_dirend(data_dir);
      return fail;
    }

    int scan_database_dir(const char* dir_name) noexcept
    {
      const std::string dir_path{make_path(mysql_real_data_home, dir_name)};
      MY_DIR *dir_info= my_dir(dir_path.c_str(), MYF(MY_WANT_STAT));
      if (!dir_info)
        return dir_error(dir_path.c_str());
      dir_contents dir_tables;
      for (const fileinfo &fi :
             st_::span<const fileinfo>{dir_info->dir_entry,
                                       dir_info->number_of_files})
      {
        const LEX_CSTRING filename {fi.name, strlen(fi.name)};
        if (filename.length >= ext_len)
        {
          /* Length of filename without extension. */
          size_t base_filename_len= filename.length - ext_len;
          const char* suffix = filename.str + base_filename_len;
          if (match_ext(suffix, index_ext))
          {
            if (!is_tmp_table(filename))
            {
              dir_tables.emplace_back(filename.str, base_filename_len);
            }
          }
        }
      }
      if (!dir_tables.empty())
        tables.emplace_back(dir_name, std::move(dir_tables));
      my_dirend(dir_info);
      return 0;
    }

    static bool is_tmp_table(const LEX_CSTRING &filename) noexcept
    {
      return begins_with(filename, tmp_prefix);
    }

    void build_table_lists() noexcept
    {
      /* This relies on the assumption that all the non-transactional
      non-user tables are in MYSQL_SCHEMA ("mysql"). We copy the
      transactional non-user tables in this schema under a higher lock
      level than strictly necessary to avoid the cost of inspecting
      these tables.
      Note that directory name matching relies on the fact that
      MYSQL_SCHEMA_NAME has the same representation in the filesystem
      charset (as provided by directory listing) and the internal
      identifier charset, which is true at least as long as it's
      comprised of ASCII letters, digits and underscore character
      only. */
      assert(schema_name_is_its_own_filename());

      for (const database_dir& dir : tables)
      {
        if (match_str(dir.first, MYSQL_SCHEMA_NAME))
          for (const std::string& table : dir.second)
            system_tables.emplace_back(dir.first, table);
        else
          for (const std::string& table : dir.second)
            user_tables.emplace_back(dir.first, table);
      }
    }

    int scan_logs() noexcept
    {
      const char *base_dir= maria_data_root;
      MY_DIR *dir_info= my_dir(base_dir, MYF(MY_WANT_STAT));
      if (!dir_info)
        return dir_error(base_dir);
      for (const fileinfo &fi :
             st_::span<const fileinfo>{dir_info->dir_entry,
                                       dir_info->number_of_files})
      {
        const LEX_CSTRING filename {fi.name, strlen(fi.name)};
        if (begins_with(filename, log_file_prefix))
          log_files.emplace_back(LEX_STRING_WITH_LEN(filename));
        else if (is_control_file_name(filename))
          have_control_file = true;
      }
      my_dirend(dir_info);
      return 0;
    }

    bool ensure_target_dirs(const backup_target *target) noexcept
    {
      for (const database_dir &dir : tables)
        if (::ensure_target_subdir(target, dir.first.c_str()) != 0)
          return true;
      return false;
    }

    int copy_table(const backup_target *target, const backup_sink *sink,
                   const table_ref& table) noexcept
    {
      dir_ref dir_name = table.first;
      tablename_ref table_name = table.second;
      std::string index_path;
      index_path.reserve(dir_name.size() + table_name.size() + 5);
      index_path= dir_name;
      index_path += '/';
      index_path.append(table_name.begin(), table_name.end());
      std::string data_path;
      data_path.reserve(dir_name.size() + table_name.size() + 5);
      data_path= index_path;
      index_path+= index_ext;
      data_path+= data_ext;

      return copy_table_file(target, sink, index_path) ||
             copy_table_file(target, sink, data_path);
    }

    int copy_control_file(const backup_target *target, const backup_sink *sink) noexcept
    {
      if (!have_control_file)
        return 0;
      return copy_log_file(target, sink, control_file_name.str);
    }

    int copy_table_file(const backup_target *target,
                        const backup_sink *sink, 
                        const std::string &path) const noexcept
    {
      return copy_table_file(target, sink, path.c_str());
    }

    int copy_table_file(const backup_target *target,
                        const backup_sink *sink,
                        const char *path) const noexcept
    {
#ifdef _WIN32
      return ::copy_datafile_to_target(path, target, sink);
#else
      return ::copy_datafile_to_target(datadir_fd, path, target, sink);
#endif
    }

    int copy_log_file(const backup_target *target,
                      const backup_sink *sink,
                      const char *filename)
    {
#ifndef _WIN32
      int src_fd = openat(logdir_fd, filename, O_RDONLY);
      if (src_fd < 0)
      {
        my_error(ER_CANT_OPEN_FILE, MYF(0),
                 make_path(maria_data_root, filename).c_str(),
                 errno);
        return 1;
      }
      int ret_val=  copy_fd_to_target(src_fd, target, filename, sink);
      close(src_fd);
      return ret_val;
#else
      return copy_entire_file(make_path(maria_data_root, filename).c_str(),
                              filename, target, sink);
#endif
    }

    static bool match_ext(const char* ext1, const char* ext2) noexcept
    {
      return memcmp(ext1, ext2, ext_len) == 0;
    }

    static bool begins_with(const LEX_CSTRING &str,
                            const LEX_CSTRING &prefix) noexcept
    {
      if (str.length < prefix.length)
        return false;
      return memcmp(str.str, prefix.str, prefix.length) == 0;
    }

    static bool is_control_file_name(const LEX_CSTRING &str)
    {
      return str.length == control_file_name.length &&
        memcmp(str.str, control_file_name.str, control_file_name.length) == 0;
    }

    static bool match_str(const std::string &str1,
                          const LEX_CSTRING &str2) noexcept
    {
      return str1.length() == str2.length &&
             memcmp(str1.data(), str2.str, str2.length) == 0;
    }

#ifndef DBUG_OFF
    /* Whether MYSQL_SCHEMA_NAME is left unchanged by the conversion from the
    identifier charset to the filesystem charset. That is what allows
    build_table_lists() to compare it with a directory name byte by byte. */
    static bool schema_name_is_its_own_filename() noexcept
    {
      char filename[FN_REFLEN];
      uint length= tablename_to_filename(MYSQL_SCHEMA_NAME.str, filename,
                                         sizeof(filename));
      return length == MYSQL_SCHEMA_NAME.length &&
             memcmp(filename, MYSQL_SCHEMA_NAME.str, length) == 0;
    }
#endif
  };
}

void *aria_backup_start(THD *thd, const backup_target *target,
                        backup_phase phase, const backup_sink *sink) noexcept
{
  Aria_backup *aria_backup {};
  if (phase == BACKUP_PHASE_PREPARE_START)
  {
    return 0;
  }
  else if (phase == BACKUP_PHASE_START)
  {
    assert(!sink->ha_data);
    aria_backup= new Aria_backup();
    if (aria_backup->initialize())
    {
      delete aria_backup;
      goto error;
    }
    return aria_backup;
  }

  assert (sink->ha_data != reinterpret_cast<void*>(-1));
  aria_backup= static_cast<Aria_backup*>(sink->ha_data);
  assert(aria_backup);
  switch(phase)
  {
  case BACKUP_PHASE_NO_DDL:
    if (aria_backup->start_copy_no_ddl(target, sink))
      goto error;
    break;
  case BACKUP_PHASE_NO_COMMIT:
    if (aria_backup->start_copy_no_commit())
      goto error;
    break;
  default:
    break;
  }
  return sink->ha_data;
error:
  return reinterpret_cast<void*>(-1);
}


int aria_backup_step(THD*, const backup_target *target, backup_phase phase,
                     const backup_sink *sink) noexcept
{
  assert (sink->ha_data != reinterpret_cast<void*>(-1));
  Aria_backup *aria_backup= static_cast<Aria_backup*>(sink->ha_data);
  assert(aria_backup);
  switch (phase)
  {
  case BACKUP_PHASE_NO_DDL:
    return aria_backup->no_ddl_copy_step(target, sink);
  case BACKUP_PHASE_NO_COMMIT:
    return aria_backup->no_commit_copy_step(target, sink);
  default:
    return 0;
  }
}

int aria_backup_end(THD *thd, const backup_target *target, backup_phase phase,
                    const backup_sink *sink) noexcept
{
  assert (sink->ha_data != reinterpret_cast<void*>(-1));
  Aria_backup *aria_backup= static_cast<Aria_backup*>(sink->ha_data);
  switch (phase) {
  case BACKUP_PHASE_NO_COMMIT:
    assert(aria_backup);
    return aria_backup->end();
  case BACKUP_PHASE_FINISH:
    delete aria_backup;
    /* fall through */
  default:
    return 0;
  }
}
