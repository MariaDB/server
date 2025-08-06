/*****************************************************************************

Copyright (c) 2017, 2024, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file arch/arch0log.cc
 Innodb implementation for log archive

 *******************************************************/

#include "arch0log.h"
#include "clone0clone.h"
#include "log0log.h"
#include "srv0start.h"

#include "sql_class.h"

/** Chunk size for archiving redo log */
const uint ARCH_LOG_CHUNK_SIZE = 1024 * 1024;

os_offset_t Log_Arch_Client_Ctx::get_archived_file_size() const
{
  return m_group->get_file_size();
}

void Log_Arch_Client_Ctx::get_header_size(uint &header_sz,
                                          uint &trailer_sz) const
{
  header_sz= log_t::START_OFFSET;
  trailer_sz= OS_FILE_LOG_BLOCK_SIZE;
}

int Log_Arch_Client_Ctx::start(byte *header, uint len)
{
  ut_ad(len >= log_t::START_OFFSET);

  auto err= arch_sys->log_sys()->start(m_group, m_begin_lsn, header, false);

  if (err != 0)
    return err;

  m_state= ARCH_CLIENT_STATE_STARTED;

  ib::info() << "Clone Start LOG ARCH : start LSN : "
             << m_begin_lsn;

  return 0;
}

/** Stop redo log archiving. Exact trailer length is returned as out
parameter which could be less than the redo block size.
@param[out]     trailer redo trailer. Caller must allocate buffer.
@param[in]      len     trailer length
@param[out]     offset  trailer block offset
@return error code */
int Log_Arch_Client_Ctx::stop(byte *trailer, uint32_t len, uint64_t &offset)
{
  ut_ad(m_state == ARCH_CLIENT_STATE_STARTED);
  ut_ad(trailer == nullptr || len >= OS_FILE_LOG_BLOCK_SIZE);

  auto err= arch_sys->log_sys()->stop(m_group, m_end_lsn, trailer, len);

  lsn_t start_lsn= m_group->get_begin_lsn();

  start_lsn= ut_uint64_align_down(start_lsn, OS_FILE_LOG_BLOCK_SIZE);
  lsn_t stop_lsn= ut_uint64_align_down(m_end_lsn, OS_FILE_LOG_BLOCK_SIZE);

  lsn_t file_capacity= m_group->get_file_size();

  file_capacity-= log_t::START_OFFSET;

  offset= (stop_lsn - start_lsn) % file_capacity;

  offset+= log_t::START_OFFSET;

  m_state= ARCH_CLIENT_STATE_STOPPED;

  ib::info() << "Clone Stop  LOG ARCH : end LSN : " << m_end_lsn;

  return err;
}

/** Get archived data file details
@param[in]      cbk_func        callback called for each file
@param[in]      ctx             callback function context
@return error code */
int Log_Arch_Client_Ctx::get_files(Log_Arch_Cbk *cbk_func, void *ctx)
{
  ut_ad(m_state == ARCH_CLIENT_STATE_STOPPED);

  int err= 0;

  auto size= m_group->get_file_size();

  /* Check if the archived redo log is less than one block size. In this
  case we send the data in trailer buffer. */
  auto low_begin= ut_uint64_align_down(m_begin_lsn, OS_FILE_LOG_BLOCK_SIZE);

  auto low_end= ut_uint64_align_down(m_end_lsn, OS_FILE_LOG_BLOCK_SIZE);

  if (low_begin == low_end)
  {
    err= cbk_func(nullptr, size, 0, ctx);
    return err;
  }

  /* Get the start lsn of the group */
  auto start_lsn= m_group->get_begin_lsn();
  start_lsn= ut_uint64_align_down(start_lsn, OS_FILE_LOG_BLOCK_SIZE);

  ut_ad(m_begin_lsn >= start_lsn);

  /* Calculate first file index and offset for this client. */
  lsn_t lsn_diff= m_begin_lsn - start_lsn;
  uint64_t capacity= size - log_t::START_OFFSET;

  auto idx= static_cast<uint>(lsn_diff / capacity);
  uint64_t offset= lsn_diff % capacity;

  /* Set start lsn to the beginning of file. */
  start_lsn= m_begin_lsn - offset;

  offset+= log_t::START_OFFSET;
  offset= ut_uint64_align_down(offset, OS_FILE_LOG_BLOCK_SIZE);

  /* Callback with all archive file names that holds the range of log
  data for this client. */
  while (start_lsn < m_end_lsn)
  {
    char name[MAX_ARCH_LOG_FILE_NAME_LEN];
    m_group->get_file_name(idx, name, MAX_ARCH_LOG_FILE_NAME_LEN);

    idx++;
    start_lsn+= capacity;

    /* For last file adjust the size based on end lsn. */
    if (start_lsn >= m_end_lsn)
    {
      lsn_diff=
          ut_uint64_align_up(start_lsn - m_end_lsn, OS_FILE_LOG_BLOCK_SIZE);
      size-= lsn_diff;
    }

    err= cbk_func(name, size, offset, ctx);

    if (err != 0)
      break;

    /* offset could be non-zero only for first file. */
    offset= 0;
  }

  return err;
}

