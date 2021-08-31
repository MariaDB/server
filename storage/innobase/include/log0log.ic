/*****************************************************************************

Copyright (c) 1995, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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
@file include/log0log.ic
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#include "mach0data.h"
#include "assume_aligned.h"
#include "ut0crc32.h"

extern ulong srv_log_buffer_size;

/************************************************************//**
Gets a log block flush bit.
@return TRUE if this block was the first to be written in a log flush */
UNIV_INLINE
ibool
log_block_get_flush_bit(
/*====================*/
	const byte*	log_block)	/*!< in: log block */
{
  static_assert(LOG_BLOCK_HDR_NO == 0, "compatibility");
  static_assert(LOG_BLOCK_FLUSH_BIT_MASK == 0x80000000, "compatibility");

  return *log_block & 0x80;
}

/************************************************************//**
Sets the log block flush bit. */
UNIV_INLINE
void
log_block_set_flush_bit(
/*====================*/
	byte*	log_block,	/*!< in/out: log block */
	ibool	val)		/*!< in: value to set */
{
  static_assert(LOG_BLOCK_HDR_NO == 0, "compatibility");
  static_assert(LOG_BLOCK_FLUSH_BIT_MASK == 0x80000000, "compatibility");

  if (val)
    *log_block|= 0x80;
  else
    *log_block&= 0x7f;
}

/************************************************************//**
Gets a log block number stored in the header.
@return log block number stored in the block header */
UNIV_INLINE
ulint
log_block_get_hdr_no(
/*=================*/
	const byte*	log_block)	/*!< in: log block */
{
  static_assert(LOG_BLOCK_HDR_NO == 0, "compatibility");
  return mach_read_from_4(my_assume_aligned<4>(log_block)) &
    ~LOG_BLOCK_FLUSH_BIT_MASK;
}

