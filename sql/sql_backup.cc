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

#include "my_global.h"
#include "mdl.h"
#include "mysys_err.h"
#include "sql_class.h"
#include "sql_base.h"
#include "sql_backup.h"
#include "sql_backup_interface.h"
#include "sql_parse.h"
#include "my_atomic_wrapper.h"
#include "tpool.h"
#include "aligned.h"
#include "my_dir.h"
#include <unordered_map>

static bool backup_execute(THD *thd, const char *target, const char *command,
                           int threads);

/**
   @brief BACKUP SERVER command

   BACKUP SERVER copies the storage of a running server optionally
   using multiple CONCURRENT threads, each one writing to the target
   directory or its own tar stream.

   BACKUP SERVER TO '/path/to/directory' [ 1 CONCURRENT ];
   BACKUP SERVER WITH [ 1 CONCURRENT ] 'command';

   The operation is divided into multiple stages; @see backup_stages

   Each stage is divided into "start" and "end", which are invoked
   by this thread, and a number of "step" in between that may be
   invoked by multiple CONCURRENT threads.
   @see handlerton::backup_start
   @see handlerton::backup_step
   @see handlerton::backup_end

   Most stages are tied with BACKUP STAGE locks; the actual first work stage:
   @see BACKUP_START
   @see MDL_BACKUP_START

   The snapshot for transactional storage engines is determined in:
   @see BACKUP_LOCK_COMMIT
   @see MDL_BACKUP_WAIT_COMMIT

   BACKUP_PREPARE, BACKUP_ABORTED, BACKUP_FINISHED are executed while
   not holding any MDL.

   @param thd   client connection
   @return whether the operation failed
*/
bool Sql_cmd_backup::execute(THD *thd)
{
  return backup_execute(thd, target, command, threads);
}

#ifdef _WIN32
using tpool::pread;
using tpool::pwrite;

/**
   Create a subdirectory unless we are streaming or it pre-exists.
   @param name      base name of subdirectory to create
   @return error code (also errno will be set)
   @retval 0 on success (errno might not be touched)
*/
int backup_target::mkdir(const char *name) const noexcept
{
  if (!path)
    return 0;
  int ret= snprintf(NULL, 0, "%s/%s", path, name) + 1;
  char *path= static_cast<char*>(malloc(ret));
  if (!path)
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), name);
    return 1;
  }
  snprintf(path, ret, "%s/%s", this->path, name);
  if (CreateDirectory(path, NULL))
    ret= 0;
  else
  {
    const DWORD err= GetLastError();
    ret= err != ERROR_ALREADY_EXISTS;
    if (ret)
    {
      my_osmaperr(err);
      my_error(ER_CANT_CREATE_FILE, MYF(0), path, errno);
    }
  }
  free(path);
  return ret;
}

/**
   Create a file in the target directory.
   @param name   file name
   @return file handle
   @retval backup_sink::NO_STREAM in case of an error
*/
my_native_file backup_target::create_file(const char *name) const noexcept
{
  std::string p{path};
  p.push_back('/');
  p.append(name);
  HANDLE dst= CreateFile(p.c_str(), GENERIC_WRITE, 0, my_win_file_secattr(),
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dst == INVALID_HANDLE_VALUE)
  {
    my_osmaperr(GetLastError());
    my_error(ER_CANT_CREATE_FILE, MYF(0), p.c_str(), errno);
  }
  return dst;
}

bool backup_target::seek(my_native_file fd, my_off_t offset) noexcept
{
  LARGE_INTEGER pos;
  pos.QuadPart= offset;
  if (SetFilePointerEx(fd, pos, nullptr, FILE_BEGIN))
    return false;
  my_osmaperr(GetLastError());
  return true;
}
#else
# include <sys/types.h>
# include <dirent.h>
# ifdef __linux__
#  include <sys/sendfile.h>
# endif

/**
   Create a subdirectory unless we are streaming or it pre-exists.
   @param name      base name of subdirectory to create
   @return error code (also errno will be set)
   @retval 0 on success (errno might not be touched)
*/
int backup_target::mkdir(const char *name) const noexcept
{
  if (fd == -1 || likely(!mkdirat(fd, name, 0777)) || errno == EEXIST)
    return 0;
  my_error(ER_CANT_CREATE_FILE, MYF(0), name, errno);
  return 1;
}

/**
   Create a file in the target directory.
   @param name   file name
   @return file handle
   @retval backup_sink::NO_STREAM in case of an error
*/
my_native_file backup_target::create_file(const char *name) const noexcept
{
  constexpr int flags{O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC};
  int dst{openat(fd, name, flags, 0666)};
  if (dst < 0)
    my_error(ER_CANT_CREATE_FILE, MYF(0), name, errno);
  return dst;
}

