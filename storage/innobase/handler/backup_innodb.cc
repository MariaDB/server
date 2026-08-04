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
#include "sql_backup_interface.h"
#include "trx0trx.h"
#include "buf0flu.h"
#include "log0crypt.h"
#include "dict0load.h"
#include <vector>
#ifdef __linux__
# include <fcntl.h>
# include <linux/falloc.h>
#endif

/** Associate a transaction with the current session
@param thd   session
@return InnoDB transaction */
trx_t *check_trx_exists(THD *thd) noexcept;

namespace
{
/** Backup state; protected by log_sys.latch */
class InnoDB_backup
{
  /** Backup context */
  struct context
  {
    /** Start LSN of the first backed up log file */
    lsn_t first_lsn;
    /** Start LSN of the last log file, or LSN_MAX if not determined yet */
    lsn_t max_first_lsn;
    /** Final LSN of the backup, or LSN_MAX if not determined yet */
    lsn_t last_lsn;
    /** size of the first log file */
    uint64_t first_size;
    /** Checkpoint at the start of the backup */
    lsn_t checkpoint;
    /** Log record pointing to the checkpoint */
    lsn_t checkpoint_end_lsn;
    /** the original state of innodb_log_archive before/after backup */
    bool archived;
    /** whether end() was invoked */
    bool cleaned_up;
    /** the start LSN of the last hard-linked file, or 0 */
    std::atomic<lsn_t> last_hardlink;

    /**
       Note that a log file was hard-linked.
       @param lsn   start LSN of a hard-linked file
    */
    void note_hardlink(lsn_t lsn) noexcept
    {
      for (lsn_t last= last_hardlink.load(std::memory_order_relaxed);
           last < lsn && !last_hardlink.
             compare_exchange_weak(last, lsn,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed); ) {}
    }

    /** Ensure that the last, hard-linked log file is not shared with
    the server data directory, by copying it until the final LSN
    @param target    backup target directory
    @param hl        last_hardlink
    @return error code
    @retval 0 on success
    */
    ATTRIBUTE_COLD int de_hardlink(const backup_target &target, lsn_t hl)
      noexcept
    {
#ifdef _WIN32
      std::string src{target.path};
      src.push_back('/');
      std::string dst{src};
      src.append("ib_logfile101");
      log_sys.append_archive_name(dst, hl);
      const char *const s_{src.c_str()}, *const d_{dst.c_str()};
      if (!MoveFileEx(d_, s_, 0))
      {
        my_osmaperr(GetLastError());
        my_error(ER_ERROR_ON_RENAME, MYF(ME_ERROR_LOG), d_, s_, errno);
        return 1;
      }
      HANDLE s, d;
      for (;;)
      {
        s= CreateFile(s_, GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      my_win_file_secattr(), OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s != INVALID_HANDLE_VALUE)
          break;
        switch (GetLastError()) {
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
          std::this_thread::sleep_for(std::chrono::seconds(1));
          continue;
        }
        my_osmaperr(GetLastError());
        my_error(ER_FILE_NOT_FOUND, MYF(ME_ERROR_LOG), s_, errno);
        return 1;
      }
      d= CreateFile(d_, GENERIC_WRITE, 0, my_win_file_secattr(),
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (d == INVALID_HANDLE_VALUE)
      {
      error_return:
        my_osmaperr(GetLastError());
        std::ignore= CloseHandle(s);
        my_error(ER_ERROR_ON_RENAME, MYF(ME_ERROR_LOG), s_, d_, errno);
        return 1;
      }
#else
      std::string dst;
      log_sys.append_archive_name(dst, hl);
      const char *const d_{dst.c_str()};
      int d{-1};
      int err= ER_FILE_NOT_FOUND;
      int s= openat(target.fd, d_, O_RDONLY);
      if (s == -1)
      {
      error_return:
        my_error(err, MYF(ME_ERROR_LOG), d_, errno);
        if (s != -1)
          std::ignore= close(s);
        return 1;
      }
      err= ER_CANT_DELETE_FILE;
      if (unlinkat(target.fd, d_, 0))
        goto error_return;
      err= ER_CANT_CREATE_FILE;
      d= openat(target.fd, d_, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0666);
      if (d < 0)
        goto error_return;
#endif
      const uint64_t end{log_sys.START_OFFSET + last_lsn - hl};
      /* First, extend the file to a valid size. */
#ifdef _WIN32
      int f;
      {
        LARGE_INTEGER li;
        li.QuadPart= std::max<uint64_t>(log_sys.FILE_SIZE_MIN,
                                        (end + 4095) & ~4095ULL);
        f= !SetFilePointerEx(d, li, nullptr, FILE_BEGIN) || !SetEndOfFile(d);
      }
#else
      int f=
        ftruncate(d, std::max<off_t>(log_sys.FILE_SIZE_MIN,
                                     (end + 4095) & ~4095LL));
#endif
      if (!f &&
          !(f= copy_file(s, d, log_sys.START_OFFSET +
                         (hl == first_lsn) * (checkpoint - hl), end)) &&
          hl == first_lsn)
      {
        uint64_t cp_buf[8]{};
        write_checkpoint_buf(cp_buf,
                             checkpoint_end_lsn - hl + log_sys.START_OFFSET);
        f= write_checkpoint(d, cp_buf);
      }
      if (IF_WIN(!CloseHandle(d), close(d)) | f)
        goto error_return;
      std::ignore= IF_WIN(CloseHandle(s), close(s));
#ifdef _WIN32
      if (!DeleteFile(s_))
      {
        my_osmaperr(GetLastError());
        my_error(ER_CANT_DELETE_FILE, MYF(ME_ERROR_LOG), s_, errno);
        return 1;
      }
#endif
      return 0;
    }

    /**
       Finish a backup.
       @param target  backup target
       @param sink    backup worker context
       @return error code
       @retval 0 on success
    */
    int cleanup(const backup_target &target, const backup_sink &sink) noexcept
    {
      const lsn_t hl{last_hardlink.load(std::memory_order_relaxed)};
      if (hl == LSN_MAX)
        return 0;
      log_sys.latch.rd_lock();
      const lsn_t current_first_lsn{log_sys.get_first_lsn()};
      log_sys.latch.rd_unlock();
      if (hl == current_first_lsn)
      {
        ut_ad(sink.stream == sink.NO_STREAM);
        if (int fail= de_hardlink(target, hl))
          return fail;
      }
      return write_config(target, sink);
    }
  };

