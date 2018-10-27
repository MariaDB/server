/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Google Inc.
Copyright (c) 2014, 2018, MariaDB Corporation.

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
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file log/log0log.cc
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"
#include <debug_sync.h>
#include <my_service_manager.h>

#include "log0log.h"
#include "log0crypt.h"
#include "mem0mem.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "fil0fil.h"
#include "dict0boot.h"
#include "dict0stats_bg.h"
#include "btr0defragment.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "srv0mon.h"
#include "sync0sync.h"

/*
General philosophy of InnoDB redo-logs:

1) Every change to a contents of a data page must be done
through mtr, which in mtr_commit() writes log records
to the InnoDB redo log.

2) Normally these changes are performed using a mlog_write_ulint()
or similar function.

3) In some page level operations only a code number of a
c-function and its parameters are written to the log to
reduce the size of the log.

  3a) You should not add parameters to these kind of functions
  (e.g. trx_undo_header_create())

  3b) You should not add such functionality which either change
  working when compared with the old or are dependent on data
  outside of the page. These kind of functions should implement
  self-contained page transformation and it should be unchanged
  if you don't have very essential reasons to change log
  semantics or format.

*/

/** Redo log system */
log_t	log_sys;

/** Whether to generate and require checksums on the redo log pages */
my_bool	innodb_log_checksums;

/** Pointer to the log checksum calculation function */
log_checksum_func_t log_checksum_algorithm_ptr;

/* Next log block number to do dummy record filling if no log records written
for a while */
static ulint		next_lbn_to_pad = 0;

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

/* Codes used in unlocking flush latches */
#define LOG_UNLOCK_NONE_FLUSHED_LOCK	1
#define LOG_UNLOCK_FLUSH_LOCK		2

/** Event to wake up log_scrub_thread */
os_event_t	log_scrub_event;
/** Whether log_scrub_thread is active */
bool		log_scrub_thread_active;

extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(log_scrub_thread)(void*);

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

		lsn = log_sys.lsn;
	}

	return(lsn);
}

/** Extends the log buffer.
@param[in]	len	requested minimum size in bytes */
void log_buffer_extend(ulong len)
{
	byte	tmp_buf[OS_FILE_LOG_BLOCK_SIZE];

	log_mutex_enter_all();

	while (log_sys.is_extending) {
		/* Another thread is trying to extend already.
		Needs to wait for. */
		log_mutex_exit_all();

		log_buffer_flush_to_disk();

		log_mutex_enter_all();

		if (srv_log_buffer_size > len) {
			/* Already extended enough by the others */
			log_mutex_exit_all();
			return;
		}
	}

	if (len >= srv_log_buffer_size / 2) {
		DBUG_EXECUTE_IF("ib_log_buffer_is_short_crash",
				DBUG_SUICIDE(););

		/* log_buffer is too small. try to extend instead of crash. */
		ib::warn() << "The redo log transaction size " << len <<
			" exceeds innodb_log_buffer_size="
			<< srv_log_buffer_size << " / 2). Trying to extend it.";
	}

	log_sys.is_extending = true;

	while (ut_calc_align_down(log_sys.buf_free,
				  OS_FILE_LOG_BLOCK_SIZE)
	       != ut_calc_align_down(log_sys.buf_next_to_write,
				     OS_FILE_LOG_BLOCK_SIZE)) {
		/* Buffer might have >1 blocks to write still. */
		log_mutex_exit_all();

		log_buffer_flush_to_disk();

		log_mutex_enter_all();
	}

	ulong move_start = ut_calc_align_down(
		log_sys.buf_free,
		OS_FILE_LOG_BLOCK_SIZE);
	ulong move_end = log_sys.buf_free;

	/* store the last log block in buffer */
	ut_memcpy(tmp_buf, log_sys.buf + move_start,
		  move_end - move_start);

	log_sys.buf_free -= move_start;
	log_sys.buf_next_to_write -= move_start;

	/* free previous after getting the right address */
	if (!log_sys.first_in_use) {
		log_sys.buf -= srv_log_buffer_size;
	}
	ut_free_dodump(log_sys.buf, srv_log_buffer_size * 2);

	/* reallocate log buffer */
	srv_log_buffer_size = len;

	log_sys.buf = static_cast<byte*>(
		ut_malloc_dontdump(srv_log_buffer_size * 2));
	TRASH_ALLOC(log_sys.buf, srv_log_buffer_size * 2);

	log_sys.first_in_use = true;

	log_sys.max_buf_free = srv_log_buffer_size / LOG_BUF_FLUSH_RATIO
		- LOG_BUF_FLUSH_MARGIN;

	/* restore the last log block */
	ut_memcpy(log_sys.buf, tmp_buf, move_end - move_start);

	ut_ad(log_sys.is_extending);
	log_sys.is_extending = false;

	log_mutex_exit_all();

	ib::info() << "innodb_log_buffer_size was extended to "
		<< srv_log_buffer_size << ".";
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

	/* actual length stored per block */
	const ulint	len_per_blk = OS_FILE_LOG_BLOCK_SIZE
		- (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE);

	/* actual data length in last block already written */
	ulint	extra_len = (log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE);

	ut_ad(extra_len >= LOG_BLOCK_HDR_SIZE);
	extra_len -= LOG_BLOCK_HDR_SIZE;

	/* total extra length for block header and trailer */
	extra_len = ((len + extra_len) / len_per_blk)
		* (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE);

	return(len + extra_len);
}

/** Check margin not to overwrite transaction log from the last checkpoint.
If would estimate the log write to exceed the log_group_capacity,
waits for the checkpoint is done enough.
@param[in]	len	length of the data to be written */