bool backup_target::seek(my_native_file fd, my_off_t offset) noexcept
{
  return offset != my_off_t(lseek(fd, offset, SEEK_SET));
}
#endif

/**
   Copy a file using positioned reads.
   @tparam stream true=write to a stream, false=pwrite to a file
   @param in_fd   source file
   @param out_fd  destination
   @param o       start offset
   @param end     last offset (exclusive)
   @return error code (non-positive)
   @retval 0  on success
*/
template<bool stream>
static int pread_write(IF_WIN(native_file_handle, int) in_fd,
                       my_native_file out_fd,
                       uint64_t o, uint64_t end) noexcept
{
  assert(out_fd != backup_sink::NO_STREAM);
  assert(end >= o);
  uint64_t count{end - o};
  if (!count)
    return 0;
  constexpr size_t max_size{4 << 20};
  /* An integer multiple of 4096 bytes, up to max_size */
  size_t size{count >= max_size ? max_size : (size_t(count) + 4095) & ~4095};
  char *b= static_cast<char*>(aligned_malloc(size, 4096));
  if (!b)
  {
    /*
      _aligned_malloc() and memalign() set errno, but posix_memalign()
      returns the error number. The alignment is valid, so it is ENOMEM.
    */
    errno= ENOMEM;
    return -1;
  }
  ssize_t ret;
  for (;;)
  {
    if (count < size)
      size= count;
    ret= pread(in_fd, b, ssize_t(size), o);
    if (ret > 0)
    {
      uint64_t wo{o};
      o+= ret;
      count-= uint64_t(ret);

      if (!stream)
      {
        const char *buf= b;
        size_t size{size_t(ret)};
        do
        {
          ssize_t wrote= pwrite(out_fd, buf, size, wo);
          assert(wrote <= ssize_t(size));
          if (wrote < 0)
          {
            ret= int(wrote);
            break;
          }
          buf+= wrote;
          wo+= wrote;
          size-= size_t(wrote);
        }
        while (size);
        assert(wo == o || ret < 0);
      }
      else if (backup_sink::write(out_fd, b, size_t(ret)))
      {
        ret= -1;
        break;
      }
    }
    if (ret < 0)
    {
      /* pread(), pwrite() or backup_sink::write() failed */
      IF_WIN(my_osmaperr(GetLastError()),);
      break;
    }
    if (!count)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  aligned_free(b);
  return int(ret);
}

namespace backup {

#ifdef _WIN32
int copy(native_file_handle in_fd,
         my_native_file out_fd, uint64_t o, uint64_t end) noexcept
{
  return pread_write<false>(in_fd, out_fd, o, end);
}

/**
   Copy or stream a file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param path       file name
   @param dir_prefix length of the path prefix to omit from the backup
   @return error code (also errno will be set)
   @retval 0 on success (errno might not be touched)
*/
int copy_or_stream(const backup_target &target, const backup_sink &sink,
                   const char *path, size_t dir_prefix) noexcept
{
  int ret= -1;
  if (sink.stream == sink.NO_STREAM)
  {
    int len= snprintf(NULL, 0, "%s/%s", target.path, path + dir_prefix) + 1;
    char *dstpath= static_cast<char*>(malloc(len));
    if (!dstpath)
      my_error(ER_TOO_LONG_IDENT, MYF(0), path + dir_prefix);
    else
    {
      snprintf(dstpath, len, "%s/%s", target.path, path + dir_prefix);
      ret= my_copy(path, dstpath, MYF(MY_WME));
      free(dstpath);
    }
  }
  else
  {
    LARGE_INTEGER li;
    HANDLE src, dst= sink.stream;
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
        Sleep(10);
        continue;
      }

      my_osmaperr(GetLastError());
      my_error(ER_FILE_NOT_FOUND, MYF(ME_ERROR_LOG), path, errno);
      return ret;
    }

    ret= !GetFileSizeEx(src, &li) ||
      sink.create_file(path + dir_prefix, 0644, li.QuadPart, nullptr, 0) ||
      sink.append(src, 0, li.QuadPart) || sink.zeropad(size_t(li.QuadPart));
    (void) CloseHandle(src);

    if (ret)
    {
      my_osmaperr(GetLastError());
      my_error(ER_ERROR_ON_WRITE, MYF(0), path + dir_prefix, errno);
    }
  }
  return ret;
}
#else
/**
   Copy or stream a file.
   @param target     BACKUP SERVER target (possibly, a directory)
   @param sink       per-thread context (possibly, a stream to write to)
   @param src        source file descriptor from open(), to be closed here
   @param path       file name
   @return error code (also errno will be set)
   @retval 0 on success
*/
int copy_or_stream(const backup_target &target, const backup_sink &sink,
                   int src, const char *path) noexcept
{
  int ret= -1, dst= sink.stream;
  if (src < 0)
  {
    my_error(ER_CANT_OPEN_FILE, MYF(0), path, errno);
    return ret;
  }
  if (dst < 0)
  {
    dst= openat(target.fd, path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (dst < 0)
      my_error(ER_CANT_CREATE_FILE, MYF(0), path, errno);
    else
    {
      ret= my_copy_file(src, dst) || close(dst);
      if (ret)
      write_error:
        my_error(ER_ERROR_ON_WRITE, MYF(0), path, errno);
    }
  }
  else
  {
    uint64_t end= uint64_t(lseek(src, 0, SEEK_END));
    ret= sink.create_file(path, 0644, end, NULL, 0) ||
      sink.append(src, 0, end) || sink.zeropad(size_t(end));
    if (ret)
      goto write_error;
  }
  close(src);
  return ret;
}

int copy_or_stream(const backup_target &target, const backup_sink &sink,
                   const char *path, size_t dir_prefix) noexcept
{
  return copy_or_stream(target, sink,
                        open(path, O_RDONLY), path + dir_prefix);
}
#endif

}

