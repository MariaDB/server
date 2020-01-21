/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file mtr/mtr0log.cc
Mini-transaction log routines

Created 12/7/1995 Heikki Tuuri
*******************************************************/

#include "mtr0log.h"
#include "buf0buf.h"
#include "dict0dict.h"
#include "log0recv.h"
#include "page0page.h"
#include "buf0dblwr.h"
#include "dict0boot.h"

/********************************************************//**
Parses an initial log record written by mtr_t::write_low().
@return parsed record end, NULL if not a complete record */
const byte*
mlog_parse_initial_log_record(
/*==========================*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	mlog_id_t*	type,	/*!< out: log record type: MLOG_1BYTE, ... */
	ulint*		space,	/*!< out: space id */
	ulint*		page_no)/*!< out: page number */
{
	if (end_ptr < ptr + 1) {

		return(NULL);
	}

	*type = mlog_id_t(*ptr & ~MLOG_SINGLE_REC_FLAG);
	if (UNIV_UNLIKELY(*type > MLOG_BIGGEST_TYPE
			  && !EXTRA_CHECK_MLOG_NUMBER(*type))) {
		recv_sys.found_corrupt_log = true;
		return NULL;
	}

	ptr++;

	if (end_ptr < ptr + 2) {

		return(NULL);
	}

	*space = mach_parse_compressed(&ptr, end_ptr);

	if (ptr != NULL) {
		*page_no = mach_parse_compressed(&ptr, end_ptr);
	}

	return(const_cast<byte*>(ptr));
}

/********************************************************//**
Parses a log record written by mtr_t::write(), mtr_t::memset().
@return parsed record end, NULL if not a complete record or a corrupt record */
const byte*
mlog_parse_nbytes(
/*==============*/
	mlog_id_t	type,	/*!< in: log record type: MLOG_1BYTE, ... */
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	byte*		page,	/*!< in: page where to apply the log
				record, or NULL */
	void*		page_zip)/*!< in/out: compressed page, or NULL */
{
	ulint		offset;
	ulint		val;
	ib_uint64_t	dval;

	ut_ad(type <= MLOG_8BYTES || type == MLOG_MEMSET);
	ut_a(!page || !page_zip
	     || type == MLOG_MEMSET
	     || !fil_page_index_page_check(page));
	if (end_ptr < ptr + 2) {
		return NULL;
	}

	offset = mach_read_from_2(ptr);
	ptr += 2;

	if (UNIV_UNLIKELY(offset >= srv_page_size)) {
		goto corrupt;
	}

	switch (type) {
	case MLOG_MEMSET:
		if (end_ptr < ptr + 3) {
			return NULL;
		}
		val = mach_read_from_2(ptr);
		ptr += 2;
		if (UNIV_UNLIKELY(offset + val > srv_page_size)) {
			goto corrupt;
		}
		if (page) {
			memset(page + offset, *ptr, val);
			if (page_zip) {
				ut_ad(offset + val <= PAGE_DATA
				      || !fil_page_index_page_check(page));
				memset(static_cast<page_zip_des_t*>(page_zip)
				       ->data + offset, *ptr, val);
			}
		}
		return const_cast<byte*>(++ptr);
	case MLOG_8BYTES:
		dval = mach_u64_parse_compressed(&ptr, end_ptr);

		if (ptr == NULL) {
			return NULL;
		}

		if (page) {
			if (page_zip) {
				mach_write_to_8
					(((page_zip_des_t*) page_zip)->data
					 + offset, dval);
			}
			mach_write_to_8(page + offset, dval);
		}

		return const_cast<byte*>(ptr);
	default:
		val = mach_parse_compressed(&ptr, end_ptr);
	}

	if (ptr == NULL) {
		return NULL;
	}

	switch (type) {
	case MLOG_1BYTE:
		if (val > 0xFFUL) {
			goto corrupt;
		}
		if (page) {
			if (page_zip) {
				mach_write_to_1
					(((page_zip_des_t*) page_zip)->data
					 + offset, val);
			}
			mach_write_to_1(page + offset, val);
		}
		break;
	case MLOG_2BYTES:
		if (val > 0xFFFFUL) {
			goto corrupt;
		}
		if (page) {
			if (page_zip) {
				mach_write_to_2
					(((page_zip_des_t*) page_zip)->data
					 + offset, val);
			}
			mach_write_to_2(page + offset, val);
		}

		break;
	case MLOG_4BYTES:
		if (page) {
			if (page_zip) {
				mach_write_to_4
					(((page_zip_des_t*) page_zip)->data
					 + offset, val);
			}
			mach_write_to_4(page + offset, val);
		}
		break;
	default:
	corrupt:
		recv_sys.found_corrupt_log = true;
		ptr = NULL;
	}

	return const_cast<byte*>(ptr);
}

