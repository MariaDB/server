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

/** Try to write-fix a block.
@param s  expected state()
@return new s (right before potentially setting the write-fix);
a write-fix was acquired if !is_freed(s) && !is_io_fixed(s) holds */
inline uint32_t buf_page_t::write_fix_try(uint32_t s) noexcept
{
  /* The calling thread must hold a fix() */
  ut_ad(s > FREED);
  /*
    set_freed(), set_reinit() or flush() may run concurrently.
    compare_exchange_strong() ensures that the write-fix was set by us.
  */
  while (!is_freed(s) && !is_io_fixed(s) &&
         !zip.fix.compare_exchange_strong(s, s + (WRITE_FIX - UNFIXED),
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed));
  return s;
}

/** Try to undo a successful write_fix_try(). */
inline void buf_page_t::write_unfix_try() noexcept
{
  uint32_t s{state()};
  /* set_freed() or set_reinit() may clear the write-fix before or
  during this loop. We must use a compare-and-exchange loop. */
  while (is_write_fixed(s) &&
         !zip.fix.compare_exchange_weak(s, s - (WRITE_FIX - UNFIXED),
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed));
}

/**
   Ensure that there are no page writes in progress.
   @param end        array of fil_space_t::BACKUP_BATCH_SIZE block descriptors
   @param space_id   tablespace identifier
   @param end_page   last page number that is being copied
   @return pointer to the new end of the array, of write-fixed blocks
*/
static buf_page_t **innodb_backup_batch_wait(buf_page_t **end,
                                             uint32_t space_id,
                                             uint32_t end_page) noexcept
{
  const page_id_t start
    {space_id, end_page & ~(fil_space_t::BACKUP_BATCH_SIZE - 1)};
  ut_ad(end_page - 1 > start.page_no());
  for (page_id_t id{space_id, end_page}; id != start; --id)
  {
    auto &chain= buf_pool.page_hash.cell_get(id.fold());
    page_hash_latch &hash_lock{buf_pool.page_hash.lock_get(chain)};
    hash_lock.lock_shared();
    buf_page_t *const b= *end= buf_pool.page_hash.get(id, chain);
    if (b && b->oldest_modification_acquire() > 2)
    {
      uint32_t state{b->fix()};
      /* The above buffer-fix froze b in buf_pool.page_hash */
      hash_lock.unlock_shared();
      /*
        We found out that this page was dirty, and eventually such pages
        must be either freed or written back to the file system.

        The lock-free buf_page_t::write_fix_try() aims to block
        concurrent asynchronous buf_page_t::flush(). It will not
        block any access to the page in the buffer pool; we are
        not holding any page latch.
      */
      state= b->write_fix_try(state + 1);
      if (UNIV_LIKELY(!b->is_io_fixed(state)))
      {
        if (UNIV_LIKELY(!b->is_freed(state)))
        {
          /* Schedule a call of b->write_unfix_try() and b->unfix(). */
          end++;
          continue;
        }
        /*
          Freed blocks will not be written back to the file system.
          As noted in fil_space_t::flush_freed(), we do allow
          concurrent FALLOC_FL_PUNCH_HOLE of PAGE_COMPRESSED pages
          and NUL writes by immediate_scrub_data_uncompressed=ON
          while the data is being backed up. This should be fine,
          because those operations are only overwriting freed
          (garbage) data.
        */
      }
      else if (b->is_write_fixed(state))
      {
        /* Wait for buf_page_t::write_complete() */
        b->lock.u_lock();
        ut_ad(!b->is_io_fixed());
        b->lock.u_unlock();
      }

      /*
        Any subsequent write to this page will be held up by
        fil_space_t::backup_page_end() until
        fil_space_t::backup_stop() is invoked by
        InnoDB_backup::backup_batch_stop().
      */
      b->unfix();
    }
    else
      hash_lock.unlock_shared();
  }
  return end;
}