/** Release archived data so that system can purge it */
void Log_Arch_Client_Ctx::release()
{
  if (m_state == ARCH_CLIENT_STATE_INIT)
    return;

  if (m_state == ARCH_CLIENT_STATE_STARTED)
  {
    uint64_t dummy_offset;
    uint32_t dummy_len= 0;

    /* This is for cleanup in error cases. */
    stop(nullptr, dummy_len, dummy_offset);
  }

  ut_ad(m_state == ARCH_CLIENT_STATE_STOPPED);

  arch_sys->log_sys()->release(m_group, false);

  m_group= nullptr;

  m_begin_lsn= LSN_MAX;
  m_end_lsn= LSN_MAX;

  m_state= ARCH_CLIENT_STATE_INIT;
}

os_offset_t Arch_Log_Sys::get_recommended_file_size() const
{
  if (!log_sys.is_opened() && !log_sys.is_mmap())
  {
    ut_d(ut_error);
    /* This shouldn't be executed, but if there was a bug,
    we would prefer to return some value instead of crash,
    because the archiver must not crash the server. */
    return srv_log_file_size;
  }
  return log_sys.file_size;
}

void Arch_Log_Sys::update_header(byte *header, lsn_t checkpoint_lsn,
                                 lsn_t end_lsn)
{
  /* Copy Header information. */
  /* TODO: Synchronize with Key rotation or block it. */
  auto start_lsn= ut_uint64_align_down(checkpoint_lsn, OS_FILE_LOG_BLOCK_SIZE);
  log_t::header_write(header, start_lsn, log_sys.is_encrypted(), true);

  /* Write checkpoint information */
  for (int i= 0; i < 2; i++)
  {
    auto c= header;
    c+= (i == 0) ? log_t::CHECKPOINT_1 : log_t::CHECKPOINT_2;
    mach_write_to_8(c, checkpoint_lsn);
    mach_write_to_8(c + 8, end_lsn);
    mach_write_to_4(c + 60, my_crc32c(0, c, 60));
  }
}

