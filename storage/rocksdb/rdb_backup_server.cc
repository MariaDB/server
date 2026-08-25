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
#include <my_dir.h>
#include <mysqld_error.h>

#include <atomic>
#include <new>
#include <string>
#include <tuple>
#include <vector>

#ifndef _WIN32
# include <fcntl.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

#include "./rdb_mariadb_port.h"
#include "./ha_rocksdb_proto.h"
#include "./rdb_backup_server.h"

namespace myrocks {

namespace {

std::string rocksdb_backup_checkpoint_dir()
{
  std::string dir(rocksdb_datadir);
  while (!dir.empty() && dir.back() == '/')
    dir.pop_back();
  dir.push_back('/');
  dir.append(ROCKSDB_CHECKPOINT_SUBDIR);
  return dir;
}

/** BACKUP SERVER context for the RocksDB engine, attached to
backup_sink::ha_data. It is created single-threaded at
BACKUP_PHASE_NO_COMMIT (where it freezes a consistent,
hardlink-based rocksdb::Checkpoint and enumerates its files) and
drained by up to N step threads at BACKUP_PHASE_FINISH, which copy
the immutable checkpoint files into the target's "#rocksdb" subdirectory.
Because SST files are immutable, copying can safely happen after
the backup MDL has been released.
Each rocksdb_backup_step() call claims one file index via claim_next();
a claim past file_count() means that worker is done. */
class RocksDB_backup
{
public:
  RocksDB_backup()= default;
  ~RocksDB_backup() noexcept
  {
#ifndef _WIN32
    if (checkpoint_fd >= 0)
      std::ignore= close(checkpoint_fd);
#endif
  }

  RocksDB_backup(const RocksDB_backup &)= delete;
  RocksDB_backup &operator=(const RocksDB_backup &)= delete;

  /** Freeze a rocksdb::Checkpoint into checkpoint_dir.
  @return true on failure (my_error() already raised) */
  bool create_checkpoint() noexcept;

  /** Enumerate the checkpoint directory (non-recursive) into files[].
  @return 0 on success, non-zero on error */
  int scan_checkpoint() noexcept;

  /** Copy one checkpoint file @name into the target's "#rocksdb/" directory
  or oldgnu tar stream.
  @return 0 on success, non-zero on error */
  int copy_one(const backup_target &target, const backup_sink &sink,
               const std::string &name) noexcept;

  /** Create the "#rocksdb" subdirectory in a directory target.
  For a streaming target this is a no-op (the tar entry name carries the
  directory). Called exactly once per backup, from
  rocksdb_backup_start(BACKUP_PHASE_FINISH), which the server invokes
  single-threaded before the step threads fan out; no internal
  once-only guard is therefore needed.
  @return 0 on success, non-zero on error */
  int ensure_dest_subdir(const backup_target &target) noexcept;

  /** Remove the server-side checkpoint directory. It is safe on both
  the FINISH and ABORT paths, and when no checkpoint was ever created. */
  void remove_checkpoint() noexcept
  {
    if (!checkpoint_created)
      return;
    rdb_remove_checkpoint(checkpoint_dir.c_str());
    checkpoint_created= false;
  }

