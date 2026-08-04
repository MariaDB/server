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
#if 1 // tc_purge(), tdc_purge()
# include "sql_class.h"
# include "table_cache.h"
#endif
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "span.h"

/*
  Implementation of functions declatred in ma_backup.h:
  BACKUP SERVER support for Aria engine
*/

namespace
{
  /** Backup state; protected by log_sys.latch */
  class Aria_backup
  {
  public:
    Aria_backup()= default;
    ~Aria_backup()
    {
#ifndef _WIN32
      if (datadir_fd >= 0)
        std::ignore= close(datadir_fd);
      if (logdir_fd >= 0)
        std::ignore= close(logdir_fd);
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
      datadir_fd= open(mysql_real_data_home, O_DIRECTORY);
      if (datadir_fd < 0)
      {
        my_error(ER_CANT_READ_DIR, MYF(0), mysql_real_data_home, errno);
        return true;
      }
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

    int end(const backup_target &target, const backup_sink &sink) noexcept
    {
      int ret_val= perform_backup(target, sink);
      assert(translog_purge_disabled);
      translog_purge_disabled= false;
      translog_enable_purge();
      return ret_val;
    }
  private:
#ifndef _WIN32
    /** The server data directory (Aria table files) */
    int datadir_fd{-1};
    /** The Aria log directory aria_log_dir_path (logs, control file) */
    int logdir_fd{-1};
#endif
    /** whether the Aria translog_disable_purge() is in effect */
    bool translog_purge_disabled{false};
    static constexpr const char zerobuf[511]{};
    using dir_contents = std::vector<std::string>;
    using database_dir = std::pair<std::string, dir_contents>;
    std::vector<database_dir> database_dirs;
    std::vector<std::string> log_files;
    bool have_control_file = false;

    int perform_backup(const backup_target &target, const backup_sink &sink)
      noexcept
    {
      return scan_datadir() || copy_databases(target, sink) ||
        copy_control_file(target, sink) ||
        translog_flush(translog_get_horizon()) ||
        copy_logs(target, sink);
    }

    ATTRIBUTE_COLD ATTRIBUTE_NOINLINE
      static int dir_error(const char *name) noexcept
    {
      my_error(ER_CANT_READ_DIR, MYF(0), name, my_errno);
      return 1;
    }

    int scan_datadir() noexcept
    {
      /* Scan the server data directory for Aria table files. */
      MY_DIR *data_dir= my_dir(mysql_real_data_home, MYF(MY_WANT_STAT));
      if (!data_dir)
        return dir_error(mysql_real_data_home);
      int fail= 0;
      for (const fileinfo &fi :
             st_::span<const fileinfo>{data_dir->dir_entry,
                                       data_dir->number_of_files})
        if ((fi.mystat->st_mode & S_IFMT) == S_IFDIR)
          if ((fail= scan_database_dir(fi.name)) != 0)
            break;
      my_dirend(data_dir);
      if (fail)
        return fail;

      /* Scan aria_log_dir_path for the transaction logs and control file. */
      MY_DIR *log_dir= my_dir(maria_data_root, MYF(MY_WANT_STAT));
      if (!log_dir)
        return dir_error(maria_data_root);
      for (const fileinfo &fi :
             st_::span<const fileinfo>{log_dir->dir_entry,
                                       log_dir->number_of_files})
      {
        if (!strncmp(fi.name, C_STRING_WITH_LEN("aria_log.")))
          log_files.emplace_back(fi.name);
        else if (!strcmp(fi.name, "aria_log_control"))
          have_control_file = true;
      }
      my_dirend(log_dir);
      return 0;
    }

    int scan_database_dir(const char* dir_name) noexcept
    {
      const std::string dir_path{make_path(mysql_real_data_home, dir_name)};
      MY_DIR *dir_info= my_dir(dir_path.c_str(), MYF(MY_WANT_STAT));
      if (!dir_info)
        return dir_error(dir_path.c_str());
      std::vector<std::string> files_to_backup;
      for (const fileinfo &fi :
             st_::span<const fileinfo>{dir_info->dir_entry,
                                       dir_info->number_of_files})
        if (is_db_file(fi.name))
          files_to_backup.emplace_back(fi.name);
      if (!files_to_backup.empty())
        database_dirs.emplace_back(dir_name, std::move(files_to_backup));
      my_dirend(dir_info);
      return 0;
    }

    int copy_databases(const backup_target &target, const backup_sink &sink)
      noexcept
    {
      for (const database_dir &dir : database_dirs)
      {
        if (sink.stream != sink.NO_STREAM);
        else if (int fail= ensure_target_subdir(target, dir.first.c_str()))
          return fail;
        if (int fail= copy_database(target, sink, dir))
          return fail;
      }
      return 0;
    }

    /*
       Create directory in the target directory if it does not exist.
       Return 0 on success, non-0 on failure. Set errno in case of failure
    */
    int ensure_target_subdir(const backup_target &target, const char *name)
      noexcept
    {
#ifdef _WIN32
      if (CreateDirectory(make_path(target.path, name).c_str(), nullptr))
        return 0;
      DWORD err= GetLastError();
      if (err == ERROR_ALREADY_EXISTS)
        return 0;
      my_osmaperr(err);
#else
      if (likely(!mkdirat(target.fd, name, 0777) || errno == EEXIST))
        return 0;
#endif
      my_error(ER_CANT_CREATE_FILE, MYF(0), name, errno);
      return 1;
    }

    int copy_database(const backup_target &target, const backup_sink &sink,
                      const database_dir& dir) noexcept
    {
      std::string file_path;
      for (const std::string &file : dir.second)
      {
        file_path= dir.first;
        file_path.push_back('/');
        file_path.append(file);
        if (int fail= copy_file(target, sink, file_path.c_str(), false))
          return fail;
      }
      return 0;
    }

    int copy_control_file(const backup_target &target, const backup_sink &sink)
      noexcept
    {
      if (!have_control_file)
        return 0;
      return copy_file(target, sink, "aria_log_control", true);
    }

    int copy_logs(const backup_target &target, const backup_sink &sink)
      noexcept
    {
      for (const std::string &file : log_files)
        if (int fail= copy_file(target, sink, file.c_str(), true))
          return fail;
      return 0;
    }

    int copy_file(const backup_target &target, const backup_sink &sink,
                  const char *path, bool is_log) const noexcept
    {
#ifndef _WIN32
      int ret_val{0};
      int src_fd{openat(is_log ? logdir_fd : datadir_fd, path, O_RDONLY)};
      if (src_fd < 0)
      {
        my_error(ER_CANT_OPEN_FILE, MYF(0), path, errno);
        return 1;
      }
      int tgt_fd{sink.stream};
      if (tgt_fd == sink.NO_STREAM)
      {
        tgt_fd= openat(target.fd, path,
                       O_CREAT | O_EXCL | O_WRONLY, 0666);
        if (tgt_fd < 0)
        {
          my_error(ER_CANT_CREATE_FILE, MYF(0), path, errno);
          ret_val= 1;
        }
        else
        {
          ret_val= copy_entire_file(src_fd, tgt_fd);
          if (ret_val | close(tgt_fd))
          {
          write_error:
            my_error(ER_ERROR_ON_WRITE, MYF(0), path, errno);
            ret_val= 1;
          }
        }
      }
      else
      {
        uint64_t end= uint64_t(lseek(src_fd, 0, SEEK_END));
        if (backup_stream_start(tgt_fd, path, 0644, end, nullptr, 0) ||
            backup_stream_append(src_fd, tgt_fd, 0, end))
          goto write_error;
        if (size_t pad= size_t(end) & 511)
          if (backup_stream_write(tgt_fd, zerobuf, 512 - pad))
            goto write_error;
      }

      close(src_fd);
      return ret_val;
#else
      const std::string src_path
        {make_path(is_log ? maria_data_root : mysql_real_data_home, path)};

      if (sink.stream == sink.NO_STREAM)
      {
        std::string dest_path{make_path(target.path, path)};
        if (!CopyFileEx(src_path.c_str(), dest_path.c_str(),
                        nullptr, nullptr, nullptr, COPY_FILE_NO_BUFFERING))
        {
          my_osmaperr(GetLastError());
          my_error(ER_CANT_CREATE_FILE, MYF(0), dest_path.c_str(), errno);
          return 1;
        }
      }
      else
      {
        HANDLE src, dst{sink.stream};
        for (;;)
        {
          src= CreateFile(src_path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                          my_win_file_secattr(), OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
          if (src != INVALID_HANDLE_VALUE)
            break;
          switch (GetLastError()) {
          case ERROR_SHARING_VIOLATION:
          case ERROR_LOCK_VIOLATION:
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
          }

          my_osmaperr(GetLastError());
          my_error(ER_FILE_NOT_FOUND, MYF(ME_ERROR_LOG), src_path.c_str(),
                   errno);
          return -1;
        }

        LARGE_INTEGER li;
        if (!GetFileSizeEx(src, &li))
        {
        write_error:
          my_osmaperr(GetLastError());
          my_error(ER_ERROR_ON_WRITE, MYF(0), path, errno);
          if (src != INVALID_HANDLE_VALUE)
            CloseHandle(src);
          return -1;
        }

        if (backup_stream_start(dst, path, 0644, li.QuadPart, nullptr, 0) ||
            backup_stream_append_plain(src, dst, 0, li.QuadPart))
          goto write_error;

        if (size_t pad= size_t(li.LowPart) & 511)
          if (backup_stream_write(dst, zerobuf, 512 - pad))
            goto write_error;
        if (!CloseHandle(src))
        {
          src= INVALID_HANDLE_VALUE;
          goto write_error;
        }
      }
      return 0;
#endif
    }


    static bool is_db_file(const char* file_name) noexcept
    {
      size_t len= strlen(file_name);
      if (len < 4)
        return false;
      uint32_t suffix;
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
        return true;
      }
    }

    /**
       Construct a file path.
       @param dir   directory name
       @param name  file name
       @return dir/name
    */
    static std::string make_path(const char *dir, const char *name)
    {
      std::string path{dir};
      path.push_back('/');
      path.append(name);
      return path;
    }
  };
}

void *aria_backup_start(THD *thd, const backup_target *target,
                        backup_phase phase, const backup_sink *sink) noexcept
{
  switch (phase) {
  case BACKUP_PHASE_PREPARE_START:
    return 0;
  default:
    return sink->ha_data;
  case BACKUP_PHASE_NO_COMMIT:
    assert(!sink->ha_data);
    Aria_backup *aria_backup{new Aria_backup};
    if (aria_backup->initialize())
    {
      delete aria_backup;
      return reinterpret_cast<void*>(-1);
    }
    return aria_backup;
  }
}

#if 0 // FIXME: implement the actual copying here
int aria_backup_step(THD*, const backup_target*, backup_phase,
                     const backup_sink*) noexcept
{
  return 0;
}
#endif

int aria_backup_end(THD *thd, const backup_target *target, backup_phase phase,
                    const backup_sink *sink) noexcept
{
  Aria_backup *aria_backup= static_cast<Aria_backup*>(sink->ha_data);
  switch (phase) {
  case BACKUP_PHASE_NO_COMMIT:
    assert(aria_backup);
#if 1 // FIXME: invoke these only for Aria, MyISAM, CSV but not others
    tc_purge();
    tdc_purge(true);
#endif
    return aria_backup->end(*target, *sink);
  case BACKUP_PHASE_FINISH:
    delete aria_backup;
    /* fall through */
  default:
    return 0;
  }
}