/** Start redo log archiving.
If archiving is already in progress, the client
is attached to current group.
@param[out]     group           log archive group
@param[out]     start_lsn       start lsn for client
@param[out]     header          redo log header
@param[in]      is_durable      if client needs durable archiving
@return error code */
int Arch_Log_Sys::start(Arch_Group *&group, lsn_t &start_lsn, byte *header,
                        bool is_durable)
{
  bool create_new_group= false;

  memset(header, 0, log_t::START_OFFSET);
  log_make_checkpoint();

  arch_mutex_enter();

  if (m_state == ARCH_STATE_READ_ONLY)
  {
    arch_mutex_exit();
    return 0;
  }

  /* Wait for idle state, if preparing to idle. */
  if (!wait_idle())
  {
    int err= 0;

    if (srv_shutdown_state.load() >= SRV_SHUTDOWN_CLEANUP)
    {
      err= ER_QUERY_INTERRUPTED;
      my_error(err, MYF(0));
    }
    else
    {
      err= ER_INTERNAL_ERROR;
      my_error(err, MYF(0), "Log Archiver wait too long");
    }

    arch_mutex_exit();
    return err;
  }

  ut_ad(m_state != ARCH_STATE_PREPARE_IDLE);

  if (m_state == ARCH_STATE_ABORT)
  {
    arch_mutex_exit();
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    return ER_QUERY_INTERRUPTED;
  }

  /* Start archiver task, if needed. */
  if (m_state == ARCH_STATE_INIT)
  {
    auto err= arch_sys->start_archiver();

    if (err != 0)
    {
      arch_mutex_exit();
      sql_print_error("Could not start Log Archiver background task");
      return err;
    }
  }

  /* Start archiving from checkpoint LSN. */
  log_sys.latch.wr_lock(SRW_LOCK_CALL);

  start_lsn= log_sys.last_checkpoint_lsn;
  lsn_t checkpoint_end_lsn= log_sys.last_checkpoint_end_lsn;

  auto aligned_lsn= ut_uint64_align_down(start_lsn, OS_FILE_LOG_BLOCK_SIZE);
  const auto start_index= 0;
  const auto start_offset= log_sys.calc_lsn_offset(aligned_lsn);

  /* Need to create a new group if archiving is not in progress. */
  if (m_state == ARCH_STATE_IDLE || m_state == ARCH_STATE_INIT)
  {
    m_archived_lsn.store(aligned_lsn);
    create_new_group= true;
  }

  /* Set archiver state to active. */
  if (m_state != ARCH_STATE_ACTIVE)
  {
    m_state= ARCH_STATE_ACTIVE;
    arch_sys->signal_archiver();
  }

  log_sys.latch.wr_unlock();

  /* Create a new group. */
  if (create_new_group)
  {
    m_current_group = UT_NEW(Arch_Group(start_lsn, log_t::START_OFFSET, &m_mutex),
                             mem_key_archive);
    if (!m_current_group)
    {
      arch_mutex_exit();

      my_error(ER_OUTOFMEMORY, MYF(0), sizeof(Arch_Group));
      return ER_OUTOFMEMORY;
    }

    auto db_err=
        m_current_group->init_file_ctx(ARCH_DIR, ARCH_LOG_DIR, ARCH_LOG_FILE, 0,
                                       get_recommended_file_size(), 0);

    if (db_err != DB_SUCCESS)
    {
      arch_mutex_exit();
      my_error(ER_OUTOFMEMORY, MYF(0), sizeof(Arch_File_Ctx));
      return ER_OUTOFMEMORY;
    }

    m_start_log_index= start_index;
    m_start_log_offset= start_offset;

    m_chunk_size= ARCH_LOG_CHUNK_SIZE;

    m_group_list.push_back(m_current_group);
  }

  /* Attach to the current group. */
  m_current_group->attach(is_durable);

  group= m_current_group;

  arch_mutex_exit();

  /* Update header with checkpoint LSN. Note, that arch mutex is released
  and m_current_group should no longer be accessed. The group cannot be
  freed as we have already attached to it. */
  update_header(header, start_lsn, checkpoint_end_lsn);

  return 0;
}

#ifdef UNIV_DEBUG
void Arch_Group::adjust_end_lsn(lsn_t &stop_lsn, uint32_t &blk_len)
{
  stop_lsn= ut_uint64_align_down(get_begin_lsn(), OS_FILE_LOG_BLOCK_SIZE);

  stop_lsn+= get_file_size() - log_t::START_OFFSET;
  blk_len= 0;

  /* Increase Stop LSN 64 bytes ahead of file end not exceeding
  redo block size. */
  DBUG_EXECUTE_IF("clone_arch_log_extra_bytes",
      blk_len= OS_FILE_LOG_BLOCK_SIZE;
      stop_lsn+= 64;
      stop_lsn= std::min(stop_lsn, log_sys.get_lsn_approx()););
}

void Arch_Group::adjust_copy_length(lsn_t arch_lsn, uint32_t &copy_len)
{
  lsn_t end_lsn= LSN_MAX;
  uint32_t blk_len= 0;
  adjust_end_lsn(end_lsn, blk_len);

  if (end_lsn <= arch_lsn)
  {
    copy_len= 0;
    return;
  }

  /* Adjust if copying beyond end LSN. */
  auto len_left= end_lsn - arch_lsn;
  len_left= ut_uint64_align_down(len_left, OS_FILE_LOG_BLOCK_SIZE);

  if (len_left < copy_len)
    copy_len= static_cast<uint32_t>(len_left);
}

#endif /* UNIV_DEBUG */

