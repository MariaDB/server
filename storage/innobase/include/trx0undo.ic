/*****************************************************************************

Copyright (c) 1996, 2013, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

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
@file include/trx0undo.ic
Transaction undo log

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "data0type.h"
#include "page0page.h"

/***********************************************************************//**
Builds a roll pointer.
@return roll pointer */
UNIV_INLINE
roll_ptr_t
trx_undo_build_roll_ptr(
/*====================*/
	ibool	is_insert,	/*!< in: TRUE if insert undo log */
	ulint	rseg_id,	/*!< in: rollback segment id */
	ulint	page_no,	/*!< in: page number */
	ulint	offset)		/*!< in: offset of the undo entry within page */
{
	roll_ptr_t	roll_ptr;
	compile_time_assert(DATA_ROLL_PTR_LEN == 7);
	ut_ad(is_insert == 0 || is_insert == 1);
	ut_ad(rseg_id < TRX_SYS_N_RSEGS);
	ut_ad(offset < 65536);

	roll_ptr = (roll_ptr_t) is_insert << ROLL_PTR_INSERT_FLAG_POS
		| (roll_ptr_t) rseg_id << ROLL_PTR_RSEG_ID_POS
		| (roll_ptr_t) page_no << ROLL_PTR_PAGE_POS
		| offset;
	return(roll_ptr);
}

/***********************************************************************//**
Decodes a roll pointer. */
UNIV_INLINE
void
trx_undo_decode_roll_ptr(
/*=====================*/
	roll_ptr_t	roll_ptr,	/*!< in: roll pointer */
	ibool*		is_insert,	/*!< out: TRUE if insert undo log */
	ulint*		rseg_id,	/*!< out: rollback segment id */
	ulint*		page_no,	/*!< out: page number */
	ulint*		offset)		/*!< out: offset of the undo
					entry within page */
{
	compile_time_assert(DATA_ROLL_PTR_LEN == 7);
	ut_ad(roll_ptr < (1ULL << 56));
	*offset = (ulint) roll_ptr & 0xFFFF;
	roll_ptr >>= 16;
	*page_no = (ulint) roll_ptr & 0xFFFFFFFF;
	roll_ptr >>= 32;
	*rseg_id = (ulint) roll_ptr & 0x7F;
	roll_ptr >>= 7;
	*is_insert = (ibool) roll_ptr; /* TRUE==1 */
}

/***********************************************************************//**
Returns TRUE if the roll pointer is of the insert type.
@return TRUE if insert undo log */
UNIV_INLINE
ibool
trx_undo_roll_ptr_is_insert(
/*========================*/
	roll_ptr_t	roll_ptr)	/*!< in: roll pointer */
{
	compile_time_assert(DATA_ROLL_PTR_LEN == 7);
	ut_ad(roll_ptr < (1ULL << (ROLL_PTR_INSERT_FLAG_POS + 1)));
	return((ibool) (roll_ptr >> ROLL_PTR_INSERT_FLAG_POS));
}

/***********************************************************************//**
Returns true if the record is of the insert type.
@return true if the record was freshly inserted (not updated). */
UNIV_INLINE
bool
trx_undo_trx_id_is_insert(
/*======================*/
	const byte*	trx_id)	/*!< in: DB_TRX_ID, followed by DB_ROLL_PTR */
{
	compile_time_assert(DATA_TRX_ID + 1 == DATA_ROLL_PTR);
	return bool(trx_id[DATA_TRX_ID_LEN] >> 7);
}

/*****************************************************************//**
Writes a roll ptr to an index page. In case that the size changes in
some future version, this function should be used instead of
mach_write_... */
UNIV_INLINE
void
trx_write_roll_ptr(
/*===============*/
	byte*		ptr,		/*!< in: pointer to memory where
					written */
	roll_ptr_t	roll_ptr)	/*!< in: roll ptr */
{
	compile_time_assert(DATA_ROLL_PTR_LEN == 7);
	mach_write_to_7(ptr, roll_ptr);
}

/*****************************************************************//**
Reads a roll ptr from an index page. In case that the roll ptr size
changes in some future version, this function should be used instead of
mach_read_...
@return roll ptr */
UNIV_INLINE
roll_ptr_t
trx_read_roll_ptr(
/*==============*/
	const byte*	ptr)	/*!< in: pointer to memory from where to read */
{
	compile_time_assert(DATA_ROLL_PTR_LEN == 7);
	return(mach_read_from_7(ptr));
}

/** Gets an undo log page and x-latches it.
@param[in]	page_id		page id
@param[in,out]	mtr		mini-transaction
@return pointer to page x-latched */
UNIV_INLINE
page_t*
trx_undo_page_get(const page_id_t page_id, mtr_t* mtr)
{
	buf_block_t*	block = buf_page_get(page_id, univ_page_size,
					     RW_X_LATCH, mtr);

	buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

	return(buf_block_get_frame(block));
}

/** Gets an undo log page and s-latches it.
@param[in]	page_id		page id
@param[in,out]	mtr		mini-transaction
@return pointer to page s-latched */
UNIV_INLINE
page_t*
trx_undo_page_get_s_latched(const page_id_t page_id, mtr_t* mtr)
{
	buf_block_t*	block = buf_page_get(page_id, univ_page_size,
					     RW_S_LATCH, mtr);

	buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

	return(buf_block_get_frame(block));
}

/** Determine the end offset of undo log records of an undo log page.
@param[in]	undo_page	undo log page
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset
@return end offset */
inline
uint16_t
trx_undo_page_get_end(const page_t* undo_page, ulint page_no, ulint offset)
{
	if (page_no == page_get_page_no(undo_page)) {
		if (uint16_t end = mach_read_from_2(TRX_UNDO_NEXT_LOG
						    + offset + undo_page)) {
			return end;
		}
	}

	return mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
				+ undo_page);
}

/******************************************************************//**
Returns the next undo log record on the page in the specified log, or
NULL if none exists.
@return pointer to record, NULL if none */
UNIV_INLINE
trx_undo_rec_t*
trx_undo_page_get_next_rec(
/*=======================*/
	trx_undo_rec_t*	rec,	/*!< in: undo log record */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset)	/*!< in: undo log header offset on page */
{
	page_t*	undo_page;
	ulint	end;
	ulint	next;

	undo_page = (page_t*) ut_align_down(rec, srv_page_size);

	end = trx_undo_page_get_end(undo_page, page_no, offset);

	next = mach_read_from_2(rec);

	if (next == end) {

		return(NULL);
	}

	return(undo_page + next);
}