  /** pointer to backup context, or nullptr if no backup is active */
  context *ctx;

  /** the original innodb_log_file_size, or 0 */
  uint64_t old_size;

  /** collection of files and sizes to be copied */
  std::vector<uint64_t> queue;
  /** collection of completed log archive files to be
  hard-linked, copied, or moved */
  std::vector<lsn_t> logs;

public:
  /**
     Start of BACKUP SERVER: collect all files to be backed up
     @param thd     current session
     @return ctx
     @retval -1 on failure
  */
  void *init(THD *thd) noexcept
  {
    log_sys.latch.wr_lock();
    ut_ad(!ctx);
    ut_ad(queue.empty());
    if (!logs.empty())
    {
      /* A new BACKUP SERVER is being invoked before a previous one
      had been fully finalized. Clean up any log files. */
      delete_logs();
      logs.clear();
    }

    const bool fail{log_sys.backup_start(&old_size, thd)};

    if (!fail)
    {
      lsn_t start_end;
      const lsn_t start=
#if 1 /* TODO: for incremental backup, allow the start to be specified */
        log_sys.get_latest_checkpoint(start_end);
#else
        log_sys.archived_checkpoint;
      start_end= log_sys.archived_lsn;
#endif
      ctx= new context{
        log_sys.get_first_lsn(), LSN_MAX, LSN_MAX, log_sys.file_size,
        start, start_end, !old_size, false, 0
      };

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
          queue.emplace_back
            (uint64_t{std::min(space.size, space.free_limit)} << 32 |
             space.id);
      mysql_mutex_unlock(&fil_system.mutex);
    }
    log_sys.latch.wr_unlock();
    DEBUG_SYNC(thd, "innodb_backup_start");
    return fail ? reinterpret_cast<void*>(-1) : ctx;
  }