/** Stop redo log archiving.
If other clients are there, the client is detached from
the current group.
@param[out]     group           log archive group
@param[out]     stop_lsn        stop lsn for client
@param[out]     log_blk         redo log trailer block
@param[in]      blk_len         length in bytes
@return error code */
int Arch_Log_Sys::stop(Arch_Group *group, lsn_t &stop_lsn, byte *log_blk,
                       uint32_t blk_len)
{
  int err= 0;
  stop_lsn= m_archived_lsn.load();

  if (log_blk != nullptr)
  {
    /* Get the current LSN and trailer block. */
    /* TODO: Log archiving for mmap */
    /* TODO: Block concurrent log file resize. */
    /* TODO: Test log archiving across file boundary. */
    log_sys.get_last_block(stop_lsn, log_blk, blk_len);

    DBUG_EXECUTE_IF("clone_arch_log_stop_file_end",
                    group->adjust_end_lsn(stop_lsn, blk_len););

    /* Will throw error, if shutdown. We still continue
    with detach but return the error. */
    err= wait_archive_complete(stop_lsn);
  }

  arch_mutex_enter();

  if (m_state == ARCH_STATE_READ_ONLY)
  {
    arch_mutex_exit();
    return 0;
  }

  auto count_active_client= group->detach(stop_lsn, nullptr);
  ut_ad(group->is_referenced());

  if (!group->is_active() && err == 0)
  {
    /* Archiving for the group has already stopped. */
    my_error(ER_INTERNAL_ERROR, MYF(0), "Clone: Log Archiver failed");
    err= ER_INTERNAL_ERROR;
  }

  if (group->is_active() && count_active_client == 0)
  {
    /* No other active client. Prepare to get idle. */
    if (m_state == ARCH_STATE_ACTIVE)
    {
      /* The active group must be the current group. */
      ut_ad(group == m_current_group);
      m_state= ARCH_STATE_PREPARE_IDLE;
      arch_sys->signal_archiver();
    }
  }
  arch_mutex_exit();
  return err;
}

void Arch_Log_Sys::force_abort()
{
  lsn_t lsn_max= LSN_MAX; /* unused */
  uint to_archive= 0;     /* unused */
  check_set_state(true, &lsn_max, &to_archive);
  /* Above line changes state to ARCH_STATE_PREPARE_IDLE or ARCH_STATE_ABORT.
  Let us notify the background thread to give it chance to notice the change and
  wait for it to transition to ARCH_STATE_IDLE before returning (in case of
  ARCH_STATE_ABORT, wait_idle() does nothing).*/
  arch_mutex_enter();
  wait_idle();
  arch_mutex_exit();
}

void Arch_Log_Sys::update_state(Arch_State state)
{
  mysql_mutex_assert_owner(&m_mutex);
  log_sys.latch.rd_lock(SRW_LOCK_CALL);
  m_state= state;
  log_sys.latch.rd_unlock();
}

void Arch_Log_Sys::wait_archiver(lsn_t next_write_lsn)
{
  ut_ad(log_sys.latch_have_wr());
  if (!is_active())
    return;

  lsn_t archiver_lsn= get_archived_lsn();
  lsn_t limit_lsn= log_sys.log_capacity + archiver_lsn;
  if (limit_lsn >= next_write_lsn)
    return;

  log_sys.latch.wr_unlock();

  /* Sleep for 10 millisecond. */
  Clone_Msec sleep_time(10);
  /* Generate alert message every 1 second. */
  Clone_Sec alert_interval(1);
  /* Wait for 5 second for archiver to catch up then abort archiver. */
  Clone_Sec time_out(5);

  auto check_fn= [&](bool alert, bool &result)
  {
    mysql_mutex_assert_owner(&m_mutex);
    if (srv_shutdown_state.load() >= SRV_SHUTDOWN_CLEANUP)
      return ER_QUERY_INTERRUPTED;

    lsn_t archiver_lsn= get_archived_lsn();
    lsn_t limit_lsn= log_sys.log_capacity + archiver_lsn;

    result= limit_lsn < next_write_lsn;
    if (result && alert)
      sql_print_information("Innodb: Log writer waiting for archiver."
          " Next LSN to write: %" PRIu64 ", Archiver LSN: %" PRIu64 ".",
          next_write_lsn, archiver_lsn);
     return 0;
  };

  /* Need to wait for archiver to catch up. */
  arch_sys->signal_archiver();

  bool is_timeout= false;
  arch_mutex_enter();
  auto err= Clone_Sys::wait(sleep_time, time_out, alert_interval, check_fn,
                            &m_mutex, is_timeout);
  arch_mutex_exit();

  if (err == 0 && is_timeout)
  {
    force_abort();
    sql_print_error("Innodb: Log writer waited too long for archiver"
      " (5 seconds). Next LSN to write: %" PRIu64 ", Archiver LSN: %" PRIu64
      ". Aborted redo-archiver task. Consider increasing innodb_redo_log_size.",
      next_write_lsn, archiver_lsn);
  }
  log_sys.latch.wr_lock(SRW_LOCK_CALL);
}

