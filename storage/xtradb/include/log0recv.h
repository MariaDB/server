/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation.

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
@file include/log0recv.h
Recovery

Created 9/20/1997 Heikki Tuuri
*******************************************************/

#ifndef log0recv_h
#define log0recv_h

#include "univ.i"
#include "ut0byte.h"
#include "buf0types.h"
#include "hash0hash.h"
#include "log0log.h"
#include <list>

/******************************************************//**
Checks the 4-byte checksum to the trailer checksum field of a log
block.  We also accept a log block in the old format before
InnoDB-3.23.52 where the checksum field contains the log block number.
@return TRUE if ok, or if the log block may be in the format of InnoDB
version predating 3.23.52 */
UNIV_INTERN
ibool
log_block_checksum_is_ok_or_old_format(
/*===================================*/
	const byte*	block,	/*!< in: pointer to a log block */
	bool		print_err); /*!< in print error ? */

/*******************************************************//**
Calculates the new value for lsn when more data is added to the log. */
UNIV_INTERN
ib_uint64_t
recv_calc_lsn_on_data_add(
/*======================*/
	lsn_t		lsn,	/*!< in: old lsn */
	ib_uint64_t	len);	/*!< in: this many bytes of data is
				added, log block headers not included */

#ifdef UNIV_HOTBACKUP
extern ibool	recv_replay_file_ops;

/*******************************************************************//**
Reads the checkpoint info needed in hot backup.
@return	TRUE if success */
UNIV_INTERN
ibool
recv_read_checkpoint_info_for_backup(
/*=================================*/
	const byte*	hdr,	/*!< in: buffer containing the log group
				header */
	lsn_t*		lsn,	/*!< out: checkpoint lsn */
	lsn_t*		offset,	/*!< out: checkpoint offset in the log group */
	lsn_t*		cp_no,	/*!< out: checkpoint number */
	lsn_t*		first_header_lsn)
				/*!< out: lsn of of the start of the
				first log file */
	MY_ATTRIBUTE((nonnull));
/*******************************************************************//**
Scans the log segment and n_bytes_scanned is set to the length of valid
log scanned. */
UNIV_INTERN
void
recv_scan_log_seg_for_backup(
/*=========================*/
	byte*		buf,		/*!< in: buffer containing log data */
	ulint		buf_len,	/*!< in: data length in that buffer */
	lsn_t*		scanned_lsn,	/*!< in/out: lsn of buffer start,
					we return scanned lsn */
	ulint*		scanned_checkpoint_no,
					/*!< in/out: 4 lowest bytes of the
					highest scanned checkpoint number so
					far */
	ulint*		n_bytes_scanned);/*!< out: how much we were able to
					scan, smaller than buf_len if log
					data ended here */
#endif /* UNIV_HOTBACKUP */
/*******************************************************************//**
Returns TRUE if recovery is currently running.
@return	recv_recovery_on */
UNIV_INLINE
ibool
recv_recovery_is_on(void);
/*=====================*/
/************************************************************************//**
Applies the hashed log records to the page, if the page lsn is less than the
lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool. */
UNIV_INTERN
void
recv_recover_page_func(
/*===================*/
#ifndef UNIV_HOTBACKUP
	ibool		just_read_in,
				/*!< in: TRUE if the i/o handler calls
				this for a freshly read page */
#endif /* !UNIV_HOTBACKUP */
	buf_block_t*	block);	/*!< in/out: buffer block */
#ifndef UNIV_HOTBACKUP
/** Wrapper for recv_recover_page_func().
Applies the hashed log records to the page, if the page lsn is less than the
lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool.
@param jri	in: TRUE if just read in (the i/o handler calls this for
a freshly read page)
@param block	in/out: the buffer block
*/
# define recv_recover_page(jri, block)	recv_recover_page_func(jri, block)
#else /* !UNIV_HOTBACKUP */
/** Wrapper for recv_recover_page_func().
Applies the hashed log records to the page, if the page lsn is less than the
lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool.
@param jri	in: TRUE if just read in (the i/o handler calls this for
a freshly read page)
@param block	in/out: the buffer block
*/
# define recv_recover_page(jri, block)	recv_recover_page_func(block)
#endif /* !UNIV_HOTBACKUP */
/********************************************************//**
Recovers from a checkpoint. When this function returns, the database is able
to start processing of new user transactions, but the function
recv_recovery_from_checkpoint_finish should be called later to complete
the recovery and free the resources used in it.
@return	error code or DB_SUCCESS */
UNIV_INTERN
dberr_t
recv_recovery_from_checkpoint_start_func(
/*=====================================*/
#ifdef UNIV_LOG_ARCHIVE
	ulint		type,		/*!< in: LOG_CHECKPOINT or
					LOG_ARCHIVE */
	lsn_t		limit_lsn,	/*!< in: recover up to this lsn
					if possible */
#endif /* UNIV_LOG_ARCHIVE */
	lsn_t		min_flushed_lsn,/*!< in: min flushed lsn from
					data files */
	lsn_t		max_flushed_lsn);/*!< in: max flushed lsn from
					 data files */
