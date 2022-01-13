/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2009, Google Inc.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
@file include/log0log.h
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#ifndef log0log_h
#define log0log_h

#include "dyn0buf.h"
#include "sync0rw.h"
#include "log0types.h"
#include "os0event.h"
#include "os0file.h"

#ifndef UINT32_MAX
#define UINT32_MAX             (4294967295U)
#endif

/** Redo log group */
struct log_group_t;

/** Magic value to use instead of log checksums when they are disabled */
#define LOG_NO_CHECKSUM_MAGIC 0xDEADBEEFUL

/* Margin for the free space in the smallest log group, before a new query
step which modifies the database, is started */

#define LOG_CHECKPOINT_FREE_PER_THREAD	(4 * UNIV_PAGE_SIZE)
#define LOG_CHECKPOINT_EXTRA_FREE	(8 * UNIV_PAGE_SIZE)

/** Append a string to the log.
@param[in]	str		string
@param[in]	len		string length
@param[out]	start_lsn	start LSN of the log record
@return end lsn of the log record, zero if did not succeed */
UNIV_INLINE
lsn_t
log_reserve_and_write_fast(
	const void*	str,
	ulint		len,
	lsn_t*		start_lsn);
/***********************************************************************//**
Checks if there is need for a log buffer flush or a new checkpoint, and does
this if yes. Any database operation should call this when it has modified
more than about 4 pages. NOTE that this function may only be called when the
OS thread owns no synchronization objects except the dictionary mutex. */
UNIV_INLINE
void
log_free_check(void);
/*================*/

/** Extends the log buffer.
@param[in]	len	requested minimum size in bytes */
void
log_buffer_extend(
	ulint	len);

/** Check margin not to overwrite transaction log from the last checkpoint.
If would estimate the log write to exceed the log_group_capacity,
waits for the checkpoint is done enough.
@param[in]	len	length of the data to be written */

void
log_margin_checkpoint_age(
	ulint	len);

/** Open the log for log_write_low. The log must be closed with log_close.
@param[in]	len	length of the data to be written
@return start lsn of the log record */
lsn_t
log_reserve_and_open(
	ulint	len);
/************************************************************//**
Writes to the log the string given. It is assumed that the caller holds the
log mutex. */
void
log_write_low(
/*==========*/
	const byte*	str,		/*!< in: string */
	ulint		str_len);	/*!< in: string length */
/************************************************************//**
Closes the log.
@return lsn */
lsn_t
log_close(void);
/*===========*/
/************************************************************//**
Gets the current lsn.
@return current lsn */
UNIV_INLINE
lsn_t
log_get_lsn(void);
/*=============*/
/************************************************************//**
Gets the current lsn.
@return	current lsn */
UNIV_INLINE
lsn_t
log_get_lsn_nowait(void);
/*=============*/
/************************************************************//**
Gets the last lsn that is fully flushed to disk.
@return	last flushed lsn */
UNIV_INLINE
ib_uint64_t
log_get_flush_lsn(void);
/*=============*/
/****************************************************************
Gets the log group capacity. It is OK to read the value without
holding log_sys->mutex because it is constant.
@return log group capacity */
UNIV_INLINE
lsn_t
log_get_capacity(void);
/*==================*/
/****************************************************************
Get log_sys::max_modified_age_async. It is OK to read the value without
holding log_sys::mutex because it is constant.
@return max_modified_age_async */
UNIV_INLINE
lsn_t
log_get_max_modified_age_async(void);
/*================================*/
/** Initializes the redo logging subsystem. */
void
log_sys_init();

/** Initialize the redo log.
@param[in]	n_files		number of files */
void
log_init(ulint n_files);
/** Calculate the recommended highest values for lsn - last_checkpoint_lsn
and lsn - buf_get_oldest_modification().
@param[in]	file_size	requested innodb_log_file_size
@retval true on success
@retval false if the smallest log group is too small to
accommodate the number of OS threads in the database server */
bool
log_set_capacity(ulonglong file_size)
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************//**
Completes an i/o to a log file. */
void
log_io_complete(
/*============*/
	log_group_t*	group);	/*!< in: log group */
/******************************************************//**
This function is called, e.g., when a transaction wants to commit. It checks
that the log has been written to the log file up to the last log entry written
by the transaction. If there is a flush running, it waits and checks if the
flush flushed enough. If not, starts a new flush. */
void
log_write_up_to(
/*============*/
	lsn_t	lsn,	/*!< in: log sequence number up to which
			the log should be written, LSN_MAX if not specified */
	bool	flush_to_disk);
			/*!< in: true if we want the written log
			also to be flushed to disk */