  /* Interface used by rocksdb_backup_step() across N threads. */
  size_t claim_next() noexcept { return next.fetch_add(1); }
  size_t file_count() const noexcept { return files.size(); }
  const std::string &file_at(size_t i) const noexcept { return files[i]; }

private:
  /** <rocksdb_datadir>/mariabackup-checkpoint.
  Assigned once by create_checkpoint(); read-only afterwards. */
  std::string checkpoint_dir;
  /** Checkpoint entries (SST, MANIFEST, CURRENT, OPTIONS, WAL, ...),
  as names relative to checkpoint_dir. Populated once by scan_checkpoint(),
  which runs single-threaded during BACKUP_PHASE_NO_COMMIT, and never
  modified afterwards: no element is added, removed or rewritten for
  the remaining life of the object. */
  std::vector<std::string> files;
  /** Next index into files[] to hand to a step thread.
  Atomic because this is the only point of mutable shared state between
  the BACKUP_PHASE_FINISH step threads: they all run against this one
  object (the server gives every thread its own backup_target_phase,
  but they share one ha_data map, hence one RocksDB_backup), and
  each takes its work item by incrementing this counter with no
  lock held. */
  std::atomic<size_t> next{0};
  /** Whether a server-side checkpoint currently exists. */
  bool checkpoint_created{false};
#ifndef _WIN32
  /** open(checkpoint_dir, O_DIRECTORY), for openat() of source files.
  Opened once by scan_checkpoint(); afterwards only used as a stable
  openat() base by the step threads, never reassigned. */
  int checkpoint_fd{-1};
#endif
};

int RocksDB_backup::ensure_dest_subdir(const backup_target &target) noexcept
{
#ifndef _WIN32
  if (target.fd < 0)
    return 0;
  if (mkdirat(target.fd, ROCKSDB_BACKUP_DIR, 0777) && errno != EEXIST)
  {
    my_error(ER_CANT_CREATE_FILE, MYF(0), ROCKSDB_BACKUP_DIR, errno);
    return 1;
  }
#else
  if (!target.path)
    return 0;
  std::string path(target.path);
  path.push_back('/');
  path.append(ROCKSDB_BACKUP_DIR);
  if (my_mkdir(path.c_str(), 0777, MYF(0)) && errno != EEXIST)
  {
    my_error(ER_CANT_CREATE_FILE, MYF(0), ROCKSDB_BACKUP_DIR, errno);
    return 1;
  }
#endif
  return 0;
}

bool RocksDB_backup::create_checkpoint() noexcept
{
  checkpoint_dir= rocksdb_backup_checkpoint_dir();
  /* Drop a stale checkpoint left behind by a previous, failed backup.
  BACKUP SERVER is serialized by MDL_BACKUP_START, so no user-level lock is
  needed to protect the fixed checkpoint path. */
  if (!access(checkpoint_dir.c_str(), F_OK))
    rdb_remove_checkpoint(checkpoint_dir.c_str());
  if (rdb_create_checkpoint(checkpoint_dir.c_str()))
    return true;
  checkpoint_created= true;
  return false;
}

int RocksDB_backup::scan_checkpoint() noexcept
{
#ifndef _WIN32
  checkpoint_fd= open(checkpoint_dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (checkpoint_fd < 0)
  {
    my_error(ER_CANT_READ_DIR, MYF(0), checkpoint_dir.c_str(), errno);
    return 1;
  }
#endif
  MY_DIR *dir_info= my_dir(checkpoint_dir.c_str(), MYF(MY_DONT_SORT));
  if (!dir_info)
  {
    my_error(ER_CANT_READ_DIR, MYF(0), checkpoint_dir.c_str(), my_errno);
    return 1;
  }
  /* A rocksdb::Checkpoint is a flat directory, so a non-recursive scan is
  sufficient (SST files, MANIFEST, CURRENT, OPTIONS, *.log). */
  for (size_t i= 0; i < dir_info->number_of_files; i++)
  {
    const char *name= dir_info->dir_entry[i].name;
    if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
      continue;
    files.emplace_back(name);
  }
  my_dirend(dir_info);
  return 0;
}

int RocksDB_backup::copy_one(const backup_target &target,
                             const backup_sink &sink,
                             const std::string &name) noexcept
{
  /* Destination is relative to the target: "#rocksdb/<name>". The default
  rocksdb_datadir "./#rocksdb" makes restore transparent. tar uses forward
  slashes; Win32 file APIs accept them too. */
  std::string rel(ROCKSDB_BACKUP_DIR);
  rel.push_back('/');
  rel.append(name);
#ifndef _WIN32
  int src= openat(checkpoint_fd, name.c_str(), O_RDONLY);
  if (src < 0)
  {
    my_error(ER_CANT_OPEN_FILE, MYF(0), name.c_str(), errno);
    return 1;
  }
  int ret_val= 0;
  if (sink.stream == sink.NO_STREAM)
  {
    /* Directory target: copy the file into <target>/#rocksdb/<name>. */
    int dst= openat(target.fd, rel.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (dst < 0)
    {
      my_error(ER_CANT_CREATE_FILE, MYF(0), rel.c_str(), errno);
      ret_val= 1;
    }
    else
    {
      ret_val= copy_entire_file(src, dst);
      if (ret_val | close(dst))
      {
        my_error(ER_ERROR_ON_WRITE, MYF(0), rel.c_str(), errno);
        ret_val= 1;
      }
    }
  }
  else
  {
    /* Stream target. Checkpoint SST files are immutable, so the
    Linux sendfile(2) fast path (backup_stream_append_async) is safe here,
    unlike live data files, there is no concurrent writer to race. */
    uint64_t end= uint64_t(lseek(src, 0, SEEK_END));
    if (backup_stream_start(sink.stream, rel.c_str(), 0644, end, nullptr, 0) ||
        backup_stream_append_async(src, sink.stream, 0, end) ||
        backup_stream_zeropad(sink.stream, size_t(end)))
    {
      my_error(ER_ERROR_ON_WRITE, MYF(0), rel.c_str(), errno);
      ret_val= 1;
    }
  }
  std::ignore= close(src);
  return ret_val;
#else
  std::string src_path(checkpoint_dir);
  src_path.push_back('/');
  src_path.append(name);
  if (sink.stream == sink.NO_STREAM)
  {
    /* Directory target */
    std::string dest_path(target.path);
    dest_path.push_back('/');
    dest_path.append(rel);
    if (!CopyFileEx(src_path.c_str(), dest_path.c_str(),
                    nullptr, nullptr, nullptr, COPY_FILE_NO_BUFFERING))
    {
      my_osmaperr(GetLastError());
      my_error(ER_CANT_CREATE_FILE, MYF(0), dest_path.c_str(), errno);
      return 1;
    }
    return 0;
  }
  /* Stream target */
  HANDLE dst= sink.stream;
  HANDLE src= CreateFile(src_path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         my_win_file_secattr(), OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (src == INVALID_HANDLE_VALUE)
  {
    my_osmaperr(GetLastError());
    my_error(ER_FILE_NOT_FOUND, MYF(0), src_path.c_str(), errno);
    return 1;
  }
  LARGE_INTEGER li;
  int ret_val= 0;
  if (!GetFileSizeEx(src, &li) ||
      backup_stream_start(dst, rel.c_str(), 0644, li.QuadPart, nullptr, 0) ||
      backup_stream_append_plain(src, dst, 0, li.QuadPart) ||
      backup_stream_zeropad(dst, size_t(li.LowPart)))
  {
    my_osmaperr(GetLastError());
    my_error(ER_ERROR_ON_WRITE, MYF(0), rel.c_str(), errno);
    ret_val= 1;
  }
  CloseHandle(src);
  return ret_val;
#endif
}

}  // anonymous namespace

/** During BACKUP_PHASE_NO_COMMIT phase, create #rocksdb destination subdirectory
in start operation */
void *rocksdb_backup_start(THD *thd MY_ATTRIBUTE((__unused__)),
                           const backup_target *target,
                           backup_phase phase, const backup_sink *sink) noexcept
{
  switch (phase) {
  case BACKUP_PHASE_PREPARE_START:
    /* Called with no locks held and sink/target == nullptr, before the phase
    loop. Nothing to pre-allocate; the checkpoint is frozen at NO_COMMIT. */
    return nullptr;
  case BACKUP_PHASE_NO_COMMIT:
  {
    assert(!sink->ha_data);
    RocksDB_backup *bk= new (std::nothrow) RocksDB_backup;
    if (!bk)
    {
      my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(RocksDB_backup));
      return reinterpret_cast<void *>(-1);
    }
    if (bk->create_checkpoint() || bk->scan_checkpoint())
    {
      bk->remove_checkpoint();             // drop it if create succeeded
      delete bk;
      return reinterpret_cast<void *>(-1);
    }
    return bk;
  }
  case BACKUP_PHASE_FINISH:
  {
    RocksDB_backup *bk= static_cast<RocksDB_backup *>(sink->ha_data);
    /* Create the destination subdirectory single-threaded, before the step
    workers fan out, so no worker can race a half-created directory. */
    if (bk && bk->ensure_dest_subdir(*target))
      return reinterpret_cast<void *>(-1);
    return bk;
  }
  default:
    return sink->ha_data;
  }
}

/** During step process of BACKUP PHASE FINISH phase, copy
the immutable checkpoint files fanned out across N threads
@retval -1 in case of failure
@retval 0 on success */
int rocksdb_backup_step(THD *,
                        const backup_target *target,
                        backup_phase phase, const backup_sink *sink) noexcept
{
  if (phase != BACKUP_PHASE_FINISH)
    return 0;

  RocksDB_backup *bk= static_cast<RocksDB_backup *>(sink->ha_data);
  if (!bk)
    return 0;
  size_t i= bk->claim_next();
  if (i >= bk->file_count())
    return 0;
  if (bk->copy_one(*target, *sink, bk->file_at(i)))
    return -1;
  return int(bk->file_count() - i);
}

/** During end process of BACKUP PHASE FINISH phase, remove
the checkpoint and freeing the context
@retval 0 on success */
int rocksdb_backup_end(THD *thd MY_ATTRIBUTE((__unused__)),
                       const backup_target *target MY_ATTRIBUTE((__unused__)),
                       backup_phase phase, const backup_sink *sink) noexcept
{
  if (phase != BACKUP_PHASE_FINISH)
    return 0;
  RocksDB_backup *bk= static_cast<RocksDB_backup *>(sink->ha_data);
  if (!bk)
    return 0;
  bk->remove_checkpoint();
  delete bk;
  return 0;
}

}  // namespace myrocks
