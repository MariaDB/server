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

/*
  BACKUP SERVER support for the RocksDB (MyRocks) engine.
*/

#define MYSQL_SERVER 1

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

#include "./rdb_mariadb_port.h"
#include "./ha_rocksdb_proto.h"
#include "./rdb_backup_server.h"

namespace myrocks {

namespace {

/**
   Report that a directory cannot be read.
   @param name   directory name
*/
ATTRIBUTE_COLD ATTRIBUTE_NOINLINE void dir_error(const char *name)
{
#ifdef _WIN32
  my_osmaperr(GetLastError());
#endif
  my_error(ER_CANT_READ_DIR, MYF(0), name, errno);
}

/** BACKUP SERVER state for the RocksDB engine,
attached to backup_sink::ha_data.
It is created single-threaded at BACKUP_PHASE_NO_COMMIT,
where it freezes a consistent, hardlink-based
rocksdb::Checkpoint and opens that directory for iteration.

At BACKUP_PHASE_FINISH, up to N step threads drain the
directory, copying the files into the target's "#rocksdb"
subdirectory. Because SST files are immutable,
copying can safely happen after the backup MDL has been released.

Each rocksdb_backup_step() claims one file name from the
directory iteration inside the critical section, then copies
that file outside it. The name is copied to the caller's stack,
because the struct dirent (or WIN32_FIND_DATAA) belongs to the
shared iteration state and would be overwritten by the next
step thread. */
struct RocksDB_backup
{
#ifndef _WIN32
  /** checkpoint directory stream; dirfd() is the openat() base */
  DIR *dir;
#else
  /** checkpoint directory iterator, or INVALID_HANDLE_VALUE once the
  iteration has been exhausted */
  HANDLE dir;
  /** FindFirstFileA()/FindNextFile() buffer for dir, holding the entry
  to be consumed by the next rocksdb_backup_step() */
  WIN32_FIND_DATAA d;
#endif
  /** mutex protecting the directory iteration state across
  concurrent rocksdb_backup_step() */
  pthread_mutex_t mutex;
  /** whether a server-side checkpoint currently exists */
  bool checkpoint_created;
  /** <rocksdb_datadir>/#rocksdb; assigned before the step threads
  exist and read-only afterwards */
  char checkpoint_dir[FN_REFLEN];
  /** Length of the checkpoint_dir prefix to omit from the
  name of a file in the backup i.e offset of the ROCKSDB_BACKUP_DIR
  component in checkpoint_dir. */
  size_t dest_prefix;
};

/**
   Compose <rocksdb_datadir>/#rocksdb and the length of the prefix
   that has to be omitted from it to name a file in the backup.
   @param bk   BACKUP SERVER state
   @return error code
   @retval 0 on success
*/
int rocksdb_checkpoint_dir(RocksDB_backup *bk)
{
  size_t len= strlen(rocksdb_datadir);
  while (len && rocksdb_datadir[len - 1] == '/')
    len--;
  if (sizeof bk->checkpoint_dir <=
      (size_t) snprintf(bk->checkpoint_dir, sizeof bk->checkpoint_dir,
                        "%.*s/%s", (int) len, rocksdb_datadir,
                        ROCKSDB_CHECKPOINT_SUBDIR))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), rocksdb_datadir);
    return 1;
  }
  bk->dest_prefix= strlen(bk->checkpoint_dir) - (sizeof ROCKSDB_BACKUP_DIR - 1);
  return 0;
}

/**
   Create the "#rocksdb" subdirectory in a directory target.
   For a streaming target this is a no-op
   (the tar entry name carries the directory).
   Invoked once per backup, from rocksdb_backup_start
   (BACKUP_PHASE_FINISH), which the server calls
   single-threaded before the step threads fan out.
   @param target   BACKUP SERVER target
   @return error code
   @retval 0 on success
*/
int rocksdb_backup_mkdir(const backup_target *target)
{
#ifndef _WIN32
  if (target->fd < 0 ||
      !mkdirat(target->fd, ROCKSDB_BACKUP_DIR, 0777) ||
      errno == EEXIST)
    return 0;
#else
  if (!target->path)
    return 0;
  char path[FN_REFLEN];
  if (sizeof path <= (size_t) snprintf(path, sizeof path, "%s/%s",
                                       target->path,
                                       ROCKSDB_BACKUP_DIR))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), target->path);
    return 1;
  }
  if (!my_mkdir(path, 0777, MYF(0)) || errno == EEXIST)
    return 0;