/** write to the log file up to the last log entry.
@param[in]	sync	whether we want the written log
also to be flushed to disk. */
void log_buffer_flush_to_disk(bool sync= true);


/** Prepare to invoke log_write_and_flush(), before acquiring log_sys.mutex. */
#define log_write_and_flush_prepare() log_write_mutex_enter()

/** Durably write the log up to log_sys.lsn and release log_sys.mutex. */
ATTRIBUTE_COLD void log_write_and_flush();

/****************************************************************//**
This functions writes the log buffer to the log file and if 'flush'
is set it forces a flush of the log file as well. This is meant to be
called from background master thread only as it does not wait for
the write (+ possible flush) to finish. */
void
log_buffer_sync_in_background(
/*==========================*/
	bool	flush);	/*<! in: flush the logs to disk */
/** Make a checkpoint. Note that this function does not flush dirty
blocks from the buffer pool: it only checks what is lsn of the oldest
modification in the pool, and writes information about the lsn in
log files. Use log_make_checkpoint() to flush also the pool.
@param[in]	sync		whether to wait for the write to complete
@return true if success, false if a checkpoint write was already running */
bool log_checkpoint(bool sync);

/** Make a checkpoint */
void log_make_checkpoint();

/****************************************************************//**
Makes a checkpoint at the latest lsn and writes it to first page of each
data file in the database, so that we know that the file spaces contain
all modifications up to that lsn. This can only be called at database
shutdown. This function also writes all log in log files to the log archive. */
void
logs_empty_and_mark_files_at_shutdown(void);
/*=======================================*/
/** Read a log group header page to log_sys->checkpoint_buf.
@param[in]	group	log group
@param[in]	header	0 or LOG_CHEKCPOINT_1 or LOG_CHECKPOINT2 */
void
log_group_header_read(
	const log_group_t*	group,
	ulint			header);
/** Write checkpoint info to the log header and invoke log_mutex_exit().
@param[in]	sync	whether to wait for the write to complete
@param[in]	end_lsn	start LSN of the MLOG_CHECKPOINT mini-transaction */
void
log_write_checkpoint_info(bool sync, lsn_t end_lsn);

/** Set extra data to be written to the redo log during checkpoint.
@param[in]	buf	data to be appended on checkpoint, or NULL
@return pointer to previous data to be appended on checkpoint */
mtr_buf_t*
log_append_on_checkpoint(
	mtr_buf_t*	buf);
/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */
void
log_check_margins(void);

/********************************************************//**
Sets the field values in group to correspond to a given lsn. For this function
to work, the values must already be correctly initialized to correspond to
some lsn, for instance, a checkpoint lsn. */
void
log_group_set_fields(
/*=================*/
	log_group_t*	group,	/*!< in/out: group */
	lsn_t		lsn);	/*!< in: lsn for which the values should be
				set */