/**
   Append a file snippet to stream,
   after a corresponding call to create_file().

   Note that tar uses 512-byte blocks. If end-start is not a multiple of
   512 bytes, write() or zeropad() must be invoked to pad the output.
   @param src   source file
   @param start first offset to copy
   @param end   first offset not to copy
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_sink::append(IF_WIN(native_file_handle,int) src,
                        uint64_t start, uint64_t end) const noexcept
{
  return pread_write<true>(src, stream, start, end);
}

#ifdef _WIN32
int backup_sink::append(my_native_file src,
                        uint64_t start, uint64_t end) const noexcept
{
  return append(native_file_handle{src}, start, end);
}
#elif defined __linux__
/**
   Zero-copy append an immutable file snippet to a stream.

   The caller guarantees that this section of the file will remain
   intact until the stream is closed. This guarantee is needed
   because the receiving end of the stream pipe might delay consuming
   the data, and the operating system might point the pipe buffer
   to the block cache for a long time.

   Note that tar uses 512-byte blocks. If end-start is not a multiple of
   512 bytes, write() or zeropad() must be invoked afterwards.
   @param src    file that will not be modified until the stream is closed
   @param start  first offset to copy
   @param end    first offset not to copy
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_sink::append_async(int src, uint64_t start, uint64_t end)
  const noexcept
{
  assert(stream != NO_STREAM);
  assert(end >= start);
  for (off_t offset{off_t(start)};;)
  {
    const size_t c{size_t(std::min<off_t>(end - offset, INT_MAX >> 20 << 20))};
    ssize_t ret{sendfile(stream, src, &offset, c)};
    if (ret < 0)
    {
      if (errno == EAGAIN)
        continue;
      return int(ret);
    }
    if (offset == off_t(end))
      return 0;
    if (!ret)
      return -1;
  }
}
#endif

/**
   Zero-pad a the stream to a multiple of 512 bytes.
   @param written least significant bits of the number of payload appended
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_sink::zeropad(size_t written) const noexcept
{
  static constexpr const char zerobuf[511]{'\0'};
  written&= 511;
  return written ? write(zerobuf, 512 - written) : 0;
}

/** Append to the configuration file.
@param target   backup target directory
@param config   the configuration file snippet to append
@param size     length of the snippet
@return error code (non-positive)
@retval 0   on success */
int backup_target::config_append(const char *config, size_t size)
  const noexcept
{
  /* FIXME: append to a pre-created configuration file */
  my_native_file dst{create_file("backup.cnf")};
  if (IF_WIN(dst != INVALID_HANDLE_VALUE, dst >= 0))
  {
    int err{backup_sink::write(dst, config, size)};
    if (!(close(dst) | err))
      return 0;
  }
  my_error(ER_CANT_CREATE_FILE, MYF(ME_ERROR_LOG), "backup.cnf", errno);
  return -1;
}

/** process-wide backup context */
struct backup_context
{
  /** Engine-specific context, shared by all threads.  Only
  modified by the calling thread while no backup_step() is pending;
  backup_step() must use find(), never operator[], because
  operator[] is not safe for concurrent use. */
  std::unordered_map<const handlerton*,void*> ha_data;
  /** The data directory */
  MY_NO_CACHE_DIR *const dir;
  /** The database directory that we are reading, or NULL */
  MY_NO_CACHE_DIR *subdir{};
  /** whether the operation failed */
  bool fail{};
  /** mutex protecting d, subdir, fail, db_name */
  std::mutex mutex;
  /** name that subdir was opened for */
  char db_name[FN_REFLEN];

  backup_context() :
    dir(my_dir_open(mysql_data_home, nullptr,
                    MYF(MY_DIR_ONLY_DIRS | MY_WME | ME_WARNING))) {}
  bool invalid() const noexcept { return !dir; }
  ~backup_context()
  {
    if (dir)
      my_dir_close(dir);
    if (subdir)
      my_dir_close(subdir);
  }