  /**
     Process a file that was collected at init().
     This may be invoked from multiple concurrent threads.
     @param target  backup target
     @param phase   backup phase
     @param sink    backup worker context
     @return number of files remaining, or negative on error
     @retval 0 on completion
  */
  int step(const backup_target &target, backup_phase phase,
           const backup_sink &sink) noexcept
  {
    uint64_t id_limit{0};
    lsn_t lsn{0};
    log_sys.latch.wr_lock();
    const lsn_t first{log_sys.get_first_lsn()};
    ut_ad(sink.ha_data);
    ut_ad(ctx ? ctx == sink.ha_data
          : phase == BACKUP_PHASE_FINISH || phase == BACKUP_PHASE_NO_COMMIT);
    ut_ad(static_cast<context*>(sink.ha_data)->last_lsn == LSN_MAX
          ? phase == BACKUP_PHASE_START : !ctx);
    size_t size{queue.size()};
    ut_ad(!size || phase == BACKUP_PHASE_START);
    if (!logs.empty())
    {
      lsn= logs.back();
      logs.pop_back();
      if (!size)
        size= logs.size();
    }
    else if (size)
    {
      ut_ad(phase == BACKUP_PHASE_START);
      size--;
      id_limit= queue.back();
      queue.pop_back();
    }
    log_sys.latch.wr_unlock();

    if (lsn)
    {
      if (UNIV_UNLIKELY(lsn > first))
        /* Wait for checkpoint_complete(). */
        buf_flush_sync_batch(lsn, true);
      if (replicate(lsn, target, sink, lsn < first))
        return -1;
    }
    else if (!id_limit);
    else if (fil_space_t *space= fil_space_t::get(uint32_t(id_limit)))
    {
      int res= -1;
      uint32_t start{0}, limit{uint32_t(id_limit >> 32)};
#ifdef _WIN32
      if (sink.stream == sink.NO_STREAM)
      {
        for (fil_node_t *node= UT_LIST_GET_FIRST(space->chain);;)
        {
          if ((res= backup(target.path, node, start, limit)))
            break;
          fil_node_t *next= UT_LIST_GET_NEXT(chain, node);
          if (!next)
            break;
          const uint32_t size{node->size};
          start+= size;
          if (limit >= size)
            limit-= size;
          else
            limit= 0;
          node= next;
        }
      }
      else
      {
        for (fil_node_t *node= UT_LIST_GET_FIRST(space->chain);;)
        {
          if ((res= stream(sink.stream, node, start, limit)))
            break;
          fil_node_t *next= UT_LIST_GET_NEXT(chain, node);
          if (!next)
            break;
          const uint32_t size{node->size};
          start+= size;
          if (limit >= size)
            limit-= size;
          else
            limit= 0;
          node= next;
        }
      }
#else
      int fd;
      int (*method)(int, fil_node_t *, uint32_t, uint32_t);
      if (sink.stream == sink.NO_STREAM)
      {
        fd= target.fd;
        method= backup;
      }
      else
      {
        fd= sink.stream;
        method= stream;
      }
      for (fil_node_t *node= UT_LIST_GET_FIRST(space->chain);;)
      {
        if ((res= (*method)(fd, node, start, limit)))
          break;
        fil_node_t *next= UT_LIST_GET_NEXT(chain, node);
        if (!next)
          break;
        const uint32_t size{node->size};
        start+= size;
        if (limit >= size)
          limit-= size;
        else
          limit= 0;
        node= next;
      }
#endif
      space->release();
      if (res)
        return res;
    }

    size= std::min(size_t{std::numeric_limits<int>::max()}, size);
    return int(size);
  }

  /**
     Determine the logical time of the backup snapshot.
  */
  void commit() noexcept
  {
    log_sys.latch.wr_lock();
    ut_ad(queue.empty());
    ut_ad(ctx);
    ut_ad(ctx->last_lsn == LSN_MAX);
    const lsn_t last_lsn{log_sys.get_lsn()};
    lsn_t lsn{log_sys.get_first_lsn()};
    if (logs.empty() || logs.back() != lsn)
    {
      /* Schedule the remaining log for copying */
      logs.emplace_back(lsn);
      const lsn_t next_lsn{lsn + log_sys.capacity()};
      if (next_lsn < last_lsn)
        logs.emplace_back(lsn= next_lsn);
    }
    ctx->max_first_lsn= lsn;
    ctx->last_lsn= last_lsn;
    ctx= nullptr; /* unsubscribe to checkpoint_complete() */
    log_sys.latch.wr_unlock();
  }