/************************************************************//**
Gets a log block flush bit.
@return TRUE if this block was the first to be written in a log flush */
UNIV_INLINE
ibool
log_block_get_flush_bit(
/*====================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Gets a log block number stored in the header.
@return log block number stored in the block header */
UNIV_INLINE
ulint
log_block_get_hdr_no(
/*=================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Gets a log block data length.
@return log block data length measured as a byte offset from the block start */
UNIV_INLINE
ulint
log_block_get_data_len(
/*===================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Sets the log block data length. */
UNIV_INLINE
void
log_block_set_data_len(
/*===================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	len);		/*!< in: data length */

/** Calculates the checksum for a log block using the CRC32 algorithm.
@param[in]	block	log block
@return checksum */
UNIV_INLINE
ulint
log_block_calc_checksum_crc32(
	const byte*	block);

/************************************************************//**
Gets a log block checksum field value.
@return checksum */
UNIV_INLINE
ulint
log_block_get_checksum(
/*===================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Sets a log block checksum field value. */
UNIV_INLINE
void
log_block_set_checksum(
/*===================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	checksum);	/*!< in: checksum */
/************************************************************//**
Gets a log block first mtr log record group offset.
@return first mtr log record group byte offset from the block start, 0
if none */
UNIV_INLINE
ulint
log_block_get_first_rec_group(
/*==========================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Sets the log block first mtr log record group offset. */
UNIV_INLINE
void
log_block_set_first_rec_group(
/*==========================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	offset);	/*!< in: offset, 0 if none */
/************************************************************//**
Gets a log block checkpoint number field (4 lowest bytes).
@return checkpoint no (4 lowest bytes) */
UNIV_INLINE
ulint
log_block_get_checkpoint_no(
/*========================*/
	const byte*	log_block);	/*!< in: log block */
/************************************************************//**
Initializes a log block in the log buffer. */
UNIV_INLINE
void
log_block_init(
/*===========*/
	byte*	log_block,	/*!< in: pointer to the log buffer */
	lsn_t	lsn);		/*!< in: lsn within the log block */
/************************************************************//**
Converts a lsn to a log block number.
@return log block number, it is > 0 and <= 1G */
UNIV_INLINE
ulint
log_block_convert_lsn_to_no(
/*========================*/
	lsn_t	lsn);	/*!< in: lsn of a byte within the block */
/******************************************************//**
Prints info of the log. */
void
log_print(
/*======*/
	FILE*	file);	/*!< in: file where to print */
/******************************************************//**
Peeks the current lsn.
@return TRUE if success, FALSE if could not get the log system mutex */
ibool
log_peek_lsn(
/*=========*/
	lsn_t*	lsn);	/*!< out: if returns TRUE, current lsn is here */
/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */
void
log_refresh_stats(void);
/*===================*/
/********************************************************//**
Closes all log groups. */
void
log_group_close_all(void);
/*=====================*/
/** Shut down the redo log subsystem. */
void
log_shutdown();

/** Whether to require checksums on the redo log pages */
extern my_bool	innodb_log_checksums;

/* Values used as flags */
#define LOG_FLUSH	7652559
#define LOG_CHECKPOINT	78656949

/* The counting of lsn's starts from this value: this must be non-zero */
#define LOG_START_LSN		((lsn_t) (16 * OS_FILE_LOG_BLOCK_SIZE))

/* Offsets of a log block header */
#define	LOG_BLOCK_HDR_NO	0	/* block number which must be > 0 and
					is allowed to wrap around at 2G; the
					highest bit is set to 1 if this is the
					first log block in a log flush write
					segment */
#define LOG_BLOCK_FLUSH_BIT_MASK 0x80000000UL
					/* mask used to get the highest bit in
					the preceding field */
#define	LOG_BLOCK_HDR_DATA_LEN	4	/* number of bytes of log written to
					this block */
#define	LOG_BLOCK_FIRST_REC_GROUP 6	/* offset of the first start of an
					mtr log record group in this log block,
					0 if none; if the value is the same
					as LOG_BLOCK_HDR_DATA_LEN, it means
					that the first rec group has not yet
					been catenated to this log block, but
					if it will, it will start at this
					offset; an archive recovery can
					start parsing the log records starting
					from this offset in this log block,
					if value not 0 */
#define LOG_BLOCK_CHECKPOINT_NO	8	/* 4 lower bytes of the value of
					log_sys->next_checkpoint_no when the
					log block was last written to: if the
					block has not yet been written full,
					this value is only updated before a
					log buffer flush */
#define LOG_BLOCK_HDR_SIZE	12	/* size of the log block header in
					bytes */

/* Offsets of a log block trailer from the end of the block */
#define	LOG_BLOCK_CHECKSUM	4	/* 4 byte checksum of the log block
					contents; in InnoDB versions
					< 3.23.52 this did not contain the
					checksum but the same value as
					.._HDR_NO */
#define	LOG_BLOCK_TRL_SIZE	4	/* trailer size in bytes */

/** Offsets inside the checkpoint pages (redo log format version 1) @{ */
/** Checkpoint number */
#define LOG_CHECKPOINT_NO		0
/** Log sequence number up to which all changes have been flushed */
#define LOG_CHECKPOINT_LSN		8
/** Byte offset of the log record corresponding to LOG_CHECKPOINT_LSN */
#define LOG_CHECKPOINT_OFFSET		16
/** log_sys_t::buf_size at the time of the checkpoint (not used) */
#define LOG_CHECKPOINT_LOG_BUF_SIZE	24
/** MariaDB 10.2.5 encrypted redo log encryption key version (32 bits)*/
#define LOG_CHECKPOINT_CRYPT_KEY	32
/** MariaDB 10.2.5 encrypted redo log random nonce (32 bits) */
#define LOG_CHECKPOINT_CRYPT_NONCE	36
/** MariaDB 10.2.5 encrypted redo log random message (MY_AES_BLOCK_SIZE) */
#define LOG_CHECKPOINT_CRYPT_MESSAGE	40
/** start LSN of the MLOG_CHECKPOINT mini-transaction corresponding
to this checkpoint, or 0 if the information has not been written */
#define LOG_CHECKPOINT_END_LSN		OS_FILE_LOG_BLOCK_SIZE - 16

/* @} */

/** Offsets of a log file header */
/* @{ */
/** Log file header format identifier (32-bit unsigned big-endian integer).
This used to be called LOG_GROUP_ID and always written as 0,
because InnoDB never supported more than one copy of the redo log. */
#define LOG_HEADER_FORMAT	0
/** Redo log subformat (originally 0). In format version 0, the
LOG_FILE_START_LSN started here, 4 bytes earlier than LOG_HEADER_START_LSN,
which the LOG_FILE_START_LSN was renamed to.
Subformat 1 is for the fully redo-logged TRUNCATE
(no MLOG_TRUNCATE records or extra log checkpoints or log files) */
#define LOG_HEADER_SUBFORMAT	4
/** LSN of the start of data in this log file (with format version 1;
in format version 0, it was called LOG_FILE_START_LSN and at offset 4). */
#define LOG_HEADER_START_LSN	8
/** A null-terminated string which will contain either the string 'ibbackup'
and the creation time if the log file was created by mysqlbackup --restore,
or the MySQL version that created the redo log file. */
#define LOG_HEADER_CREATOR	16
/** End of the log file creator field. */
#define LOG_HEADER_CREATOR_END	(LOG_HEADER_CREATOR + 32)
/** Contents of the LOG_HEADER_CREATOR field */
#define LOG_HEADER_CREATOR_CURRENT		\
	"MariaDB "				\
	IB_TO_STR(MYSQL_VERSION_MAJOR) "."	\
	IB_TO_STR(MYSQL_VERSION_MINOR) "."	\
	IB_TO_STR(MYSQL_VERSION_PATCH)

/** The redo log format identifier corresponding to the current format version.
Stored in LOG_HEADER_FORMAT.
To prevent crash-downgrade to earlier 10.2 due to the inability to
roll back a retroactively introduced TRX_UNDO_RENAME_TABLE undo log record,
MariaDB 10.2.18 and later will use the 10.3 format, but LOG_HEADER_SUBFORMAT
1 instead of 0. MariaDB 10.3 will use subformat 0 (5.7-style TRUNCATE) or 2
(MDEV-13564 backup-friendly TRUNCATE). */
#define LOG_HEADER_FORMAT_10_3		103
/** The old MariaDB 10.2.2..10.2.17 log format */
#define LOG_HEADER_FORMAT_10_2		1
/** Future MariaDB 10.4 log format */
#define LOG_HEADER_FORMAT_10_4		104
/** Encrypted MariaDB redo log */
#define LOG_HEADER_FORMAT_ENCRYPTED	(1U<<31)

/* @} */

#define LOG_CHECKPOINT_1	OS_FILE_LOG_BLOCK_SIZE
					/* first checkpoint field in the log
					header; we write alternately to the
					checkpoint fields when we make new
					checkpoints; this field is only defined
					in the first log file of a log group */
#define LOG_CHECKPOINT_2	(3 * OS_FILE_LOG_BLOCK_SIZE)
					/* second checkpoint field in the log
					header */
#define LOG_FILE_HDR_SIZE	(4 * OS_FILE_LOG_BLOCK_SIZE)

/* As long as fil_io() is used to handle log io, log group max size is limited
by (maximum page number) * (minimum page size). Page number type is uint32_t.
Remove this limitation if page number is no longer used for log file io. */
static const ulonglong log_group_max_size =
	((ulonglong(UINT32_MAX) + 1) * UNIV_PAGE_SIZE_MIN - 1);

/** The state of a log group */
enum log_group_state_t {
	/** No corruption detected */
	LOG_GROUP_OK,
	/** Corrupted */
	LOG_GROUP_CORRUPTED
};

typedef ib_mutex_t	LogSysMutex;
typedef ib_mutex_t	FlushOrderMutex;

/** Log group consists of a number of log files, each of the same size; a log
group is implemented as a space in the sense of the module fil0fil.
Currently, this is only protected by log_sys->mutex. However, in the case
of log_write_up_to(), we will access some members only with the protection
of log_sys->write_mutex, which should affect nothing for now. */
struct log_group_t{
	/** number of files in the group */
	ulint				n_files;
	/** format of the redo log: e.g., LOG_HEADER_FORMAT_10_3 */
	uint32_t			format;
	/** redo log subformat: 0 with separately logged TRUNCATE,
	1 with fully redo-logged TRUNCATE */
	uint32_t			subformat;
	/** individual log file size in bytes, including the header */
	lsn_t				file_size;
	/** corruption status */
	log_group_state_t		state;
	/** lsn used to fix coordinates within the log group */
	lsn_t				lsn;
	/** the byte offset of the above lsn */
	lsn_t				lsn_offset;

	/** used only in recovery: recovery scan succeeded up to this
	lsn in this log group */
	lsn_t				scanned_lsn;
	/** unaligned checkpoint header */
	byte*				checkpoint_buf_ptr;
	/** buffer for writing a checkpoint header */
	byte*				checkpoint_buf;

	/** @return whether the redo log is encrypted */
	bool is_encrypted() const
	{
		return((format & LOG_HEADER_FORMAT_ENCRYPTED) != 0);
	}

	/** @return capacity in bytes */
	inline lsn_t capacity() const
	{
		return((file_size - LOG_FILE_HDR_SIZE) * n_files);
	}
};

/** Redo log buffer */
struct log_t{
	char		pad1[CACHE_LINE_SIZE];
					/*!< Padding to prevent other memory
					update hotspots from residing on the
					same memory cache line */
	lsn_t		lsn;		/*!< log sequence number */
	ulint		buf_free;	/*!< first free offset within the log
					buffer in use */

	char		pad2[CACHE_LINE_SIZE];/*!< Padding */
	LogSysMutex	mutex;		/*!< mutex protecting the log */
	char		pad3[CACHE_LINE_SIZE]; /*!< Padding */
	LogSysMutex	write_mutex;	/*!< mutex protecting writing to log
					file and accessing to log_group_t */
	char		pad4[CACHE_LINE_SIZE];/*!< Padding */
	FlushOrderMutex	log_flush_order_mutex;/*!< mutex to serialize access to
					the flush list when we are putting
					dirty blocks in the list. The idea
					behind this mutex is to be able
					to release log_sys->mutex during
					mtr_commit and still ensure that
					insertions in the flush_list happen
					in the LSN order. */
	byte*		buf_ptr;	/*!< unaligned log buffer, which should
					be of double of buf_size */
	byte*		buf;		/*!< log buffer currently in use;
					this could point to either the first
					half of the aligned(buf_ptr) or the
					second half in turns, so that log
					write/flush to disk don't block
					concurrent mtrs which will write
					log to this buffer */
	bool		first_in_use;	/*!< true if buf points to the first
					half of the aligned(buf_ptr), false
					if the second half */
	ulint		buf_size;	/*!< log buffer size of each in bytes */
	ulint		max_buf_free;	/*!< recommended maximum value of
					buf_free for the buffer in use, after
					which the buffer is flushed */
	bool		check_flush_or_checkpoint;
					/*!< this is set when there may
					be need to flush the log buffer, or
					preflush buffer pool pages, or make
					a checkpoint; this MUST be TRUE when
					lsn - last_checkpoint_lsn >
					max_checkpoint_age; this flag is
					peeked at by log_free_check(), which
					does not reserve the log mutex */
	/** the redo log */
	log_group_t			log;

	/** The fields involved in the log buffer flush @{ */

	ulint		buf_next_to_write;/*!< first offset in the log buffer
					where the byte content may not exist
					written to file, e.g., the start
					offset of a log record catenated
					later; this is advanced when a flush
					operation is completed to all the log
					groups */
	lsn_t		write_lsn;	/*!< last written lsn */
	lsn_t		current_flush_lsn;/*!< end lsn for the current running
					write + flush operation */
	lsn_t		flushed_to_disk_lsn;
					/*!< how far we have written the log
					AND flushed to disk */
	ulint		n_pending_flushes;/*!< number of currently
					pending flushes; protected by
					log_sys_t::mutex */
	os_event_t	flush_event;	/*!< this event is in the reset state
					when a flush is running;
					os_event_set() and os_event_reset()
					are protected by log_sys_t::mutex */
	ulint		n_log_ios;	/*!< number of log i/os initiated thus
					far */
	ulint		n_log_ios_old;	/*!< number of log i/o's at the
					previous printout */
	time_t		last_printout_time;/*!< when log_print was last time
					called */
	/* @} */

	/** Fields involved in checkpoints @{ */
	lsn_t		log_group_capacity; /*!< capacity of the log group; if
					the checkpoint age exceeds this, it is
					a serious error because it is possible
					we will then overwrite log and spoil
					crash recovery */
	lsn_t		max_modified_age_async;
					/*!< when this recommended
					value for lsn -
					buf_pool_get_oldest_modification()
					is exceeded, we start an
					asynchronous preflush of pool pages */
	lsn_t		max_modified_age_sync;
					/*!< when this recommended
					value for lsn -
					buf_pool_get_oldest_modification()
					is exceeded, we start a
					synchronous preflush of pool pages */
	lsn_t		max_checkpoint_age_async;
					/*!< when this checkpoint age
					is exceeded we start an
					asynchronous writing of a new
					checkpoint */
	lsn_t		max_checkpoint_age;
					/*!< this is the maximum allowed value
					for lsn - last_checkpoint_lsn when a
					new query step is started */
	ib_uint64_t	next_checkpoint_no;
					/*!< next checkpoint number */
	lsn_t		last_checkpoint_lsn;
					/*!< latest checkpoint lsn */
	lsn_t		next_checkpoint_lsn;
					/*!< next checkpoint lsn */
	mtr_buf_t*	append_on_checkpoint;
					/*!< extra redo log records to write
					during a checkpoint, or NULL if none.
					The pointer is protected by
					log_sys->mutex, and the data must
					remain constant as long as this
					pointer is not NULL. */
	ulint		n_pending_checkpoint_writes;
					/*!< number of currently pending
					checkpoint writes */
	rw_lock_t	checkpoint_lock;/*!< this latch is x-locked when a
					checkpoint write is running; a thread
					should wait for this without owning
					the log mutex */
	byte*		checkpoint_buf_ptr;/* unaligned checkpoint header */
	byte*		checkpoint_buf;	/*!< checkpoint header is read to this
					buffer */
	/* @} */

	/** @return whether the redo log is encrypted */
	bool is_encrypted() const
	{
		return(log.is_encrypted());
	}
};

/** Redo log system */
extern log_t*	log_sys;

/** Test if flush order mutex is owned. */
#define log_flush_order_mutex_own()			\
	mutex_own(&log_sys->log_flush_order_mutex)

/** Acquire the flush order mutex. */
#define log_flush_order_mutex_enter() do {		\
	mutex_enter(&log_sys->log_flush_order_mutex);	\
} while (0)
/** Release the flush order mutex. */
# define log_flush_order_mutex_exit() do {		\
	mutex_exit(&log_sys->log_flush_order_mutex);	\
} while (0)

/** Test if log sys mutex is owned. */
#define log_mutex_own() mutex_own(&log_sys->mutex)

/** Test if log sys write mutex is owned. */
#define log_write_mutex_own() mutex_own(&log_sys->write_mutex)

/** Acquire the log sys mutex. */
#define log_mutex_enter() mutex_enter(&log_sys->mutex)

/** Acquire the log sys write mutex. */
#define log_write_mutex_enter() mutex_enter(&log_sys->write_mutex)

/** Acquire all the log sys mutexes. */
#define log_mutex_enter_all() do {		\
	mutex_enter(&log_sys->write_mutex);	\
	mutex_enter(&log_sys->mutex);		\
} while (0)

/** Release the log sys mutex. */
#define log_mutex_exit() mutex_exit(&log_sys->mutex)

/** Release the log sys write mutex.*/
#define log_write_mutex_exit() mutex_exit(&log_sys->write_mutex)

/** Release all the log sys mutexes. */
#define log_mutex_exit_all() do {		\
	mutex_exit(&log_sys->mutex);		\
	mutex_exit(&log_sys->write_mutex);	\
} while (0)

/** Calculate the offset of an lsn within a log group.
@param[in]	lsn	log sequence number
@param[in]	group	log group
@return offset within the log group */
lsn_t
log_group_calc_lsn_offset(
	lsn_t			lsn,
	const log_group_t*	group);

/* log scrubbing speed, in bytes/sec */
extern ulonglong innodb_scrub_log_speed;

/** Event to wake up log_scrub_thread */
extern os_event_t	log_scrub_event;
/** Whether log_scrub_thread is active */
extern bool		log_scrub_thread_active;

#include "log0log.inl"

#endif
