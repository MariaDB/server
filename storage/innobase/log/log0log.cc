/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Google Inc.
Copyright (c) 2014, 2020, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file log/log0log.cc
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include <debug_sync.h>
#include <my_service_manager.h>

#include "log0log.h"
#include "log0crypt.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "fil0fil.h"
#include "dict0stats_bg.h"
#include "btr0defragment.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "srv0mon.h"
#include "buf0dump.h"
#include "log0sync.h"

/*
General philosophy of InnoDB redo-logs:

Every change to a contents of a data page must be done
through mtr_t, and mtr_t::commit() will write log records
to the InnoDB redo log. */

/** Redo log system */
log_t	log_sys;

/* These control how often we print warnings if the last checkpoint is too
old */
static bool	log_has_printed_chkp_warning = false;
static time_t	log_last_warning_time;

static bool	log_has_printed_chkp_margine_warning = false;
static time_t	log_last_margine_warning_time;

/* A margin for free space in the log buffer before a log entry is catenated */
#define LOG_BUF_WRITE_MARGIN	(4 * OS_FILE_LOG_BLOCK_SIZE)

/* Margins for free space in the log buffer after a log entry is catenated */
#define LOG_BUF_FLUSH_RATIO	2
#define LOG_BUF_FLUSH_MARGIN	(LOG_BUF_WRITE_MARGIN		\
				 + (4U << srv_page_size_shift))

/* This parameter controls asynchronous making of a new checkpoint; the value
should be bigger than LOG_POOL_PREFLUSH_RATIO_SYNC */

#define LOG_POOL_CHECKPOINT_RATIO_ASYNC	32

/* This parameter controls synchronous preflushing of modified buffer pages */
#define LOG_POOL_PREFLUSH_RATIO_SYNC	16

/* The same ratio for asynchronous preflushing; this value should be less than
the previous */
#define LOG_POOL_PREFLUSH_RATIO_ASYNC	8

/****************************************************************//**
Returns the oldest modified block lsn in the pool, or log_sys.lsn if none
exists.
@return LSN of oldest modification */
static
lsn_t
log_buf_pool_get_oldest_modification(void)
/*======================================*/
{
	lsn_t	lsn;

	ut_ad(log_mutex_own());

	lsn = buf_pool_get_oldest_modification();

	if (!lsn) {

		lsn = log_sys.get_lsn();
	}

	return(lsn);
}

/** Extends the log buffer.
@param[in]	len	requested minimum size in bytes */
void log_buffer_extend(ulong len)
{
	const size_t new_buf_size = ut_calc_align(len, srv_page_size);
	byte* new_buf = static_cast<byte*>(
		ut_malloc_dontdump(new_buf_size * 2, PSI_INSTRUMENT_ME));
	TRASH_ALLOC(new_buf, new_buf_size * 2);

	log_mutex_enter();

	if (len <= srv_log_buffer_size) {
		/* Already extended enough by the others */
		log_mutex_exit();
		ut_free_dodump(new_buf, new_buf_size * 2);
		return;
	}

	ib::warn() << "The redo log transaction size " << len <<
		" exceeds innodb_log_buffer_size="
		<< srv_log_buffer_size << " / 2). Trying to extend it.";

	const byte* old_buf_begin = log_sys.buf;
	const ulong old_buf_size = srv_log_buffer_size;
	byte* old_buf = log_sys.first_in_use
		? log_sys.buf : log_sys.buf - old_buf_size;
	srv_log_buffer_size = static_cast<ulong>(new_buf_size);
	log_sys.buf = new_buf;
	log_sys.first_in_use = true;
	memcpy_aligned<OS_FILE_LOG_BLOCK_SIZE>(log_sys.buf, old_buf_begin,
					       log_sys.buf_free);

	log_sys.max_buf_free = new_buf_size / LOG_BUF_FLUSH_RATIO
		- LOG_BUF_FLUSH_MARGIN;

	log_mutex_exit();

	ut_free_dodump(old_buf, old_buf_size);

	ib::info() << "innodb_log_buffer_size was extended to "
		<< new_buf_size << ".";
}

/** Calculate actual length in redo buffer and file including
block header and trailer.
@param[in]	len	length to write
@return actual length to write including header and trailer. */
static inline
ulint
log_calculate_actual_len(
	ulint len)
{
	ut_ad(log_mutex_own());

	const ulint	framing_size = log_sys.framing_size();
	/* actual length stored per block */
	const ulint	len_per_blk = OS_FILE_LOG_BLOCK_SIZE - framing_size;

	/* actual data length in last block already written */
	ulint	extra_len = (log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE);

	ut_ad(extra_len >= LOG_BLOCK_HDR_SIZE);
	extra_len -= LOG_BLOCK_HDR_SIZE;

	/* total extra length for block header and trailer */
	extra_len = ((len + extra_len) / len_per_blk) * framing_size;

	return(len + extra_len);
}

/** Check margin not to overwrite transaction log from the last checkpoint.
If would estimate the log write to exceed the log_capacity,
waits for the checkpoint is done enough.
@param[in]	len	length of the data to be written */

void
log_margin_checkpoint_age(
	ulint	len)
{
	ulint	margin = log_calculate_actual_len(len);

	ut_ad(log_mutex_own());

	if (margin > log_sys.log_capacity) {
		/* return with warning output to avoid deadlock */
		if (!log_has_printed_chkp_margine_warning
		    || difftime(time(NULL),
				log_last_margine_warning_time) > 15) {
			log_has_printed_chkp_margine_warning = true;
			log_last_margine_warning_time = time(NULL);

			ib::error() << "The transaction log file is too"
				" small for the single transaction log (size="
				<< len << "). So, the last checkpoint age"
				" might exceed the log capacity "
				<< log_sys.log_capacity << ".";
		}

		return;
	}

	/* Our margin check should ensure that we never reach this condition.
	Try to do checkpoint once. We cannot keep waiting here as it might
	result in hang in case the current mtr has latch on oldest lsn */
	const lsn_t lsn = log_sys.get_lsn();

	if (lsn - log_sys.last_checkpoint_lsn + margin
	    > log_sys.log_capacity) {
		/* The log write of 'len' might overwrite the transaction log
		after the last checkpoint. Makes checkpoint. */

		const bool flushed_enough = lsn
			- log_buf_pool_get_oldest_modification() + margin
			<= log_sys.log_capacity;

		log_sys.set_check_flush_or_checkpoint();
		log_mutex_exit();

		DEBUG_SYNC_C("margin_checkpoint_age_rescue");

		if (!flushed_enough) {
			os_thread_sleep(100000);
		}
		log_checkpoint();

		log_mutex_enter();
	}

	return;
}

/** Open the log for log_write_low. The log must be closed with log_close.
@param[in]	len	length of the data to be written
@return start lsn of the log record */
lsn_t
log_reserve_and_open(
	ulint	len)
{
	ulint	len_upper_limit;
#ifdef UNIV_DEBUG
	ulint	count			= 0;
#endif /* UNIV_DEBUG */

loop:
	ut_ad(log_mutex_own());

	/* Calculate an upper limit for the space the string may take in the
	log buffer */

	len_upper_limit = LOG_BUF_WRITE_MARGIN + srv_log_write_ahead_size
			  + (5 * len) / 4;

	if (log_sys.buf_free + len_upper_limit > srv_log_buffer_size) {
		log_mutex_exit();

		DEBUG_SYNC_C("log_buf_size_exceeded");

		/* Not enough free space, do a write of the log buffer */
		log_sys.initiate_write(false);

		srv_stats.log_waits.inc();

		ut_ad(++count < 50);

		log_mutex_enter();
		goto loop;
	}

	return(log_sys.get_lsn());
}