/** Release the current group from client.
@param[in]      group           group the client is attached to
@param[in]      is_durable      if client needs durable archiving */
void Arch_Log_Sys::release(Arch_Group *group, bool is_durable)
{
  arch_mutex_enter();

  group->release(is_durable);

  /* Check if there are other references or archiving is still
  in progress. */
  if (group->is_referenced() || group->is_active())
  {
    arch_mutex_exit();
    return;
  }
  /* Cleanup the group. */
  ut_ad(group != m_current_group);

  m_group_list.remove(group);
  UT_DELETE(group);
  arch_mutex_exit();
}

/** Check and set log archive system state and output the
amount of redo log available for archiving.
@param[in]      is_abort        need to abort
@param[in,out]  archived_lsn    LSN up to which redo log is archived
@param[out]     to_archive      amount of redo log to be archived */
Arch_State Arch_Log_Sys::check_set_state(bool is_abort, lsn_t *archived_lsn,
                                         uint *to_archive)
{
  auto is_shutdown= (srv_shutdown_state.load() == SRV_SHUTDOWN_LAST_PHASE ||
                     srv_shutdown_state.load() == SRV_SHUTDOWN_EXIT_THREADS);

  auto need_to_abort= (is_abort || is_shutdown);
  *to_archive= 0;
  lsn_t last_write_lsn= 0;
  arch_mutex_enter();

  switch (m_state) {
    case ARCH_STATE_ACTIVE:

      if (*archived_lsn != LSN_MAX)
      {
        /* Update system archived LSN from input */
        ut_ad(*archived_lsn >= m_archived_lsn.load());
        m_archived_lsn.store(*archived_lsn);
      }
      else
      {
        /* If input is not initialized,
        set from system archived LSN */
        *archived_lsn= m_archived_lsn.load();
      }

      lsn_t lsn_diff;

      last_write_lsn= log_sys.is_mmap()
                      ? log_sys.get_flushed_lsn()
                      : log_sys.write_lsn.load();
      /* Check redo log data ready to archive. */
      ut_ad(last_write_lsn >= m_archived_lsn.load());

      lsn_diff= last_write_lsn - m_archived_lsn.load();

      lsn_diff= ut_uint64_align_down(lsn_diff, OS_FILE_LOG_BLOCK_SIZE);

      /* Adjust archive data length if bigger than chunks size. */
      if (lsn_diff < m_chunk_size)
        *to_archive = static_cast<uint>(lsn_diff);
      else
        *to_archive = m_chunk_size;

      if (!need_to_abort)
        break;

      if (!is_shutdown)
      {
        ut_ad(is_abort);
        /* If caller asked to abort, move to prepare idle state. Archiver
        thread will move to IDLE state eventually. */
        update_state(ARCH_STATE_PREPARE_IDLE);
        break;
      }
      [[fallthrough]];

    case ARCH_STATE_PREPARE_IDLE:
    {
      /* No active clients. Mark the group inactive and move
      to idle state. */
      m_current_group->disable(m_archived_lsn.load());

      /* If no client reference, free the group. */
      if (!m_current_group->is_referenced()) {
        m_group_list.remove(m_current_group);

        UT_DELETE(m_current_group);
      }

      m_current_group= nullptr;
      update_state(ARCH_STATE_IDLE);
    }
      [[fallthrough]];

    case ARCH_STATE_IDLE:
    case ARCH_STATE_INIT:

      /* Abort archiver thread only in case of shutdown. */
      if (is_shutdown)
        update_state(ARCH_STATE_ABORT);
      break;

    case ARCH_STATE_ABORT:
      /* We could abort archiver from log_writer when
      it is already in the aborted state (shutdown). */
      break;

    default:
      ut_d(ut_error);
  }

  auto ret_state= m_state;
  arch_mutex_exit();

  return ret_state;
}