void
log_margin_checkpoint_age(
	ulint	len)
{
	ulint	margin = log_calculate_actual_len(len);

	ut_ad(log_mutex_own());

	if (margin > log_sys.log_group_capacity) {
		/* return with warning output to avoid deadlock */
		if (!log_has_printed_chkp_margine_warning
		    || difftime(time(NULL),
				log_last_margine_warning_time) > 15) {
			log_has_printed_chkp_margine_warning = true;
			log_last_margine_warning_time = time(NULL);

			ib::error() << "The transaction log files are too"
				" small for the single transaction log (size="
				<< len << "). So, the last checkpoint age"
				" might exceed the log group capacity "
				<< log_sys.log_group_capacity << ".";
		}

		return;
	}

	/* Our margin check should ensure that we never reach this condition.
	Try to do checkpoint once. We cannot keep waiting here as it might
	result in hang in case the current mtr has latch on oldest lsn */
	if (log_sys.lsn - log_sys.last_checkpoint_lsn + margin
	    > log_sys.log_group_capacity) {
		/* The log write of 'len' might overwrite the transaction log
		after the last checkpoint. Makes checkpoint. */

		bool	flushed_enough = false;

		if (log_sys.lsn - log_buf_pool_get_oldest_modification()
		    + margin
		    <= log_sys.log_group_capacity) {
			flushed_enough = true;
		}

		log_sys.check_flush_or_checkpoint = true;
		log_mutex_exit();

		DEBUG_SYNC_C("margin_checkpoint_age_rescue");

		if (!flushed_enough) {
			os_thread_sleep(100000);
		}
		log_checkpoint(true, false);

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

	if (log_sys.is_extending) {
		log_mutex_exit();

		/* Log buffer size is extending. Writing up to the next block
		should wait for the extending finished. */

		os_thread_sleep(100000);

		ut_ad(++count < 50);

		log_mutex_enter();
		goto loop;
	}

	/* Calculate an upper limit for the space the string may take in the
	log buffer */

	len_upper_limit = LOG_BUF_WRITE_MARGIN + srv_log_write_ahead_size
			  + (5 * len) / 4;

	if (log_sys.buf_free + len_upper_limit > srv_log_buffer_size) {
		log_mutex_exit();

		DEBUG_SYNC_C("log_buf_size_exceeded");

		/* Not enough free space, do a write of the log buffer */
		log_buffer_sync_in_background(false);

		srv_stats.log_waits.inc();

		ut_ad(++count < 50);

		log_mutex_enter();
		goto loop;
	}

	return(log_sys.lsn);
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
	ulint	data_len;
	byte*	log_block;

	ut_ad(log_mutex_own());
part_loop:
	/* Calculate a part length */

	data_len = (log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE) + str_len;

	if (data_len <= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {

		/* The string fits within the current log block */

		len = str_len;
	} else {
		data_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE;

		len = OS_FILE_LOG_BLOCK_SIZE
			- (log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE)
			- LOG_BLOCK_TRL_SIZE;
	}

	memcpy(log_sys.buf + log_sys.buf_free, str, len);

	str_len -= len;
	str = str + len;

	log_block = static_cast<byte*>(
		ut_align_down(log_sys.buf + log_sys.buf_free,
			      OS_FILE_LOG_BLOCK_SIZE));

	log_block_set_data_len(log_block, data_len);

	if (data_len == OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
		/* This block became full */
		log_block_set_data_len(log_block, OS_FILE_LOG_BLOCK_SIZE);
		log_block_set_checkpoint_no(log_block,
					    log_sys.next_checkpoint_no);
		len += LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE;

		log_sys.lsn += len;

		/* Initialize the next block header */
		log_block_init(log_block + OS_FILE_LOG_BLOCK_SIZE,
			       log_sys.lsn);
	} else {
		log_sys.lsn += len;
	}

	log_sys.buf_free += ulong(len);

	ut_ad(log_sys.buf_free <= srv_log_buffer_size);

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

	lsn = log_sys.lsn;

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
		log_sys.check_flush_or_checkpoint = true;
	}

	checkpoint_age = lsn - log_sys.last_checkpoint_lsn;

	if (checkpoint_age >= log_sys.log_group_capacity) {
		DBUG_EXECUTE_IF(
			"print_all_chkp_warnings",
			log_has_printed_chkp_warning = false;);

		if (!log_has_printed_chkp_warning
		    || difftime(time(NULL), log_last_warning_time) > 15) {

			log_has_printed_chkp_warning = true;
			log_last_warning_time = time(NULL);

			ib::error() << "The age of the last checkpoint is "
				<< checkpoint_age << ", which exceeds the log"
				" group capacity "
				<< log_sys.log_group_capacity
				<< ".";
		}
	}

	if (checkpoint_age <= log_sys.max_modified_age_sync) {
		goto function_exit;
	}

	oldest_lsn = buf_pool_get_oldest_modification();

	if (!oldest_lsn
	    || lsn - oldest_lsn > log_sys.max_modified_age_sync
	    || checkpoint_age > log_sys.max_checkpoint_age_async) {
		log_sys.check_flush_or_checkpoint = true;
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

	lsn_t smallest_capacity = (file_size - LOG_FILE_HDR_SIZE)
		* srv_n_log_files;
	/* Add extra safety */
	smallest_capacity -= smallest_capacity / 10;

	/* For each OS thread we must reserve so much free space in the
	smallest log group that it can accommodate the log entries produced
	by single query steps: running out of free log space is a serious
	system error which requires rebooting the database. */

	free = LOG_CHECKPOINT_FREE_PER_THREAD * (10 + srv_thread_concurrency)
		+ LOG_CHECKPOINT_EXTRA_FREE;
	if (free >= smallest_capacity / 2) {
		ib::error() << "Cannot continue operation. ib_logfiles are too"
			" small for innodb_thread_concurrency="
			<< srv_thread_concurrency << ". The combined size of"
			" ib_logfiles should be bigger than"
			" 200 kB * innodb_thread_concurrency. "
			<< INNODB_PARAMETERS_MSG;
		return(false);
	}

	margin = smallest_capacity - free;
	margin = margin - margin / 10;	/* Add still some extra safety */

	log_mutex_enter();

	log_sys.log_group_capacity = smallest_capacity;

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
  mutex_create(LATCH_ID_LOG_WRITE, &write_mutex);
  mutex_create(LATCH_ID_LOG_FLUSH_ORDER, &log_flush_order_mutex);

  /* Start the lsn from one log block from zero: this way every
  log record has a non-zero start lsn, a fact which we will use */

  lsn= LOG_START_LSN;

  ut_ad(srv_log_buffer_size >= 16 * OS_FILE_LOG_BLOCK_SIZE);
  ut_ad(srv_log_buffer_size >= 4U << srv_page_size_shift);

  buf= static_cast<byte*>(ut_malloc_dontdump(srv_log_buffer_size * 2));
  TRASH_ALLOC(buf, srv_log_buffer_size * 2);

  first_in_use= true;

  max_buf_free= srv_log_buffer_size / LOG_BUF_FLUSH_RATIO -
    LOG_BUF_FLUSH_MARGIN;
  check_flush_or_checkpoint= true;

  n_log_ios_old= n_log_ios;
  last_printout_time= time(NULL);

  buf_next_to_write= 0;
  is_extending= false;
  write_lsn= lsn;
  flushed_to_disk_lsn= 0;
  n_pending_flushes= 0;
  flush_event = os_event_create("log_flush_event");
  os_event_set(flush_event);
  n_log_ios= 0;
  n_log_ios_old= 0;
  log_group_capacity= 0;
  max_modified_age_async= 0;
  max_modified_age_sync= 0;
  max_checkpoint_age_async= 0;
  max_checkpoint_age= 0;
  next_checkpoint_no= 0;
  next_checkpoint_lsn= 0;
  append_on_checkpoint= NULL;
  n_pending_checkpoint_writes= 0;

  last_checkpoint_lsn= lsn;
  rw_lock_create(checkpoint_lock_key, &checkpoint_lock, SYNC_NO_ORDER_CHECK);

  log_block_init(buf, lsn);
  log_block_set_first_rec_group(buf, LOG_BLOCK_HDR_SIZE);

  buf_free= LOG_BLOCK_HDR_SIZE;
  lsn= LOG_START_LSN + LOG_BLOCK_HDR_SIZE;

  MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE, lsn - last_checkpoint_lsn);

  log_scrub_thread_active= !srv_read_only_mode && srv_scrub_log;
  if (log_scrub_thread_active) {
    log_scrub_event= os_event_create("log_scrub_event");
    os_thread_create(log_scrub_thread, NULL, NULL);
  }
}

/** Initialize the redo log.
@param[in]	n_files		number of files */
void log_t::files::create(ulint n_files)
{
  ut_ad(n_files <= SRV_N_LOG_FILES_MAX);
  ut_ad(this == &log_sys.log);
  ut_ad(log_sys.is_initialised());

  this->n_files= n_files;
  format= srv_encrypt_log
    ? LOG_HEADER_FORMAT_CURRENT | LOG_HEADER_FORMAT_ENCRYPTED
    : LOG_HEADER_FORMAT_CURRENT;
  subformat= 2;
  file_size= srv_log_file_size;
  lsn= LOG_START_LSN;
  lsn_offset= LOG_FILE_HDR_SIZE;

  byte* ptr= static_cast<byte*>(ut_zalloc_nokey(LOG_FILE_HDR_SIZE * n_files
						+ OS_FILE_LOG_BLOCK_SIZE));
  file_header_bufs_ptr= ptr;
  ptr= static_cast<byte*>(ut_align(ptr, OS_FILE_LOG_BLOCK_SIZE));

  memset(file_header_bufs, 0, sizeof file_header_bufs);

  for (ulint i = 0; i < n_files; i++, ptr += LOG_FILE_HDR_SIZE)
    file_header_bufs[i] = ptr;
}

/******************************************************//**
Writes a log file header to a log file space. */
static
void
log_file_header_flush(
	ulint		nth_file,	/*!< in: header to the nth file in the
					log file space */
	lsn_t		start_lsn)	/*!< in: log file data starts at this
					lsn */
{
	byte*	buf;
	lsn_t	dest_offset;

	ut_ad(log_write_mutex_own());
	ut_ad(!recv_no_log_write);
	ut_a(nth_file < log_sys.log.n_files);
	ut_ad((log_sys.log.format & ~LOG_HEADER_FORMAT_ENCRYPTED)
	      == LOG_HEADER_FORMAT_CURRENT);

	buf = log_sys.log.file_header_bufs[nth_file];

	memset(buf, 0, OS_FILE_LOG_BLOCK_SIZE);
	mach_write_to_4(buf + LOG_HEADER_FORMAT, log_sys.log.format);
	mach_write_to_4(buf + LOG_HEADER_SUBFORMAT, log_sys.log.subformat);
	mach_write_to_8(buf + LOG_HEADER_START_LSN, start_lsn);
	strcpy(reinterpret_cast<char*>(buf) + LOG_HEADER_CREATOR,
	       LOG_HEADER_CREATOR_CURRENT);
	ut_ad(LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR
	      >= sizeof LOG_HEADER_CREATOR_CURRENT);
	log_block_set_checksum(buf, log_block_calc_checksum_crc32(buf));

	dest_offset = nth_file * log_sys.log.file_size;

	DBUG_PRINT("ib_log", ("write " LSN_PF
			      " file " ULINTPF " header",
			      start_lsn, nth_file));

	log_sys.n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	srv_stats.os_log_pending_writes.inc();

	const ulint	page_no = ulint(dest_offset >> srv_page_size_shift);

	fil_io(IORequestLogWrite, true,
	       page_id_t(SRV_LOG_SPACE_FIRST_ID, page_no),
	       univ_page_size,
	       ulint(dest_offset & (srv_page_size - 1)),
	       OS_FILE_LOG_BLOCK_SIZE, buf, NULL);

	srv_stats.os_log_pending_writes.dec();
}

/******************************************************//**
Stores a 4-byte checksum to the trailer checksum field of a log block
before writing it to a log file. This checksum is used in recovery to
check the consistency of a log block. */
static
void
log_block_store_checksum(
/*=====================*/
	byte*	block)	/*!< in/out: pointer to a log block */
{
	log_block_set_checksum(block, log_block_calc_checksum(block));
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
	bool		write_header	= new_data_offset == 0;
	lsn_t		next_offset;
	ulint		i;

	ut_ad(log_write_mutex_own());
	ut_ad(!recv_no_log_write);
	ut_a(len % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a(start_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);

loop:
	if (len == 0) {

		return;
	}

	next_offset = log_sys.log.calc_lsn_offset(start_lsn);

	if (write_header
	    && next_offset % log_sys.log.file_size == LOG_FILE_HDR_SIZE) {
		/* We start to write a new log file instance in the group */

		ut_a(next_offset / log_sys.log.file_size <= ULINT_MAX);

		log_file_header_flush(
			ulint(next_offset / log_sys.log.file_size), start_lsn);
		srv_stats.os_log_written.add(OS_FILE_LOG_BLOCK_SIZE);

		srv_stats.log_writes.inc();
	}

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
		ut_ad(pad_len >= len
		      || i * OS_FILE_LOG_BLOCK_SIZE >= len - pad_len
		      || log_block_get_hdr_no(
			      buf + i * OS_FILE_LOG_BLOCK_SIZE)
			 == log_block_get_hdr_no(buf) + i);
		log_block_store_checksum(buf + i * OS_FILE_LOG_BLOCK_SIZE);
	}

	log_sys.n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	srv_stats.os_log_pending_writes.inc();

	ut_a((next_offset >> srv_page_size_shift) <= ULINT_MAX);

	const ulint	page_no = ulint(next_offset >> srv_page_size_shift);

	fil_io(IORequestLogWrite, true,
	       page_id_t(SRV_LOG_SPACE_FIRST_ID, page_no),
	       univ_page_size,
	       ulint(next_offset & (srv_page_size - 1)), write_len, buf, NULL);

	srv_stats.os_log_pending_writes.dec();

	srv_stats.os_log_written.add(write_len);
	srv_stats.log_writes.inc();

	if (write_len < len) {
		start_lsn += write_len;
		len -= write_len;
		buf += write_len;

		write_header = true;

		goto loop;
	}
}

/** Flush the recently written changes to the log file.
and invoke log_mutex_enter(). */
static
void
log_write_flush_to_disk_low()
{
	/* FIXME: This is not holding log_sys.mutex while
	calling os_event_set()! */
	ut_a(log_sys.n_pending_flushes == 1); /* No other threads here */

	bool	do_flush = srv_file_flush_method != SRV_O_DSYNC;

	if (do_flush) {
		fil_flush(SRV_LOG_SPACE_FIRST_ID);
	}

	MONITOR_DEC(MONITOR_PENDING_LOG_FLUSH);

	log_mutex_enter();
	if (do_flush) {
		log_sys.flushed_to_disk_lsn = log_sys.current_flush_lsn;
	}

	log_sys.n_pending_flushes--;

	os_event_set(log_sys.flush_event);
}

/** Switch the log buffer in use, and copy the content of last block
from old log buffer to the head of the to be used one. Thus, buf_free and
buf_next_to_write would be changed accordingly */
static inline
void
log_buffer_switch()
{
	ut_ad(log_mutex_own());
	ut_ad(log_write_mutex_own());

	const byte*	old_buf = log_sys.buf;
	ulint		area_end = ut_calc_align(log_sys.buf_free,
						 OS_FILE_LOG_BLOCK_SIZE);

	if (log_sys.first_in_use) {
		log_sys.first_in_use = false;
		ut_ad(log_sys.buf == ut_align(log_sys.buf,
					       OS_FILE_LOG_BLOCK_SIZE));
		log_sys.buf += srv_log_buffer_size;
	} else {
		log_sys.first_in_use = true;
		log_sys.buf -= srv_log_buffer_size;
		ut_ad(log_sys.buf == ut_align(log_sys.buf,
					       OS_FILE_LOG_BLOCK_SIZE));
	}

	/* Copy the last block to new buf */
	ut_memcpy(log_sys.buf,
		  old_buf + area_end - OS_FILE_LOG_BLOCK_SIZE,
		  OS_FILE_LOG_BLOCK_SIZE);

	log_sys.buf_free %= OS_FILE_LOG_BLOCK_SIZE;
	log_sys.buf_next_to_write = log_sys.buf_free;
}

/** Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param[in]	lsn		log sequence number that should be
included in the redo log file write
@param[in]	flush_to_disk	whether the written log should also
be flushed to the file system */
void
log_write_up_to(
	lsn_t	lsn,
	bool	flush_to_disk)
{
#ifdef UNIV_DEBUG
	ulint		loop_count	= 0;
#endif /* UNIV_DEBUG */
	byte*           write_buf;
	lsn_t           write_lsn;

	ut_ad(!srv_read_only_mode);

	if (recv_no_ibuf_operations) {
		/* Recovery is running and no operations on the log files are
		allowed yet (the variable name .._no_ibuf_.. is misleading) */

		return;
	}

loop:
	ut_ad(++loop_count < 128);

#if UNIV_WORD_SIZE > 7
	/* We can do a dirty read of LSN. */
	/* NOTE: Currently doesn't do dirty read for
	(flush_to_disk == true) case, because the log_mutex
	contention also works as the arbitrator for write-IO
	(fsync) bandwidth between log files and data files. */
	if (!flush_to_disk && log_sys.write_lsn >= lsn) {
		return;
	}
#endif

	log_write_mutex_enter();
	ut_ad(!recv_no_log_write);

	lsn_t	limit_lsn = flush_to_disk
		? log_sys.flushed_to_disk_lsn
		: log_sys.write_lsn;

	if (limit_lsn >= lsn) {
		log_write_mutex_exit();
		return;
	}

	/* If it is a write call we should just go ahead and do it
	as we checked that write_lsn is not where we'd like it to
	be. If we have to flush as well then we check if there is a
	pending flush and based on that we wait for it to finish
	before proceeding further. */
	if (flush_to_disk
	    && (log_sys.n_pending_flushes > 0
		|| !os_event_is_set(log_sys.flush_event))) {
		/* Figure out if the current flush will do the job
		for us. */
		bool work_done = log_sys.current_flush_lsn >= lsn;

		log_write_mutex_exit();

		os_event_wait(log_sys.flush_event);

		if (work_done) {
			return;
		} else {
			goto loop;
		}
	}

	log_mutex_enter();
	if (!flush_to_disk
	    && log_sys.buf_free == log_sys.buf_next_to_write) {
		/* Nothing to write and no flush to disk requested */
		log_mutex_exit_all();
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
			      log_sys.lsn));
	if (flush_to_disk) {
		log_sys.n_pending_flushes++;
		log_sys.current_flush_lsn = log_sys.lsn;
		MONITOR_INC(MONITOR_PENDING_LOG_FLUSH);
		os_event_reset(log_sys.flush_event);

		if (log_sys.buf_free == log_sys.buf_next_to_write) {
			/* Nothing to write, flush only */
			log_mutex_exit_all();
			log_write_flush_to_disk_low();
			log_mutex_exit();
			return;
		}
	}

	start_offset = log_sys.buf_next_to_write;
	end_offset = log_sys.buf_free;

	area_start = ut_calc_align_down(start_offset, OS_FILE_LOG_BLOCK_SIZE);
	area_end = ut_calc_align(end_offset, OS_FILE_LOG_BLOCK_SIZE);

	ut_ad(area_end - area_start > 0);

	log_block_set_flush_bit(log_sys.buf + area_start, TRUE);
	log_block_set_checkpoint_no(
		log_sys.buf + area_end - OS_FILE_LOG_BLOCK_SIZE,
		log_sys.next_checkpoint_no);

	write_lsn = log_sys.lsn;
	write_buf = log_sys.buf;

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
					       LSN_PF "," LSN_PF,
					       log_sys.write_lsn, lsn);
	}

	if (log_sys.is_encrypted()) {
		log_crypt(write_buf + area_start, log_sys.write_lsn,
			  area_end - area_start);
	}

	/* Do the write to the log files */
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


	if (srv_file_flush_method == SRV_O_DSYNC) {
		/* O_SYNC means the OS did not buffer the log file at all:
		so we have also flushed to disk what we have written */
		log_sys.flushed_to_disk_lsn = log_sys.write_lsn;
	}

	log_write_mutex_exit();

	if (flush_to_disk) {
		log_write_flush_to_disk_low();
		ib_uint64_t flush_lsn = log_sys.flushed_to_disk_lsn;
		log_mutex_exit();

		innobase_mysql_log_notify(flush_lsn);
	}
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