#ifdef UNIV_LOG_ARCHIVE
/** Wrapper for recv_recovery_from_checkpoint_start_func().
Recovers from a checkpoint. When this function returns, the database is able
to start processing of new user transactions, but the function
recv_recovery_from_checkpoint_finish should be called later to complete
the recovery and free the resources used in it.
@param type	in: LOG_CHECKPOINT or LOG_ARCHIVE
@param lim	in: recover up to this log sequence number if possible
@param min	in: minimum flushed log sequence number from data files
@param max	in: maximum flushed log sequence number from data files
@return	error code or DB_SUCCESS */
# define recv_recovery_from_checkpoint_start(type,lim,min,max)		\
	recv_recovery_from_checkpoint_start_func(type,lim,min,max)
#else /* UNIV_LOG_ARCHIVE */
/** Wrapper for recv_recovery_from_checkpoint_start_func().
Recovers from a checkpoint. When this function returns, the database is able
to start processing of new user transactions, but the function
recv_recovery_from_checkpoint_finish should be called later to complete
the recovery and free the resources used in it.
@param type	ignored: LOG_CHECKPOINT or LOG_ARCHIVE
@param lim	ignored: recover up to this log sequence number if possible
@param min	in: minimum flushed log sequence number from data files
@param max	in: maximum flushed log sequence number from data files
@return	error code or DB_SUCCESS */
# define recv_recovery_from_checkpoint_start(type,lim,min,max)		\
	recv_recovery_from_checkpoint_start_func(min,max)
#endif /* UNIV_LOG_ARCHIVE */
/********************************************************//**
Completes recovery from a checkpoint. */
UNIV_INTERN
void
recv_recovery_from_checkpoint_finish(void);
/*======================================*/
/********************************************************//**
Initiates the rollback of active transactions. */
UNIV_INTERN
void
recv_recovery_rollback_active(void);
/*===============================*/

/*******************************************************************//**
Tries to parse a single log record and returns its length.
@return	length of the record, or 0 if the record was not complete */
UNIV_INTERN
ulint
recv_parse_log_rec(
/*===============*/
	byte*	ptr,	/*!< in: pointer to a buffer */
	byte*	end_ptr,/*!< in: pointer to the buffer end */
	byte*	type,	/*!< out: type */
	ulint*	space,	/*!< out: space id */
	ulint*	page_no,/*!< out: page number */
	byte**	body);	/*!< out: log record body start */

/*******************************************************//**
Scans log from a buffer and stores new log data to the parsing buffer.
Parses and hashes the log records if new data found.  Unless
UNIV_HOTBACKUP is defined, this function will apply log records
automatically when the hash table becomes full.
@return TRUE if limit_lsn has been reached, or not able to scan any
more in this log group */
UNIV_INTERN
ibool
recv_scan_log_recs(
/*===============*/
	ulint		available_memory,/*!< in: we let the hash table of recs
					to grow to this size, at the maximum */
	ibool		store_to_hash,	/*!< in: TRUE if the records should be
					stored to the hash table; this is set
					to FALSE if just debug checking is
					needed */
	const byte*	buf,		/*!< in: buffer containing a log
					segment or garbage */
	ulint		len,		/*!< in: buffer length */
	lsn_t		start_lsn,	/*!< in: buffer start lsn */
	lsn_t*		contiguous_lsn,	/*!< in/out: it is known that all log
					groups contain contiguous log data up
					to this lsn */
	lsn_t*		group_scanned_lsn);/*!< out: scanning succeeded up to
					this lsn */
/******************************************************//**
Resets the logs. The contents of log files will be lost! */
UNIV_INTERN
void
recv_reset_logs(
/*============*/
#ifdef UNIV_LOG_ARCHIVE
	ulint		arch_log_no,	/*!< in: next archived log file number */
	ibool		new_logs_created,/*!< in: TRUE if resetting logs
					is done at the log creation;
					FALSE if it is done after
					archive recovery */
#endif /* UNIV_LOG_ARCHIVE */
	lsn_t		lsn);		/*!< in: reset to this lsn
					rounded up to be divisible by
					OS_FILE_LOG_BLOCK_SIZE, after
					which we add
					LOG_BLOCK_HDR_SIZE */
