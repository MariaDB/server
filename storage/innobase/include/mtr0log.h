/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2019, MariaDB Corporation.

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
@file include/mtr0log.h
Mini-transaction logging routines

Created 12/7/1995 Heikki Tuuri
*******************************************************/

#ifndef mtr0log_h
#define mtr0log_h

#include "mtr0mtr.h"
#include "dyn0buf.h"

// Forward declaration
struct dict_index_t;

/********************************************************//**
Catenates 1 - 4 bytes to the mtr log. The value is not compressed. */
UNIV_INLINE
void
mlog_catenate_ulint(
/*================*/
	mtr_buf_t*	dyn_buf,	/*!< in/out: buffer to write */
	ulint		val,		/*!< in: value to write */
	mlog_id_t	type);		/*!< in: type of value to write */
/********************************************************//**
Catenates 1 - 4 bytes to the mtr log. */
UNIV_INLINE
void
mlog_catenate_ulint(
/*================*/
	mtr_t*		mtr,	/*!< in: mtr */
	ulint		val,	/*!< in: value to write */
	mlog_id_t	type);	/*!< in: MLOG_1BYTE, MLOG_2BYTES, MLOG_4BYTES */
/********************************************************//**
Catenates n bytes to the mtr log. */
void
mlog_catenate_string(
/*=================*/
	mtr_t*		mtr,	/*!< in: mtr */
	const byte*	str,	/*!< in: string to write */
	ulint		len);	/*!< in: string length */
/********************************************************//**
Catenates a compressed ulint to mlog. */
UNIV_INLINE
void
mlog_catenate_ulint_compressed(
/*===========================*/
	mtr_t*		mtr,	/*!< in: mtr */
	ulint		val);	/*!< in: value to write */
/********************************************************//**
Opens a buffer to mlog. It must be closed with mlog_close.
@return buffer, NULL if log mode MTR_LOG_NONE */
UNIV_INLINE
byte*
mlog_open(
/*======*/
	mtr_t*		mtr,	/*!< in: mtr */
	ulint		size);	/*!< in: buffer size in bytes; MUST be
				smaller than DYN_ARRAY_DATA_SIZE! */
/********************************************************//**
Closes a buffer opened to mlog. */
UNIV_INLINE
void
mlog_close(
/*=======*/
	mtr_t*		mtr,	/*!< in: mtr */
	byte*		ptr);	/*!< in: buffer space from ptr up was
				not used */

/** Write 1, 2, 4, or 8 bytes to a file page.
@param[in]      block   file page
@param[in,out]  ptr     pointer in file page
@param[in]      val     value to write
@tparam l       number of bytes to write
@tparam w       write request type
@tparam V       type of val */
template<unsigned l,mtr_t::write_type w,typename V>
inline void mtr_t::write(const buf_block_t &block, byte *ptr, V val)
{
  ut_ad(ut_align_down(ptr, srv_page_size) == block.frame);
  ut_ad(m_log_mode == MTR_LOG_NONE || m_log_mode == MTR_LOG_NO_REDO ||
        !block.page.zip.data ||
        /* written by fil_crypt_rotate_page() or innodb_make_page_dirty()? */
        (w == FORCED && l == 1 && ptr == &block.frame[FIL_PAGE_SPACE_ID]) ||
        mach_read_from_2(block.frame + FIL_PAGE_TYPE) <= FIL_PAGE_TYPE_ZBLOB2);
  static_assert(l == 1 || l == 2 || l == 4 || l == 8, "wrong length");

  switch (l) {
  case 1:
    if (w == OPT && mach_read_from_1(ptr) == val) return;
    ut_ad(w != NORMAL || mach_read_from_1(ptr) != val);
    ut_ad(val == static_cast<byte>(val));
    *ptr= static_cast<byte>(val);
    break;
  case 2:
    ut_ad(val == static_cast<uint16_t>(val));
    if (w == OPT && mach_read_from_2(ptr) == val) return;
    ut_ad(w != NORMAL  || mach_read_from_2(ptr) != val);
    mach_write_to_2(ptr, static_cast<uint16_t>(val));
    break;
  case 4:
    ut_ad(val == static_cast<uint32_t>(val));
    if (w == OPT && mach_read_from_4(ptr) == val) return;
    ut_ad(w != NORMAL  || mach_read_from_4(ptr) != val);
    mach_write_to_4(ptr, static_cast<uint32_t>(val));
    break;
  case 8:
    if (w == OPT && mach_read_from_8(ptr) == val) return;
    ut_ad(w != NORMAL  || mach_read_from_8(ptr) != val);
    mach_write_to_8(ptr, val);
    break;
  }
  byte *log_ptr= mlog_open(this, 11 + 2 + (l == 8 ? 9 : 5));
  if (!log_ptr)
    return;
  if (l == 8)
    log_write(block, ptr, static_cast<mlog_id_t>(l), log_ptr, uint64_t{val});
  else
    log_write(block, ptr, static_cast<mlog_id_t>(l), log_ptr,
              static_cast<uint32_t>(val));
}