dberr_t Arch_Log_Sys::copy_log(Arch_File_Ctx *file_ctx, lsn_t start_lsn,
                               uint length)
{
  dberr_t err= DB_SUCCESS;

  if (file_ctx->is_closed())
  {
    /* Open system redo log file context */
    err= file_ctx->open(true, LSN_MAX, m_start_log_index, m_start_log_offset,
                        get_recommended_file_size());
    if (err != DB_SUCCESS)
      return err;
  }

  uint write_size= 0;
  Arch_Group *curr_group = arch_sys->log_sys()->get_arch_group();

  /* Copy log data into one or more files in archiver group. */
  while (length > 0)
  {
    auto len_copy = static_cast<uint64_t>(length);
    auto len_left = file_ctx->bytes_left();

    /* Current file is over, switch to next file. */
    if (len_left == 0)
    {
      err= file_ctx->open_next(LSN_MAX, log_t::START_OFFSET, 0);
      if (err != DB_SUCCESS)
        return (err);

      len_left= file_ctx->bytes_left();
      ut_ad(len_left > 0);
    }

    if (len_left == 0)
      return DB_ERROR;

    /* Write as much as possible from current file. */
    write_size= len_left < len_copy ? static_cast<uint>(len_left) : length;

    err = curr_group->write_to_file(file_ctx, nullptr, write_size, false, false);
    if (err != DB_SUCCESS)
      return (err);

    ut_ad(length >= write_size);
    length-= write_size;
    start_lsn+= write_size;
  }
  return DB_SUCCESS;
}

bool Arch_Log_Sys::wait_idle() {
  mysql_mutex_assert_owner(&m_mutex);

  if (m_state == ARCH_STATE_PREPARE_IDLE) {
    arch_sys->signal_archiver();
    bool is_timeout= false;
    int alert_count= 0;
    auto thd= current_thd;

    auto err= Clone_Sys::wait_default(
        [&](bool alert, bool &result) {
          mysql_mutex_assert_owner(&m_mutex);
          result= (m_state == ARCH_STATE_PREPARE_IDLE);

          if (srv_shutdown_state.load() >= SRV_SHUTDOWN_CLEANUP ||
              (thd && thd_killed(thd)))
          {
            if (thd) my_error(ER_QUERY_INTERRUPTED, MYF(0));
            return ER_QUERY_INTERRUPTED;
          }

          if (result)
          {
            arch_sys->signal_archiver();
            /* Print messages every 1 minute - default is 5 seconds. */
            if (alert && ++alert_count == 12)
            {
              alert_count= 0;
              ib::info() << "Log Archiving start: waiting for idle state";
            }
          }
          return 0;
        },
        &m_mutex, is_timeout);

    if (err == 0 && is_timeout)
    {
      err = ER_INTERNAL_ERROR;
      ib::info() << "Log Archiving start: wait for idle state timed out";
      ut_d(ut_error);
    }

    if (err != 0)
      return false;
  }
  return true;
}