  /**
     Finish copying or finalize the backup.
     @param thd     current session
     @param phase   backup phase
     @param sink    backup worker context
     @return error code
     @retval 0 on success
  */
  int end(THD *thd, backup_phase phase, const backup_sink &sink) noexcept
  {
    context *const ctx{static_cast<context*>(sink.ha_data)};
    if (!ctx /* InnoDB_backup::init() must have failed */ ||
        ctx->cleaned_up /* aborting after phase=BACKUP_PHASE_NO_COMMIT */)
      return 0;
    ctx->cleaned_up= true;
    if (phase == BACKUP_PHASE_ABORT)
      ctx->last_hardlink.store(LSN_MAX, std::memory_order_relaxed);
    log_sys.latch.wr_lock();
    ut_ad(!this->ctx || this->ctx == ctx);
    this->ctx= nullptr; /* fini() will delete the object */
    ut_ad(!log_sys.resize_in_progress());
    ut_ad(log_sys.archive);
    queue.clear();
    int fail{0};
    if (!old_size)
      logs.clear();
    else
    {
      delete_logs();
      logs.clear();
      log_sys.latch.wr_unlock();
      fail= log_sys.backup_stop_archiving(thd);
      log_sys.latch.wr_lock();
    }

    log_sys.backup_stop(old_size, thd);
    return fail;
  }

  /**
     Clean up after end().
     @param target  backup target
     @param sink    backup worker context
     @return error code
     @retval 0 on success
  */
  int fini(const backup_target &target, const backup_sink &sink) noexcept
  {
    if (context *ctx{static_cast<context*>(sink.ha_data)})
    {
      ut_ad(ctx != this->ctx);
      int fail{ctx->cleanup(target, sink)};
      delete ctx;
      return fail;
    }
    return 0;
  }

  /**
     Complete the first checkpoint in a new archive log file.
  */
  void checkpoint_complete() noexcept
  {
    ut_ad(log_sys.latch_have_wr());
    if (ctx)
      logs.emplace_back(log_sys.get_first_lsn() - log_sys.capacity());
  }

private:
  /** Safely start backing up a tablespace file
  @param end  last page to copy */
  static void backup_batch_start(fil_space_t *space, uint32_t end) noexcept
  {
    if (space->backup_start(end))
      os_aio_wait_until_no_pending_writes(false);
  }
  /* Stop backing up a tablespace */
  static void backup_batch_stop(fil_space_t *space) noexcept
  { space->backup_stop(); }

  /**
     Delete unnecessary logs that had been created for backup.
  */
  void delete_logs() noexcept
  {
    ut_ad(log_sys.latch_have_wr());
    ut_ad(old_size);
    const lsn_t first_lsn{log_sys.get_first_lsn()};
    for (const lsn_t lsn : logs)
      if (lsn != first_lsn)
        IF_WIN(DeleteFile,unlink)(log_sys.get_archive_path(lsn).c_str());
  }