/** Write a byte string to a page.
@param[in,out]  b       buffer page
@param[in]      ofs     byte offset from b->frame
@param[in]      str     the data to write
@param[in]      len     length of the data to write */
inline
void mtr_t::memcpy(buf_block_t *b, ulint offset, const void *str, ulint len)
{
  ::memcpy(b->frame + offset, str, len);
  memcpy(*b, offset, len);
}

/** Writes a log record about an operation.
@param[in]	type		redo log record type
@param[in]	space_id	tablespace identifier
@param[in]	page_no		page number
@param[in,out]	log_ptr		current end of mini-transaction log
@param[in,out]	mtr		mini-transaction
@return	end of mini-transaction log */
UNIV_INLINE
byte*
mlog_write_initial_log_record_low(
	mlog_id_t	type,
	ulint		space_id,
	ulint		page_no,
	byte*		log_ptr,
	mtr_t*		mtr);

/********************************************************//**
Writes the initial part of a log record (3..11 bytes).
If the implementation of this function is changed, all
size parameters to mlog_open() should be adjusted accordingly!
@return new value of log_ptr */
UNIV_INLINE
byte*
mlog_write_initial_log_record_fast(
/*===============================*/
	const byte*	ptr,	/*!< in: pointer to (inside) a buffer
				frame holding the file page where
				modification is made */
	mlog_id_t	type,	/*!< in: log item type: MLOG_1BYTE, ... */
	byte*		log_ptr,/*!< in: pointer to mtr log which has
				been opened */
	mtr_t*		mtr);	/*!< in: mtr */
/********************************************************//**
Parses an initial log record written by mlog_write_initial_log_record_low().
@return parsed record end, NULL if not a complete record */
const byte*
mlog_parse_initial_log_record(
/*==========================*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	mlog_id_t*	type,	/*!< out: log record type: MLOG_1BYTE, ... */
	ulint*		space,	/*!< out: space id */
	ulint*		page_no);/*!< out: page number */
/********************************************************//**
Parses a log record written by mtr_t::write(), mtr_t::memset().
@return parsed record end, NULL if not a complete record */
const byte*
mlog_parse_nbytes(
/*==============*/
	mlog_id_t	type,	/*!< in: log record type: MLOG_1BYTE, ... */
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	byte*		page,	/*!< in: page where to apply the log record,
				or NULL */
	void*		page_zip);/*!< in/out: compressed page, or NULL */
/********************************************************//**
Parses a log record written by mtr_t::memcpy().
@return parsed record end, NULL if not a complete record */
const byte*
mlog_parse_string(
/*==============*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	byte*		page,	/*!< in: page where to apply the log record,
				or NULL */
	void*		page_zip);/*!< in/out: compressed page, or NULL */

/********************************************************//**
Opens a buffer for mlog, writes the initial log record and,
if needed, the field lengths of an index.  Reserves space
for further log entries.  The log entry must be closed with
mtr_close().
@return buffer, NULL if log mode MTR_LOG_NONE */
byte*
mlog_open_and_write_index(
/*======================*/
	mtr_t*			mtr,	/*!< in: mtr */
	const byte*		rec,	/*!< in: index record or page */
	const dict_index_t*	index,	/*!< in: record descriptor */
	mlog_id_t		type,	/*!< in: log item type */
	ulint			size);	/*!< in: requested buffer size in bytes
					(if 0, calls mlog_close() and
					returns NULL) */

/********************************************************//**
Parses a log record written by mlog_open_and_write_index.
@return parsed record end, NULL if not a complete record */
const byte*
mlog_parse_index(
/*=============*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	bool		comp,	/*!< in: TRUE=compact record format */
	dict_index_t**	index);	/*!< out, own: dummy index */

/** Insert, update, and maybe other functions may use this value to define an
extra mlog buffer size for variable size data */
#define MLOG_BUF_MARGIN	256

#include "mtr0log.ic"

#endif /* mtr0log_h */