/** Wait for redo log archive up to the target LSN.
We need to wait till current log sys LSN during archive stop.
@param[in]      target_lsn      target archive LSN to wait for
@return error code */
int Arch_Log_Sys::wait_archive_complete(lsn_t target_lsn)
{
  target_lsn= ut_uint64_align_down(target_lsn, OS_FILE_LOG_BLOCK_SIZE);

  /* Check and wait for archiver thread if needed. */
  if (m_archived_lsn.load() < target_lsn)
  {
    arch_sys->signal_archiver();

    bool is_timeout= false;
    int alert_count= 0;
    auto thd= current_thd;

    auto err= Clone_Sys::wait_default(
        [&](bool alert, bool &result)
    {
          /* Read consistent state. */
          arch_mutex_enter();
          auto state= m_state;
          arch_mutex_exit();

          /* Check if we need to abort. */
          if (state == ARCH_STATE_ABORT ||
              srv_shutdown_state.load() >= SRV_SHUTDOWN_CLEANUP ||
              (thd && thd_killed(thd)))
          {
            if (thd) my_error(ER_QUERY_INTERRUPTED, MYF(0));
            return ER_QUERY_INTERRUPTED;
          }

          if (state == ARCH_STATE_IDLE || state == ARCH_STATE_PREPARE_IDLE)
          {
            my_error(ER_INTERNAL_ERROR, MYF(0), "Clone: Log Archiver failed");
            return ER_INTERNAL_ERROR;
          }

          ut_ad(state == ARCH_STATE_ACTIVE);

          /* Check if archived LSN is behind target. */
          auto archived_lsn= m_archived_lsn.load();
          result= (archived_lsn < target_lsn);

          lsn_t last_write_lsn= log_sys.is_mmap()
				? log_sys.get_flushed_lsn()
				: log_sys.write_lsn.load();
          /* Trigger flush if needed */
          auto flush= last_write_lsn < target_lsn;

          if (result)
          {
            /* More data needs to be archived. */
            arch_sys->signal_archiver();

            /* Write system redo log if needed. */
            if (flush)
              log_write_up_to(target_lsn, false);

            /* Print messages every 1 minute - default is 5 seconds. */
            if (alert && ++alert_count == 12)
            {
              alert_count= 0;
              ib::info()
                  << "Clone Log archive stop: waiting for archiver to "
                     "finish archiving log till LSN: "
                  << target_lsn << " Archived LSN: " << archived_lsn;
            }
          }
          return 0;
        },
        nullptr, is_timeout);

    if (err == 0 && is_timeout)
    {
      ib::info() << "Clone Log archive stop: wait for Archiver timed out";

      err= ER_INTERNAL_ERROR;
      my_error(ER_INTERNAL_ERROR, MYF(0), "Clone: Log Archiver wait too long");
      ut_d(ut_error);
    }
    return err;
  }
  return 0;
}

/** Archive accumulated redo log in current group.
This interface is for archiver background task to archive redo log
data by calling it repeatedly over time.
@param[in, out] init            true when called the first time; it will then
                                be set to false
@param[in]      curr_ctx        system redo logs to copy data from
@param[out]     arch_lsn        LSN up to which archiving is completed
@param[out]     wait            true, if no more redo to archive
@return true, if archiving is aborted */
bool Arch_Log_Sys::archive(bool init, Arch_File_Ctx *curr_ctx, lsn_t *arch_lsn,
                           bool *wait)
{
  dberr_t err= DB_SUCCESS;
  bool is_abort= false;

  /* Initialize system redo log file context first time. */
  if (init)
  {
    /* We will use curr_ctx to read data from existing log file.*/
    err= curr_ctx->init(srv_log_group_home_dir, nullptr,
                        LOG_FILE_NAME_PREFIX, 1);
    if (err != DB_SUCCESS)
      is_abort= true;
  }

  /* Find archive system state and amount of log data to archive. */
  uint32_t arch_len= 0;
  auto curr_state = check_set_state(is_abort, arch_lsn, &arch_len);

  if (curr_state == ARCH_STATE_ACTIVE)
  {
    /* Adjust archiver length to no go beyond file end. */
    DBUG_EXECUTE_IF("clone_arch_log_stop_file_end",
                    m_current_group->adjust_copy_length(*arch_lsn, arch_len););

    /* Simulate archive error. */
    DBUG_EXECUTE_IF("clone_redo_no_archive", arch_len = 0;);

    if (arch_len == 0)
    {
      /* Nothing to archive. Need to wait. */
      *wait = true;
      return false;
    }

    /* Copy data from system redo log files to archiver files */
    err= copy_log(curr_ctx, *arch_lsn, arch_len);

    /* Simulate archive error. */
    DBUG_EXECUTE_IF("clone_redo_archive_error", err = DB_ERROR;);

    if (err == DB_SUCCESS)
    {
      *arch_lsn+= arch_len;
      *wait= false;
      return false;
    }

    /* Force abort in case of an error archiving data. */
    curr_state= check_set_state(true, arch_lsn, &arch_len);
  }

  if (curr_state == ARCH_STATE_ABORT) {
    curr_ctx->close();
    return true;
  }

  if (curr_state == ARCH_STATE_IDLE || curr_state == ARCH_STATE_INIT)
  {
    curr_ctx->close();
    *arch_lsn= LSN_MAX;
    *wait= true;
    return false;
  }

  ut_ad(curr_state == ARCH_STATE_PREPARE_IDLE);
  *wait= false;
  return false;
}