/**
Write a log record for writing 1, 2, 4, or 8 bytes.
@param[in]      type    number of bytes to write
@param[in]      block   file page
@param[in]      ptr     pointer within block.frame
@param[in,out]  l       log record buffer
@return new end of mini-transaction log */
byte *mtr_t::log_write_low(mlog_id_t type, const buf_block_t &block,
                           const byte *ptr, byte *l)
{
  ut_ad(type == MLOG_1BYTE || type == MLOG_2BYTES || type == MLOG_4BYTES ||
        type == MLOG_8BYTES);
  ut_ad(block.page.state == BUF_BLOCK_FILE_PAGE);
  ut_ad(ptr >= block.frame + FIL_PAGE_OFFSET);
  ut_ad(ptr + unsigned(type) <=
        &block.frame[srv_page_size - FIL_PAGE_DATA_END]);
  l= log_write_low(type, block.page.id, l);
  mach_write_to_2(l, page_offset(ptr));
  return l + 2;
}

/**
Write a log record for writing 1, 2, or 4 bytes.
@param[in]      block   file page
@param[in,out]  ptr     pointer in file page
@param[in]      l       number of bytes to write
@param[in,out]  log_ptr log record buffer
@param[in]      val     value to write */
void mtr_t::log_write(const buf_block_t &block, byte *ptr, mlog_id_t l,
                      byte *log_ptr, uint32_t val)
{
  ut_ad(l == MLOG_1BYTE || l == MLOG_2BYTES || l == MLOG_4BYTES);
  log_ptr= log_write_low(l, block, ptr, log_ptr);
  log_ptr+= mach_write_compressed(log_ptr, val);
  m_log.close(log_ptr);
}

/**
Write a log record for writing 8 bytes.
@param[in]      block   file page
@param[in,out]  ptr     pointer in file page
@param[in]      l       number of bytes to write
@param[in,out]  log_ptr log record buffer
@param[in]      val     value to write */
void mtr_t::log_write(const buf_block_t &block, byte *ptr, mlog_id_t l,
                      byte *log_ptr, uint64_t val)
{
  ut_ad(l == MLOG_8BYTES);
  log_ptr= log_write_low(l, block, ptr, log_ptr);
  log_ptr+= mach_u64_write_compressed(log_ptr, val);
  m_log.close(log_ptr);
}

/** Log a write of a byte string to a page.
@param[in]      b       buffer page
@param[in]      ofs     byte offset from b->frame
@param[in]      len     length of the data to write */
void mtr_t::memcpy(const buf_block_t &b, ulint ofs, ulint len)
{
  ut_ad(len);
  ut_ad(ofs <= ulint(srv_page_size));
  ut_ad(ofs + len <= ulint(srv_page_size));

  set_modified();
  if (m_log_mode != MTR_LOG_ALL)
  {
    ut_ad(m_log_mode == MTR_LOG_NONE || m_log_mode == MTR_LOG_NO_REDO);
    return;
  }

  ut_ad(ofs + len < PAGE_DATA || !b.page.zip.data ||
        mach_read_from_2(b.frame + FIL_PAGE_TYPE) <= FIL_PAGE_TYPE_ZBLOB2);

  byte *l= log_write_low(MLOG_WRITE_STRING, b.page.id, m_log.open(11 + 2 + 2));
  mach_write_to_2(l, ofs);
  mach_write_to_2(l + 2, len);
  m_log.close(l + 4);
  m_log.push(b.frame + ofs, static_cast<uint32_t>(len));
}