/************************************************************//**
Writes to the log the string given. It is assumed that the caller holds the
log mutex. */
void
log_write_low(
/*==========*/
	const byte*	str,		/*!< in: string */
	ulint		str_len)	/*!< in: string length */
{
	ulint	len;

	ut_ad(log_mutex_own());
	const ulint trailer_offset = log_sys.trailer_offset();
part_loop:
	/* Calculate a part length */

	ulint data_len = (log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE) + str_len;

	if (data_len <= trailer_offset) {

		/* The string fits within the current log block */

		len = str_len;
	} else {
		data_len = trailer_offset;

		len = trailer_offset
			- log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE;
	}

	memcpy(log_sys.buf + log_sys.buf_free, str, len);

	str_len -= len;
	str = str + len;

	byte* log_block = static_cast<byte*>(
		ut_align_down(log_sys.buf + log_sys.buf_free,
			      OS_FILE_LOG_BLOCK_SIZE));

	log_block_set_data_len(log_block, data_len);
	lsn_t lsn = log_sys.get_lsn();

	if (data_len == trailer_offset) {
		/* This block became full */
		log_block_set_data_len(log_block, OS_FILE_LOG_BLOCK_SIZE);
		log_block_set_checkpoint_no(log_block,
					    log_sys.next_checkpoint_no);
		len += log_sys.framing_size();

		lsn += len;

		/* Initialize the next block header */
		log_block_init(log_block + OS_FILE_LOG_BLOCK_SIZE, lsn);
	} else {
		lsn += len;
	}

	log_sys.set_lsn(lsn);
	log_sys.buf_free += len;

	ut_ad(log_sys.buf_free <= size_t{srv_log_buffer_size});

	if (str_len > 0) {
		goto part_loop;
	}

	srv_stats.log_write_requests.inc();
}

/************************************************************//**
Closes the log.
@return lsn */
lsn_t
log_close(void)
/*===========*/
{
	byte*		log_block;
	ulint		first_rec_group;
	lsn_t		oldest_lsn;
	lsn_t		lsn;
	lsn_t		checkpoint_age;

	ut_ad(log_mutex_own());

	lsn = log_sys.get_lsn();

	log_block = static_cast<byte*>(
		ut_align_down(log_sys.buf + log_sys.buf_free,
			      OS_FILE_LOG_BLOCK_SIZE));

	first_rec_group = log_block_get_first_rec_group(log_block);

	if (first_rec_group == 0) {
		/* We initialized a new log block which was not written
		full by the current mtr: the next mtr log record group
		will start within this block at the offset data_len */

		log_block_set_first_rec_group(
			log_block, log_block_get_data_len(log_block));
	}

	if (log_sys.buf_free > log_sys.max_buf_free) {
		log_sys.set_check_flush_or_checkpoint();
	}

	checkpoint_age = lsn - log_sys.last_checkpoint_lsn;

	if (checkpoint_age >= log_sys.log_capacity) {
		DBUG_EXECUTE_IF(
			"print_all_chkp_warnings",
			log_has_printed_chkp_warning = false;);

		if (!log_has_printed_chkp_warning
		    || difftime(time(NULL), log_last_warning_time) > 15) {

			log_has_printed_chkp_warning = true;
			log_last_warning_time = time(NULL);

			ib::error() << "The age of the last checkpoint is "
				    << checkpoint_age
				    << ", which exceeds the log capacity "
				    << log_sys.log_capacity << ".";
		}
	}

	if (checkpoint_age <= log_sys.max_modified_age_sync ||
	    log_sys.check_flush_or_checkpoint()) {
		goto function_exit;
	}

	oldest_lsn = buf_pool_get_oldest_modification();

	if (!oldest_lsn
	    || lsn - oldest_lsn > log_sys.max_modified_age_sync
	    || checkpoint_age > log_sys.max_checkpoint_age_async) {
		log_sys.set_check_flush_or_checkpoint();
	}
function_exit:

	return(lsn);
}

/** Calculate the recommended highest values for lsn - last_checkpoint_lsn
and lsn - buf_get_oldest_modification().
@param[in]	file_size	requested innodb_log_file_size
@retval true on success
@retval false if the smallest log group is too small to
accommodate the number of OS threads in the database server */
bool
log_set_capacity(ulonglong file_size)
{
	lsn_t		margin;
	ulint		free;

	lsn_t smallest_capacity = file_size - LOG_FILE_HDR_SIZE;
	/* Add extra safety */
	smallest_capacity -= smallest_capacity / 10;

	/* For each OS thread we must reserve so much free space in the
	smallest log group that it can accommodate the log entries produced
	by single query steps: running out of free log space is a serious
	system error which requires rebooting the database. */

	free = LOG_CHECKPOINT_FREE_PER_THREAD * (10 + srv_thread_concurrency)
		+ LOG_CHECKPOINT_EXTRA_FREE;
	if (free >= smallest_capacity / 2) {
		ib::error() << "Cannot continue operation. " << LOG_FILE_NAME
			    << " is too small for innodb_thread_concurrency="
			    << srv_thread_concurrency << ". The size of "
			    << LOG_FILE_NAME
			    << " should be bigger than 200 kB * "
			       "innodb_thread_concurrency. "
			    << INNODB_PARAMETERS_MSG;
		return(false);
	}

	margin = smallest_capacity - free;
	margin = margin - margin / 10;	/* Add still some extra safety */

	log_mutex_enter();

	log_sys.log_capacity = smallest_capacity;

	log_sys.max_modified_age_async = margin
		- margin / LOG_POOL_PREFLUSH_RATIO_ASYNC;
	log_sys.max_modified_age_sync = margin
		- margin / LOG_POOL_PREFLUSH_RATIO_SYNC;

	log_sys.max_checkpoint_age_async = margin - margin
		/ LOG_POOL_CHECKPOINT_RATIO_ASYNC;
	log_sys.max_checkpoint_age = margin;

	log_mutex_exit();

	return(true);
}