  /**
     Determine if a built-in file may be backed up.
     @param file_name   candidate file name
     @param len         strlen(file_name)
     @return whether the file may be included
  */
  static bool is_db_file(const char *file_name, size_t len) noexcept
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
    case 0x2e69736c: /* .isl ENGINE=InnoDB DATA DIRECTORY "symbolic link" */
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
    case 0x6c73692e: /* .isl ENGINE=InnoDB DATA DIRECTORY "symbolic link" */
#endif
      return true;
    }
    return len == 6 && !memcmp(file_name, C_STRING_WITH_LEN("db.opt"));
  }

  /**
     Consume a data file name and copy the file if needed.
     @param target     BACKUP SERVER target
     @param sink       per-thread context (possibly, a stream to write to)
     @retval 1 on success if some work remains
     @retval 0 on successful completion
     @retval -1 on error
  */
  int step(const backup_target &target, const backup_sink &sink) noexcept
  {
    const char *filename{};
    char path[FN_REFLEN + 2];
    const size_t datadir_len{strlen(mysql_data_home) + 1};
    assert(dir);
    {
      std::lock_guard<std::mutex> _{mutex};

      if (fail)
      {
      err_exit:
        fail= true;
        my_error(ER_UNKNOWN_ERROR, MYF(0));
        return -1;
      }

      for (;;)
      {
        if (!subdir)
          switch (my_dir_read_next(dir, path, sizeof path, nullptr, MYF(0))) {
          case MY_DIR_EOF:
            goto end;
          default:
            // FIXME: my_dir_read_next() should report the actual name
            my_error(ER_TOO_LONG_IDENT, MYF(ME_WARNING), mysql_data_home);
            break;
          case MY_DIR_OK:
            if (is_in_ignore_db_dirs_list(path))
              continue;
            do
            {
              if (target.mkdir(path));
              else if (int(sizeof db_name) <=
                       snprintf(db_name, sizeof db_name, "%s/%s",
                                mysql_data_home, path))
                my_error(ER_TOO_LONG_IDENT, MYF(ME_WARNING), path);
              else if ((subdir=
                        my_dir_open(db_name, nullptr,
                                    MYF(MY_DIR_ONLY_FILES |
                                        MY_WME | ME_WARNING))))
                continue;
              goto err_exit;
            }
            while (false);
          }

        assert(subdir);
        if (subdir)
          switch (my_dir_read_next(subdir, path, sizeof path, nullptr, MYF(0))) {
          case MY_DIR_OK:
            break;
          case MY_DIR_EOF:
            my_dir_close(subdir);
            subdir= nullptr;
            continue;
          default:
            // FIXME: my_dir_read_next() should report the actual name
            my_error(ER_TOO_LONG_IDENT, MYF(ME_WARNING), db_name);
            goto err_exit;
          }

        /* Consume a file name */
        const char *dir_name{db_name + datadir_len};
        size_t dir_len= strlen(dir_name), f_len= strlen(path);
        if (dir_len + f_len + 2 > sizeof path)
        {
          my_error(ER_TOO_LONG_IDENT, MYF(0), path);
          goto err_exit;
        }

        memmove(path + dir_len + 1, path, f_len + 1);
        memcpy(path, dir_name, dir_len);
        path[dir_len]= '/';
        filename= path;
        break;
      }
    }

  end:
    if (!filename)
      return 0;

    const char *name{strrchr(filename, '/')};
    assert(name); /* we constructed this with %s/%s */
    size_t len= strlen(++name);
    static_assert(tmp_file_prefix_length == 4);
    if (len < 4 ||
        /*
          As noted in MDEV-25854, file names that start with #sql
          must be excluded from the backup. For example, a call to
          MDL_context::upgrade_shared_lock() in
          mysql_inplace_alter_table() could time out, resulting in
          cleanup_table_after_inplace_alter() deleting a
          #sql-alter*.frm file before we get a chance to copy it.
        */
        !memcmp(name, tmp_file_prefix, tmp_file_prefix_length) ||
        /* The storage engines copy their other files */
        !is_db_file(name, len));
    else if (backup::copy_or_stream(target, sink,
#ifndef _WIN32
                                    openat(dirfd(static_cast<DIR*>(dir->dirp)),
                                           filename, O_RDONLY),
#endif
                                    filename))
      return -1;

    return 1;
  }

#ifndef _WIN32
#else

#endif
};

/** per-thread backup context */
struct backup_target_stage
{
  /** target directory or stream */
  backup_target target;
  /** current backup stage */
  backup_stages stage;
  /** backup worker state (output stream) */
  backup_sink sink;
  /** stream object for streaming backup */
  FILE *stream;
  /** handlerton::backup_step return value in multi-threaded operation */
  int ret;
  /** process-wide backup context */
  backup_context &context;

