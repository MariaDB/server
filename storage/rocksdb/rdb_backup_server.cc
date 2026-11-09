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

#include <my_global.h>
#include <my_sys.h>
#include <mysqld_error.h>

#ifndef _WIN32
# include <fcntl.h>
# include <sys/types.h>
# include <dirent.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

#include "sql_backup_interface.h"
#include "rdb_mariadb_port.h"
#include "ha_rocksdb_proto.h"
#include "rdb_backup_server.h"

namespace myrocks {

namespace {
/** The name of the backup snapshot in the server data directory. */
constexpr const char BACKUP_DIR[]{"#rocksdb"};

/** BACKUP SERVER state for the RocksDB engine,
attached to backup_sink::ha_data.
It is created single-threaded at BACKUP_LOCK_COMMIT,
where it freezes a consistent, hardlink-based
rocksdb::Checkpoint and opens that directory for iteration.

At BACKUP_FINISHED, up to N step threads drain the
directory, copying the files into the target's "#rocksdb"
subdirectory. Because SST files are immutable,
copying can safely happen after the backup MDL has been released.

Each backup_step() claims one file name from the
directory iteration inside the critical section, then copies
that file outside it. The name is copied to the caller's stack,
because the struct dirent (or WIN32_FIND_DATAA) belongs to the
shared iteration state and would be overwritten by the next
step thread. */
class RocksDB_backup
{
public:
  /** Destructor */
  ~RocksDB_backup()
  {
    if (checkpoint_created)
      rdb_remove_checkpoint(checkpoint_dir);
#ifndef _WIN32
    if (dir)
      closedir(dir); /* also closes dirfd(dir) */
#else
    if (dir != INVALID_HANDLE_VALUE)
      FindClose(dir);
#endif
  }
  /**
     Try to create a checkpoint.
     @return error code
     @retval 0 on success
  */
  int checkpoint_create() noexcept;

  /**
     Prepare for concurrent step() to copy what checkpoint_create() produced.
     @param target  BACKUP SERVER target
     @param sink    per-thread context
     @return whether the operation failed
  */
  bool checkpoint_prepare(const backup_target &target,
                          const backup_sink &sink) noexcept;

  /**
     Copy a file from a checkpoint.
     @param target  BACKUP SERVER target
     @param sink    per-thread context
  */
  int step(const backup_target &target, const backup_sink &sink) noexcept;

private:
#ifndef _WIN32
  /** checkpoint directory stream; dirfd(dir) is the openat() base */
  DIR *dir{};
#else
  /** checkpoint directory iterator, or INVALID_HANDLE_VALUE once the
  iteration has been exhausted */
  HANDLE dir{INVALID_HANDLE_VALUE};
  /** FindFirstFileA()/FindNextFile() buffer for dir, holding the entry
  to be consumed by the next rocksdb_backup_step() */
  WIN32_FIND_DATAA d;
#endif
  /** mutex protecting the directory iteration state across
  concurrent rocksdb_backup_step() */
  std::mutex mutex;
  /** whether a server-side checkpoint currently exists */
  bool checkpoint_created{};
  /** Length of the rocksdb_datadir prefix to omit from the
  name of a file in the backup, i.e., the length of the
  rocksdb_datadir prefix. */
  size_t dest_prefix{};
  /** strlen(checkpoint_dir) */
  size_t checkpoint_dir_len{};
  /** <rocksdb_datadir>/#rocksdb; assigned before the step threads
  exist and read-only afterwards */
  char checkpoint_dir[FN_REFLEN];
};

/**
   Compose <rocksdb_datadir>/#rocksdb and the length of the prefix
   that has to be omitted from it to name a file in the backup.
   @return error code
   @retval 0 on success
*/
int RocksDB_backup::checkpoint_create() noexcept
{
  size_t len{strlen(rocksdb_datadir)};
  while (len && rocksdb_datadir[len - 1] == '/')
    len--;
  dest_prefix= len + 1;
  checkpoint_dir_len=
    snprintf(checkpoint_dir, sizeof checkpoint_dir,
             "%.*s/%s", int(len), rocksdb_datadir, BACKUP_DIR);

  if (int(sizeof checkpoint_dir) IF_WIN(+ 2,) <= checkpoint_dir_len)
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), rocksdb_datadir);
    return 1;
  }

  /*
    Drop any stale checkpoint left behind by a previous,
    failed backup. BACKUP SERVER is serialized by MDL_BACKUP_START,
    so no user-level lock is needed to protect the fixed
    checkpoint path.
  */
  rdb_remove_checkpoint(checkpoint_dir);
  if (int err= rdb_create_checkpoint(checkpoint_dir))
    return err;
  checkpoint_created= true;
#ifndef _WIN32
  const int dfd{open(checkpoint_dir, O_RDONLY | O_DIRECTORY)};
  if (dfd >= 0)
  {
    if ((dir= fdopendir(dfd)))
      return 0;
    close(dfd);
  }
#else
  strcpy(checkpoint_dir + checkpoint_dir_len, "/*");
  dir= FindFirstFileA(checkpoint_dir, &d);
  checkpoint_dir[checkpoint_dir_len]= '\0';
  if (dir != INVALID_HANDLE_VALUE)
    return 0;
  my_osmaperr(GetLastError());
#endif
  my_error(ER_CANT_READ_DIR, MYF(0), checkpoint_dir, errno);
  return 1;
}