/** Write a byte string to a ROW_FORMAT=COMPRESSED page.
@param[in]      b       ROW_FORMAT=COMPRESSED index page
@param[in]      ofs     byte offset from b.zip.data
@param[in]      len     length of the data to write */
void mtr_t::zmemcpy(const buf_page_t &b, ulint offset, ulint len)
{
  ut_ad(page_zip_simple_validate(&b.zip));
  ut_ad(len);
  ut_ad(offset + len <= page_zip_get_size(&b.zip));
  ut_ad(mach_read_from_2(b.zip.data + FIL_PAGE_TYPE) == FIL_PAGE_INDEX ||
        mach_read_from_2(b.zip.data + FIL_PAGE_TYPE) == FIL_PAGE_RTREE);

  set_modified();
  if (m_log_mode != MTR_LOG_ALL)
  {
    ut_ad(m_log_mode == MTR_LOG_NONE || m_log_mode == MTR_LOG_NO_REDO);
    return;
  }

  byte *l= log_write_low(MLOG_ZIP_WRITE_STRING, b.id, m_log.open(11 + 2 + 2));
  mach_write_to_2(l, offset);
  mach_write_to_2(l + 2, len);
  m_log.close(l + 4);
  m_log.push(b.zip.data + offset, static_cast<uint32_t>(len));
}

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
	void*		page_zip)/*!< in/out: compressed page, or NULL */
{
	ulint	offset;
	ulint	len;

	ut_a(!page || !page_zip
	     || (fil_page_get_type(page) != FIL_PAGE_INDEX
		 && fil_page_get_type(page) != FIL_PAGE_RTREE));

	if (end_ptr < ptr + 4) {

		return(NULL);
	}

	offset = mach_read_from_2(ptr);
	ptr += 2;
	len = mach_read_from_2(ptr);
	ptr += 2;

	if (offset >= srv_page_size || len + offset > srv_page_size) {
		recv_sys.found_corrupt_log = TRUE;

		return(NULL);
	}

	if (end_ptr < ptr + len) {

		return(NULL);
	}

	if (page) {
		if (page_zip) {
			memcpy(((page_zip_des_t*) page_zip)->data
				+ offset, ptr, len);
		}
		memcpy(page + offset, ptr, len);
	}

	return(ptr + len);
}

/** Initialize a string of bytes.
@param[in,out]  b       buffer page
@param[in]      ofs     byte offset from block->frame
@param[in]      len     length of the data to write
@param[in]      val     the data byte to write */
void mtr_t::memset(const buf_block_t* b, ulint ofs, ulint len, byte val)
{
  ut_ad(len);
  ut_ad(ofs <= ulint(srv_page_size));
  ut_ad(ofs + len <= ulint(srv_page_size));
  ut_ad(ofs + len < PAGE_DATA || !b->page.zip.data ||
        mach_read_from_2(b->frame + FIL_PAGE_TYPE) <= FIL_PAGE_TYPE_ZBLOB2);
  ::memset(ofs + b->frame, val, len);

  set_modified();
  if (m_log_mode != MTR_LOG_ALL)
  {
    ut_ad(m_log_mode == MTR_LOG_NONE || m_log_mode == MTR_LOG_NO_REDO);
    return;
  }

  byte *l= log_write_low(MLOG_MEMSET, b->page.id, m_log.open(11 + 2 + 2 + 1));
  mach_write_to_2(l, ofs);
  mach_write_to_2(l + 2, len);
  l[4]= val;
  m_log.close(l + 5);
}