#endif
  my_error(ER_CANT_CREATE_FILE, MYF(0), ROCKSDB_BACKUP_DIR, errno);
  return 1;
}

/**
   Copy one checkpoint file into the target's "#rocksdb/" directory
   or oldgnu tar stream.
   The file is stored as ROCKSDB_BACKUP_DIR "/" <name>, which
   is the tail of its path in the checkpoint directory;
   @param target  BACKUP SERVER target
   @param sink    per-thread context
   @param bk      BACKUP SERVER state
   @param name    base name of the checkpoint file to copy
   @return error code
   @retval 0 on success
*/
int rocksdb_backup_file(const backup_target *target,
                        const backup_sink *sink,
                        const RocksDB_backup *bk, const char *name)
{
  char src_path[FN_REFLEN];
  if (sizeof src_path <= (size_t) snprintf(src_path, sizeof src_path, "%s/%s",
                                           bk->checkpoint_dir, name))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), name);
    return 1;
  }
  return backup::copy_or_stream(*target, *sink, src_path,
                                bk->dest_prefix) != 0;
}

/**
   Back up one checkpoint file, claiming its name from the shared
   directory iteration inside a critical section and copying the file
   outside it.
   @param target  BACKUP SERVER target
   @param sink    per-thread context
   @retval 1 on success if some work remains
   @retval 0 on successful completion
   @retval -1 on error
*/
int rocksdb_backup_files(const backup_target *target,
                         const backup_sink *sink)
{
  RocksDB_backup *const bk= static_cast<RocksDB_backup*>(sink->ha_data);
  const char *filename= NULL;
  char path[FN_REFLEN];
  int left;
#ifndef _WIN32
  const struct dirent *d;
  struct stat sb;
  int dfd;
  assert(bk->dir);
#endif

  pthread_mutex_lock(&bk->mutex);
#ifndef _WIN32
  dfd= dirfd(bk->dir);
  /* A rocksdb::Checkpoint is a flat directory, so a non-recursive
  scan is sufficient (SST files, MANIFEST, CURRENT, OPTIONS, *.log).
  Anything that is not a regular file, "." and ".." included,
  is skipped. */
  while ((d= readdir(bk->dir)) != NULL)
  {
    switch (d->d_type) {
    default:
      continue;
    case DT_REG:
    case DT_LNK:
      break;
    case DT_UNKNOWN:
      if (fstatat(dfd, d->d_name, &sb, 0) ||
          (sb.st_mode & S_IFMT) != S_IFREG)
        continue;
    }

    /* Consume a file name. The struct dirent belongs to the
    directory stream, and the next readdir() by another step thread
    may overwrite it, so copy the name out before releasing
    the mutex. */
    if ((int) sizeof path <= snprintf(path, sizeof path, "%s", d->d_name))
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), d->d_name);
      pthread_mutex_unlock(&bk->mutex);
      return -1;
    }
    filename= path;
    break;
  }

  left= d != NULL;
#else
  if (bk->dir == INVALID_HANDLE_VALUE)
  {
    /* A previous rocksdb_backup_step() exhausted the iteration. */
    pthread_mutex_unlock(&bk->mutex);
    return 0;
  }

  do
  {
    if (bk->d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;

    /* Consume a file name; see the comment in the POSIX branch above. */
    if ((int) sizeof path <= snprintf(path, sizeof path, "%s",
                                      bk->d.cFileName))
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), bk->d.cFileName);
      pthread_mutex_unlock(&bk->mutex);
      return -1;
    }
    filename= path;
  }
  while ((left= FindNextFile(bk->dir, &bk->d)) && !filename);

  if (!left)
  {
    /* Do not invoke FindNextFile() on an exhausted iterator, whose
    WIN32_FIND_DATAA would be stale: close it and let any further
    rocksdb_backup_step() report completion above. */
    FindClose(bk->dir);
    bk->dir= INVALID_HANDLE_VALUE;
  }
#endif
  pthread_mutex_unlock(&bk->mutex);

  if (filename &&
      rocksdb_backup_file(target, sink, bk, filename))
    return -1;
  return left;
}

}  // anonymous namespace