  /**
     Scan the data directory and copy files if needed.
     @return whether an error occurred
  */
  int step() const noexcept
  {
    if (stage == BACKUP_LOCK_COMMIT)
      return context.step(target, sink);
    return 0;
  }
};

/**
   Inform a storage engine of an upcoming backup by invoking
   handlerton::backup_start(BACKUP_PREPARE) before
   acquiring any locks.
   @param thd     current session
   @param plugin  storage engine
   @return whether the operation failed
*/
static my_bool backup_preparation(THD *thd, plugin_ref plugin, void*) noexcept
{
  const auto bs= plugin_hton(plugin)->backup_start;
  return bs && bs(thd, nullptr, BACKUP_PREPARE, nullptr);
}

/**
   Invoke handlerton::backup_start() on a storage engine,
   when there are no pending handlerton::backup_step() in any thread.
   @param thd     current session
   @param plugin  storage engine
   @param arg     the backup_target_stage context
   @return whether the operation failed
*/
static my_bool backup_start(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  const handlerton *hton= plugin_hton(plugin);
  backup_target_stage &t{*static_cast<backup_target_stage*>(arg)};
  assert(int{t.stage} >= 0);
  if (hton->backup_start)
  {
    t.sink.ha_data= t.context.ha_data[hton];
    void *data= hton->backup_start(thd, &t.target, t.stage, &t.sink);
    if (data == reinterpret_cast<void*>(-1))
      return true;
    assert(!t.context.ha_data[hton] || t.context.ha_data[hton] == data);
    t.context.ha_data[hton]= data;
  }
  return false;
}

/**
   Invoke handlerton::backup_end() on a storage engine,
   when there are no pending handlerton::backup_step() in any thread.
   @param thd     current session
   @param plugin  storage engine
   @param arg     the backup_target_stage context
   @return whether the operation failed
*/
static my_bool backup_end(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  const handlerton *hton= plugin_hton(plugin);
  backup_target_stage &t{*static_cast<backup_target_stage*>(arg)};
  if (hton->backup_end)
  {
    t.sink.ha_data= t.context.ha_data[hton];
    return hton->backup_end(thd, &t.target, t.stage, &t.sink);
  }
  return false;
}

/**
   Invoke handlerton::backup_step() on a storage engine in a thread
   that may or may not be associated with a BACKUP SERVER connection,
   between handlerton::backup_start() and handlerton::backup_end()
   of the same stage.
   @param thd     the BACKUP SERVER session
   @param plugin  storage engine
   @param arg     the backup_target_phase context
   @return whether the operation failed
*/
static my_bool backup_step(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  const handlerton *hton= plugin_hton(plugin);
  backup_target_stage &t{*static_cast<backup_target_stage*>(arg)};
  assert(int{t.stage} >= 0);
  if (hton->backup_step)
  {
    const auto it= t.context.ha_data.find(hton);
    t.sink.ha_data= it == t.context.ha_data.end() ? nullptr : it->second;
    while (int res= hton->backup_step(thd, &t.target, t.stage, &t.sink))
      if (res < 0)
        return true;
  }
  return false;
}


/**
   Execute all handlerton::backup_step() until completion or failure.
   @param thd           current connection
   @param target_stage  backup target and stage
   @return whether the operation failed
*/
static bool backup_step_one(THD *thd, backup_target_stage *target_stage)
{
  while (int ret= target_stage->step())
    if (ret < 0)
      return ret;
  return plugin_foreach_with_mask(thd, backup_step,
                                  MYSQL_STORAGE_ENGINE_PLUGIN,
                                  PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                  target_stage);
}


/**
   Initialize a thread that was created by the thread pool.

   Any thread that uses mysys functions must call my_thread_init()
   before it does so; my_errno and other mysys variables are stored in
   a thread local variable that my_thread_init() allocates.
*/
static void backup_thread_init() noexcept
{
  my_thread_init();
  my_thread_set_name("backup_worker");
  /* Make the thread visible in performance_schema.threads */
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *psi{PSI_CALL_new_thread(key_thread_backup_worker, NULL, 0)};
  PSI_CALL_set_thread_os_id(psi);
  PSI_CALL_set_thread(psi);
#endif
}


/** Clean up a thread that was initialized by backup_thread_init() */
static void backup_thread_end() noexcept
{
  PSI_CALL_delete_current_thread();
  my_thread_end();
}


/** Invoke backup_step() in a background task */
static void backup_step_callback(void *arg) noexcept
{
  backup_target_stage &t{*static_cast<backup_target_stage*>(arg)};
  assert(!t.ret);
  while (int ret= t.step())
    if (ret < 0)
    {
      t.ret= true;
      return;
    }
  t.ret= plugin_foreach_with_mask(nullptr, backup_step,
                                  MYSQL_STORAGE_ENGINE_PLUGIN,
                                  PLUGIN_IS_DELETED|PLUGIN_IS_READY, &t);
}