bool RocksDB_backup::checkpoint_prepare(const backup_target &target,
                                        const backup_sink &sink) noexcept
{
  assert(checkpoint_created);
#ifndef _WIN32
  if (target.fd < 0)
    return false;
  if (!renameat(AT_FDCWD, checkpoint_dir, target.fd, BACKUP_DIR))
  {
    checkpoint_created= false;
    return false;
  }
  if (!mkdirat(target.fd, BACKUP_DIR, 0777))
    return false;
#else
  const char *const dir{target.path};
  if (!dir)
    return false;
  else
  {
    char path[FN_REFLEN];
    if (int(sizeof path) <=
        snprintf(path, sizeof path, "%s/%s", dir, BACKUP_DIR))
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), dir);
      return true;
    }
    if (MoveFileEx(checkpoint_dir, path, 0))
    {
      checkpoint_created= false;
      return false;
    }
    if (my_mkdir(path, 0777, MYF(0)))
      return false;
  }
#endif
  my_error(ER_CANT_CREATE_FILE, MYF(0), BACKUP_DIR, errno);
  return true;
}

int RocksDB_backup::step(const backup_target &target, const backup_sink &sink)
  noexcept
{
  if (!checkpoint_created)
    return 0;
  const char *filename= nullptr;
  char path[FN_REFLEN * 2];
  int left;
#ifndef _WIN32
  const struct dirent *d;
  struct stat sb;
  int dfd;
  assert(dir);
#endif
  assert(strlen(checkpoint_dir) == checkpoint_dir_len);
  strcpy(path, checkpoint_dir);
  path[checkpoint_dir_len]= '/';

  mutex.lock();
#ifndef _WIN32
  dfd= dirfd(dir);
  /* A rocksdb::Checkpoint is a flat directory, so a non-recursive
  scan is sufficient (SST files, MANIFEST, CURRENT, OPTIONS, *.log).
  Anything that is not a regular file, "." and ".." included,
  is skipped. */
  while ((d= readdir(dir)))
  {
    switch (d->d_type) {
    default:
      continue;
    case DT_REG:
      break;
    case DT_LNK:
    case DT_UNKNOWN:
      if (fstatat(dfd, d->d_name, &sb, 0) || (sb.st_mode & S_IFMT) != S_IFREG)
        continue;
    }

    /*
      Consume a file name. The d may be modified by another step()
      thread after our mutex.unlock().
    */
    if (strlen(d->d_name) + checkpoint_dir_len + 1 >= sizeof path)
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), d->d_name);
      left= -1;
      goto func_exit;
    }
    strcpy(path + checkpoint_dir_len + 1, d->d_name);
    filename= path;
    break;
  }

  left= d != nullptr;
#else
  if (dir == INVALID_HANDLE_VALUE)
  {
    /* A previous step() exhausted the iteration. */
    left= 0;
    goto func_exit;
  }

  do
  {
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;

    /*
      Consume a file name. The d may be modified by another step()
      thread as soon as we release mutex.
    */
    if (strlen(d.cFileName) + checkpoint_dir_len + 1 >= sizeof path)
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), d.cFileName);
      left= -1;
      goto func_exit;
    }
    strcpy(path + checkpoint_dir_len + 1, d.cFileName);
    filename= path;
  }
  while ((left= FindNextFile(dir, &d)) && !filename);

  if (!left)
  {
    /*
      Do not invoke FindNextFile() on an exhausted iterator, whose
      WIN32_FIND_DATAA would be stale: close it and let any further
      step() report completion above.
    */
    FindClose(dir);
    dir= INVALID_HANDLE_VALUE;
  }
#endif
 func_exit:
  mutex.unlock();

  if (filename && backup::copy_or_stream(target, sink, filename, dest_prefix))
    left= -1;

  return left;
}

}  // anonymous namespace

void *backup_start(THD *, const backup_target *target,
                   backup_stages stage, const backup_sink *sink) noexcept
{
  switch (stage) {
  case BACKUP_PREPARE:
    return nullptr;
  case BACKUP_FINISHED:
    if (sink->ha_data && static_cast<RocksDB_backup*>(sink->ha_data)->
        checkpoint_prepare(*target, *sink))
      break;
    /* fall through */
  default:
    return sink->ha_data;
  case BACKUP_LOCK_COMMIT:
    assert(!sink->ha_data);
    RocksDB_backup *bk;
    try { bk= new RocksDB_backup{}; }
    catch (std::bad_alloc&) {
      my_error(ER_OUTOFMEMORY, MYF(0), int(sizeof *bk));
      return reinterpret_cast<void*>(-1);
    }
    if (!bk->checkpoint_create())
      return bk;
    delete bk;
  }
  return reinterpret_cast<void*>(-1);
}

int backup_step(THD *, const backup_target *target,
                backup_stages stage, const backup_sink *sink) noexcept
{
  return stage == BACKUP_FINISHED && sink->ha_data
    ? static_cast<RocksDB_backup*>(sink->ha_data)->step(*target, *sink)
    : 0;
}

int backup_end(THD *, const backup_target *,
               backup_stages stage, const backup_sink *sink) noexcept
{
  if (stage == BACKUP_FINISHED && sink)
    delete static_cast<RocksDB_backup*>(sink->ha_data);
  return 0;
}

}  // namespace myrocks
