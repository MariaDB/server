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
#include "sql_class.h"
#include "backup_innodb.h"
#include "trx0trx.h"
#include <vector>

/** Associate a transaction with the current session
@param thd   session
@return InnoDB transaction */
trx_t *check_trx_exists(THD *thd) noexcept;

#ifdef _WIN32
#elif defined __APPLE__
# include <sys/attr.h>
# include <sys/clonefile.h>
# include <copyfile.h>
# define copy_file(src, dst, off) \
  fcopyfile(src, dst, nullptr, COPYFILE_ALL | COPYFILE_CLONE)
#else
using copying_step= ssize_t(int,int,size_t,off_t*);
template<copying_step step>
static ssize_t copy(int in_fd, int out_fd, off_t c) noexcept
{
  ssize_t ret;
  for (off_t offset{0};;)
  {
    off_t count= c;
    if (count > INT_MAX >> 20 << 20)
      count = INT_MAX >> 20 << 20;
    ret= step(in_fd, out_fd, size_t(count), &offset);
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
      return 0;
    if (!ret)
      return -1;
  }
  return ret;
}
# if defined __linux__ || defined __FreeBSD__
/* Copy between files in a single (type of) file system */
static inline ssize_t
copy_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return copy_file_range(in_fd, offset, out_fd, nullptr, count, 0);
}
#  define cfr(src,dst,size) copy<copy_step>(src, dst, size)
# endif
# ifdef __linux__
#  include <sys/sendfile.h>
/* Copy a file to a stream or to a regular file. */
static inline ssize_t
send_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return sendfile(out_fd, in_fd, offset, count);
}
# else
#  include <sys/mman.h>
/** Copy a file using a memory mapping.
@param in_fd   source file
@param out_fd  destination
@param count   number of bytes to copy
@return error code
@retval 0  on success
@retval 1  if a memory mapping failed */
static ssize_t mmap_copy(int in_fd, int out_fd, off_t count)
{
#if SIZEOF_SIZE_T < 8
  if (count != ssize_t(count))
    return 1;
#endif
  void *p= mmap(nullptr, count, PROT_READ, MAP_SHARED, in_fd, 0);
  if (p == MAP_FAILED)
    return 1;
  ssize_t ret;
  size_t c= size_t(count);
  for (const char *b= static_cast<const char*>(p);; b+= ret)
  {
    ret= write(out_fd, b, std::min(c, size_t(INT_MAX >> 20 << 20)));
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
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
  munmap(p, count);
  return ret;
}

static ssize_t pread_write(int in_fd, int out_fd, off_t count) noexcept
{
  constexpr size_t READ_WRITE_SIZE= 65536;
  char *b= static_cast<char*>(aligned_malloc(READ_WRITE_SIZE, 4096));
  if (!b)
    return -1;
  ssize_t ret;
  for (off_t o= 0;; o+= ret)
  {
    ret= pread(in_fd, b, ssize_t(std::min(count, off_t{READ_WRITE_SIZE})), o);
    if (ret > 0)
      ret= write(out_fd, b, ret);
    if (ret < 0)
      break;
    count-= ret;
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
  return ret;
}
# endif

/** Copy a file.
@param src  source file descriptor
@param dst  target to append src to
@param size amount of data to be copied
@return error code (negative)
@retval 0   on success */
static int copy_file(int src, int dst, off_t size) noexcept
{
  ssize_t ret;
# ifdef cfr
  if (!(ret= cfr(src, dst, size)))
    return int(ret);
#  ifdef __linux__
  if (errno == EOPNOTSUPP)
#  endif
# endif
# ifdef __linux__ // starting with Linux 2.6.33, we can rely on sendfile(2)
    ret= copy<send_step>(src, dst, size);
# else
  if ((ret= mmap_copy(src, dst, size)) == 1)
    ret= pread_write(src, dst, size);
# endif
  ut_ad(ret <= 0);
  return int(ret);
}
#endif

namespace
{
/** Backup state; protected by log_sys.latch */
class InnoDB_backup
{
  /** first LSN of last log that was copied (1 if none yet),
  or 0 if backup is not active */
  lsn_t max_first_lsn;
  /** the original innodb_log_file_size, or 0 */
  uint64_t old_size;

  /** collection of files to be copied */
  std::vector<uint32_t> queue;
  /** collection of completed log archive files to be
  hard-linked, copied, or moved */
  std::vector<lsn_t> logs;

  /** target directory name or handle */
  IF_WIN(const char*,int) target;

  /** the checkpoint from which the backup starts */
  lsn_t checkpoint;
  /** end_lsn of the checkpoint at the backup start */
  lsn_t checkpoint_end_lsn;
public:
  /**
     Start of BACKUP SERVER: collect all files to be backed up
     @param thd     current session
     @param target  target directory
     @return error code
     @retval 0 on success
  */
  int init(THD *thd, IF_WIN(const char*,int) target) noexcept
  {
    trx_t *trx= check_trx_exists(thd);
    if (trx->id || trx->state != TRX_STATE_NOT_STARTED)
    {
      ut_ad(trx->state != TRX_STATE_BACKUP);
      my_error(ER_CANT_DO_THIS_DURING_AN_TRANSACTION, MYF(0));
      return 1;
    }

    log_sys.latch.wr_lock();
    ut_ad(!max_first_lsn);
    ut_ad(queue.empty());
    if (!logs.empty())
    {
      /* A new BACKUP SERVER is being invoked before a previous one
      had been fully finalized. Clean up any log files. */
      if (old_size)
        delete_logs();
      logs.clear();
    }

    const bool fail{log_sys.backup_start(&old_size, thd)};

    if (!fail)
    {
      const lsn_t start{log_sys.archived_checkpoint};
      checkpoint= start;
      checkpoint_end_lsn= log_sys.archived_lsn;

      this->target= target;
      /* Collect all tablespaces that have been created before our
      start checkpoint. Newer tablespaces will be recovered by the
      innodb_log_archive=ON recovery.

      If a tablespace is deleted before step() is invoked, the file
      will not be copied, and a FILE_DELETE record in the log will
      ensure correct recovery.

      If a tablespace is renamed between this and end(), the recovery
      of a FILE_RENAME record will ensure the correct file name,
      no matter which name was used by step(). */
      mysql_mutex_lock(&fil_system.mutex);
      for (fil_space_t &space : fil_system.space_list)
        if (space.id < SRV_SPACE_ID_UPPER_BOUND &&
            !space.is_being_imported() &&
            /* FIXME: how to initialize create_lsn for old files, to
            have efficient incremental backup?
            fil_node_t::read_page0() cannot assign it from
            FIL_PAGE_LSN because that would not reflect the file
            creation but for example allocating or freeing a page.

            The easy parts of initializing space->create_lsn are
            as follows:
            (1) In log_parse_file() when processing FILE_CREATE
            (2) In deferred_spaces.create() */
            space.get_create_lsn() < start)
          queue.emplace_back(space.id);
      mysql_mutex_unlock(&fil_system.mutex);
      max_first_lsn= 1;
      trx->state= TRX_STATE_BACKUP;
    }
    log_sys.latch.wr_unlock();
    DEBUG_SYNC(thd, "innodb_backup_start");
    return fail;
  }

  /**
     Process a file that was collected at init().
     This may be invoked from multiple concurrent threads.
     @param thd   current session
     @return number of files remaining, or negative on error
     @retval 0 on completion
  */
  int step(THD *thd) noexcept
  {
    uint32_t id= FIL_NULL;
    lsn_t lsn= 0;
    log_sys.latch.wr_lock();
    ut_ad(max_first_lsn);
    size_t size{queue.size()};
    if (!logs.empty())
    {
      lsn= logs.back();
      if (max_first_lsn < lsn)
        max_first_lsn= lsn;
      logs.pop_back();
      if (!size)
        size= logs.size();
    }
    else if (size)
    {
      size--;
      id= queue.back();
      queue.pop_back();
    }
    log_sys.latch.wr_unlock();

    if (lsn)
    {
      if (link_or_move(lsn, nullptr))
        return -1;
    }
    else if (fil_space_t *space= fil_space_t::get(id))
    {
      int res= -1;
      for (fil_node_t *node= UT_LIST_GET_FIRST(space->chain); node;
           node= UT_LIST_GET_NEXT(chain, node))
        if ((res= backup(node)))
          break;
      space->release();
      if (res)
        return res;
    }

    size= std::min(size_t{std::numeric_limits<int>::max()}, size);
    return int(size);
  }

  /**
     Finish copying and determine the logical time of the backup snapshot.
     fini() must be invoked on the same thd.
     @param thd   current session
     @param abort whether BACKUP SERVER was aborted
     @return error code
     @retval 0 on success
  */
  int end(THD *thd, bool abort) noexcept
  {
    int fail= 0;
    log_sys.latch.wr_lock();
    if (abort)
    {
    skip_log_dup:
      queue.clear();
      if (old_size)
        delete_logs();
      logs.clear();
    }
    else
    {
      ut_ad(max_first_lsn);
      ut_ad(queue.empty());
      trx_t *trx= thd_to_trx(thd);
      if (!trx || trx->state != TRX_STATE_BACKUP)
        goto skip_log_dup;
      const lsn_t last_lsn{log_sys.get_flushed_lsn(std::memory_order_relaxed)};
      while (!logs.empty())
      {
        const lsn_t lsn{logs.back()};
        if (lsn > last_lsn)
          break;
        if (lsn > max_first_lsn)
          max_first_lsn= lsn;
        logs.pop_back();
        log_sys.latch.wr_unlock();
        fail= link_or_move(lsn, nullptr);
        log_sys.latch.wr_lock();
        if (fail)
          goto skip_log_dup;
      }

      {
        const lsn_t lsn{log_sys.get_first_lsn()};
        if (lsn > max_first_lsn && lsn < last_lsn)
        {
          max_first_lsn= lsn;
          log_sys.latch.wr_unlock();
          bool live_hardlink= false;
          fail= link_or_move(lsn, &live_hardlink);
          log_sys.latch.wr_lock();
          if (fail)
            goto skip_log_dup;
          if (!live_hardlink)
            max_first_lsn= 0;
        }
        else
          goto skip_log_dup;
      }

      trx->start_time_micro= max_first_lsn;
      trx->commit_lsn= last_lsn;
    }

    ut_ad(!log_sys.resize_in_progress());
    ut_ad(log_sys.archive);
    if (old_size)
    {
      log_sys.latch.wr_unlock();
      log_sys.backup_stop_archiving(thd);
      log_sys.latch.wr_lock();
    }
    max_first_lsn= 0;
    log_sys.backup_stop(old_size, thd);
    return fail;
  }

  /**
     Clean up after end().
     @param thd     the parameter that had been passed to end()
     @param target  target directory
     @return error code
     @retval 0 on success
  */
  int fini(THD *thd, IF_WIN(const char*,int) target) noexcept
  {
    int fail= 0;
    log_sys.latch.wr_lock();
    if (!max_first_lsn)
    {
      ut_ad(queue.empty());
      if (old_size)
        delete_logs();
      logs.clear();
    }
    log_sys.latch.wr_unlock();

    trx_t *trx= thd_to_trx(thd);
    if (!trx || trx->state != TRX_STATE_BACKUP)
      ut_ad("invalid state" == 0);
    else
    {
      ut_ad(!trx->id);
      if (const lsn_t first_lsn{trx->start_time_micro})
      {
        trx->start_time_micro= 0;
        /* Copy our clone of the last log until the final LSN */
#ifdef _WIN32
        std::string src{target};
        src.push_back('/');
        std::string dst{src};
        src.push_back("ib_logfile101");
        log_sys.append_archive_name(dst, first_lsn);
        const char *s= src.c_str(), *d= dst.c_str();

        if (!CopyFileExA(s, d, nullptr, nullptr, nullptr,
                         COPY_FILE_NO_BUFFERING) ||
            !MoveFileEx(d, s, MOVEFILE_REPLACE_EXISTING))
        {
          my_osmaperr(GetLastError());
          my_error(ER_ERROR_ON_RENAME, MYF(ME_ERROR_LOG), s, d, errno);
          fail= 1;
        }
#else
        int s= openat(target, "ib_logfile101", O_RDONLY);
        std::string dst;
        log_sys.append_archive_name(dst, first_lsn);
        int d;
        if (s == -1)
        {
          my_error(ER_FILE_NOT_FOUND, MYF(ME_ERROR_LOG), "ib_logfile101",
                   errno);
          fail= 1;
          goto done;
        }
# ifdef __APPLE__
        fail= clonefileat(s, target, dst.c_str(), 0);
        if (!fail)
          goto done;
        if (errno != ENOTSUP)
          goto fail;
# endif
        d= openat(target, dst.c_str(), O_CREAT | O_EXCL | O_TRUNC | O_WRONLY,
                  0666);
        if (d < 0)
        {
        fail:
          fail= 1;
          my_error(ER_ERROR_ON_RENAME, MYF(ME_ERROR_LOG), s, d, errno);
          close(s);
        }
        else
        {
          fail= copy_file(s, d,
                          ((trx->commit_lsn - first_lsn) + 4095) & ~4095ULL);
          if (close(d) || fail)
            goto fail;
          if (unlinkat(target, "ib_logfile101", 0))
          {
            my_error(ER_CANT_DELETE_FILE, MYF(ME_ERROR_LOG),
                     "ib_logfile101", errno);
            fail= 1;
          }
          if (close(s))
          {
            my_error(ER_ERROR_ON_CLOSE, MYF(ME_ERROR_LOG), "ib_logfile101",
                     errno);
            fail= 1;
          }
        }
      done:;
#endif
      }
      trx->commit_lsn= 0;
      trx->state= TRX_STATE_NOT_STARTED;
    }
    return fail;
  }

  /**
     Complete the first checkpoint in a new archive log file.
  */
  void checkpoint_complete() noexcept
  {
    ut_ad(log_sys.latch_have_wr());
    if (max_first_lsn)
      logs.emplace_back(log_sys.get_first_lsn() - log_sys.capacity());
  }

private:
  /** Safely start backing up a tablespace file */
  static void backup_start(fil_space_t *space) noexcept
  {
    if (space->backup_start(space->size))
      os_aio_wait_until_no_pending_writes(false);
  }
  /* Stop backing up a tablespace */
  static void backup_stop(fil_space_t *space) noexcept
  { space->backup_stop(); }

  /** Delete unnecessary logs that had been created for backup. */
  void delete_logs() noexcept
  {
    ut_ad(old_size);
    for (const lsn_t lsn : logs)
      IF_WIN(DeleteFile,unlink)(log_sys.get_archive_path(lsn).c_str());
  }

  /**
     Back up a persistent InnoDB data file.
     @param node  InnoDB data file
  */
  int backup(fil_node_t *node) noexcept
  {
    for (bool tried_mkdir{false};;)
    {
#ifdef _WIN32
      backup_start(node->space);
      std::string path{target};
      path.push_back('/');
      path.append(node->name);
      bool ok= CopyFileExA(node->name, path.c_str(), nullptr, nullptr, nullptr,
                           COPY_FILE_NO_BUFFERING);
      backup_stop(node->space);
      if (!ok)
      {
        unsigned long err= GetLastError();
        if (err == ERROR_PATH_NOT_FOUND && !tried_mkdir &&
            node->space->id && !srv_is_undo_tablespace(node->space->id))
        {
          tried_mkdir= true;
          path= target;
          const char *sep= strchr(node->name, '/');
          ut_ad(sep);
          sep= strchr(sep + 1, '/');
          ut_ad(sep);
          path.append(node->name, size_t(sep - node->name));
          if (CreateDirectory(path.c_str(),
                              my_dir_security_attributes.lpSecurityDescriptor
                              ? &my_dir_security_attributes : nullptr) ||
              (err= GetLastError()) == ERROR_ALREADY_EXISTS)
            continue;
        }

        my_osmaperr(err);
        goto fail;
      }
      break;
#else
      int f;
# ifdef __APPLE__
      backup_start(node->space);
      f= fclonefileat(node->handle, target, node->name, 0);
      backup_stop(node->space);
      if (!f)
        break;
      switch (errno) {
      case ENOENT:
        goto try_mkdir;
      case ENOTSUP:
        break;
      default:
        goto fail;
      }
# endif
      f= openat(target, node->name,
                O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0666);
      if (f < 0)
      {
        if (errno == ENOENT)
        {
# ifdef __APPLE__
        try_mkdir:
# endif
          if (!tried_mkdir && node->space->id &&
              !srv_is_undo_tablespace(node->space->id))
          {
            tried_mkdir= true;
            const char *sep= strchr(node->name, '/');
            ut_ad(sep);
            sep= strchr(sep + 1, '/');
            ut_ad(sep);
            std::string dir{node->name, size_t(sep - node->name)};
            if (!mkdirat(target, dir.c_str(), 0777) || errno == EEXIST)
              continue;
          }
        }
        goto fail;
      }

      /* TODO: push down page range locking */
      backup_start(node->space);
      int err= copy_file(node->handle, f,
                         off_t{node->size} * node->space->physical_size());
      backup_stop(node->space);
      if (close(f) || err)
        goto fail;
      break;
#endif
    }
    return 0;
  fail:
    my_error(ER_CANT_CREATE_FILE, MYF(0), node->name, errno);
    return -1;
  }

  /** Hard-link (copy) or rename (move) an archive log file.
  @param lsn   The first LSN in the file
  @param clone pointer to a flag that will be set if a live log was
               hard-linked (needing deduplication),
               or nullptr if the source log file is known to be read-only
  @return error code
  @retval 0 on success */
  int link_or_move(lsn_t lsn, bool *clone) noexcept
  {
    ut_ad(!clone || !*clone);
    const std::string p{log_sys.get_archive_path(lsn)};
    const char *const path= p.c_str(), *basename= strrchr(path, '/');
    if (!basename)
      basename= path;
    else
      basename++;
    const bool move{!clone && old_size};

#ifdef _WIN32
    const bool closed{clone && log_sys.close_file_if_at(lsn)};
    std::string b{target};
    if (clone)
      b.push_back("/ib_logfile101");
    else
    {
      b.push_back('/');
      b.append(basename);
    }
    const char *destname= b.str();

    unsigned long err;
    if (move)
    {
      if (!MoveFileEx(path, destname, MOVEFILE_COPY_ALLOWED))
      {
      fail:
        err= GetLastError();
      got_err:
        if (closed)
          log_sys.resume_file();
        my_osmaperr(err);
        my_error(ER_ERROR_ON_RENAME, MYF(ME_ERROR_LOG), path, basename, errno);
        return -1;
      }
    }
    else if (!CreateHardLink(destname, path, nullptr))
    {
      if ((err= GetLastError()) != ERROR_NOT_SAME_DEVICE)
        goto got_err;
      /* Hard-linking failed. Try copying with the final name. */
      b.assign(target);
      b.push_back('/');
      b.append(basename);
      destname= b.str();

      if (!CopyFileExA(path, destname, nullptr, nullptr, nullptr,
                       COPY_FILE_NO_BUFFERING))
        goto fail;
    }
    else if (clone)
      *clone= true;
    if (closed)
      log_sys.resume_file();
#else
    if (move
        ? !renameat(AT_FDCWD, path, target, basename)
        : !linkat(AT_FDCWD, path, target, clone ? "ib_logfile101" : basename,
                  AT_SYMLINK_FOLLOW))
    {
      if (clone)
        *clone= !move;
      return 0;
    }
    else if (errno != EXDEV)
    {
    fail:
      my_error(ER_ERROR_ON_RENAME, MYF(ME_ERROR_LOG), path, basename, errno);
      return -1;
    }
    else
    {
      int src= open(path, O_RDONLY);
      if (src < 0)
        goto fail;
      if (move && unlink(path))
      {
      close_and_fail:
        std::ignore= close(src);
        goto fail;
      }
      int dst= openat(target, basename,
                      O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0666);
      if (dst < 0)
        goto close_and_fail;
      int err= copy_file(src, dst, lseek(src, 0, SEEK_END));
      if ((close(dst) | close(src)) || err)
        goto fail;
    }
#endif
    return 0;
  }
};

/** The backup context; protected by log_sys.latch */
static InnoDB_backup backup;
}

bool log_t::backup_start(uint64_t *old_size, THD *thd) noexcept
{
  ut_ad(latch_have_wr());
  ut_ad(!backup);
  backup= true;
  *old_size= 0;
  if (archive)
    return false;
  const uint64_t old_file_size{file_size};
  latch.wr_unlock();
  bool fail;
  if (old_file_size > ARCHIVE_FILE_SIZE_MAX)
  {
    if (resize_start(ARCHIVE_FILE_SIZE_MAX, thd, true) == RESIZE_STARTED)
      resize_finish(thd);
    latch.wr_lock();
    if (file_size > ARCHIVE_FILE_SIZE_MAX)
      goto too_big;
    latch.wr_unlock();
  }
  fail= set_archive(true, thd, true);
  latch.wr_lock();
  if (!fail)
  {
    *old_size= old_file_size;
    return false;
  }
 too_big:
  ut_ad(backup);
  backup= false;
  const uint64_t new_file_size{file_size};
  latch.wr_unlock();
  if (old_file_size != new_file_size && old_file_size &&
      resize_start(old_file_size, thd) == RESIZE_STARTED)
    resize_finish(thd);
  latch.wr_lock();
  return true;
}

void log_t::backup_stop(uint64_t old_size, THD *thd) noexcept
{
  ut_ad(latch_have_wr());
  /* We will be invoked with old_size=0 after a failed backup_start(),
  or if innodb_log_archive=ON held during a successful backup_start(). */
  ut_ad(!old_size || !resize_in_progress());
  ut_ad(!old_size || backup);
  backup= false;
  const uint64_t new_size{file_size};
  latch.wr_unlock();
  if (old_size && old_size != new_size &&
      resize_start(old_size, thd) == RESIZE_STARTED)
    resize_finish(thd);
}

int innodb_backup_start(THD *thd, IF_WIN(const char*,int) target) noexcept
{
  return backup.init(thd, target);
}

int innodb_backup_step(THD *thd) noexcept
{
  return backup.step(thd);
}

int innodb_backup_end(THD *thd, bool abort) noexcept
{
  return backup.end(thd, abort);
}

int innodb_backup_finalize(THD *thd, IF_WIN(const char*,int) target) noexcept
{
  return backup.fini(thd, target);
}

void innodb_backup_checkpoint() noexcept
{
  backup.checkpoint_complete();
}