/**
   Execute all handlerton::backup_step() until completion or failure.
   @param thd           current connection
   @param target_stage  backup target and stage
   @param threads       number of execution threads
   @param tp            thread pool
*/
static bool backup_steps(THD *thd, backup_target_stage *target_stage,
                         int threads, tpool::thread_pool *tp)
{
  if (unlikely(thd->killed != NOT_KILLED))
    return true;
  if (threads == 1)
    return backup_step_one(thd, target_stage);
  tpool::waitable_task *const tasks=
    static_cast<tpool::waitable_task*>(alloca(threads * sizeof *tasks));
  assert(target_stage[0].sink.id == threads - 1);
  for (int n{threads}; --n; )
  {
    assert(target_stage[n].sink.id == (threads - 1) - n);
    target_stage[n].stage= target_stage->stage;
    tp->submit_task(new (&tasks[n]) tpool::waitable_task{backup_step_callback,
                                                         &target_stage[n]});
  }
  const bool flagged{backup_step_one(thd, target_stage)};
  bool fail{flagged};

  for (int n{threads}; --n; )
  {
    tasks[n].wait();
    if (target_stage[n].ret)
      fail= true;
  }

  if (fail && !flagged)
    my_error(ER_UNKNOWN_ERROR, MYF(0));
  return fail;
}

/**
   BACKUP SERVER driver

   @param thd     client connection
   @param target  BACKUP SERVER TO directory to be created
   @param command BACKUP SERVER WITH command
   @param threads number of CONCURRENT threads to employ
   @return whether the operation failed
*/
static bool backup_execute(THD *thd, const char *target, const char *command,
                           int threads)
{
  assert(!!target == !command);
  assert(threads > 0);

  if (!target)
  {
    /*
      Disallow path separators and the quote character.
    */
    if (false ||
#ifndef _WIN32
        strchr(command, '\'') /* quote */ ||
#else
        strchr(command, '"') /* quote */ ||
        strchr(command, '\\') /* special inside quotes */ ||
#endif
        strchr(command, '/') /* disallow path separator */)
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), "BACKUP SERVER WITH");
      return true;
    }
  }
  else if (error_if_data_home_dir(target, "BACKUP SERVER TO"))
    return true;

  bool fail= plugin_foreach_with_mask(thd, backup_preparation,
                                      MYSQL_STORAGE_ENGINE_PLUGIN,
                                      PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                      nullptr);
  if (fail)
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }

  if (run_backup_stage(thd, BACKUP_START))
    return true;

  tpool::thread_pool *tp= nullptr;
  backup_context context{};
  if (context.invalid())
  {
  release_and_exit:
    run_backup_stage(thd, BACKUP_END);
    return true;
  }
#ifndef NDEBUG
# define calloca(n) memset(alloca(n), 0, n)
#else
# define calloca(n) alloca(n)
#endif
  backup_target_stage *target_stage= static_cast<backup_target_stage*>
    (calloca(threads * sizeof *target_stage));