/** Initialize the redo log subsystem. */
void log_t::create()
{
  ut_ad(this == &log_sys);
  ut_ad(!is_initialised());
  m_initialised= true;

  mutex_create(LATCH_ID_LOG_SYS, &mutex);
  mutex_create(LATCH_ID_LOG_FLUSH_ORDER, &log_flush_order_mutex);

  /* Start the lsn from one log block from zero: this way every
  log record has a non-zero start lsn, a fact which we will use */

  set_lsn(LOG_START_LSN + LOG_BLOCK_HDR_SIZE);
  set_flushed_lsn(0);

  ut_ad(srv_log_buffer_size >= 16 * OS_FILE_LOG_BLOCK_SIZE);
  ut_ad(srv_log_buffer_size >= 4U << srv_page_size_shift);

  buf= static_cast<byte*>(ut_malloc_dontdump(srv_log_buffer_size * 2, PSI_INSTRUMENT_ME));
  TRASH_ALLOC(buf, srv_log_buffer_size * 2);

  first_in_use= true;

  max_buf_free= srv_log_buffer_size / LOG_BUF_FLUSH_RATIO -
    LOG_BUF_FLUSH_MARGIN;
  set_check_flush_or_checkpoint();

  n_log_ios_old= n_log_ios;
  last_printout_time= time(NULL);

  buf_next_to_write= 0;
  last_checkpoint_lsn= write_lsn= LOG_START_LSN;
  n_log_ios= 0;
  n_log_ios_old= 0;
  log_capacity= 0;
  max_modified_age_async= 0;
  max_modified_age_sync= 0;
  max_checkpoint_age_async= 0;
  max_checkpoint_age= 0;
  next_checkpoint_no= 0;
  next_checkpoint_lsn= 0;
  n_pending_checkpoint_writes= 0;

  log_block_init(buf, LOG_START_LSN);
  log_block_set_first_rec_group(buf, LOG_BLOCK_HDR_SIZE);

  buf_free= LOG_BLOCK_HDR_SIZE;
}

mapped_file_t::~mapped_file_t() noexcept
{
  if (!m_area.empty())
    unmap();
}

dberr_t mapped_file_t::map(const char *path, bool read_only,
                           bool nvme) noexcept
{
  auto fd= mysql_file_open(innodb_log_file_key, path,
                           read_only ? O_RDONLY : O_RDWR, MYF(MY_WME));
  if (fd == -1)
    return DB_ERROR;

  const auto file_size= os_file_get_size(path).m_total_size;

  const int nvme_flag= nvme ? MAP_SYNC : 0;
  void *ptr= my_mmap(0, static_cast<size_t>(file_size),
                     read_only ? PROT_READ : PROT_READ | PROT_WRITE,
                     MAP_SHARED_VALIDATE | nvme_flag, fd, 0);
  mysql_file_close(fd, MYF(MY_WME));

  if (ptr == MAP_FAILED)
    return DB_ERROR;

  m_area= {static_cast<byte *>(ptr),
           static_cast<span<byte>::size_type>(file_size)};
  return DB_SUCCESS;
}

dberr_t mapped_file_t::unmap() noexcept
{
  ut_ad(!m_area.empty());

  if (my_munmap(m_area.data(), m_area.size()))
    return DB_ERROR;

  m_area= {};
  return DB_SUCCESS;
}

file_os_io::file_os_io(file_os_io &&rhs) : m_fd(rhs.m_fd)
{
  rhs.m_fd= OS_FILE_CLOSED;
}

file_os_io &file_os_io::operator=(file_os_io &&rhs)
{
  std::swap(m_fd, rhs.m_fd);
  return *this;
}

file_os_io::~file_os_io() noexcept
{
  if (is_opened())
    close();
}

dberr_t file_os_io::open(const char *path, bool read_only) noexcept
{
  ut_ad(!is_opened());

  bool success;
  auto tmp_fd= os_file_create(
      innodb_log_file_key, path, OS_FILE_OPEN | OS_FILE_ON_ERROR_NO_EXIT,
      OS_FILE_NORMAL, OS_LOG_FILE, read_only, &success);
  if (!success)
    return DB_ERROR;

  m_durable_writes= srv_file_flush_method == SRV_O_DSYNC;
  m_fd= tmp_fd;
  return success ? DB_SUCCESS : DB_ERROR;
}

dberr_t file_os_io::rename(const char *old_path, const char *new_path) noexcept
{
  return os_file_rename(innodb_log_file_key, old_path, new_path) ? DB_SUCCESS
                                                                 : DB_ERROR;
}

dberr_t file_os_io::close() noexcept
{
  if (!os_file_close(m_fd))
    return DB_ERROR;

  m_fd= OS_FILE_CLOSED;
  return DB_SUCCESS;
}

dberr_t file_os_io::read(os_offset_t offset, span<byte> buf) noexcept
{
  return os_file_read(IORequestRead, m_fd, buf.data(), offset, buf.size());
}

dberr_t file_os_io::write(const char *path, os_offset_t offset,
                          span<const byte> buf) noexcept
{
  return os_file_write(IORequestWrite, path, m_fd, buf.data(), offset,
                       buf.size());
}

dberr_t file_os_io::flush() noexcept
{
  return os_file_flush(m_fd) ? DB_SUCCESS : DB_ERROR;
}

#ifdef HAVE_PMEM

#include <libpmem.h>

static bool is_pmem(const char *path) noexcept
{
  mapped_file_t mf;
  return mf.map(path, true, true) == DB_SUCCESS ? true : false;
}

class file_pmem_io final : public file_io
{
public:
  file_pmem_io() noexcept : file_io(true) {}

  dberr_t open(const char *path, bool read_only) noexcept final
  {
    return m_file.map(path, read_only, true);
  }
  dberr_t rename(const char *old_path, const char *new_path) noexcept final
  {
    return os_file_rename(innodb_log_file_key, old_path, new_path) ? DB_SUCCESS
                                                                   : DB_ERROR;
  }
  dberr_t close() noexcept final { return m_file.unmap(); }
  dberr_t read(os_offset_t offset, span<byte> buf) noexcept final
  {
    memcpy(buf.data(), m_file.data() + offset, buf.size());
    return DB_SUCCESS;
  }
  dberr_t write(const char *, os_offset_t offset,
                span<const byte> buf) noexcept final
  {
    pmem_memcpy_persist(m_file.data() + offset, buf.data(), buf.size());
    return DB_SUCCESS;
  }
  dberr_t flush() noexcept final
  {
    ut_ad(0);
    return DB_SUCCESS;
  }

private:
  mapped_file_t m_file;
};
#endif

dberr_t log_file_t::open(bool read_only) noexcept
{
  ut_a(!is_opened());

#ifdef HAVE_PMEM
  auto ptr= is_pmem(m_path.c_str())
                ? std::unique_ptr<file_io>(new file_pmem_io)
                : std::unique_ptr<file_io>(new file_os_io);
#else
  auto ptr= std::unique_ptr<file_io>(new file_os_io);
#endif

  if (dberr_t err= ptr->open(m_path.c_str(), read_only))
    return err;

  m_file= std::move(ptr);
  return DB_SUCCESS;
}

bool log_file_t::is_opened() const noexcept
{
  return static_cast<bool>(m_file);
}

dberr_t log_file_t::rename(std::string new_path) noexcept
{
  if (dberr_t err= m_file->rename(m_path.c_str(), new_path.c_str()))
    return err;

  m_path = std::move(new_path);
  return DB_SUCCESS;
}

dberr_t log_file_t::close() noexcept
{
  ut_a(is_opened());

  if (dberr_t err= m_file->close())
    return err;

  m_file.reset();
  return DB_SUCCESS;
}

dberr_t log_file_t::read(os_offset_t offset, span<byte> buf) noexcept
{
  ut_ad(is_opened());
  return m_file->read(offset, buf);
}

bool log_file_t::writes_are_durable() const noexcept
{
  return m_file->writes_are_durable();
}

dberr_t log_file_t::write(os_offset_t offset, span<const byte> buf) noexcept
{
  ut_ad(is_opened());
  return m_file->write(m_path.c_str(), offset, buf);
}

dberr_t log_file_t::flush() noexcept
{
  ut_ad(is_opened());
  return m_file->flush();
}

