/*****************************************************************************

Copyright (c) 1996, 2013, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
	bool	is_insert,	/*!< in: TRUE if insert undo log */
	ulint	rseg_id,	/*!< in: rollback segment id */
	uint32_t page_no,	/*!< in: page number */
	uint16_t offset)		/*!< in: offset of the undo entry within page */
{
  compile_time_assert(DATA_ROLL_PTR_LEN == 7);
  ut_ad(rseg_id < TRX_SYS_N_RSEGS);

  return roll_ptr_t{is_insert} << ROLL_PTR_INSERT_FLAG_POS |
    roll_ptr_t{rseg_id} << ROLL_PTR_RSEG_ID_POS |
    roll_ptr_t{page_no} << ROLL_PTR_PAGE_POS | offset;
}

/***********************************************************************//**
Decodes a roll pointer. */
UNIV_INLINE
void
trx_undo_decode_roll_ptr(
/*=====================*/
	roll_ptr_t	roll_ptr,	/*!< in: roll pointer */
	bool*		is_insert,	/*!< out: TRUE if insert undo log */
	ulint*		rseg_id,	/*!< out: rollback segment id */
	uint32_t*	page_no,	/*!< out: page number */
	uint16_t*	offset)		/*!< out: offset of the undo
					entry within page */
{
  compile_time_assert(DATA_ROLL_PTR_LEN == 7);
  ut_ad(roll_ptr < (1ULL << 56));
  *offset= static_cast<uint16_t>(roll_ptr);
  *page_no= static_cast<uint32_t>(roll_ptr >> 16);
  *rseg_id= static_cast<ulint>(roll_ptr >> 48 & 0x7F);
  *is_insert= static_cast<bool>(roll_ptr >> 55);
}

/***********************************************************************//**
Determine if DB_ROLL_PTR is of the insert type.
@return true if insert */
UNIV_INLINE
bool
trx_undo_roll_ptr_is_insert(
/*========================*/
	roll_ptr_t	roll_ptr)	/*!< in: roll pointer */
{
	compile_time_assert(DATA_ROLL_PTR_LEN == 7);
	ut_ad(roll_ptr < (1ULL << (ROLL_PTR_INSERT_FLAG_POS + 1)));
	return static_cast<bool>(roll_ptr >> ROLL_PTR_INSERT_FLAG_POS);
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

/** Gets an undo log page and x-latches it.
@param[in]	page_id		page id
@param[in,out]	mtr		mini-transaction
@return pointer to page x-latched */
UNIV_INLINE
buf_block_t*
trx_undo_page_get(const page_id_t page_id, mtr_t* mtr)
{
  return buf_page_get(page_id, 0, RW_X_LATCH, mtr);
}

/** Gets an undo log page and s-latches it.
@param[in]	page_id		page id
@param[in,out]	mtr		mini-transaction
@return pointer to page s-latched */
UNIV_INLINE
buf_block_t*
trx_undo_page_get_s_latched(const page_id_t page_id, mtr_t* mtr)
{
  return buf_page_get(page_id, 0, RW_S_LATCH, mtr);
}

/** Determine the end offset of undo log records of an undo log page.
@param[in]	undo_page	undo log page
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset
@return end offset */
inline
uint16_t trx_undo_page_get_end(const buf_block_t *undo_page, uint32_t page_no,
                               uint16_t offset)
{
  if (page_no == undo_page->page.id().page_no())
    if (uint16_t end = mach_read_from_2(TRX_UNDO_NEXT_LOG + offset +
					undo_page->page.frame))
      return end;

  return mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE +
			  undo_page->page.frame);
}

/** Get the next record in an undo log.
@param[in]      undo_page       undo log page
@param[in]      rec             undo record offset in the page
@param[in]      page_no         undo log header page number
@param[in]      offset          undo log header offset on page
@return undo log record, the page latched, NULL if none */
inline trx_undo_rec_t*
trx_undo_page_get_next_rec(const buf_block_t *undo_page, uint16_t rec,
                           uint32_t page_no, uint16_t offset)
{
  uint16_t end= trx_undo_page_get_end(undo_page, page_no, offset);
  uint16_t next= mach_read_from_2(undo_page->page.frame + rec);
  return next == end ? nullptr : undo_page->page.frame + next;
}