  /**
     Back up a persistent InnoDB data file.
     @param target backup target directory
     @param node   InnoDB data file
     @param start  the page number at the start of the file
     @param limit  the size of the file at the start of backup
     @return error code (non-positive)
     @retval 0 on success
  */
  static int backup(IF_WIN(const char *,int) target, fil_node_t *node,
                    uint32_t start, uint32_t limit) noexcept
  {
    for (bool tried_mkdir{false};;)
    {
#ifdef _WIN32
      std::string path{target};
      path.push_back('/');
      path.append(node->name);
      HANDLE f= CreateFile(path.c_str(), GENERIC_WRITE, 0,
                           my_win_file_secattr(), CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE)
      {
        unsigned long err= GetLastError();
        if (err == ERROR_PATH_NOT_FOUND && !tried_mkdir &&
            node->space->id && !srv_is_undo_tablespace(node->space->id))
        {
          tried_mkdir= true;
          path.erase(path.rfind('/'));
          if (CreateDirectory(path.c_str(),
                              my_dir_security_attributes.lpSecurityDescriptor
                              ? &my_dir_security_attributes : nullptr) ||
              (err= GetLastError()) == ERROR_ALREADY_EXISTS)
            continue;
        }

        my_osmaperr(err);
        goto fail;
      }
#else
      int f;
# ifdef __APPLE__
      /* aio::synchronous() in another thread may concurrently invoke
      pwrite(2) on node->handle. We assume that both pwrite(2) and
      fclonefileat(2) are atomic with respect to each other. Should
      this assumption be invalid, some data files in the backup may be
      corrupted. This corruption can be fixed by either removing this
      special handling, or by implementing file-level locking. */
      f= fclonefileat(node->handle, target, node->name, 0);
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
#endif
      const uint32_t page_size{node->space->physical_size()};
      int err{0};

      if (node->size > limit)
      {
        /* Expand the file to its logical size. */
#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart= uint64_t{node->size} * page_size;
        err= !SetFilePointerEx(f, li, nullptr, FILE_BEGIN) || !SetEndOfFile(f);
#else
        err= ftruncate(f, uint64_t{node->size} * page_size);
#endif
        if (err)
          limit= 0;
      }
      else if (node->size < limit)
        limit= node->size;

      for (uint32_t page{0}; page < limit; )
      {
        {
          const uint32_t end{start + fil_space_t::BACKUP_BATCH_SIZE};
          backup_batch_start(node->space, end);
          start= end;
        }
        uint32_t last{std::min(limit, page + fil_space_t::BACKUP_BATCH_SIZE)};
        /* TODO: avoid copying freed page ranges, or pages that were
        allocated after the backup started */
        err= copy_file(node->handle, f, uint64_t{page} * page_size,
                       uint64_t{last} * page_size);
        page= last;
        backup_batch_stop(node->space);
        if (err)
          break;
      }

      if (IF_WIN(!CloseHandle(f), close(f)) | err)
        goto fail;
      break;
    }
    return 0;
  fail:
    my_error(ER_CANT_CREATE_FILE, MYF(0), node->name, errno);
    return -1;
  }

  /**
     Stream a persistent InnoDB data file.
     @param stream backup target stream
     @param node   InnoDB data file
     @param start  the page number at the start of the file
     @param limit  the size of the file at the start of backup
     @return error code (non-positive)
     @retval 0 on success
  */
  static int stream(IF_WIN(HANDLE,int) stream, fil_node_t *node,
                    uint32_t start, uint32_t limit) noexcept
  {
    const uint32_t file_size{node->size},
      page_size{node->space->physical_size()};
    backup_chunk chunk[2]{
      {0, uint64_t{limit} * page_size},
      {uint64_t{file_size} * page_size, 0}
    };
    if (file_size < limit)
    {
      limit= file_size;
      chunk[0].length= chunk[1].offset;
    }
    int err= backup_stream_start(stream, node->name, 0644,
                                 chunk[0].length,
                                 chunk, (file_size > limit) * 2);
    if (err)
      limit= 0;

    for (uint32_t page{0}; page < limit; )
    {
      {
        const uint32_t end{start + fil_space_t::BACKUP_BATCH_SIZE};
        backup_batch_start(node->space, end);
        start= end;
      }
      uint32_t last{std::min(limit, page + fil_space_t::BACKUP_BATCH_SIZE)};
      /* TODO: avoid copying freed page ranges, or pages that were
      allocated after the backup started */
      err= backup_stream_append(node->handle, stream,
                                uint64_t{page} * page_size,
                                uint64_t{last} * page_size);
      page= last;
      backup_batch_stop(node->space);
      if (err)
        break;
    }

    if (err)
      my_error(ER_IO_WRITE_ERROR, MYF(0), errno, strerror(errno),
               "BACKUP SERVER");
    return err;
  }

private:
  /**
     Initialize a checkpoint header buffer pointing to the start of the backup.
     @param buf   checkpoint buffer
     @param c     offset of the FILE_CHECKPOINT mini-transaction
  */
  static void write_checkpoint_buf(uint64_t *buf, uint64_t c) noexcept
  {
    ut_ad(c >= log_sys.START_OFFSET);
    if (log_sys.is_encrypted())
      log_crypt_write_header(reinterpret_cast<byte*>(buf), true);
    buf[4 * log_sys.is_encrypted()]= my_htobe64(c);
  }

  /** Write a checkpoint header pointing to the start of the backup.
  @param dst       target file
  @param buf       checkpoint header
  @return error code
  @retval 0 on success */
  static int write_checkpoint(IF_WIN(HANDLE,int) dst, const void *buf) noexcept
  {
#ifdef _WIN32
    using tpool::pwrite;
#endif
    for (ssize_t o= 0, count= 64; count;)
    {
      ssize_t ret=
        pwrite(dst, static_cast<const char*>(buf) + o, count, o);
      if (ret <= 0 || ret > count)
        return -1;
      o+= ret;
      count-= ret;
    }
    return 0;
  }

public:
  /** Maximum length of the configuration string */
  static constexpr size_t CONFIG_SIZE=
    sizeof "[server]\n# checkpoint=" +
    sizeof "innodb_log_recovery_start=" +
    sizeof "innodb_log_recovery_target=\n" + 45 * 3;