namespace
{
/** Backup state; protected by log_sys.latch */
class InnoDB_backup
{
public:
  InnoDB_backup() { mutex.init(); }
  ~InnoDB_backup() { mutex.destroy(); }

private:
  /** Backup context */
  struct context
  {
    /** Start LSN of the first backed up log file */
    const lsn_t first_lsn;
    /** Start LSN of the last log file, or LSN_MAX if not determined yet */
    lsn_t max_first_lsn;
    /** Final LSN of the backup, or LSN_MAX if not determined yet */
    lsn_t last_lsn;
    /** size of the first log file */
    const uint64_t first_size;
    /** Checkpoint at the start of the backup */
    const lsn_t checkpoint;
    /** Log record pointing to the checkpoint */
    const lsn_t checkpoint_end_lsn;
    /** the original state of innodb_log_archive before/after backup */
    const bool archived;
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
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                      nullptr);
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
      if (!f)
      {
        const uint64_t begin= log_sys.START_OFFSET +
          (hl == first_lsn) * (checkpoint - hl);
#ifndef _WIN32
        std::ignore= posix_fadvise(s, begin, end - begin,
                                   POSIX_FADV_SEQUENTIAL);
#endif
#ifdef copy_file_shortcut
        if (1 == (f= copy_file_shortcut(s, d, begin, end)))
#endif
        f= copy_file(s, d, begin, end);
#ifndef _WIN32
        std::ignore= posix_fadvise(s, 0, 0, POSIX_FADV_DONTNEED);
#endif
        if (!f && hl == first_lsn)
        {
          uint64_t cp_buf[8]{};
          write_checkpoint_buf(cp_buf,
                               checkpoint_end_lsn - hl + log_sys.START_OFFSET);
          f= write_checkpoint(d, cp_buf);
        }
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
        sql_print_information("de-hardlink");
        if (int fail= de_hardlink(target, hl))
          return fail;
        sql_print_information("de-hardlinked");
      }
      return write_config(target, sink);
    }
  };

  /** pointer to backup context, or nullptr if no backup is active */
  context *ctx;

  /** the original innodb_log_file_size, or 0 */
  uint64_t old_size;

  /** mutex protecting queue, non_log */
  srw_mutex mutex;
  /** collection of files and sizes, followed by any log files to be copied */
  std::vector<uint64_t> queue;
  /** number of non-log files at the start of the queue */
  size_t non_log;

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
    mutex.wr_lock();
    ut_ad(!non_log);
    if (!queue.empty())
      /* A new BACKUP SERVER is being invoked before a previous one
      had been fully finalized. Clean up any log files. */
      delete_logs();
    mutex.wr_unlock();

    if (log_sys.backup_start(&old_size, thd))
    {
      log_sys.latch.wr_unlock();
    fail:
      my_error(ER_OUT_OF_RESOURCES, MYF(ME_ERROR_LOG));
      return reinterpret_cast<void*>(-1);
    }

    mutex.wr_lock();

    try
    {
      lsn_t start_end;
      const lsn_t start=
#if 1 /* TODO: for incremental backup, allow the start to be specified */
        log_sys.get_latest_checkpoint(start_end);
#else
      log_sys.archived_checkpoint;
      start_end= log_sys.archived_lsn;
#endif
      ut_ad(start_end >= start);
      ut_ad(start >= log_sys.get_first_lsn());

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
            !space.is_being_imported() && !space.is_stopping() &&
            space.create_lsn <= start) try
        {
          /* FIXME: how to initialize create_lsn for old files, to
          have efficient incremental backup?
          fil_node_t::read_page0() cannot assign it from
          FIL_PAGE_LSN because that would not reflect the file
          creation but for example allocating or freeing a page.
          Perhaps we can read it from page 1 (change buffer bitmap)?

          The easy parts of initializing space->create_lsn are
          as follows:
          (1) In log_parse_file() when processing FILE_CREATE
          (2) In deferred_spaces.create()
          (3) In fil_ibd_create() outside recovery */
          queue.emplace_back
            (uint64_t{space.id} |
             uint64_t{std::min(space.size, space.free_limit)} << 32);
        } catch(...) { mysql_mutex_unlock(&fil_system.mutex); throw; }
      mysql_mutex_unlock(&fil_system.mutex);
      non_log= queue.size();
    }
    catch (std::bad_alloc&) {
      queue.clear();
      delete ctx;
      ctx= nullptr;
      log_sys.backup_stop(old_size, thd);
      goto fail;
    }

    mutex.wr_unlock();
    log_sys.latch.wr_unlock();
    DEBUG_SYNC(thd, "innodb_backup_start");
    return ctx;
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
    mutex.wr_lock();
    ut_ad(sink.ha_data);
    ut_ad(ctx ? ctx == sink.ha_data
          : phase == BACKUP_PHASE_FINISH || phase == BACKUP_PHASE_NO_COMMIT);
    ut_ad(static_cast<context*>(sink.ha_data)->last_lsn == LSN_MAX
          ? phase == BACKUP_PHASE_START : !ctx);
    const size_t size{queue.size()}, non_log_files{non_log};
    ut_ad(size >= non_log_files);

    if (UNIV_UNLIKELY(!size))
    {
      mutex.wr_unlock();
      return 0;
    }

    non_log-= size == non_log_files;
    id_limit= queue.back();
    queue.pop_back();
    mutex.wr_unlock();

    if (size > non_log_files)
    {
      log_sys.latch.rd_lock();
      const lsn_t first{log_sys.get_first_lsn()};
      log_sys.latch.rd_unlock();
      if (UNIV_UNLIKELY(id_limit > first))
        /* Wait for checkpoint_complete(). */
        buf_flush_sync_batch(id_limit, true);
      if (replicate(id_limit, target, sink, id_limit < first))
        return -1;
    }
    else if (fil_space_t *space= fil_space_t::get(uint32_t(id_limit)))
    {
      ut_ad(phase == BACKUP_PHASE_START);
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
# ifdef HAVE_POSIX_FALLOCATE
        if (limit & 3 && !UT_LIST_GET_NEXT(chain, node))
        {
          const uint32_t page_size{space->physical_size()};
          if ((limit * page_size) & 4095)
            /* os_file_set_size() extends ROW_FORMAT=COMPRESSED files to
            multiples of 4096 bytes. There may be up to 3 pages
            (of 1024 bytes) that have not been written out yet.
            We must cap the limit to the actual file size. */
            limit=
              std::min(limit,
                       uint32_t(os_file_get_size(node->handle) / page_size));
        }
# endif
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

    return int(std::min(size_t{std::numeric_limits<int>::max()}, size - 1));
  }

  /**
     Determine the logical time of the backup snapshot.
  */
  void commit() noexcept
  {
    log_sys.latch.wr_lock();
    ut_ad(ctx);
    ut_ad(ctx->last_lsn == LSN_MAX);
    const lsn_t last_lsn{log_sys.get_lsn()};
    lsn_t lsn{log_sys.get_first_lsn()};
    mutex.wr_lock();
    ut_ad(!non_log);
    if (queue.empty() || queue.back() != lsn)
    {
      /* Schedule the remaining log for copying */
      queue.emplace_back(lsn);
      const lsn_t next_lsn{lsn + log_sys.capacity()};
      if (next_lsn < last_lsn)
        queue.emplace_back(lsn= next_lsn);
    }
    mutex.wr_unlock();
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
    int fail{0};
    if (!old_size)
    {
      mutex.wr_lock();
      queue.clear();
      non_log= 0;
      mutex.wr_unlock();
    }
    else
    {
      log_sys.latch.wr_unlock();
      sql_print_information("stop archiving: " LSN_PF,
                            log_sys.last_checkpoint_lsn.load());
      /* FIXME: execute this at a later stage,
      after MDL_BACKUP_WAIT_COMMIT has been released!
      This may wait several seconds for some page flushing! */
      fail= log_sys.backup_stop_archiving(thd);
      sql_print_information("stopped archiving: " LSN_PF,
                            log_sys.last_checkpoint_lsn.load());
      log_sys.latch.wr_lock();
      mutex.wr_lock();
      delete_logs();
      mutex.wr_unlock();
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
    {
      const lsn_t lsn{log_sys.get_first_lsn() - log_sys.capacity()};
      mutex.wr_lock();
      queue.emplace_back(lsn);
      mutex.wr_unlock();
    }
  }

private:
  /**
     Safely start backing up a tablespace file.
     @param end      array of fil_space_t::BACKUP_BATCH_SIZE block descriptors
     @param space    tablespace that is being backed up
     @param end_page first page not to copy
     @return pointer to the new end of the array, of write-fixed blocks
  */
  static buf_page_t **backup_batch_start(buf_page_t **end,
                                         fil_space_t *space, uint32_t end_page)
    noexcept
  {
    ut_ad(end_page);
    space->backup_start(end_page);
    /* Block any writes that might be posted after checking
    fil_space_t::backup_page_end(). */
    return innodb_backup_batch_wait(end, space->id, end_page - 1);
  }
  /**
     Stop backing up a tablespace.
     @param space   tablespace
     @param begin   first write-fixed block descriptor
     @param end     end of write-fixed block descriptors
  */
  static void backup_batch_stop(fil_space_t *space,
                                buf_page_t **begin, buf_page_t **end) noexcept
  {
    space->backup_stop();
    while (begin != end)
    {
      buf_page_t *b= *begin++;
      b->write_unfix_try();
      b->unfix();
    }
  }

  /**
     Delete unnecessary logs that had been created for backup.
  */
  void delete_logs() noexcept
  {
    ut_ad(log_sys.latch_have_wr());
    ut_ad(old_size);
    ut_ad(non_log <= queue.size());

    const lsn_t first_lsn{log_sys.get_first_lsn()};
    size_t i{non_log};
    non_log= 0;
    while (i < queue.size())
    {
      const lsn_t lsn{queue[i++]};
      if (lsn != first_lsn)
        IF_WIN(DeleteFile,unlink)(log_sys.get_archive_path(lsn).c_str());
    }
    queue.clear();
  }

#ifdef copy_file_shortcut
  /**
     Try copy_file_shortcut() to back up a persistent InnoDB data file.
     @param dst          target file handle
     @param node         InnoDB data file
     @param start        the page number at the start of the file
     @param limit        the size of the file before the doublewrite buffer
     @param page_size    node->space->physical_size()
     @param final_limit  the size of the file at init(), or 0 if no dblwr
     @param blocks       descriptor array of fil_space_t::BACKUP_BATCH_SIZE
     @return error code (non-positive)
     @retval 0 on success
  */
  static int copy_file_shortcut_try(int dst, fil_node_t *node,
                                    uint32_t start, uint32_t limit,
                                    uint32_t page_size, uint32_t final_limit,
                                    buf_page_t **blocks)
    noexcept
  {
# if 0
    return 1; // work around https://github.com/rr-debugger/rr/issues/4059
# endif
    for (uint32_t page{0};;)
    {
      while (page < limit)
      {
        start+= fil_space_t::BACKUP_BATCH_SIZE;
        buf_page_t **end= backup_batch_start(blocks, node->space, start);
        const uint64_t o{uint64_t{page} * page_size};
        page= std::min(limit, page + fil_space_t::BACKUP_BATCH_SIZE);
        /* TODO: avoid copying freed page ranges, or pages that were
        allocated after the backup started */
        int err{copy_file_shortcut(node->handle, dst,
                                   o, uint64_t{page} * page_size)};
        backup_batch_stop(node->space, blocks, end);
        if (err)
          return err;
#ifndef _WIN32
        std::ignore= posix_fadvise(node->handle, o,
                                   uint64_t{page} * page_size - o,
                                   POSIX_FADV_DONTNEED);
#endif
      }

      if (final_limit != 0 && page == buf_dblwr.begin())
      {
        /* Copy the rest after the doublewrite buffer. */
        limit= final_limit;
        page+= buf_dblwr.size();
      }
      else
        return 0;
    }
  }
#endif

  /**
     Back up a persistent InnoDB data file.
     @param target backup target directory
     @param node   InnoDB data file
     @param start  the page number at the start of the file
     @param limit  the size of the file at init()
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
      if (node->size < limit)
        limit= node->size;
#ifndef _WIN32
      std::ignore= posix_fadvise(node->handle, 0, off_t(limit) * page_size,
                                 POSIX_FADV_SEQUENTIAL);
#endif
      /*
        For the system tablespace, a minimum size has been configured
        which may be larger than the currently used size. Preserve the
        original size.

        For other persistent data files, fil_node_t::read_page0()
        expects at least 4 * innodb_page_size bytes. Small
        ROW_FORMAT=COMPRESSED files may be zero-filled to this size.
      */
      const uint64_t min_size=
        std::max(uint64_t{FIL_IBD_FILE_INITIAL_SIZE} << srv_page_size_shift,
                 uint64_t{node->size} * page_size);
      if (uint64_t{limit} * page_size < min_size)
      {
        /* Expand the target file to the minimum size. */
#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart= min_size;
        err= !SetFilePointerEx(f, li, nullptr, FILE_BEGIN) || !SetEndOfFile(f);
#else
        err= ftruncate(f, min_size);
#endif
        if (err)
          limit= 0;
      }

      const uint32_t final_limit=
        node == fil_system.sys_space->chain.start &&
        buf_dblwr.begin() + buf_dblwr.size() == buf_dblwr.end() &&
        limit > buf_dblwr.end()
        ? limit : 0;
      if (final_limit)
        limit= buf_dblwr.begin();

      buf_page_t *blocks[fil_space_t::BACKUP_BATCH_SIZE];
#ifdef copy_file_shortcut
      err= copy_file_shortcut_try(f, node, start, limit,
                                  page_size, final_limit, blocks);
      if (err == 1)
#endif
      {
#ifdef copy_file_mmap
        const size_t c{size_t{final_limit ? final_limit : limit} * page_size};
        void *p= mmap(nullptr, c, PROT_READ, MAP_SHARED, node->handle, 0);
        if (p != MAP_FAILED)
        {
          for (uint32_t page{0};;)
          {
            while (page < limit)
            {
              start+= fil_space_t::BACKUP_BATCH_SIZE;
              buf_page_t **end= backup_batch_start(blocks, node->space, start);
              const uint64_t o{uint64_t{page} * page_size};
              page= std::min(limit, page + fil_space_t::BACKUP_BATCH_SIZE);
              /* TODO: avoid copying freed page ranges, or pages that were
              allocated after the backup started */
              err= copy_file_mmap(p, f, o, uint64_t{page} * page_size);
              backup_batch_stop(node->space, blocks, end);
              if (err)
                break;
#ifndef _WIN32
              std::ignore= posix_fadvise(node->handle, o,
                                         uint64_t{page} * page_size - o,
                                         POSIX_FADV_DONTNEED);
#endif
            }

            if (final_limit != 0 && !err && page == buf_dblwr.begin())
            {
              /* Copy the rest after the doublewrite buffer. */
              limit= final_limit;
              page+= buf_dblwr.size();
            }
            else
              break;
          }
          munmap(p, c);
        }
        else
#endif
          for (uint32_t page{0};;)
          {
            while (page < limit)
            {
              start+= fil_space_t::BACKUP_BATCH_SIZE;
              buf_page_t **end= backup_batch_start(blocks, node->space, start);
              const uint64_t o{uint64_t{page} * page_size};
              page= std::min(limit, page + fil_space_t::BACKUP_BATCH_SIZE);
              /* TODO: avoid copying freed page ranges, or pages that were
              allocated after the backup started */
              err= copy_file(node->handle, f, o, uint64_t{page} * page_size);
              backup_batch_stop(node->space, blocks, end);
              if (err)
                break;
#ifndef _WIN32
              std::ignore= posix_fadvise(node->handle, o,
                                         uint64_t{page} * page_size - o,
                                         POSIX_FADV_DONTNEED);
#endif
            }

            if (final_limit != 0 && !err && page == buf_dblwr.begin())
            {
              /* Copy the rest after the doublewrite buffer. */
              limit= final_limit;
              page+= buf_dblwr.size();
            }
            else
              break;
          }
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
     @param limit  the size of the file at init()
     @return error code (non-positive)
     @retval 0 on success
  */
  static int stream(IF_WIN(HANDLE,int) stream, fil_node_t *node,
                    uint32_t start, uint32_t limit) noexcept
  {
    const uint32_t page_size{node->space->physical_size()},
      file_size= std::max(std::max(limit, node->size),
                          (FIL_IBD_FILE_INITIAL_SIZE << srv_page_size_shift) /
                          page_size);
    uint64_t physical_size{uint64_t{limit} * page_size};
    backup_chunk chunk[3]{
      {0, physical_size},
      {uint64_t{file_size} * page_size, 0}
    };
    size_t n_chunk= (file_size > limit) * 2;

    if (file_size < limit)
    {
      limit= file_size;
      chunk[0].length= chunk[1].offset;
    }

#ifndef _WIN32
    std::ignore= posix_fadvise(node->handle, 0, chunk[0].length,
                               POSIX_FADV_SEQUENTIAL);
#endif

    if (node == fil_system.sys_space->chain.start &&
        buf_dblwr.begin() + buf_dblwr.size() == buf_dblwr.end() &&
        limit > buf_dblwr.end())
    {
      n_chunk= 2 + !!n_chunk;
      limit= buf_dblwr.begin();
      physical_size-= uint64_t{buf_dblwr.size()} * page_size;
      memmove(chunk + 1, chunk, 2 * sizeof *chunk);
      chunk[0].length= uint64_t{limit} * page_size;
      chunk[1].offset= uint64_t{buf_dblwr.end()} * page_size;
      chunk[1].length-= chunk[1].offset;
    }

    int err= backup_stream_start(stream, node->name, 0644,
                                 physical_size, chunk, n_chunk);
    if (err)
      limit= 0;

    uint32_t page{0};

  loop:
    while (page < limit)
    {
      buf_page_t *blocks[fil_space_t::BACKUP_BATCH_SIZE], **end= blocks;
      {
        const uint32_t end_page{start + fil_space_t::BACKUP_BATCH_SIZE};
        end= backup_batch_start(end, node->space, end_page);
        start= end_page;
      }
      uint32_t last{std::min(limit, page + fil_space_t::BACKUP_BATCH_SIZE)};
      /* TODO: avoid copying freed page ranges, or pages that were
      allocated after the backup started */
      err= backup_stream_append(node->handle, stream,
                                uint64_t{page} * page_size,
                                uint64_t{last} * page_size);
      page= last;
      backup_batch_stop(node->space, blocks, end);
      if (err)
        goto fail;
#ifndef _WIN32
      std::ignore= posix_fadvise(node->handle, uint64_t{page} * page_size,
                                 uint64_t{last - page} * page_size,
                                 POSIX_FADV_DONTNEED);
#endif
    }

    if (limit == buf_dblwr.begin() && n_chunk == 3)
    {
      /* Copy the rest after the doublewrite buffer. */
      page+= buf_dblwr.size();
      limit= page + uint32_t(chunk[1].length >> srv_page_size_shift);
      goto loop;
    }

    if (err)
    fail:
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
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                      nullptr);
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
      chunk->offset= uint64_t(lseek(src, 0, SEEK_END));
      std::ignore= posix_fadvise(src, 0, chunk->offset, POSIX_FADV_SEQUENTIAL);
      if (dst != sink.stream)
      {
        err= copy_entire_file(src, dst);
        goto close_dst;
      }
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
#ifdef _WIN32
      std::ignore= posix_fadvise(src, chunk[-1].offset, chunk[-1].length,
                                 POSIX_FADV_SEQUENTIAL);
#endif
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
  ut_ad(end_lsn >= last_checkpoint_lsn);
  backup= true;
  *old_size= 0;
  if (archive)
  {
    if (first_lsn > last_checkpoint_lsn)
    {
      /* Wait for recovery to be independent from the previous log. */
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      buf_flush_wait(end_lsn, false);
      ut_ad(first_lsn <= last_checkpoint_lsn);
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    }
    return false;
  }
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