void log_t::file::open_file(std::string path)
{
  fd= log_file_t(std::move(path));
  if (const dberr_t err= fd.open(srv_read_only_mode))
    ib::fatal() << "open(" << fd.get_path() << ") returned " << err;
}

/** Update the log block checksum. */
static void log_block_store_checksum(byte* block)
{
  log_block_set_checksum(block, log_block_calc_checksum_crc32(block));
}

void log_t::file::write_header_durable(lsn_t lsn)
{
  ut_ad(lsn % OS_FILE_LOG_BLOCK_SIZE == 0);
  ut_ad(!recv_no_log_write);
  ut_ad(log_sys.log.format == log_t::FORMAT_10_5 ||
        log_sys.log.format == log_t::FORMAT_ENC_10_5);

  // man 2 open suggests this buffer to be aligned by 512 for O_DIRECT
  MY_ALIGNED(OS_FILE_LOG_BLOCK_SIZE) byte buf[OS_FILE_LOG_BLOCK_SIZE] = {0};

  mach_write_to_4(buf + LOG_HEADER_FORMAT, log_sys.log.format);
  mach_write_to_4(buf + LOG_HEADER_SUBFORMAT, log_sys.log.subformat);
  mach_write_to_8(buf + LOG_HEADER_START_LSN, lsn);
  strcpy(reinterpret_cast<char*>(buf) + LOG_HEADER_CREATOR,
         LOG_HEADER_CREATOR_CURRENT);
  ut_ad(LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR >=
        sizeof LOG_HEADER_CREATOR_CURRENT);
  log_block_store_checksum(buf);

  DBUG_PRINT("ib_log", ("write " LSN_PF, lsn));

  log_sys.log.write(0, buf);
  if (!log_sys.log.writes_are_durable())
    log_sys.log.flush();
}

void log_t::file::read(os_offset_t offset, span<byte> buf)
{
  if (const dberr_t err= fd.read(offset, buf))
    ib::fatal() << "read(" << fd.get_path() << ") returned "<< err;
}

bool log_t::file::writes_are_durable() const noexcept
{
  return fd.writes_are_durable();
}

void log_t::file::write(os_offset_t offset, span<byte> buf)
{
  srv_stats.os_log_pending_writes.inc();
  if (const dberr_t err= fd.write(offset, buf))
    ib::fatal() << "write(" << fd.get_path() << ") returned " << err;
  srv_stats.os_log_pending_writes.dec();
  srv_stats.os_log_written.add(buf.size());
  srv_stats.log_writes.inc();
  log_sys.n_log_ios++;
}

void log_t::file::flush()
{
  log_sys.pending_flushes.fetch_add(1, std::memory_order_acquire);
  if (const dberr_t err= fd.flush())
    ib::fatal() << "flush(" << fd.get_path() << ") returned " << err;
  log_sys.pending_flushes.fetch_sub(1, std::memory_order_release);
  log_sys.flushes.fetch_add(1, std::memory_order_release);
}

void log_t::file::close_file()
{
  if (!fd.is_opened())
    return;

  if (const dberr_t err= fd.close())
    ib::fatal() << "close(" << fd.get_path() << ") returned " << err;
}

/** Initialize the redo log. */
void log_t::file::create()
{
  ut_ad(this == &log_sys.log);
  ut_ad(log_sys.is_initialised());

  format= srv_encrypt_log ? log_t::FORMAT_ENC_10_5 : log_t::FORMAT_10_5;
  subformat= 2;
  file_size= srv_log_file_size;
  lsn= LOG_START_LSN;
  lsn_offset= LOG_FILE_HDR_SIZE;
}