  /** Write the configuration parameters for restoring the backup
  @param config  buffer for configuration string
  @param ctx     backup context
  @return size of the configuration string */
  static size_t write_config_buf(char *config, const context &ctx)
    noexcept
  {
    ut_ad(ctx.last_lsn != LSN_MAX);
    return size_t(snprintf(config, CONFIG_SIZE,
                           "[server]\n# checkpoint=" LSN_PF "\n"
                           "innodb_log_recovery_start=" LSN_PF "\n"
                           "innodb_log_recovery_target=" LSN_PF "\n",
                           ctx.checkpoint, ctx.checkpoint_end_lsn,
                           ctx.last_lsn));
  }

  /** Write the configuration parameters for restoring the backup
  @param target  backup target
  @param sink    backup worker context
  @param ctx     backup context
  @return error code (non-positive)
  @retval 0   on success */
  static int write_config(const backup_target &target,
                          const backup_sink &sink) noexcept
  {
    char config[CONFIG_SIZE];
    const size_t size
      {write_config_buf(config, *static_cast<context*>(sink.ha_data))};
    return sink.stream == sink.NO_STREAM
      ? backup_config_append(IF_WIN(target.path, target.fd), config, size)
      : backup_stream_config(sink.stream, config, size);
  }

  /**
     Hard-link (copy) or rename (move) or stream an archive log file.
     @param lsn       The first LSN in the file
     @param target    backup target
     @param sink      backup context
     @param old       lsn < log_sys.get_first_lsn()
     @return error code
     @retval 0 on success
  */
  static int replicate(lsn_t lsn,
                       const backup_target &target,
                       const backup_sink &sink, bool old) noexcept
  {
    ut_ad(log_get_lsn() >= lsn);
    const std::string p{log_sys.get_archive_path(lsn)};
    const char *const path= p.c_str(), *basename= strrchr(path, '/');
    if (!basename)
      basename= path;
    else
      basename++;
    context &ctx{*static_cast<context*>(sink.ha_data)};
    const bool move{old && !ctx.archived};
    uint64_t cp_buf[8]{};
#ifdef _WIN32
    ut_ad(!target.path == (sink.stream != sink.NO_STREAM));
    std::string b;
    const char *destname= nullptr;
    if (!target.path)
      goto send_file;
    b= target.path;
    b.push_back('/');
    b.append(basename);
    destname= b.c_str();
    unsigned long err;
    if (move)
    {
      if (!MoveFileEx(path, destname, MOVEFILE_COPY_ALLOWED))
      {
      fail:
        err= GetLastError();
      got_err:
        my_osmaperr(err);
        if (target.path)
          my_error(ER_ERROR_ON_RENAME, MYF(ME_ERROR_LOG), path, basename,
                   errno);
        else
          my_error(ER_IO_WRITE_ERROR, MYF(ME_ERROR_LOG),
                   errno, strerror(errno), "BACKUP SERVER");
        return -1;
      }

      if (lsn < ctx.checkpoint)
      {
        if (!SetFileAttributes(destname, FILE_ATTRIBUTE_NORMAL))
          goto fail;
        HANDLE dh= CreateFile(destname, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dh == INVALID_HANDLE_VALUE)
          goto fail;
        if (os_file_set_sparse_win32(dh))
          std::ignore=
            os_file_punch_hole(dh, 0, log_sys.START_OFFSET +
                               ((ctx.checkpoint - lsn) & ~4095ULL));
        write_checkpoint_buf(cp_buf, ctx.checkpoint_end_lsn - lsn +
                             log_sys.START_OFFSET);
        int fail= write_checkpoint(dh, cp_buf);
        std::ignore= CloseHandle(dh);
        if (fail)
          goto fail;
      }
      return 0;
    }
    else if (CreateHardLink(destname, path, nullptr))
    {
      ctx.note_hardlink(lsn);
      return 0;
    }

    if ((err= GetLastError()) != ERROR_NOT_SAME_DEVICE)
      goto got_err;
    /* Hard-linking failed. Try copying with the final name. */
    if (target.path)
    {
      b= target.path;
      b.push_back('/');
      b.append(basename);
      destname= b.c_str();

      if (lsn >= ctx.checkpoint && lsn < ctx.max_first_lsn)
      {
        /* Copy a middle log file entirely. */
        if (CopyFileEx(path, basename, nullptr, nullptr, nullptr,
                       COPY_FILE_NO_BUFFERING))
          return 0;
        goto fail;
      }
    }

  send_file:
    HANDLE src;
    for (;;)
    {
      src= CreateFile(path, GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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
      goto fail;
    }
    HANDLE dst{sink.stream};
    if (dst == INVALID_HANDLE_VALUE)
    {
      dst= CreateFile(destname, GENERIC_WRITE, 0, my_win_file_secattr(),
                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (dst == INVALID_HANDLE_VALUE)
      {
        std::ignore= CloseHandle(src);
        goto fail;
      }
    }
#else
    if (sink.stream != sink.NO_STREAM);
    else if (move
             ? !renameat(AT_FDCWD, path, target.fd, basename)
             : !linkat(AT_FDCWD, path, target.fd, basename, AT_SYMLINK_FOLLOW))
    {
      if (!move)
        ctx.note_hardlink(lsn);
# ifdef __linux__
      else if (lsn != ctx.first_lsn);
      else if (off_t garbage= (ctx.checkpoint - lsn) & ~4095ULL)
        /* Best effort to punch a hole to free up some garbage in
        the first file. We do not care about failures. */
        if (!fchmodat(target.fd, basename, 0644, 0))
        {
          int dst= openat(target.fd, basename, O_RDWR);
          if (dst >= 0)
            fallocate(dst, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      log_sys.START_OFFSET, garbage);
          close(dst);
          std::ignore= fchmodat(target.fd, basename, 0444, 0);
        }
# endif
      return 0;
    }
    else if (errno != EXDEV)
    {
    fail:
      if (sink.stream == sink.NO_STREAM)
        my_error(ER_ERROR_ON_RENAME, MYF(ME_ERROR_LOG), path, basename, errno);
      else
        my_error(ER_IO_WRITE_ERROR, MYF(ME_ERROR_LOG), errno, strerror(errno),
                 "BACKUP SERVER");
      return -1;
    }

    const int src{open(path, O_RDONLY)};
    if (src < 0)
      goto fail;
    if (move && unlink(path))
    {
    close_and_fail:
      std::ignore= close(src);
      goto fail;
    }
    int dst{sink.stream};
    if (dst < 0)
    {
      dst= openat(target.fd, basename,
                  O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0666);
      if (dst < 0)
        goto close_and_fail;
    }
    int err;
#endif
    backup_chunk chunks[3], *chunk{chunks};
    *chunk++= {log_sys.START_OFFSET, ctx.last_lsn - lsn};
    if (lsn < ctx.checkpoint)
    {
      /* Copy the necessary part of the first log file. */
      ut_ad(lsn == ctx.first_lsn);
      write_checkpoint_buf(cp_buf, ctx.checkpoint_end_lsn - lsn +
                           log_sys.START_OFFSET);
      chunk[-1]= {0, 512};
      const lsn_t end=
        std::min(ctx.last_lsn, lsn + ctx.first_size - log_sys.START_OFFSET);
      *chunk++=
        {log_sys.START_OFFSET + ctx.checkpoint - lsn, end - ctx.checkpoint};
      chunk->offset= end - lsn;
      goto pad_size;
    }
    else if (lsn < ctx.max_first_lsn)
    {
      /* Copy a middle log file entirely. */
#ifdef _WIN32
      ut_ad(dst == sink.stream);
      chunk->offset= os_file_get_size(src);
#else
      if (dst != sink.stream)
      {
        err= copy_entire_file(src, dst);
        goto close_dst;
      }
      chunk->offset= uint64_t(lseek(src, 0, SEEK_END));
#endif
      /* Omit the checkpoint header from the stream. */
      chunk[-1].length= chunk->offset - log_sys.START_OFFSET;
      goto stream_file;
    }
    else
    {
      ut_ad(ctx.max_first_lsn == lsn);
      ut_ad(ctx.last_lsn > lsn);
      ut_ad(ctx.last_lsn != LSN_MAX);
      ut_ad(chunk[-1].length == ctx.last_lsn - lsn);
      chunk->offset= chunk[-1].length;
    pad_size:
      /* Set the logical size of the file. */
      chunk->offset=
        std::max<uint64_t>(log_sys.FILE_SIZE_MIN,
                           (chunk->offset + (log_sys.START_OFFSET + 4095)) &
                           ~4095ULL);
    }

    if (dst == sink.stream)
    {
    stream_file:
      chunk++->length= 0;
      const backup_chunk &end{chunk[-2]};
      ut_ad(chunk - chunks == 2 || chunk - chunks == 3);
      const size_t cp_size{(size_t(chunk - chunks) & 1) << 9};
      err= backup_stream_start(dst, basename,
                               0444 | int{lsn == ctx.max_first_lsn} << 7,
                               end.length + cp_size, chunks, chunk - chunks);
      if (!err && cp_size)
        err= backup_stream_write(dst, cp_buf, sizeof cp_buf) ||
          backup_stream_write(dst, field_ref_zero, cp_size - sizeof cp_buf);
      if (!err)
      {
        err= backup_stream_append_async(src, dst, end.offset,
                                        end.offset + end.length);
        if (err);
        else if (size_t pad= size_t(end.length) & 511)
          err= backup_stream_write(dst, field_ref_zero, 512 - pad);
      }
    }
    else
    {
      /* First, extend the file to a valid size. */
#ifdef _WIN32
      LARGE_INTEGER li;
      li.QuadPart= chunk->offset;
      err= !SetFilePointerEx(dst, li, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(dst) ||
#else
      err= ftruncate(dst, chunk->offset) ||
#endif
        copy_file(src, dst, chunk[-1].offset, chunk[-1].offset +
                  chunk[-1].length) ||
        (lsn < ctx.checkpoint && write_checkpoint(dst, cp_buf));
#ifdef _WIN32
      err|= !CloseHandle(dst);
#else
    close_dst:
      err|= close(dst);
#endif
    }

    if (err | IF_WIN(!CloseHandle(src), close(src)))
      goto fail;

    return 0;
  }
};

/** The backup context; protected by log_sys.latch */
static InnoDB_backup innodb_backup;
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
  const bool fail{set_archive(true, thd, true)};
  latch.wr_lock();
  if (!fail)
  {
    *old_size= old_file_size;
    return false;
  }
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

void *innodb_backup_start(THD *thd, const backup_target *,
                          backup_phase phase, const backup_sink *sink) noexcept
{
  switch (phase) {
  case BACKUP_PHASE_PREPARE_START:
    if (!fil_system.have_all_spaces)
    {
      /* To speed up startup, InnoDB does not normally open all
      tablespace files that are pointed to by SYS_TABLES.
      InnoDB_backup::init() assumes that the information of all
      tablespaces is available, including files that had been created
      before the server was started, and never opened in the course of
      the current server execution. */
      dict_load_tablespaces(nullptr, true);
      ut_ad(fil_system.have_all_spaces);
    }
    return 0;
  case BACKUP_PHASE_START:
    return innodb_backup.init(thd);
  case BACKUP_PHASE_NO_COMMIT:
    innodb_backup.commit();
    /* fall through */
  default:
    return sink->ha_data;
  }
}

int innodb_backup_step(THD *, const backup_target *target,
                       backup_phase phase, const backup_sink *sink) noexcept
{
  switch (phase) {
  case BACKUP_PHASE_START:
  case BACKUP_PHASE_NO_COMMIT:
  case BACKUP_PHASE_FINISH:
    return innodb_backup.step(*target, phase, *sink);
  default:
    return 0;
  }
}

int innodb_backup_end(THD *thd, const backup_target *target,
                      backup_phase phase, const backup_sink *sink) noexcept
{
  switch (phase) {
  default:
    return 0;
  case BACKUP_PHASE_FINISH:
    return innodb_backup.fini(*target, *sink);
  case BACKUP_PHASE_NO_COMMIT:
  case BACKUP_PHASE_ABORT:
    return innodb_backup.end(thd, phase, *sink);
  }
}

void innodb_backup_checkpoint() noexcept
{
  innodb_backup.checkpoint_complete();
}
