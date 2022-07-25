/*****************************************************************************

Copyright (c) 1996, 2013, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation.

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
@file include/trx0rseg.ic
Rollback segment

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "srv0srv.h"
#include "mtr0log.h"

/** Gets a rollback segment header.
@param[in]	space		space where placed
@param[in]	page_no		page number of the header
@param[in,out]	mtr		mini-transaction
@return rollback segment header, page x-latched */
UNIV_INLINE
trx_rsegf_t*
trx_rsegf_get(
	ulint			space,
	ulint			page_no,
	mtr_t*			mtr)
{
	buf_block_t*	block;
	trx_rsegf_t*	header;

	ut_ad(space <= srv_undo_space_id_start + srv_undo_tablespaces_active
	      || space == SRV_TMP_SPACE_ID
	      || !srv_was_started);
	ut_ad(space <= srv_undo_space_id_start + TRX_SYS_MAX_UNDO_SPACES
	      || space == SRV_TMP_SPACE_ID);

	block = buf_page_get(
		page_id_t(space, page_no), univ_page_size, RW_X_LATCH, mtr);

	buf_block_dbg_add_level(block, SYNC_RSEG_HEADER);

	header = TRX_RSEG + buf_block_get_frame(block);

	return(header);
}

/** Gets a newly created rollback segment header.
@param[in]	space		space where placed
@param[in]	page_no		page number of the header
@param[in,out]	mtr		mini-transaction
@return rollback segment header, page x-latched */
UNIV_INLINE
trx_rsegf_t*
trx_rsegf_get_new(
	ulint			space,
	ulint			page_no,
	mtr_t*			mtr)
{
	buf_block_t*	block;
	trx_rsegf_t*	header;

	ut_ad(space <= srv_undo_tablespaces_active || space == SRV_TMP_SPACE_ID
	      || !srv_was_started);
	ut_ad(space <= TRX_SYS_MAX_UNDO_SPACES || space == SRV_TMP_SPACE_ID);

	block = buf_page_get(
		page_id_t(space, page_no), univ_page_size, RW_X_LATCH, mtr);

	buf_block_dbg_add_level(block, SYNC_RSEG_HEADER_NEW);

	header = TRX_RSEG + buf_block_get_frame(block);

	return(header);
}

/***************************************************************//**
Gets the file page number of the nth undo log slot.
@return page number of the undo log segment */
UNIV_INLINE
ulint
trx_rsegf_get_nth_undo(
/*===================*/
	trx_rsegf_t*	rsegf,	/*!< in: rollback segment header */
	ulint		n,	/*!< in: index of slot */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ut_a(n < TRX_RSEG_N_SLOTS);

	return(mtr_read_ulint(rsegf + TRX_RSEG_UNDO_SLOTS
			      + n * TRX_RSEG_SLOT_SIZE, MLOG_4BYTES, mtr));
}

/***************************************************************//**
Sets the file page number of the nth undo log slot. */
UNIV_INLINE
void
trx_rsegf_set_nth_undo(
/*===================*/
	trx_rsegf_t*	rsegf,	/*!< in: rollback segment header */
	ulint		n,	/*!< in: index of slot */
	ulint		page_no,/*!< in: page number of the undo log segment */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ut_a(n < TRX_RSEG_N_SLOTS);

	mlog_write_ulint(rsegf + TRX_RSEG_UNDO_SLOTS + n * TRX_RSEG_SLOT_SIZE,
			 page_no, MLOG_4BYTES, mtr);
}

/****************************************************************//**
Looks for a free slot for an undo log segment.
@return slot index or ULINT_UNDEFINED if not found */
UNIV_INLINE
ulint
trx_rsegf_undo_find_free(
/*=====================*/
	trx_rsegf_t*	rsegf,	/*!< in: rollback segment header */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ulint		i;
	ulint		page_no;
	ulint		max_slots = TRX_RSEG_N_SLOTS;

#ifdef UNIV_DEBUG
	if (trx_rseg_n_slots_debug) {
		max_slots = ut_min(static_cast<ulint>(trx_rseg_n_slots_debug),
				   static_cast<ulint>(TRX_RSEG_N_SLOTS));
	}
#endif

	for (i = 0; i < max_slots; i++) {
		page_no = trx_rsegf_get_nth_undo(rsegf, i, mtr);

		if (page_no == FIL_NULL) {
			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}