/******************************************************//**
Writes a buffer to a log file. */
static
void
log_write_buf(
	byte*		buf,		/*!< in: buffer */
	ulint		len,		/*!< in: buffer len; must be divisible
					by OS_FILE_LOG_BLOCK_SIZE */
#ifdef UNIV_DEBUG
	ulint		pad_len,	/*!< in: pad len in the buffer len */
#endif /* UNIV_DEBUG */
	lsn_t		start_lsn,	/*!< in: start lsn of the buffer; must
					be divisible by
					OS_FILE_LOG_BLOCK_SIZE */
	ulint		new_data_offset)/*!< in: start offset of new data in
					buf: this parameter is used to decide
					if we have to write a new log file
					header */
{
	ulint		write_len;
	lsn_t		next_offset;
	ulint		i;

	ut_ad(log_write_lock_own());
	ut_ad(!recv_no_log_write);
	ut_a(len % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a(start_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);

loop:
	if (len == 0) {

		return;
	}

	next_offset = log_sys.log.calc_lsn_offset(start_lsn);

	if ((next_offset % log_sys.log.file_size) + len
	    > log_sys.log.file_size) {
		/* if the above condition holds, then the below expression
		is < len which is ulint, so the typecast is ok */
		write_len = ulint(log_sys.log.file_size
				  - (next_offset % log_sys.log.file_size));
	} else {
		write_len = len;
	}

	DBUG_PRINT("ib_log",
		   ("write " LSN_PF " to " LSN_PF
		    ": len " ULINTPF
		    " blocks " ULINTPF ".." ULINTPF,
		    start_lsn, next_offset,
		    write_len,
		    log_block_get_hdr_no(buf),
		    log_block_get_hdr_no(
			    buf + write_len
			    - OS_FILE_LOG_BLOCK_SIZE)));

	ut_ad(pad_len >= len
	      || log_block_get_hdr_no(buf)
		 == log_block_convert_lsn_to_no(start_lsn));

	/* Calculate the checksums for each log block and write them to
	the trailer fields of the log blocks */

	for (i = 0; i < write_len / OS_FILE_LOG_BLOCK_SIZE; i++) {
#ifdef UNIV_DEBUG
		ulint hdr_no_2 = log_block_get_hdr_no(buf) + i;
		DBUG_EXECUTE_IF("innodb_small_log_block_no_limit",
				hdr_no_2 = ((hdr_no_2 - 1) & 0xFUL) + 1;);
#endif
		ut_ad(pad_len >= len
			|| i * OS_FILE_LOG_BLOCK_SIZE >= len - pad_len
			|| log_block_get_hdr_no(buf + i * OS_FILE_LOG_BLOCK_SIZE) == hdr_no_2);
		log_block_store_checksum(buf + i * OS_FILE_LOG_BLOCK_SIZE);
	}

	ut_a((next_offset >> srv_page_size_shift) <= ULINT_MAX);

	log_sys.log.write(static_cast<size_t>(next_offset), {buf, write_len});

	if (write_len < len) {
		start_lsn += write_len;
		len -= write_len;
		buf += write_len;
		goto loop;
	}
}

/** Flush the recently written changes to the log file.
and invoke log_mutex_enter(). */
static void log_write_flush_to_disk_low(lsn_t lsn)
{
  log_sys.log.flush();
  ut_a(lsn >= log_sys.get_flushed_lsn());
  log_sys.set_flushed_lsn(lsn);
}

/** Switch the log buffer in use, and copy the content of last block
from old log buffer to the head of the to be used one. Thus, buf_free and
buf_next_to_write would be changed accordingly */
static inline
void
log_buffer_switch()
{
	ut_ad(log_mutex_own());
	ut_ad(log_write_lock_own());

	const byte*	old_buf = log_sys.buf;
	size_t		area_end = ut_calc_align<size_t>(
		log_sys.buf_free, OS_FILE_LOG_BLOCK_SIZE);

	if (log_sys.first_in_use) {
		log_sys.first_in_use = false;
		ut_ad(log_sys.buf == ut_align_down(log_sys.buf,
						   OS_FILE_LOG_BLOCK_SIZE));
		log_sys.buf += srv_log_buffer_size;
	} else {
		log_sys.first_in_use = true;
		log_sys.buf -= srv_log_buffer_size;
		ut_ad(log_sys.buf == ut_align_down(log_sys.buf,
						   OS_FILE_LOG_BLOCK_SIZE));
	}

	/* Copy the last block to new buf */
	memcpy_aligned<OS_FILE_LOG_BLOCK_SIZE>(
		log_sys.buf, old_buf + area_end - OS_FILE_LOG_BLOCK_SIZE,
		OS_FILE_LOG_BLOCK_SIZE);

	log_sys.buf_free %= OS_FILE_LOG_BLOCK_SIZE;
	log_sys.buf_next_to_write = log_sys.buf_free;
}

/**
Writes log buffer to disk
which is the "write" part of log_write_up_to().

This function does not flush anything.

Note : the caller must have log_mutex locked, and this
mutex is released in the function.

*/
static void log_write(bool rotate_key)
{
	ut_ad(log_mutex_own());
	ut_ad(!recv_no_log_write);
	lsn_t write_lsn;
	if (log_sys.buf_free == log_sys.buf_next_to_write) {
		/* Nothing to write */
		log_mutex_exit();
		return;
	}

	ulint		start_offset;
	ulint		end_offset;
	ulint		area_start;
	ulint		area_end;
	ulong		write_ahead_size = srv_log_write_ahead_size;
	ulint		pad_size;

	DBUG_PRINT("ib_log", ("write " LSN_PF " to " LSN_PF,
			      log_sys.write_lsn,
			      log_sys.get_lsn()));


	start_offset = log_sys.buf_next_to_write;
	end_offset = log_sys.buf_free;

	area_start = ut_2pow_round(start_offset,
				   ulint(OS_FILE_LOG_BLOCK_SIZE));
	area_end = ut_calc_align(end_offset, ulint(OS_FILE_LOG_BLOCK_SIZE));

	ut_ad(area_end - area_start > 0);

	log_block_set_flush_bit(log_sys.buf + area_start, TRUE);
	log_block_set_checkpoint_no(
		log_sys.buf + area_end - OS_FILE_LOG_BLOCK_SIZE,
		log_sys.next_checkpoint_no);

	write_lsn = log_sys.get_lsn();
	byte *write_buf = log_sys.buf;

	log_buffer_switch();

	log_sys.log.set_fields(log_sys.write_lsn);

	log_mutex_exit();
	/* Erase the end of the last log block. */
	memset(write_buf + end_offset, 0,
	       ~end_offset & (OS_FILE_LOG_BLOCK_SIZE - 1));

	/* Calculate pad_size if needed. */
	pad_size = 0;
	if (write_ahead_size > OS_FILE_LOG_BLOCK_SIZE) {
		ulint	end_offset_in_unit;
		lsn_t	end_offset = log_sys.log.calc_lsn_offset(
			ut_uint64_align_up(write_lsn, OS_FILE_LOG_BLOCK_SIZE));
		end_offset_in_unit = (ulint) (end_offset % write_ahead_size);

		if (end_offset_in_unit > 0
		    && (area_end - area_start) > end_offset_in_unit) {
			/* The first block in the unit was initialized
			after the last writing.
			Needs to be written padded data once. */
			pad_size = std::min<ulint>(
				ulint(write_ahead_size) - end_offset_in_unit,
				srv_log_buffer_size - area_end);
			::memset(write_buf + area_end, 0, pad_size);
		}
	}

	if (UNIV_UNLIKELY(srv_shutdown_state != SRV_SHUTDOWN_NONE)) {
		service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
					       "InnoDB log write: "
					       LSN_PF, log_sys.write_lsn);
	}

	if (log_sys.is_encrypted()) {
		log_crypt(write_buf + area_start, log_sys.write_lsn,
			  area_end - area_start,
			  rotate_key ? LOG_ENCRYPT_ROTATE_KEY : LOG_ENCRYPT);
	}

	/* Do the write to the log file */
	log_write_buf(
		write_buf + area_start, area_end - area_start + pad_size,
#ifdef UNIV_DEBUG
		pad_size,
#endif /* UNIV_DEBUG */
		ut_uint64_align_down(log_sys.write_lsn,
				     OS_FILE_LOG_BLOCK_SIZE),
		start_offset - area_start);
	srv_stats.log_padded.add(pad_size);
	log_sys.write_lsn = write_lsn;
	if (log_sys.log.writes_are_durable())
		log_sys.set_flushed_lsn(write_lsn);
	return;
}

static group_commit_lock write_lock;
static group_commit_lock flush_lock;

#ifdef UNIV_DEBUG
bool log_write_lock_own()
{
  return write_lock.is_owner();
}
#endif

/** Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param[in]	lsn		log sequence number that should be
included in the redo log file write
@param[in]	flush_to_disk	whether the written log should also
be flushed to the file system
@param[in]	rotate_key	whether to rotate the encryption key */
void log_write_up_to(lsn_t lsn, bool flush_to_disk, bool rotate_key)
{
  ut_ad(!srv_read_only_mode);
  ut_ad(!rotate_key || flush_to_disk);

  if (recv_no_ibuf_operations)
  {
    /* Recovery is running and no operations on the log files are
    allowed yet (the variable name .._no_ibuf_.. is misleading) */
    return;
  }

  if (flush_to_disk &&
    flush_lock.acquire(lsn) != group_commit_lock::ACQUIRED)
  {
    return;
  }

  if (write_lock.acquire(lsn) == group_commit_lock::ACQUIRED)
  {
    log_mutex_enter();
    lsn_t write_lsn= log_sys.get_lsn();
    write_lock.set_pending(write_lsn);

    log_write(rotate_key);

    ut_a(log_sys.write_lsn == write_lsn);
    write_lock.release(write_lsn);
  }

  if (!flush_to_disk)
  {
    return;
  }

  /* Flush the highest written lsn.*/
  auto flush_lsn = write_lock.value();
  flush_lock.set_pending(flush_lsn);

  if (!log_sys.log.writes_are_durable())
  {
    log_write_flush_to_disk_low(flush_lsn);
  }

  flush_lock.release(flush_lsn);

  innobase_mysql_log_notify(flush_lsn);
}

/** write to the log file up to the last log entry.
@param[in]	sync	whether we want the written log
also to be flushed to disk. */
void
log_buffer_flush_to_disk(
	bool sync)
{
	ut_ad(!srv_read_only_mode);
	log_write_up_to(log_get_lsn(), sync);
}

/********************************************************************

Tries to establish a big enough margin of free space in the log buffer, such
that a new log entry can be catenated without an immediate need for a flush. */
static
void
log_flush_margin(void)
/*==================*/
{
	lsn_t	lsn	= 0;

	log_mutex_enter();

	if (log_sys.buf_free > log_sys.max_buf_free) {
		/* We can write during flush */
		lsn = log_sys.get_lsn();
	}

	log_mutex_exit();

	if (lsn) {
		log_write_up_to(lsn, false);
	}
}