/************************************************************//**
Sets the log block number stored in the header; NOTE that this must be set
before the flush bit! */
UNIV_INLINE
void
log_block_set_hdr_no(
/*=================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	n)		/*!< in: log block number: must be > 0 and
				< LOG_BLOCK_FLUSH_BIT_MASK */
{
  static_assert(LOG_BLOCK_HDR_NO == 0, "compatibility");
  ut_ad(n > 0);
  ut_ad(n < LOG_BLOCK_FLUSH_BIT_MASK);

  mach_write_to_4(my_assume_aligned<4>(log_block), n);
}

/************************************************************//**
Gets a log block data length.
@return log block data length measured as a byte offset from the block start */
UNIV_INLINE
ulint
log_block_get_data_len(
/*===================*/
	const byte*	log_block)	/*!< in: log block */
{
  return mach_read_from_2(my_assume_aligned<2>
                          (log_block + LOG_BLOCK_HDR_DATA_LEN));
}

/************************************************************//**
Sets the log block data length. */
UNIV_INLINE
void
log_block_set_data_len(
/*===================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	len)		/*!< in: data length */
{
  mach_write_to_2(my_assume_aligned<2>(log_block + LOG_BLOCK_HDR_DATA_LEN),
                  len);
}

/************************************************************//**
Gets a log block first mtr log record group offset.
@return first mtr log record group byte offset from the block start, 0
if none */
UNIV_INLINE
ulint
log_block_get_first_rec_group(
/*==========================*/
	const byte*	log_block)	/*!< in: log block */
{
  return mach_read_from_2(my_assume_aligned<2>
                          (log_block + LOG_BLOCK_FIRST_REC_GROUP));
}

/************************************************************//**
Sets the log block first mtr log record group offset. */
UNIV_INLINE
void
log_block_set_first_rec_group(
/*==========================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	offset)		/*!< in: offset, 0 if none */
{
  mach_write_to_2(my_assume_aligned<2>
                  (log_block + LOG_BLOCK_FIRST_REC_GROUP), offset);
}

/************************************************************//**
Gets a log block checkpoint number field (4 lowest bytes).
@return checkpoint no (4 lowest bytes) */
UNIV_INLINE
ulint
log_block_get_checkpoint_no(
/*========================*/
	const byte*	log_block)	/*!< in: log block */
{
  return mach_read_from_4(my_assume_aligned<4>
                          (log_block + LOG_BLOCK_CHECKPOINT_NO));
}

/************************************************************//**
Sets a log block checkpoint number field (4 lowest bytes). */
UNIV_INLINE
void
log_block_set_checkpoint_no(
/*========================*/
	byte*		log_block,	/*!< in/out: log block */
	ib_uint64_t	no)		/*!< in: checkpoint no */
{
  mach_write_to_4(my_assume_aligned<4>(log_block + LOG_BLOCK_CHECKPOINT_NO),
                  static_cast<uint32_t>(no));
}

/************************************************************//**
Converts a lsn to a log block number.
@return log block number, it is > 0 and <= 1G */
UNIV_INLINE
ulint
log_block_convert_lsn_to_no(
/*========================*/
	lsn_t	lsn)	/*!< in: lsn of a byte within the block */
{
	return(((ulint) (lsn / OS_FILE_LOG_BLOCK_SIZE) &
		DBUG_EVALUATE_IF("innodb_small_log_block_no_limit",
			0xFUL, 0x3FFFFFFFUL)) + 1);
}

/** Calculate the CRC-32C checksum of a log block.
@param[in]	block	log block
@return checksum */
inline ulint log_block_calc_checksum_crc32(const byte* block)
{
	return ut_crc32(block, OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_CHECKSUM);
}

/************************************************************//**
Gets a log block checksum field value.
@return checksum */
UNIV_INLINE
ulint
log_block_get_checksum(
/*===================*/
	const byte*	log_block)	/*!< in: log block */
{
  return mach_read_from_4(my_assume_aligned<4>
                          (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_CHECKSUM +
                           log_block));
}

/************************************************************//**
Sets a log block checksum field value. */
UNIV_INLINE
void
log_block_set_checksum(
/*===================*/
	byte*	log_block,	/*!< in/out: log block */
	ulint	checksum)	/*!< in: checksum */
{
  mach_write_to_4(my_assume_aligned<4>
                  (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_CHECKSUM +
                   log_block), checksum);
}

/************************************************************//**
Initializes a log block in the log buffer. */
UNIV_INLINE
void
log_block_init(
/*===========*/
	byte*	log_block,	/*!< in: pointer to the log buffer */
	lsn_t	lsn)		/*!< in: lsn within the log block */
{
	ulint	no;

	no = log_block_convert_lsn_to_no(lsn);

	log_block_set_hdr_no(log_block, no);

	log_block_set_data_len(log_block, LOG_BLOCK_HDR_SIZE);
	log_block_set_first_rec_group(log_block, 0);
}

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
	lsn_t*		start_lsn)
{
	mysql_mutex_assert_owner(&log_sys.mutex);
	ut_ad(len > 0);

	const ulint	data_len = len
		+ log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE;

	if (data_len >= log_sys.trailer_offset()) {

		/* The string does not fit within the current log block
		or the log block would become full */

		return(0);
	}

	lsn_t lsn = log_sys.get_lsn();
	*start_lsn = lsn;

	memcpy(log_sys.buf + log_sys.buf_free, str, len);

	log_block_set_data_len(
                reinterpret_cast<byte*>(ut_align_down(
                        log_sys.buf + log_sys.buf_free,
                        OS_FILE_LOG_BLOCK_SIZE)),
                data_len);

	log_sys.buf_free += len;

	ut_ad(log_sys.buf_free <= size_t{srv_log_buffer_size});

	lsn += len;
	log_sys.set_lsn(lsn);

	return lsn;
}

/***********************************************************************//**
Checks if there is need for a log buffer flush or a new checkpoint, and does
this if yes. Any database operation should call this when it has modified
more than about 4 pages. NOTE that this function may only be called when the
OS thread owns no synchronization objects except dict_sys.latch. */
UNIV_INLINE
void
log_free_check(void)
/*================*/
{
	/* During row_log_table_apply(), this function will be called while we
	are holding some latches. This is OK, as long as we are not holding
	any latches on buffer blocks. */

	if (log_sys.check_flush_or_checkpoint()) {

		log_check_margins();
	}
}