#undef calloca

  if (threads <= 1);
  else if (!(tp= tpool::create_thread_pool_generic()))
  {
  oor:
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
  err_exit:
    delete tp;
#ifndef NDEBUG
    for (int t{threads}; t--; )
      assert(!target_stage[t].stream);
#endif
    goto release_and_exit;
  }
  else
    tp->set_thread_callbacks(backup_thread_init, backup_thread_end);

  if (command)
  {
    char cmd[1024];
    for (int t{threads}; t; t--)
    {
#ifndef _WIN32
      /*
        popen(3) will prepend "/bin/sh -c" which preserves the literal
        meaning of all characters enclosed in single quotes.
      */
      constexpr const char cmd_fmt[]= "'mariadb-backup-%s' %d";
#else
      /*
        my_win_popen() will prepend "cmd.exe /c ";
        add double quotes around its first argument
      */
      constexpr const char cmd_fmt[]= "\"mariadb-backup-%s\" %d";
#endif
      if (size_t(snprintf(cmd, sizeof cmd, cmd_fmt, command, t)) >= sizeof cmd)
        goto oor;
      FILE *f= my_popen(cmd, "w");
      if (!f)
      {
        while (t++ < threads)
        {
          my_pclose(target_stage[threads - t].stream);
#ifndef NDEBUG
          target_stage[threads - t].stream= nullptr;
#endif
        }
        goto oor;
      }
#ifdef _WIN32
      HANDLE sink= (HANDLE) _get_osfhandle(_fileno(f));
#else
      int sink= fileno(f);
# ifdef __linux__
      /* Request a larger pipe buffer; the default appears to be 64 KiB. */
      fcntl(sink, F_SETPIPE_SZ, 1 << 20/* 1 MiB */);
# endif
#endif
      new (&target_stage[threads - t])
        backup_target_stage{backup_target{IF_WIN(nullptr, -1)},
          BACKUP_START, backup_sink{t - 1, sink, nullptr}, f, 0, context};
    }
  }
  else if (my_mkdir(target, 0755, MYF(MY_WME)))
    goto err_exit;
  else
  {
#ifndef _WIN32
    const int dir{open(target, O_DIRECTORY)};
    if (dir < 0)
    {
      my_error(EE_CANT_MKDIR, MYF(ME_BELL), target, errno);
      goto err_exit;
    }
#endif
    for (int t{threads}; t--; )
    {
      new (&target_stage[threads - 1 - t])
        backup_target_stage{backup_target{IF_WIN(target, dir)},
          BACKUP_START, backup_sink{t, backup_sink::NO_STREAM, nullptr},
          nullptr, 0, context};
    }
  }

  int stage{BACKUP_START};
  goto backup_start;

  for (; stage <= int{BACKUP_LOCK_COMMIT}; stage++)
  {
    fail= run_backup_stage(thd, backup_stages(stage));

    if (fail)
      break;

  backup_start:
    target_stage->stage= backup_stages(stage);
    fail= plugin_foreach_with_mask(thd, backup_start,
                                   MYSQL_STORAGE_ENGINE_PLUGIN,
                                   PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                   target_stage);
    if (fail)
      break;
    fail= backup_steps(thd, target_stage, threads, tp) ||
      plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                               PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                               target_stage);
    if (fail)
      break;
  }

  fail= run_backup_stage(thd, BACKUP_END) || fail;

  if (!fail)
  {
    target_stage->stage= BACKUP_FINISHED;
    fail= plugin_foreach_with_mask(thd, backup_start,
                                   MYSQL_STORAGE_ENGINE_PLUGIN,
                                   PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                   target_stage) ||
      backup_steps(thd, target_stage, threads, tp);
  }
  else
  {
    target_stage->stage= BACKUP_ABORTED;
    plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                             PLUGIN_IS_DELETED|PLUGIN_IS_READY, target_stage);
    target_stage->stage= BACKUP_FINISHED;
  }

  fail=
    plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                             PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                             target_stage) || fail;

  delete tp;

  if (command)
  {
    if (fail)
      /* Ensure that each stream is an invalid tar stream. */
      for (int t= threads; t--; )
        std::ignore= putc('\0', target_stage[t].stream);
    int pfail{0};
    for (int t= threads; t--; )
      pfail|= my_pclose(target_stage[t].stream);
    if (!fail && (fail= !!pfail))
      my_error(ER_NET_ERROR_ON_WRITE, MYF(0));
  }
#ifndef _WIN32
  else
    std::ignore= close(target_stage->target.fd);
#endif

  if (!fail)
    my_ok(thd);
  return fail;
}

/**
   Encode an octal string.
   @param start   first byte of buffer
   @param end     first byte after buffer
   @param n       number to encode
*/
static void ustar_write_octal(char *start, char *end, uint64_t n) noexcept
{
  for (*--end= '\0'; --end >= start; n>>= 3)
    *end= char('0' + (n & 7));
}

/**
   Encode a quantity in 12 bytes.
   @param start   first byte of the buffer
   @param n       number to encode
*/
static void ustar_write_dozen(char *start, uint64_t n) noexcept
{
  if (n < 1ULL << 33)
    ustar_write_octal(start, start + 12, n);
  else
  {
    const uint32_t head{my_htobe32(1U << 31)};
    n= my_htobe64(n);
    memcpy(start, &head, 4);
    memcpy(start + 4, &n, 8);
  }
}

/** Initialize a ustar block
@param buf  GNU tape archiver --format=oldgnu header block
@param name name of the block
@param mode file access mode
@param size physical size of the following block */
static void ustar_block_init(char *buf, const char *name, mode_t mode,
                             uint64_t size) noexcept
{
  strncpy(buf, name, 99);
  buf[99]= '\0';
  ustar_write_octal(buf + 100, buf + 108, uint64_t(mode));
  ustar_write_octal(buf + 108, buf + 116, 0/* POSIX uid */);
  ustar_write_octal(buf + 116, buf + 124, 0/* POSIX gid */);
  ustar_write_dozen(buf + 124, size);
  /* last modification time */
  ustar_write_octal(buf + 136, buf + 148, 0);
  memset(buf + 148, ' ', 9); /* initial block checksum and dummy type */
  memset(buf + 157, '\0', 100); /* name of linked file (unused) */
  memcpy(buf + 257, "ustar  ", 8);
  strncpy(buf + 265, "root" /* POSIX owner name */, 32);
  strncpy(buf + 297, "root" /* POSIX group name */, 512 - 297);
  /* The caller will fill in the rest and invoke ustar_block_checksum() */
}