/** Advances the smallest lsn for which there are unflushed dirty blocks in the
buffer pool.
NOTE: this function may only be called if the calling thread owns no
synchronization objects!
@param[in]	new_oldest	try to advance oldest_modified_lsn at least to
this lsn
@return false if there was a flush batch of the same type running,
which means that we could not start this flush batch */
static bool log_preflush_pool_modified_pages(lsn_t new_oldest)
{
	bool	success;

	if (recv_recovery_is_on()) {
		/* If the recovery is running, we must first apply all
		log records to their respective file pages to get the
		right modify lsn values to these pages: otherwise, there
		might be pages on disk which are not yet recovered to the
		current lsn, and even after calling this function, we could
		not know how up-to-date the disk version of the database is,
		and we could not make a new checkpoint on the basis of the
		info on the buffer pool only. */
		recv_sys.apply(true);
	}

	if (new_oldest == LSN_MAX
	    || !buf_page_cleaner_is_active
	    || srv_is_being_started) {

		ulint	n_pages;

		success = buf_flush_lists(ULINT_MAX, new_oldest, &n_pages);

		buf_flush_wait_batch_end(BUF_FLUSH_LIST);

		if (!success) {
			MONITOR_INC(MONITOR_FLUSH_SYNC_WAITS);
		}

		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_SYNC_TOTAL_PAGE,
			MONITOR_FLUSH_SYNC_COUNT,
			MONITOR_FLUSH_SYNC_PAGES,
			n_pages);
	} else {
		/* better to wait for flushed by page cleaner */

		if (srv_flush_sync) {
			/* wake page cleaner for IO burst */
			buf_flush_request_force(new_oldest);
		}

		buf_flush_wait_flushed(new_oldest);

		success = true;
	}

	return(success);
}

/** Write checkpoint info to the log header and invoke log_mutex_exit().
@param[in]	end_lsn	start LSN of the FILE_CHECKPOINT mini-transaction */
void log_write_checkpoint_info(lsn_t end_lsn)
{
	ut_ad(log_mutex_own());
	ut_ad(!srv_read_only_mode);
	ut_ad(end_lsn == 0 || end_lsn >= log_sys.next_checkpoint_lsn);
	ut_ad(end_lsn <= log_sys.get_lsn());
	ut_ad(end_lsn + SIZE_OF_FILE_CHECKPOINT <= log_sys.get_lsn()
	      || srv_shutdown_state != SRV_SHUTDOWN_NONE);

	DBUG_PRINT("ib_log", ("checkpoint " UINT64PF " at " LSN_PF
			      " written",
			      log_sys.next_checkpoint_no,
			      log_sys.next_checkpoint_lsn));

	byte* buf = log_sys.checkpoint_buf;
	memset(buf, 0, OS_FILE_LOG_BLOCK_SIZE);

	mach_write_to_8(buf + LOG_CHECKPOINT_NO, log_sys.next_checkpoint_no);
	mach_write_to_8(buf + LOG_CHECKPOINT_LSN, log_sys.next_checkpoint_lsn);

	if (log_sys.is_encrypted()) {
		log_crypt_write_checkpoint_buf(buf);
	}

	lsn_t lsn_offset
		= log_sys.log.calc_lsn_offset(log_sys.next_checkpoint_lsn);
	mach_write_to_8(buf + LOG_CHECKPOINT_OFFSET, lsn_offset);
	mach_write_to_8(buf + LOG_CHECKPOINT_LOG_BUF_SIZE,
			srv_log_buffer_size);
	mach_write_to_8(buf + LOG_CHECKPOINT_END_LSN, end_lsn);

	log_block_store_checksum(buf);

	ut_ad(LOG_CHECKPOINT_1 < srv_page_size);
	ut_ad(LOG_CHECKPOINT_2 < srv_page_size);

	++log_sys.n_pending_checkpoint_writes;

	log_mutex_exit();

	/* Note: We alternate the physical place of the checkpoint info.
	See the (next_checkpoint_no & 1) below. */

	log_sys.log.write((log_sys.next_checkpoint_no & 1) ? LOG_CHECKPOINT_2
							   : LOG_CHECKPOINT_1,
			  {buf, OS_FILE_LOG_BLOCK_SIZE});

	log_sys.log.flush();

	log_mutex_enter();

	--log_sys.n_pending_checkpoint_writes;
	ut_ad(log_sys.n_pending_checkpoint_writes == 0);

	log_sys.next_checkpoint_no++;

	log_sys.last_checkpoint_lsn = log_sys.next_checkpoint_lsn;

	DBUG_PRINT("ib_log", ("checkpoint ended at " LSN_PF
			      ", flushed to " LSN_PF,
			      log_sys.last_checkpoint_lsn,
			      log_sys.get_flushed_lsn()));

	MONITOR_INC(MONITOR_NUM_CHECKPOINT);

	DBUG_EXECUTE_IF("crash_after_checkpoint", DBUG_SUICIDE(););

	log_mutex_exit();
}

/** Make a checkpoint. Note that this function does not flush dirty
blocks from the buffer pool: it only checks what is lsn of the oldest
modification in the pool, and writes information about the lsn in
log file. Use log_make_checkpoint() to flush also the pool.
@return true if success, false if a checkpoint write was already running */
bool log_checkpoint()
{
	lsn_t	oldest_lsn;

	ut_ad(!srv_read_only_mode);

	DBUG_EXECUTE_IF("no_checkpoint",
			/* We sleep for a long enough time, forcing
			the checkpoint doesn't happen any more. */
			os_thread_sleep(360000000););

	if (recv_recovery_is_on()) {
		recv_sys.apply(true);
	}

	switch (srv_file_flush_method) {
	case SRV_NOSYNC:
		break;
	case SRV_O_DSYNC:
	case SRV_FSYNC:
	case SRV_LITTLESYNC:
	case SRV_O_DIRECT:
	case SRV_O_DIRECT_NO_FSYNC:
#ifdef _WIN32
	case SRV_ALL_O_DIRECT_FSYNC:
#endif
		fil_flush_file_spaces(FIL_TYPE_TABLESPACE);
	}

	log_mutex_enter();

	ut_ad(!recv_no_log_write);
	oldest_lsn = log_buf_pool_get_oldest_modification();

	/* Because log also contains headers and dummy log records,
	log_buf_pool_get_oldest_modification() will return log_sys.lsn
	if the buffer pool contains no dirty buffers.
	We must make sure that the log is flushed up to that lsn.
	If there are dirty buffers in the buffer pool, then our
	write-ahead-logging algorithm ensures that the log has been
	flushed up to oldest_lsn. */

	ut_ad(oldest_lsn >= log_sys.last_checkpoint_lsn);
	if (oldest_lsn
	    > log_sys.last_checkpoint_lsn + SIZE_OF_FILE_CHECKPOINT) {
		/* Some log has been written since the previous checkpoint. */
	} else if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		/* MariaDB startup expects the redo log file to be
		logically empty (not even containing a MLOG_CHECKPOINT record)
		after a clean shutdown. Perform an extra checkpoint at
		shutdown. */
	} else {
		/* Do nothing, because nothing was logged (other than
		a FILE_CHECKPOINT marker) since the previous checkpoint. */
		log_mutex_exit();
		return(true);
	}
	/* Repeat the FILE_MODIFY records after the checkpoint, in
	case some log records between the checkpoint and log_sys.lsn
	need them. Finally, write a FILE_CHECKPOINT marker. Redo log
	apply expects to see a FILE_CHECKPOINT after the checkpoint,
	except on clean shutdown, where the log will be empty after
	the checkpoint.
	It is important that we write out the redo log before any
	further dirty pages are flushed to the tablespace files.  At
	this point, because log_mutex_own(), mtr_commit() in other
	threads will be blocked, and no pages can be added to the
	flush lists. */
	lsn_t		flush_lsn	= oldest_lsn;
	const lsn_t	end_lsn		= log_sys.get_lsn();
	const bool	do_write
		= srv_shutdown_state == SRV_SHUTDOWN_NONE
		|| flush_lsn != end_lsn;

	if (fil_names_clear(flush_lsn, do_write)) {
		flush_lsn = log_sys.get_lsn();
		ut_ad(flush_lsn >= end_lsn + SIZE_OF_FILE_CHECKPOINT);
	}

	log_mutex_exit();

	log_write_up_to(flush_lsn, true, true);

	log_mutex_enter();

	ut_ad(log_sys.get_flushed_lsn() >= flush_lsn);
	ut_ad(flush_lsn >= oldest_lsn);

	if (log_sys.last_checkpoint_lsn >= oldest_lsn) {
		log_mutex_exit();
		return(true);
	}

	if (log_sys.n_pending_checkpoint_writes > 0) {
		/* A checkpoint write is running */
		log_mutex_exit();

		return(false);
	}

	log_sys.next_checkpoint_lsn = oldest_lsn;
	log_write_checkpoint_info(end_lsn);
	ut_ad(!log_mutex_own());

	return(true);
}