/********************************************************//**
Parses a log record written by mlog_open_and_write_index.
@return parsed record end, NULL if not a complete record */
ATTRIBUTE_COLD /* only used when crash-upgrading */
const byte*
mlog_parse_index(
/*=============*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	bool		comp,	/*!< in: TRUE=compact row format */
	dict_index_t**	index)	/*!< out, own: dummy index */
{
	ulint		i, n, n_uniq;
	dict_table_t*	table;
	dict_index_t*	ind;
	ulint		n_core_fields = 0;

	if (comp) {
		if (end_ptr < ptr + 4) {
			return(NULL);
		}
		n = mach_read_from_2(ptr);
		ptr += 2;
		if (n & 0x8000) { /* record after instant ADD COLUMN */
			n &= 0x7FFF;

			n_core_fields = mach_read_from_2(ptr);

			if (!n_core_fields || n_core_fields > n) {
				recv_sys.found_corrupt_log = TRUE;
				return(NULL);
			}

			ptr += 2;

			if (end_ptr < ptr + 2) {
				return(NULL);
			}
		}

		n_uniq = mach_read_from_2(ptr);
		ptr += 2;
		ut_ad(n_uniq <= n);
		if (end_ptr < ptr + n * 2) {
			return(NULL);
		}
	} else {
		n = n_uniq = 1;
	}
	table = dict_mem_table_create("LOG_DUMMY", NULL, n, 0,
				      comp ? DICT_TF_COMPACT : 0, 0);
	ind = dict_mem_index_create(table, "LOG_DUMMY", 0, n);
	ind->n_uniq = (unsigned int) n_uniq;
	if (n_uniq != n) {
		ut_a(n_uniq + DATA_ROLL_PTR <= n);
		ind->type = DICT_CLUSTERED;
	}
	if (comp) {
		for (i = 0; i < n; i++) {
			ulint	len = mach_read_from_2(ptr);
			ptr += 2;
			/* The high-order bit of len is the NOT NULL flag;
			the rest is 0 or 0x7fff for variable-length fields,
			and 1..0x7ffe for fixed-length fields. */
			dict_mem_table_add_col(
				table, NULL, NULL,
				((len + 1) & 0x7fff) <= 1
				? DATA_BINARY : DATA_FIXBINARY,
				len & 0x8000 ? DATA_NOT_NULL : 0,
				len & 0x7fff);

			dict_index_add_col(ind, table,
					   dict_table_get_nth_col(table, i),
					   0);
		}
		dict_table_add_system_columns(table, table->heap);
		if (n_uniq != n) {
			/* Identify DB_TRX_ID and DB_ROLL_PTR in the index. */
			ut_a(DATA_TRX_ID_LEN
			     == dict_index_get_nth_col(ind, DATA_TRX_ID - 1
						       + n_uniq)->len);
			ut_a(DATA_ROLL_PTR_LEN
			     == dict_index_get_nth_col(ind, DATA_ROLL_PTR - 1
						       + n_uniq)->len);
			ind->fields[DATA_TRX_ID - 1 + n_uniq].col
				= &table->cols[n + DATA_TRX_ID];
			ind->fields[DATA_ROLL_PTR - 1 + n_uniq].col
				= &table->cols[n + DATA_ROLL_PTR];
		}

		ut_ad(table->n_cols == table->n_def);

		if (n_core_fields) {
			for (i = n_core_fields; i < n; i++) {
				ind->fields[i].col->def_val.len
					= UNIV_SQL_NULL;
			}
			ind->n_core_fields = n_core_fields;
			ind->n_core_null_bytes = UT_BITS_IN_BYTES(
				ind->get_n_nullable(n_core_fields));
		} else {
			ind->n_core_null_bytes = UT_BITS_IN_BYTES(
				unsigned(ind->n_nullable));
			ind->n_core_fields = ind->n_fields;
		}
	}
	/* avoid ut_ad(index->cached) in dict_index_get_n_unique_in_tree */
	ind->cached = TRUE;
	ut_d(ind->is_dummy = true);
	*index = ind;
	return(ptr);
}
