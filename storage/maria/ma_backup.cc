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
#include "ma_backup.h"
#include <mysqld_error.h>
#include <string>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/clonefile.h>
#include <copyfile.h>
#endif

/*
  Implementation of functions declatred in ma_backup.h:
  BACKUP SERVER support for Aria engine
*/

using namespace std::string_literals;

namespace
{
  class Source_dir
  {
  public:
    Source_dir(const char* path, myf flags) noexcept
    {
      dir_info= my_dir(path, flags);
      if (!dir_info)
      {
        my_error(ER_CANT_READ_DIR, MYF(0), path, my_errno);
      }
    }
    ~Source_dir() noexcept
    {
      my_dirend(dir_info);
    }
    bool is_error() const noexcept
    {
      return !dir_info;
    }
    template<typename Fn>
    int for_each(Fn fn) const noexcept
    {
      for (size_t i= 0; i < dir_info->number_of_files; i++)
      {
        if (fn(dir_info->dir_entry[i]) != 0)
          return 1;
      }
      return 0;
    }

  private:
    MY_DIR *dir_info {nullptr};
  };


  /** Backup state; protected by log_sys.latch */
  class Aria_backup
  {
  public:
    explicit Aria_backup(THD *thd, IF_WIN(const char*,int) target) noexcept
    : target_dir(target)
    {
       translog_disable_purge();
    }

    int end(THD *thd, bool abort) noexcept
    {
      int ret_val = 0;
      if (!abort) {
        if (int err= perform_backup() != 0)
        {
          ret_val= err;
        };
      }
      translog_enable_purge();
      return ret_val;
    }
  private:
    const IF_WIN(const char*,int) target_dir;
    static const std::vector<std::string> data_exts;;
    const std::string log_file_prefix {"aria_log."};
    using dir_name = std::string;
    using dir_contents = std::vector<std::string>;
    using database_dir = std::pair<dir_name, dir_contents>;
    std::vector<database_dir> database_dirs;
    std::vector<std::string> log_files;
    bool have_control_file = false;

    int perform_backup() noexcept
    {
      if (scan_datadir())
        return 1;
      if (copy_databases())
        return 1;
      if (copy_control_file())
        return 1;
      if (copy_logs())
        return 1;
      return 0;
    }

    int scan_datadir() noexcept
    {
      const char* base_dir = maria_data_root;
      Source_dir datadir(base_dir, MYF(MY_WANT_STAT));
      if (datadir.is_error())
        return 1;
      datadir.for_each([this](const fileinfo &fi)
      {
        if (fi.mystat->st_mode & S_IFDIR)
        {
          if (scan_database_dir(fi.name) != 0)
            return 1;
        } else if (begins_with(fi.name, log_file_prefix))
          log_files.emplace_back(fi.name);
        else if (strcmp(fi.name, "aria_log_control") == 0)
          have_control_file = true;
        return 0;
      });
      return 0;
    }

    int scan_database_dir(const char* dir_name) noexcept
    {
      const char* base_dir = maria_data_root;
      std::string dir_path = std::string(base_dir) + "/" + dir_name;
      Source_dir db_dir(dir_path.c_str(), MYF(0));
      if (db_dir.is_error())
        return 1;
      std::vector<std::string> files_to_backup;
      db_dir.for_each([&files_to_backup](const fileinfo &fi)
      {
        if (is_db_file(fi.name))
          files_to_backup.emplace_back(fi.name);
        return 0;
      });
      if (!files_to_backup.empty())
        database_dirs.emplace_back(dir_name, std::move(files_to_backup));
      return 0;
    }

    int copy_databases() noexcept
    {
      for (const database_dir& dir : database_dirs)
      {
         const char* dir_name = dir.first.c_str();
         if (mkdirat(target_dir, dir_name, 0777) != 0)
         {
            if (errno != EEXIST)
            {
              my_error(ER_CANT_CREATE_FILE, MYF(0), dir_name, errno);
              return 1;
            }
         }
         if (copy_database(dir) != 0)
           return 1;
      }
      return 0;
    }

    int copy_database(const database_dir& dir) noexcept
    {
      for (const std::string& file : dir.second)
      {
        std::string file_path= dir.first + "/" + file;
        if (copy_file(file_path) != 0)
          return 1;
      }
      return 0;
    }

    int copy_control_file() noexcept
    {
      if (!have_control_file)
        return 0;
      return copy_file("aria_log_control");
    }

    int copy_logs() noexcept
    {
      for (const std::string& file : log_files)
      {
        if (copy_file(file) != 0)
          return 1;
      }
      return 0;
    }

    int copy_file(const std::string &path) const noexcept
    {
      std::string src_path= std::string(maria_data_root) + "/" + path;
#ifdef __APPLE__
      int ret_val = 0;
      int src_fd = open(src_path.c_str(), O_RDONLY);
      if (src_fd < 0)
      {
        my_error(ER_CANT_OPEN_FILE, MYF(0), src_path.c_str(), errno);
        return 1;
      }
      int tgt_fd = openat(target_dir, path.c_str(),
                          O_CREAT | O_EXCL | O_WRONLY, 0777);
      if (tgt_fd < 0)
      {
        my_error(ER_CANT_CREATE_FILE, MYF(0), path.c_str(), errno);
        ret_val = 1;
        goto finish;
      }
      if (fcopyfile(src_fd, tgt_fd, nullptr, COPYFILE_DATA) != 0)
      {
        my_error(ER_ERROR_ON_WRITE, MYF(0), path.c_str(), errno);
        ret_val = 1;
      }
      close(tgt_fd);
    finish:
      close(src_fd);
      return ret_val;
#else
      return 1;
#endif
    }


    static bool is_db_file(const char* file_name) noexcept
    {
      for (const std::string& ext : data_exts)
      {
        if (ends_with(file_name, ext))
          return true;
      }
      return false;
    }

    static bool ends_with(const char* str, const std::string& suffix) noexcept
    {
      size_t str_len = strlen(str);
      size_t suffix_len = suffix.size();
      if (str_len < suffix_len)
        return false;
      return memcmp(str + str_len - suffix_len, 
                    suffix.data(),
                    suffix_len) == 0;
    }

    static bool begins_with(const char* str, const std::string& prefix) noexcept
    {
      return strncmp(str, prefix.data(), prefix.size()) == 0;
    }
  };

  const std::vector<std::string> Aria_backup::data_exts {".MAD"s, ".MAI"s};

  std::unique_ptr<Aria_backup> aria_backup;
}

int aria_backup_start(THD *thd, IF_WIN(const char*,int) target) noexcept
{
  aria_backup= std::make_unique<Aria_backup>(thd, target);
  return 0;
}

int aria_backup_step(THD *thd) noexcept
{
  return 0;
}

int aria_backup_end(THD *thd, bool abort) noexcept
{
  int ret_val= aria_backup->end(thd, abort);
  aria_backup.reset();
  return ret_val;
}