/** Make a checkpoint */
void log_make_checkpoint()
{
	/* Preflush pages synchronously */

	while (!log_preflush_pool_modified_pages(LSN_MAX)) {
		/* Flush as much as we can */
	}

	while (!log_checkpoint()) {
		/* Force a checkpoint */
	}
}

/****************************************************************//**
Tries to establish a big enough margin of free space in the log groups, such
that a new log entry can be catenated without an immediate need for a
checkpoint. NOTE: this function may only be called if the calling thread
owns no synchronization objects! */
static
void
log_checkpoint_margin(void)
/*=======================*/
{
	ib_uint64_t	advance;
	bool		success;
loop:
	advance = 0;

	log_mutex_enter();
	ut_ad(!recv_no_log_write);

	if (!log_sys.check_flush_or_checkpoint()) {
		log_mutex_exit();
		return;
	}

	const lsn_t oldest_lsn = log_buf_pool_get_oldest_modification();
	const lsn_t lsn = log_sys.get_lsn();
	const lsn_t age = lsn - oldest_lsn;

	if (age > log_sys.max_modified_age_sync) {

		/* A flush is urgent: we have to do a synchronous preflush */
		advance = age - log_sys.max_modified_age_sync;
	}

	const lsn_t checkpoint_age = lsn - log_sys.last_checkpoint_lsn;

	ut_ad(log_sys.max_checkpoint_age >= log_sys.max_checkpoint_age_async);
	const bool do_checkpoint
		= checkpoint_age > log_sys.max_checkpoint_age_async;

	if (checkpoint_age <= log_sys.max_checkpoint_age) {
		log_sys.set_check_flush_or_checkpoint(false);
	}

	log_mutex_exit();

	if (advance) {
		lsn_t	new_oldest = oldest_lsn + advance;

		success = log_preflush_pool_modified_pages(new_oldest);

		/* If the flush succeeded, this thread has done its part
		and can proceed. If it did not succeed, there was another
		thread doing a flush at the same time. */
		if (!success) {
			log_sys.set_check_flush_or_checkpoint();
			goto loop;
		}
	}

	if (do_checkpoint) {
		log_checkpoint();
	}
}

/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */
void log_check_margins()
{
  do
  {
    log_flush_margin();
    log_checkpoint_margin();
    ut_ad(!recv_no_log_write);
  }
  while (log_sys.check_flush_or_checkpoint());
}