/**
   Compute and write the POSIX tar block checksum.
   @param buf    POSIX tar block
*/
static void ustar_block_checksum(char *buf) noexcept
{
  uint16_t sum{0};
  for (int i{0}; i < 512; i++)
    sum+= uint16_t{uint8_t(buf[i])};
  ustar_write_octal(buf + 148, buf + 155, sum);
}

/**
   Write data to a stream or a file.
   @param dst     file or stream to write to
   @param buf     source buffer
   @param size    length of the buffer
   @return error code (non-positive)
   @retval 0 on success
*/
int backup_sink::write(my_native_file dst, const void *buf, size_t size)
  noexcept
{
#ifdef _WIN32
  for (DWORD sz= DWORD(size);;)
  {
    DWORD wrote;
    if (WriteFile(dst, buf, sz, &wrote, nullptr))
    {
      assert(wrote == sz);
      return 0;
    }
    else if (GetLastError() != ERROR_IO_PENDING)
    {
      my_osmaperr(GetLastError());
      return -1;
    }
    buf= static_cast<const char*>(buf) + wrote;
    sz-= wrote;
  }
#else
  do
  {
    ssize_t wrote= ::write(dst, buf, size);
    assert(wrote <= ssize_t(size));
    if (wrote < 0)
    {
      if (errno == EAGAIN)
        continue;
      return -1;
    }
    buf= static_cast<const char*>(buf) + wrote;
    size-= size_t(wrote);
  }
  while (size);
#endif
  return 0;
}

/**
   Copy a prefix of a NUL terminated string to a buffer, NUL-padded.
   @param b     output buffer
   @param s     NUL terminated string
   @param size  size of buf, in bytes
*/
static inline char *ustar_zeropad(char *b, const char *s, size_t size) noexcept
{
#if defined __GNUC__ && __GNUC__ >= 8
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
  return strncpy(b, s, size);
#if defined __GNUC__ && __GNUC__ >= 8
# pragma GCC diagnostic pop
#endif
}

/**
   Start streaming a file.
   @param name     file name
   @param mode     file access mode
   @param size     physical length of the file, in bytes
   @param chunks   payload chunks of a sparse file, or nullptr
   @param n_chunks number of chunks; 0 unless sparse file
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_sink::create_file(const char *name, mode_t mode, uint64_t size,
                             const backup_chunk *chunks, size_t n_chunks)
  const noexcept
{
#ifndef NDEBUG
  if (size_t n{n_chunks})
  {
    /* The physical size must be a sum of the chunk sizes. */
    uint64_t s{size};
    do s-= chunks[--n].length; while (n);
    assert(!s);
  }
#endif
  assert(stream != NO_STREAM);
  char buf[512];
  size_t s= strlen(name);
  if (s > 99)
  {
    /* Write a block that contains the full name length,
    followed by blocks that contain the full name, in
    tar --format=oldgnu */
    ustar_block_init(buf, "././@LongLink", 0644, s + 1);
    buf[156]= 'L';
    ustar_block_checksum(buf);
    if (int err= write(buf, sizeof buf))
      return err;
    const size_t whole{s & ~((sizeof buf) - 1)};
    if (whole)
      if (int err= write(name, whole))
        return err;
    /* After 512*n bytes of file name there will be a zerofilled block. */
    ustar_zeropad(buf, name + whole, sizeof buf);
    if (int err= write(buf, sizeof buf))
      return err;
  }

  ustar_block_init(buf, name, mode, size);
  if (!n_chunks)
    buf[156]= '0';
  else
  {
    buf[156]= 'S';
    char *h= &buf[386];
    if (n_chunks > 4)
      return -1; // FIXME; support more chunks
    for (size_t i= 0; i < n_chunks; i++, h+= 24)
    {
      ustar_write_dozen(h, chunks[i].offset);
      ustar_write_dozen(h + 12, chunks[i].length);
    }
    ustar_write_dozen(&buf[0x1e3], chunks[n_chunks - 1].offset +
                      chunks[n_chunks - 1].length);
  }
  ustar_block_checksum(buf);
  return write(buf, sizeof buf);
}

/**
   Append to the configuration file.
   @param config   the configuration file snippet to append
   @param size     length of the snippet
   @return error code (non-positive)
   @retval 0   on success
*/
int backup_sink::config_append(const char *config, size_t size) const noexcept
{
  /* FIXME: append to a pre-created configuration file */
  if (int ret= create_file("backup.cnf", 0644, size, nullptr, 0))
    return ret;
  char buf[512];
  const size_t whole{size & ~((sizeof buf) - 1)};
  if (whole)
    if (int err= write(config, whole))
      return err;
  if (size == whole)
    return 0;
  ustar_zeropad(buf, config + whole, sizeof buf);
  return write(buf, sizeof buf);
}