/****************************************************************//**
This functions writes the log buffer to the log file and if 'flush'
is set it forces a flush of the log file as well. This is meant to be
called from background master thread only as it does not wait for
the write (+ possible flush) to finish. */
void
log_buffer_sync_in_background(
/*==========================*/
	bool	flush)	/*!< in: flush the logs to disk */
{
	lsn_t	lsn;

	log_mutex_enter();

	lsn = log_sys.lsn;

	if (flush
	    && log_sys.n_pending_flushes > 0
	    && log_sys.current_flush_lsn >= lsn) {
		/* The write + flush will write enough */
		log_mutex_exit();
		return;
	}

	log_mutex_exit();

	log_write_up_to(lsn, flush);
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
		lsn = log_sys.lsn;
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
static
bool
log_preflush_pool_modified_pages(
	lsn_t			new_oldest)
{
	bool	success;

	if (recv_recovery_on) {
		/* If the recovery is running, we must first apply all
		log records to their respective file pages to get the
		right modify lsn values to these pages: otherwise, there
		might be pages on disk which are not yet recovered to the
		current lsn, and even after calling this function, we could
		not know how up-to-date the disk version of the database is,
		and we could not make a new checkpoint on the basis of the
		info on the buffer pool only. */
		recv_apply_hashed_log_recs(true);
	}

	if (new_oldest == LSN_MAX
	    || !buf_page_cleaner_is_active
	    || srv_is_being_started) {

		ulint	n_pages;

		success = buf_flush_lists(ULINT_MAX, new_oldest, &n_pages);

		buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);

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

/******************************************************//**
Completes a checkpoint. */
static
void
log_complete_checkpoint(void)
/*=========================*/
{
	ut_ad(log_mutex_own());
	ut_ad(log_sys.n_pending_checkpoint_writes == 0);

	log_sys.next_checkpoint_no++;

	log_sys.last_checkpoint_lsn = log_sys.next_checkpoint_lsn;
	MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE,
		    log_sys.lsn - log_sys.last_checkpoint_lsn);

	DBUG_PRINT("ib_log", ("checkpoint ended at " LSN_PF
			      ", flushed to " LSN_PF,
			      log_sys.last_checkpoint_lsn,
			      log_sys.flushed_to_disk_lsn));

	rw_lock_x_unlock_gen(&(log_sys.checkpoint_lock), LOG_CHECKPOINT);
}

/** Complete an asynchronous checkpoint write. */
void log_t::complete_checkpoint()
{
	ut_ad(this == &log_sys);
	MONITOR_DEC(MONITOR_PENDING_CHECKPOINT_WRITE);

	log_mutex_enter();

	ut_ad(n_pending_checkpoint_writes > 0);

	if (!--n_pending_checkpoint_writes) {
		log_complete_checkpoint();
	}

	log_mutex_exit();
}

/** Write checkpoint info to the log header.
@param[in]	end_lsn	start LSN of the MLOG_CHECKPOINT mini-transaction */
static
void
log_group_checkpoint(lsn_t end_lsn)
{
	lsn_t		lsn_offset;

	ut_ad(!srv_read_only_mode);
	ut_ad(log_mutex_own());
	ut_ad(end_lsn == 0 || end_lsn >= log_sys.next_checkpoint_lsn);
	ut_ad(end_lsn <= log_sys.lsn);
	ut_ad(end_lsn + SIZE_OF_MLOG_CHECKPOINT <= log_sys.lsn
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

	lsn_offset = log_sys.log.calc_lsn_offset(log_sys.next_checkpoint_lsn);
	mach_write_to_8(buf + LOG_CHECKPOINT_OFFSET, lsn_offset);
	mach_write_to_8(buf + LOG_CHECKPOINT_LOG_BUF_SIZE,
			srv_log_buffer_size);
	mach_write_to_8(buf + LOG_CHECKPOINT_END_LSN, end_lsn);

	log_block_set_checksum(buf, log_block_calc_checksum_crc32(buf));

	MONITOR_INC(MONITOR_PENDING_CHECKPOINT_WRITE);

	log_sys.n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	ut_ad(LOG_CHECKPOINT_1 < srv_page_size);
	ut_ad(LOG_CHECKPOINT_2 < srv_page_size);

	if (log_sys.n_pending_checkpoint_writes++ == 0) {
		rw_lock_x_lock_gen(&log_sys.checkpoint_lock,
				   LOG_CHECKPOINT);
	}

	/* Note: We alternate the physical place of the checkpoint info.
	See the (next_checkpoint_no & 1) below. */

	fil_io(IORequestLogWrite, false,
	       page_id_t(SRV_LOG_SPACE_FIRST_ID, 0),
	       univ_page_size,
	       (log_sys.next_checkpoint_no & 1)
	       ? LOG_CHECKPOINT_2 : LOG_CHECKPOINT_1,
	       OS_FILE_LOG_BLOCK_SIZE,
	       buf, reinterpret_cast<void*>(1) /* checkpoint write */);
}

/** Read a log group header page to log_sys.checkpoint_buf.
@param[in]	header	0 or LOG_CHECKPOINT_1 or LOG_CHECKPOINT2 */
void log_header_read(ulint header)
{
	ut_ad(log_mutex_own());

	log_sys.n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	fil_io(IORequestLogRead, true,
	       page_id_t(SRV_LOG_SPACE_FIRST_ID,
			 header >> srv_page_size_shift),
	       univ_page_size, header & (srv_page_size - 1),
	       OS_FILE_LOG_BLOCK_SIZE, log_sys.checkpoint_buf, NULL);
}

/** Write checkpoint info to the log header and invoke log_mutex_exit().
@param[in]	sync	whether to wait for the write to complete
@param[in]	end_lsn	start LSN of the MLOG_CHECKPOINT mini-transaction */
void
log_write_checkpoint_info(bool sync, lsn_t end_lsn)
{
	ut_ad(log_mutex_own());
	ut_ad(!srv_read_only_mode);

	log_group_checkpoint(end_lsn);

	log_mutex_exit();

	MONITOR_INC(MONITOR_NUM_CHECKPOINT);

	if (sync) {
		/* Wait for the checkpoint write to complete */
		rw_lock_s_lock(&log_sys.checkpoint_lock);
		rw_lock_s_unlock(&log_sys.checkpoint_lock);

		DBUG_EXECUTE_IF(
			"crash_after_checkpoint",
			DBUG_SUICIDE(););
	}
}

/** Set extra data to be written to the redo log during checkpoint.
@param[in]	buf	data to be appended on checkpoint, or NULL
@return pointer to previous data to be appended on checkpoint */
mtr_buf_t*
log_append_on_checkpoint(
	mtr_buf_t*	buf)
{
	log_mutex_enter();
	mtr_buf_t*	old = log_sys.append_on_checkpoint;
	log_sys.append_on_checkpoint = buf;
	log_mutex_exit();
	return(old);
}

/** Make a checkpoint. Note that this function does not flush dirty
blocks from the buffer pool: it only checks what is lsn of the oldest
modification in the pool, and writes information about the lsn in
log files. Use log_make_checkpoint_at() to flush also the pool.
@param[in]	sync		whether to wait for the write to complete
@param[in]	write_always	force a write even if no log
has been generated since the latest checkpoint
@return true if success, false if a checkpoint write was already running */
bool
log_checkpoint(
	bool	sync,
	bool	write_always)
{
	lsn_t	oldest_lsn;

	ut_ad(!srv_read_only_mode);

	DBUG_EXECUTE_IF("no_checkpoint",
			/* We sleep for a long enough time, forcing
			the checkpoint doesn't happen any more. */
			os_thread_sleep(360000000););

	if (recv_recovery_is_on()) {
		recv_apply_hashed_log_recs(true);
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
	if (!write_always
	    && oldest_lsn
	    <= log_sys.last_checkpoint_lsn + SIZE_OF_MLOG_CHECKPOINT) {
		/* Do nothing, because nothing was logged (other than
		a MLOG_CHECKPOINT marker) since the previous checkpoint. */
		log_mutex_exit();
		return(true);
	}
	/* Repeat the MLOG_FILE_NAME records after the checkpoint, in
	case some log records between the checkpoint and log_sys.lsn
	need them. Finally, write a MLOG_CHECKPOINT marker. Redo log
	apply expects to see a MLOG_CHECKPOINT after the checkpoint,
	except on clean shutdown, where the log will be empty after
	the checkpoint.
	It is important that we write out the redo log before any
	further dirty pages are flushed to the tablespace files.  At
	this point, because log_mutex_own(), mtr_commit() in other
	threads will be blocked, and no pages can be added to the
	flush lists. */
	lsn_t		flush_lsn	= oldest_lsn;
	const lsn_t	end_lsn		= log_sys.lsn;
	const bool	do_write
		= srv_shutdown_state == SRV_SHUTDOWN_NONE
		|| flush_lsn != end_lsn;

	if (fil_names_clear(flush_lsn, do_write)) {
		ut_ad(log_sys.lsn >= end_lsn + SIZE_OF_MLOG_CHECKPOINT);
		flush_lsn = log_sys.lsn;
	}

	log_mutex_exit();

	log_write_up_to(flush_lsn, true);

	DBUG_EXECUTE_IF(
		"using_wa_checkpoint_middle",
		if (write_always) {
			DEBUG_SYNC_C("wa_checkpoint_middle");

			const my_bool b = TRUE;
			buf_flush_page_cleaner_disabled_debug_update(
				NULL, NULL, NULL, &b);
			dict_stats_disabled_debug_update(
				NULL, NULL, NULL, &b);
			srv_master_thread_disabled_debug_update(
				NULL, NULL, NULL, &b);
		});

	log_mutex_enter();

	ut_ad(log_sys.flushed_to_disk_lsn >= flush_lsn);
	ut_ad(flush_lsn >= oldest_lsn);

	if (log_sys.last_checkpoint_lsn >= oldest_lsn) {
		log_mutex_exit();
		return(true);
	}

	if (log_sys.n_pending_checkpoint_writes > 0) {
		/* A checkpoint write is running */
		log_mutex_exit();

		if (sync) {
			/* Wait for the checkpoint write to complete */
			rw_lock_s_lock(&log_sys.checkpoint_lock);
			rw_lock_s_unlock(&log_sys.checkpoint_lock);
		}

		return(false);
	}

	log_sys.next_checkpoint_lsn = oldest_lsn;
	log_write_checkpoint_info(sync, end_lsn);
	ut_ad(!log_mutex_own());

	return(true);
}

/** Make a checkpoint at or after a specified LSN.
@param[in]	lsn		the log sequence number, or LSN_MAX
for the latest LSN
@param[in]	write_always	force a write even if no log
has been generated since the latest checkpoint */
void
log_make_checkpoint_at(
	lsn_t			lsn,
	bool			write_always)
{
	/* Preflush pages synchronously */

	while (!log_preflush_pool_modified_pages(lsn)) {
		/* Flush as much as we can */
	}

	while (!log_checkpoint(true, write_always)) {
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
	lsn_t		age;
	lsn_t		checkpoint_age;
	ib_uint64_t	advance;
	lsn_t		oldest_lsn;
	bool		success;
loop:
	advance = 0;

	log_mutex_enter();
	ut_ad(!recv_no_log_write);

	if (!log_sys.check_flush_or_checkpoint) {
		log_mutex_exit();
		return;
	}

	oldest_lsn = log_buf_pool_get_oldest_modification();

	age = log_sys.lsn - oldest_lsn;

	if (age > log_sys.max_modified_age_sync) {

		/* A flush is urgent: we have to do a synchronous preflush */
		advance = age - log_sys.max_modified_age_sync;
	}

	checkpoint_age = log_sys.lsn - log_sys.last_checkpoint_lsn;

	bool	checkpoint_sync;
	bool	do_checkpoint;

	if (checkpoint_age > log_sys.max_checkpoint_age) {
		/* A checkpoint is urgent: we do it synchronously */
		checkpoint_sync = true;
		do_checkpoint = true;
	} else if (checkpoint_age > log_sys.max_checkpoint_age_async) {
		/* A checkpoint is not urgent: do it asynchronously */
		do_checkpoint = true;
		checkpoint_sync = false;
		log_sys.check_flush_or_checkpoint = false;
	} else {
		do_checkpoint = false;
		checkpoint_sync = false;
		log_sys.check_flush_or_checkpoint = false;
	}

	log_mutex_exit();

	if (advance) {
		lsn_t	new_oldest = oldest_lsn + advance;

		success = log_preflush_pool_modified_pages(new_oldest);

		/* If the flush succeeded, this thread has done its part
		and can proceed. If it did not succeed, there was another
		thread doing a flush at the same time. */
		if (!success) {
			log_mutex_enter();
			log_sys.check_flush_or_checkpoint = true;
			log_mutex_exit();
			goto loop;
		}
	}

	if (do_checkpoint) {
		log_checkpoint(checkpoint_sync, FALSE);

		if (checkpoint_sync) {

			goto loop;
		}
	}
}

/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */
void
log_check_margins(void)
{
	bool	check;

	do {
		log_flush_margin();
		log_checkpoint_margin();
		log_mutex_enter();
		ut_ad(!recv_no_log_write);
		check = log_sys.check_flush_or_checkpoint;
		log_mutex_exit();
	} while (check);
}

/****************************************************************//**
Makes a checkpoint at the latest lsn and writes it to first page of each
data file in the database, so that we know that the file spaces contain
all modifications up to that lsn. This can only be called at database
shutdown. This function also writes all log in log files to the log archive. */
void
logs_empty_and_mark_files_at_shutdown(void)
/*=======================================*/
{
	lsn_t			lsn;
	ulint			count = 0;

	ib::info() << "Starting shutdown...";

	/* Wait until the master thread and all other operations are idle: our
	algorithm only works if the server is idle at shutdown */

	srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;
loop:
	ut_ad(lock_sys.is_initialised() || !srv_was_started);
	ut_ad(log_sys.is_initialised() || !srv_was_started);
	ut_ad(fil_system.is_initialised() || !srv_was_started);
	os_event_set(srv_buf_resize_event);

	if (!srv_read_only_mode) {
		os_event_set(srv_error_event);
		os_event_set(srv_monitor_event);
		os_event_set(srv_buf_dump_event);
		if (lock_sys.timeout_thread_active) {
			os_event_set(lock_sys.timeout_event);
		}
		if (dict_stats_event) {
			os_event_set(dict_stats_event);
		} else {
			ut_ad(!srv_dict_stats_thread_active);
		}
		if (recv_sys && recv_sys->flush_start) {
			/* This is in case recv_writer_thread was never
			started, or buf_flush_page_cleaner_coordinator
			failed to notice its termination. */
			os_event_set(recv_sys->flush_start);
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

	if (srv_error_monitor_active) {
		thread_name = "srv_error_monitor_thread";
	} else if (srv_monitor_active) {
		thread_name = "srv_monitor_thread";
	} else if (srv_buf_resize_thread_active) {
		thread_name = "buf_resize_thread";
		goto wait_suspend_loop;
	} else if (srv_dict_stats_thread_active) {
		thread_name = "dict_stats_thread";
	} else if (lock_sys.timeout_thread_active) {
		thread_name = "lock_wait_timeout_thread";
	} else if (srv_buf_dump_thread_active) {
		thread_name = "buf_dump_thread";
		goto wait_suspend_loop;
	} else if (btr_defragment_thread_active) {
		thread_name = "btr_defragment_thread";
	} else if (srv_fast_shutdown != 2 && trx_rollback_is_active) {
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

	switch (srv_get_active_thread_type()) {
	case SRV_NONE:
		if (!srv_n_fil_crypt_threads_started) {
			srv_shutdown_state = SRV_SHUTDOWN_FLUSH_PHASE;
			break;
		}
		os_event_set(fil_crypt_threads_event);
		thread_name = "fil_crypt_thread";
		goto wait_suspend_loop;
	case SRV_PURGE:
	case SRV_WORKER:
		ut_ad(!"purge was not shut down");
		srv_purge_wakeup();
		thread_name = "purge thread";
		goto wait_suspend_loop;
	case SRV_MASTER:
		thread_name = "master thread";
		goto wait_suspend_loop;
	}

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

	if (log_scrub_thread_active) {
		ut_ad(!srv_read_only_mode);
		os_event_set(log_scrub_event);
	}

	if (log_sys.is_initialised()) {
		log_mutex_enter();
		const ulint	n_write	= log_sys.n_pending_checkpoint_writes;
		const ulint	n_flush	= log_sys.n_pending_flushes;
		log_mutex_exit();

		if (log_scrub_thread_active || n_write || n_flush) {
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

	ut_ad(!log_scrub_thread_active);

	if (!buf_pool_ptr) {
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
		log_make_checkpoint_at(LSN_MAX, TRUE);

		log_mutex_enter();

		lsn = log_sys.lsn;

		const bool lsn_changed = lsn != log_sys.last_checkpoint_lsn;
		ut_ad(lsn >= log_sys.last_checkpoint_lsn);

		log_mutex_exit();

		if (lsn_changed) {
			goto loop;
		}

		/* Ensure that all buffered changes are written to the
		redo log before fil_close_all_files(). */
		fil_flush_file_spaces(FIL_TYPE_LOG);
	} else {
		lsn = srv_start_lsn;
	}

	srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

	/* Make some checks that the server really is quiet */
	ut_a(srv_get_active_thread_type() == SRV_NONE);

	service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
				       "Free innodb buffer pool");
	buf_all_freed();

	ut_a(lsn == log_sys.lsn
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);

	if (lsn < srv_start_lsn) {
		ib::error() << "Shutdown LSN=" << lsn
			<< " is less than start LSN=" << srv_start_lsn;
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
	ut_a(srv_get_active_thread_type() == SRV_NONE);

	ut_a(lsn == log_sys.lsn
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
}

/******************************************************//**
Peeks the current lsn.
@return TRUE if success, FALSE if could not get the log system mutex */
ibool
log_peek_lsn(
/*=========*/
	lsn_t*	lsn)	/*!< out: if returns TRUE, current lsn is here */
{
	if (0 == mutex_enter_nowait(&(log_sys.mutex))) {
		*lsn = log_sys.lsn;

		log_mutex_exit();

		return(TRUE);
	}

	return(FALSE);
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
		log_sys.lsn,
		log_sys.flushed_to_disk_lsn,
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
		log_sys.n_pending_flushes,
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

  os_event_destroy(flush_event);

  rw_lock_free(&checkpoint_lock);
  /* rw_lock_free() already called checkpoint_lock.~rw_lock_t();
  tame the debug assertions when the destructor will be called once more. */
  ut_ad(checkpoint_lock.magic_n == 0);
  ut_d(checkpoint_lock.magic_n = RW_LOCK_MAGIC_N);

  mutex_free(&mutex);
  mutex_free(&write_mutex);
  mutex_free(&log_flush_order_mutex);

  if (!srv_read_only_mode && srv_scrub_log)
    os_event_destroy(log_scrub_event);

  recv_sys_close();
}

/******************************************************//**
Pads the current log block full with dummy log records. Used in producing
consistent archived log files and scrubbing redo log. */
static
void
log_pad_current_log_block(void)
/*===========================*/
{
	byte		b		= MLOG_DUMMY_RECORD;
	ulint		pad_length;
	ulint		i;
	lsn_t		lsn;

	ut_ad(!recv_no_log_write);
	/* We retrieve lsn only because otherwise gcc crashed on HP-UX */
	lsn = log_reserve_and_open(OS_FILE_LOG_BLOCK_SIZE);

	pad_length = OS_FILE_LOG_BLOCK_SIZE
		- (log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE)
		- LOG_BLOCK_TRL_SIZE;
	if (pad_length
	    == (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE
		- LOG_BLOCK_TRL_SIZE)) {

		pad_length = 0;
	}

	if (pad_length) {
		srv_stats.n_log_scrubs.inc();
	}

	for (i = 0; i < pad_length; i++) {
		log_write_low(&b, 1);
	}

	lsn = log_sys.lsn;

	log_close();

	ut_a(lsn % OS_FILE_LOG_BLOCK_SIZE == LOG_BLOCK_HDR_SIZE);
}

/*****************************************************************//*
If no log record has been written for a while, fill current log
block with dummy records. */
static
void
log_scrub()
/*=========*/
{
	log_mutex_enter();
	ulint cur_lbn = log_block_convert_lsn_to_no(log_sys.lsn);

	if (next_lbn_to_pad == cur_lbn)
	{
		log_pad_current_log_block();
	}

	next_lbn_to_pad = log_block_convert_lsn_to_no(log_sys.lsn);
	log_mutex_exit();
}

/* log scrubbing speed, in bytes/sec */
UNIV_INTERN ulonglong innodb_scrub_log_speed;

/*****************************************************************//**
This is the main thread for log scrub. It waits for an event and
when waked up fills current log block with dummy records and
sleeps again.
@return this function does not return, it calls os_thread_exit() */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(log_scrub_thread)(void*)
{
	ut_ad(!srv_read_only_mode);

	while (srv_shutdown_state < SRV_SHUTDOWN_FLUSH_PHASE) {
		/* log scrubbing interval in µs. */
		ulonglong interval = 1000*1000*512/innodb_scrub_log_speed;

		os_event_wait_time(log_scrub_event, static_cast<ulint>(interval));

		log_scrub();

		os_event_reset(log_scrub_event);
	}

	log_scrub_thread_active = false;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}