extern void buf_resize_shutdown();
/****************************************************************//**
Makes a checkpoint at the latest lsn and writes it to first page of each
data file in the database, so that we know that the file spaces contain
all modifications up to that lsn. This can only be called at database
shutdown. This function also writes log in log file to the log archive. */
void
logs_empty_and_mark_files_at_shutdown(void)
/*=======================================*/
{
	lsn_t			lsn;
	ulint			count = 0;

	ib::info() << "Starting shutdown...";

	/* Wait until the master thread and all other operations are idle: our
	algorithm only works if the server is idle at shutdown */
	bool do_srv_shutdown = false;
	if (srv_master_timer) {
		do_srv_shutdown = srv_fast_shutdown < 2;
		srv_master_timer.reset();
	}

	/* Wait for the end of the buffer resize task.*/
	buf_resize_shutdown();
	dict_stats_shutdown();
	btr_defragment_shutdown();

	srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;

	if (srv_buffer_pool_dump_at_shutdown &&
		!srv_read_only_mode && srv_fast_shutdown < 2) {
		buf_dump_start();
	}
	srv_error_monitor_timer.reset();
	srv_monitor_timer.reset();
	lock_sys.timeout_timer.reset();
	if (do_srv_shutdown) {
		srv_shutdown(srv_fast_shutdown == 0);
	}


loop:
	ut_ad(lock_sys.is_initialised() || !srv_was_started);
	ut_ad(log_sys.is_initialised() || !srv_was_started);
	ut_ad(fil_system.is_initialised() || !srv_was_started);

	if (!srv_read_only_mode) {
		if (recv_sys.flush_start) {
			/* This is in case recv_writer_thread was never
			started, or buf_flush_page_cleaner
			failed to notice its termination. */
			os_event_set(recv_sys.flush_start);
		}
	}
#define COUNT_INTERVAL 600U
#define CHECK_INTERVAL 100000U
	os_thread_sleep(CHECK_INTERVAL);

	count++;

	/* Check that there are no longer transactions, except for
	PREPARED ones. We need this wait even for the 'very fast'
	shutdown, because the InnoDB layer may have committed or
	prepared transactions and we don't want to lose them. */

	if (ulint total_trx = srv_was_started && !srv_read_only_mode
	    && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    ? trx_sys.any_active_transactions() : 0) {

		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			service_manager_extend_timeout(
				COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
				"Waiting for %lu active transactions to finish",
				(ulong) total_trx);
			ib::info() << "Waiting for " << total_trx << " active"
				<< " transactions to finish";

			count = 0;
		}

		goto loop;
	}

	/* We need these threads to stop early in shutdown. */
	const char* thread_name;

   if (srv_fast_shutdown != 2 && trx_rollback_is_active) {
		thread_name = "rollback of recovered transactions";
	} else {
		thread_name = NULL;
	}

	if (thread_name) {
		ut_ad(!srv_read_only_mode);
wait_suspend_loop:
		service_manager_extend_timeout(
			COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
			"Waiting for %s to exit", thread_name);
		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			ib::info() << "Waiting for " << thread_name
				   << "to exit";
			count = 0;
		}
		goto loop;
	}

	/* Check that the background threads are suspended */

	ut_ad(!srv_any_background_activity());
	if (srv_n_fil_crypt_threads_started) {
		os_event_set(fil_crypt_threads_event);
		thread_name = "fil_crypt_thread";
		goto wait_suspend_loop;
	}

	buf_load_dump_end();

	srv_shutdown_state = SRV_SHUTDOWN_FLUSH_PHASE;

	/* At this point only page_cleaner should be active. We wait
	here to let it complete the flushing of the buffer pools
	before proceeding further. */

	count = 0;
	service_manager_extend_timeout(COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
		"Waiting for page cleaner");
	while (buf_page_cleaner_is_active) {
		++count;
		os_thread_sleep(CHECK_INTERVAL);
		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			service_manager_extend_timeout(COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
				"Waiting for page cleaner");
			ib::info() << "Waiting for page_cleaner to "
				"finish flushing of buffer pool";
			count = 0;
		}
	}

	if (log_sys.is_initialised()) {
		log_mutex_enter();
		const ulint	n_write	= log_sys.n_pending_checkpoint_writes;
		const ulint	n_flush	= log_sys.pending_flushes;
		log_mutex_exit();

		if (n_write || n_flush) {
			if (srv_print_verbose_log && count > 600) {
				ib::info() << "Pending checkpoint_writes: "
					<< n_write
					<< ". Pending log flush writes: "
					<< n_flush;
				count = 0;
			}
			goto loop;
		}
	}

	if (!buf_pool.is_initialised()) {
		ut_ad(!srv_was_started);
	} else if (ulint pending_io = buf_pool_check_no_pending_io()) {
		if (srv_print_verbose_log && count > 600) {
			ib::info() << "Waiting for " << pending_io << " buffer"
				" page I/Os to complete";
			count = 0;
		}

		goto loop;
	}

	if (srv_fast_shutdown == 2 || !srv_was_started) {
		if (!srv_read_only_mode && srv_was_started) {
			ib::info() << "MySQL has requested a very fast"
				" shutdown without flushing the InnoDB buffer"
				" pool to data files. At the next mysqld"
				" startup InnoDB will do a crash recovery!";

			/* In this fastest shutdown we do not flush the
			buffer pool:

			it is essentially a 'crash' of the InnoDB server.
			Make sure that the log is all flushed to disk, so
			that we can recover all committed transactions in
			a crash recovery. We must not write the lsn stamps
			to the data files, since at a startup InnoDB deduces
			from the stamps if the previous shutdown was clean. */

			log_buffer_flush_to_disk();
		}

		srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

		if (fil_system.is_initialised()) {
			fil_close_all_files();
		}
		return;
	}

	if (!srv_read_only_mode) {
		service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
			"ensuring dirty buffer pool are written to log");
		log_make_checkpoint();

		log_mutex_enter();

		lsn = log_sys.get_lsn();

		const bool lsn_changed = lsn != log_sys.last_checkpoint_lsn
			&& lsn != log_sys.last_checkpoint_lsn
			+ SIZE_OF_FILE_CHECKPOINT;
		ut_ad(lsn >= log_sys.last_checkpoint_lsn);

		log_mutex_exit();

		if (lsn_changed) {
			goto loop;
		}

		/* Ensure that all buffered changes are written to the
		redo log before fil_close_all_files(). */
		log_sys.log.flush();
	} else {
		lsn = recv_sys.recovered_lsn;
	}

	srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
				       "Free innodb buffer pool");
	ut_d(buf_pool.assert_all_freed());

	ut_a(lsn == log_sys.get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);

	if (UNIV_UNLIKELY(lsn < recv_sys.recovered_lsn)) {
		ib::error() << "Shutdown LSN=" << lsn
			    << " is less than start LSN="
			    << recv_sys.recovered_lsn;
	}

	srv_shutdown_lsn = lsn;

	if (!srv_read_only_mode) {
		dberr_t err = fil_write_flushed_lsn(lsn);

		if (err != DB_SUCCESS) {
			ib::error() << "Writing flushed lsn " << lsn
				<< " failed; error=" << err;
		}
	}

	fil_close_all_files();

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	ut_a(lsn == log_sys.get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
}

/******************************************************//**
Prints info of the log. */
void
log_print(
/*======*/
	FILE*	file)	/*!< in: file where to print */
{
	double	time_elapsed;
	time_t	current_time;

	log_mutex_enter();

	fprintf(file,
		"Log sequence number " LSN_PF "\n"
		"Log flushed up to   " LSN_PF "\n"
		"Pages flushed up to " LSN_PF "\n"
		"Last checkpoint at  " LSN_PF "\n",
		log_sys.get_lsn(),
		log_sys.get_flushed_lsn(),
		log_buf_pool_get_oldest_modification(),
		log_sys.last_checkpoint_lsn);

	current_time = time(NULL);

	time_elapsed = difftime(current_time,
				log_sys.last_printout_time);

	if (time_elapsed <= 0) {
		time_elapsed = 1;
	}

	fprintf(file,
		ULINTPF " pending log flushes, "
		ULINTPF " pending chkp writes\n"
		ULINTPF " log i/o's done, %.2f log i/o's/second\n",
		log_sys.pending_flushes.load(),
		log_sys.n_pending_checkpoint_writes,
		log_sys.n_log_ios,
		static_cast<double>(
			log_sys.n_log_ios - log_sys.n_log_ios_old)
		/ time_elapsed);

	log_sys.n_log_ios_old = log_sys.n_log_ios;
	log_sys.last_printout_time = current_time;

	log_mutex_exit();
}

/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */
void
log_refresh_stats(void)
/*===================*/
{
	log_sys.n_log_ios_old = log_sys.n_log_ios;
	log_sys.last_printout_time = time(NULL);
}

/** Shut down the redo log subsystem. */
void log_t::close()
{
  ut_ad(this == &log_sys);
  if (!is_initialised()) return;
  m_initialised = false;
  log.close();

  if (!first_in_use)
    buf -= srv_log_buffer_size;
  ut_free_dodump(buf, srv_log_buffer_size * 2);
  buf = NULL;

  mutex_free(&mutex);
  mutex_free(&log_flush_order_mutex);

  recv_sys.close();
}

std::string get_log_file_path(const char *filename)
{
  const size_t size= strlen(srv_log_group_home_dir) + /* path separator */ 1 +
                     strlen(filename) + /* longest suffix */ 3;
  std::string path;
  path.reserve(size);
  path.assign(srv_log_group_home_dir);

  std::replace(path.begin(), path.end(), OS_PATH_SEPARATOR_ALT,
	       OS_PATH_SEPARATOR);

  if (path.back() != OS_PATH_SEPARATOR)
    path.push_back(OS_PATH_SEPARATOR);
  path.append(filename);

  return path;
}

std::vector<std::string> get_existing_log_files_paths() {
  std::vector<std::string> result;

  for (int i= 0; i < 101; i++) {
    auto path= get_log_file_path(LOG_FILE_NAME_PREFIX)
                                 .append(std::to_string(i));
    os_file_stat_t stat;
    dberr_t err= os_file_get_status(path.c_str(), &stat, false, true);
    if (err)
      break;

    if (stat.type != OS_FILE_TYPE_FILE)
      break;

    result.push_back(std::move(path));
  }

  return result;
}