/** During BACKUP_PHASE_NO_COMMIT, freeze a rocksdb::Checkpoint
and open it for iteration; during BACKUP_PHASE_FINISH, create
the "#rocksdb" destination subdirectory. */
void *rocksdb_backup_start(THD *thd MY_ATTRIBUTE((__unused__)),
                           const backup_target *target,
                           backup_phase phase, const backup_sink *sink)
  noexcept
{
  switch (phase) {
    RocksDB_backup *bk;
  case BACKUP_PHASE_PREPARE_START:
    /* Called with no locks held and sink/target == nullptr,
    before the phase loop. Nothing to pre-allocate;
    the checkpoint is frozen at BACKUP_PHASE_NO_COMMIT. */
    assert(!sink);
    return NULL;
  case BACKUP_PHASE_NO_COMMIT:
    assert(!sink->ha_data);
    if (!(bk= static_cast<RocksDB_backup*>(calloc(1, sizeof *bk))))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof *bk);
      return reinterpret_cast<void*>(-1);
    }
    pthread_mutex_init(&bk->mutex, NULL);
#ifdef _WIN32
    bk->dir= INVALID_HANDLE_VALUE;
#endif
    if (rocksdb_checkpoint_dir(bk))
      goto err_exit;
    /* Drop a stale checkpoint left behind by a previous,
    failed backup. BACKUP SERVER is serialized by MDL_BACKUP_START,
    so no user-level lock is needed to protect the fixed
    checkpoint path. */
    if (!access(bk->checkpoint_dir, F_OK))
      rdb_remove_checkpoint(bk->checkpoint_dir);
    if (rdb_create_checkpoint(bk->checkpoint_dir))
      goto err_exit;
    bk->checkpoint_created= true;
#ifndef _WIN32
    {
      const int dfd= open(bk->checkpoint_dir, O_RDONLY | O_DIRECTORY);
      if (dfd >= 0)
      {
        if ((bk->dir= fdopendir(dfd)))
          return bk;
        close(dfd);
      }
    }
#else
    {
      char pattern[FN_REFLEN];
      if ((int) sizeof pattern > snprintf(pattern, sizeof pattern, "%s/*.*",
                                          bk->checkpoint_dir) &&
          (bk->dir= FindFirstFileA(pattern, &bk->d)) != INVALID_HANDLE_VALUE)
        return bk;
    }
#endif
    dir_error(bk->checkpoint_dir);
  err_exit:
    if (bk->checkpoint_created)
      rdb_remove_checkpoint(bk->checkpoint_dir);
    pthread_mutex_destroy(&bk->mutex);
    free(bk);
    return reinterpret_cast<void*>(-1);
  case BACKUP_PHASE_FINISH:
    /* Create the destination subdirectory single-threaded, before the
    step threads fan out, so no worker can race a half-created
    directory. */
    if (sink->ha_data && rocksdb_backup_mkdir(target))
      return reinterpret_cast<void*>(-1);
    /* fall through */
  default:
    return sink->ha_data;
  }
}

/** During the step process of BACKUP_PHASE_FINISH, copy the immutable
checkpoint files, fanned out across N threads.
@retval 1 on success if some work remains
@retval 0 on completion
@retval -1 on error */
int rocksdb_backup_step(THD *thd MY_ATTRIBUTE((__unused__)),
                        const backup_target *target,
                        backup_phase phase, const backup_sink *sink) noexcept
{
  if (phase != BACKUP_PHASE_FINISH || !sink->ha_data)
    return 0;
  return rocksdb_backup_files(target, sink);
}

/** During the end process of BACKUP_PHASE_FINISH, close the checkpoint
directory, remove the checkpoint and free the context.
@retval 0 on success */
int rocksdb_backup_end(THD *thd MY_ATTRIBUTE((__unused__)),
                       const backup_target *target MY_ATTRIBUTE((__unused__)),
                       backup_phase phase, const backup_sink *sink) noexcept
{
  if (phase != BACKUP_PHASE_FINISH || !sink)
    return 0;
  RocksDB_backup *const bk= static_cast<RocksDB_backup*>(sink->ha_data);
  if (!bk)
    return 0;
#ifndef _WIN32
  if (bk->dir)
    closedir(bk->dir);          /* this also closes dirfd(bk->dir) */
#else
  if (bk->dir != INVALID_HANDLE_VALUE)
    FindClose(bk->dir);
#endif
  if (bk->checkpoint_created)
    rdb_remove_checkpoint(bk->checkpoint_dir);
  pthread_mutex_destroy(&bk->mutex);
  free(bk);
  return 0;
}

}  // namespace myrocks