#ifdef UNIV_HOTBACKUP
/******************************************************//**
Creates new log files after a backup has been restored. */
UNIV_INTERN
void
recv_reset_log_files_for_backup(
/*============================*/
	const char*	log_dir,	/*!< in: log file directory path */
	ulint		n_log_files,	/*!< in: number of log files */
	lsn_t		log_file_size,	/*!< in: log file size */
	lsn_t		lsn);		/*!< in: new start lsn, must be
					divisible by OS_FILE_LOG_BLOCK_SIZE */
#endif /* UNIV_HOTBACKUP */
/********************************************************//**
Creates the recovery system. */
UNIV_INTERN
void
recv_sys_create(void);
/*=================*/
/**********************************************************//**
Release recovery system mutexes. */
UNIV_INTERN
void
recv_sys_close(void);
/*================*/
/********************************************************//**
Frees the recovery system memory. */
UNIV_INTERN
void
recv_sys_mem_free(void);
/*===================*/
/********************************************************//**
Inits the recovery system for a recovery operation. */
UNIV_INTERN
void
recv_sys_init(
/*==========*/
	ulint	available_memory);	/*!< in: available memory in bytes */
#ifndef UNIV_HOTBACKUP
/********************************************************//**
Reset the state of the recovery system variables. */
UNIV_INTERN
void
recv_sys_var_init(void);
/*===================*/
#endif /* !UNIV_HOTBACKUP */
/** Apply the hash table of stored log records to persistent data pages.
@param[in]	last_batch	whether the change buffer merge will be
				performed as part of the operation */
UNIV_INTERN
void
recv_apply_hashed_log_recs(bool last_batch);
#ifdef UNIV_HOTBACKUP
/*******************************************************************//**
Applies log records in the hash table to a backup. */
UNIV_INTERN
void
recv_apply_log_recs_for_backup(void);
/*================================*/
#endif

/** Block of log record data */
struct recv_data_t{
	recv_data_t*	next;	/*!< pointer to the next block or NULL */
				/*!< the log record data is stored physically
				immediately after this struct, max amount
				RECV_DATA_BLOCK_SIZE bytes of it */
};

/** Stored log record struct */
struct recv_t{
	byte		type;	/*!< log record type */
	ulint		len;	/*!< log record body length in bytes */
	recv_data_t*	data;	/*!< chain of blocks containing the log record
				body */
	lsn_t		start_lsn;/*!< start lsn of the log segment written by
				the mtr which generated this log record: NOTE
				that this is not necessarily the start lsn of
				this log record */
	lsn_t		end_lsn;/*!< end lsn of the log segment written by
				the mtr which generated this log record: NOTE
				that this is not necessarily the end lsn of
				this log record */
	UT_LIST_NODE_T(recv_t)
			rec_list;/*!< list of log records for this page */
};

/** States of recv_addr_t */
enum recv_addr_state {
	/** not yet processed */
	RECV_NOT_PROCESSED,
	/** page is being read */
	RECV_BEING_READ,
	/** log records are being applied on the page */
	RECV_BEING_PROCESSED,
	/** log records have been applied on the page, or they have
	been discarded because the tablespace does not exist */
	RECV_PROCESSED
};

/** Hashed page file address struct */
struct recv_addr_t{
	enum recv_addr_state state;
				/*!< recovery state of the page */
	unsigned	space:32;/*!< space id */
	unsigned	page_no:32;/*!< page number */
	UT_LIST_BASE_NODE_T(recv_t)
			rec_list;/*!< list of log records for this page */
	hash_node_t	addr_hash;/*!< hash node in the hash bucket chain */
};

struct recv_dblwr_t {
	void add(byte* page);

	byte* find_page(ulint space_id, ulint page_no);

	std::list<byte *> pages; /* Pages from double write buffer */

	void operator() () {
		pages.clear();
	}
};

/** Recovery system data structure */
struct recv_sys_t{
#ifndef UNIV_HOTBACKUP
	ib_mutex_t		mutex;	/*!< mutex protecting the fields apply_log_recs,
				n_addrs, and the state field in each recv_addr
				struct */
	ib_mutex_t		writer_mutex;/*!< mutex coordinating
				flushing between recv_writer_thread and
				the recovery thread. */
#endif /* !UNIV_HOTBACKUP */
	ibool		apply_log_recs;
				/*!< this is TRUE when log rec application to
				pages is allowed; this flag tells the
				i/o-handler if it should do log record
				application */
	ibool		apply_batch_on;
				/*!< this is TRUE when a log rec application
				batch is running */
	lsn_t		lsn;	/*!< log sequence number */
	ulint		last_log_buf_size;
				/*!< size of the log buffer when the database
				last time wrote to the log */
	byte*		last_block;
				/*!< possible incomplete last recovered log
				block */
	byte*		last_block_buf_start;
				/*!< the nonaligned start address of the
				preceding buffer */
	byte*		buf;	/*!< buffer for parsing log records */
	ulint		len;	/*!< amount of data in buf */
	lsn_t		parse_start_lsn;
				/*!< this is the lsn from which we were able to
				start parsing log records and adding them to
				the hash table; zero if a suitable
				start point not found yet */
	lsn_t		scanned_lsn;
				/*!< the log data has been scanned up to this
				lsn */
	ulint		scanned_checkpoint_no;
				/*!< the log data has been scanned up to this
				checkpoint number (lowest 4 bytes) */
	ulint		recovered_offset;
				/*!< start offset of non-parsed log records in
				buf */
	lsn_t		recovered_lsn;
				/*!< the log records have been parsed up to
				this lsn */
	lsn_t		limit_lsn;/*!< recovery should be made at most
				up to this lsn */
	ibool		found_corrupt_log;
				/*!< this is set to TRUE if we during log
				scan find a corrupt log block, or a corrupt
				log record, or there is a log parsing
				buffer overflow */
	/** the time when progress was last reported */
	ib_time_t	progress_time;
#ifdef UNIV_LOG_ARCHIVE
	log_group_t*	archive_group;
				/*!< in archive recovery: the log group whose
				archive is read */
#endif /* !UNIV_LOG_ARCHIVE */
	mem_heap_t*	heap;	/*!< memory heap of log records and file
				addresses*/
	hash_table_t*	addr_hash;/*!< hash table of file addresses of pages */
	ulint		n_addrs;/*!< number of not processed hashed file
				addresses in the hash table */

	recv_dblwr_t	dblwr;

	/** Determine whether redo log recovery progress should be reported.
	@param[in]	time	the current time
	@return	whether progress should be reported
		(the last report was at least 15 seconds ago) */
	bool report(ib_time_t time)
	{
		if (time - progress_time < 15) {
			return false;
		}

		progress_time = time;
		return true;
	}
};

/** The recovery system */
extern recv_sys_t*	recv_sys;

/** TRUE when applying redo log records during crash recovery; FALSE
otherwise.  Note that this is FALSE while a background thread is
rolling back incomplete transactions. */
extern ibool		recv_recovery_on;
/** If the following is TRUE, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this becomes TRUE if
the log record hash table becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

TRUE means that recovery is running and no operations on the log files
are allowed yet: the variable name is misleading. */
extern ibool		recv_no_ibuf_operations;
/** TRUE when recv_init_crash_recovery() has been called. */
extern ibool		recv_needed_recovery;
#ifdef UNIV_DEBUG
/** TRUE if writing to the redo log (mtr_commit) is forbidden.
Protected by log_sys->mutex. */
extern ibool		recv_no_log_write;
#endif /* UNIV_DEBUG */

/** TRUE if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially FALSE, and set by
recv_recovery_from_checkpoint_start_func(). */
extern ibool		recv_lsn_checks_on;
#ifdef UNIV_HOTBACKUP
/** TRUE when the redo log is being backed up */
extern ibool		recv_is_making_a_backup;
#endif /* UNIV_HOTBACKUP */
/** Maximum page number encountered in the redo log */
extern ulint		recv_max_parsed_page_no;

/** Size of the parsing buffer; it must accommodate RECV_SCAN_SIZE many
times! */
#define RECV_PARSING_BUF_SIZE	(2 * 1024 * 1024)

/** Size of block reads when the log groups are scanned forward to do a
roll-forward */
#define RECV_SCAN_SIZE		(4 * UNIV_PAGE_SIZE)

/** This many frames must be left free in the buffer pool when we scan
the log and store the scanned log records in the buffer pool: we will
use these free frames to read in pages when we start applying the
log records to the database. */
extern ulint	recv_n_pool_free_frames;

#ifndef UNIV_NONINL
#include "log0recv.ic"
#endif

#endif
