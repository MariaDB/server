/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2021, MariaDB Corporation.

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
@file trx/trx0undo.cc
Transaction undo log

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0undo.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "log0log.h"
#include "trx0rseg.h"
#include "log.h"

/* How should the old versions in the history list be managed?
   ----------------------------------------------------------
If each transaction is given a whole page for its update undo log, file
space consumption can be 10 times higher than necessary. Therefore,
partly filled update undo log pages should be reusable. But then there
is no way individual pages can be ordered so that the ordering agrees
with the serialization numbers of the transactions on the pages. Thus,
the history list must be formed of undo logs, not their header pages as
it was in the old implementation.
	However, on a single header page the transactions are placed in
the order of their serialization numbers. As old versions are purged, we
may free the page when the last transaction on the page has been purged.
	A problem is that the purge has to go through the transactions
in the serialization order. This means that we have to look through all
rollback segments for the one that has the smallest transaction number
in its history list.
	When should we do a purge? A purge is necessary when space is
running out in any of the rollback segments. Then we may have to purge
also old version which might be needed by some consistent read. How do
we trigger the start of a purge? When a transaction writes to an undo log,
it may notice that the space is running out. When a read view is closed,
it may make some history superfluous. The server can have an utility which
periodically checks if it can purge some history.
	In a parallellized purge we have the problem that a query thread
can remove a delete marked clustered index record before another query
thread has processed an earlier version of the record, which cannot then
be done because the row cannot be constructed from the clustered index
record. To avoid this problem, we will store in the update and delete mark
undo record also the columns necessary to construct the secondary index
entries which are modified.
	We can latch the stack of versions of a single clustered index record
by taking a latch on the clustered index page. As long as the latch is held,
no new versions can be added and no versions removed by undo. But, a purge
can still remove old versions from the bottom of the stack. */

/* How to protect rollback segments, undo logs, and history lists with
   -------------------------------------------------------------------
latches?
-------
The contention of the trx_sys.mutex should be minimized. When a transaction
does its first insert or modify in an index, an undo log is assigned for it.
Then we must have an x-latch to the rollback segment header.
	When the transaction performs modifications or rolls back, its
undo log is protected by undo page latches.
Only the thread that is associated with the transaction may hold multiple
undo page latches at a time. Undo pages are always private to a single
transaction. Other threads that are performing MVCC reads
or checking for implicit locks will lock at most one undo page at a time
in trx_undo_get_undo_rec_low().
	When the transaction commits, its persistent undo log is added
to the history list. If it is not suitable for reuse, its slot is reset.
In both cases, an x-latch must be acquired on the rollback segment header page.
	The purge operation steps through the history list without modifying
it until a truncate operation occurs, which can remove undo logs from the end
of the list and release undo log segments. In stepping through the list,
s-latches on the undo log pages are enough, but in a truncate, x-latches must
be obtained on the rollback segment and individual pages. */

/********************************************************************//**
Creates and initializes an undo log memory object.
@return own: the undo log memory object */
static
trx_undo_t*
trx_undo_mem_create(
/*================*/
	trx_rseg_t*	rseg,	/*!< in: rollback segment memory object */
	ulint		id,	/*!< in: slot index within rseg */
	trx_id_t	trx_id,	/*!< in: id of the trx for which the undo log
				is created */
	const XID*	xid,	/*!< in: X/Open XA transaction identification*/
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset);/*!< in: undo log header byte offset on page */

/** Determine the start offset of undo log records of an undo log page.
@param[in]	undo_page	undo log page
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset
@return start offset */
static
uint16_t
trx_undo_page_get_start(const page_t* undo_page, ulint page_no, ulint offset)
{
	return page_no == page_get_page_no(undo_page)
		? mach_read_from_2(offset + TRX_UNDO_LOG_START + undo_page)
		: TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE;
}

/** Get the first undo log record on a page.
@param[in]	page	undo log page
@param[in]	page_no	undo log header page number
@param[in]	offset	undo log header page offset
@return	pointer to first record
@retval	NULL	if none exists */
static
trx_undo_rec_t*
trx_undo_page_get_first_rec(page_t* page, ulint page_no, ulint offset)
{
	ulint start = trx_undo_page_get_start(page, page_no, offset);
	return start == trx_undo_page_get_end(page, page_no, offset)
		? NULL
		: page + start;
}

/** Get the last undo log record on a page.
@param[in]	page	undo log page
@param[in]	page_no	undo log header page number
@param[in]	offset	undo log header page offset
@return	pointer to last record
@retval	NULL	if none exists */
static
trx_undo_rec_t*
trx_undo_page_get_last_rec(page_t* page, ulint page_no, ulint offset)
{
	ulint end = trx_undo_page_get_end(page, page_no, offset);

	return trx_undo_page_get_start(page, page_no, offset) == end
		? NULL
		: page + mach_read_from_2(page + end - 2);
}

/***********************************************************************//**
Gets the previous record in an undo log from the previous page.
@return undo log record, the page s-latched, NULL if none */
static
trx_undo_rec_t*
trx_undo_get_prev_rec_from_prev_page(
/*=================================*/
	trx_undo_rec_t*	rec,	/*!< in: undo record */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset,	/*!< in: undo log header offset on page */
	bool		shared,	/*!< in: true=S-latch, false=X-latch */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ulint	space;
	ulint	prev_page_no;
	page_t* prev_page;
	page_t*	undo_page;

	undo_page = page_align(rec);

	prev_page_no = flst_get_prev_addr(undo_page + TRX_UNDO_PAGE_HDR
					  + TRX_UNDO_PAGE_NODE, mtr)
		.page;

	if (prev_page_no == FIL_NULL) {

		return(NULL);
	}

	space = page_get_space_id(undo_page);

	buf_block_t*	block = buf_page_get(
		page_id_t(space, prev_page_no), 0,
		shared ? RW_S_LATCH : RW_X_LATCH, mtr);

	buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

	prev_page = buf_block_get_frame(block);

	return(trx_undo_page_get_last_rec(prev_page, page_no, offset));
}

/** Get the previous undo log record.
@param[in]	rec	undo log record
@param[in]	page_no	undo log header page number
@param[in]	offset	undo log header page offset
@return	pointer to record
@retval	NULL if none */
static
trx_undo_rec_t*
trx_undo_page_get_prev_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset)
{
	page_t*	undo_page;
	ulint	start;

	undo_page = (page_t*) ut_align_down(rec, srv_page_size);

	start = trx_undo_page_get_start(undo_page, page_no, offset);

	if (start + undo_page == rec) {

		return(NULL);
	}

	return(undo_page + mach_read_from_2(rec - 2));
}

/***********************************************************************//**
Gets the previous record in an undo log.
@return undo log record, the page s-latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_prev_rec(
/*==================*/
	trx_undo_rec_t*	rec,	/*!< in: undo record */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset,	/*!< in: undo log header offset on page */
	bool		shared,	/*!< in: true=S-latch, false=X-latch */
	mtr_t*		mtr)	/*!< in: mtr */
{
	trx_undo_rec_t*	prev_rec;

	prev_rec = trx_undo_page_get_prev_rec(rec, page_no, offset);

	if (prev_rec) {

		return(prev_rec);
	}

	/* We have to go to the previous undo log page to look for the
	previous record */

	return(trx_undo_get_prev_rec_from_prev_page(rec, page_no, offset,
						    shared, mtr));
}

/** Gets the next record in an undo log from the next page.
@param[in]	space		undo log header space
@param[in]	undo_page	undo log page
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset on page
@param[in]	mode		latch mode: RW_S_LATCH or RW_X_LATCH
@param[in,out]	mtr		mini-transaction
@return undo log record, the page latched, NULL if none */
static
trx_undo_rec_t*
trx_undo_get_next_rec_from_next_page(
	ulint			space,
	const page_t*		undo_page,
	ulint			page_no,
	ulint			offset,
	ulint			mode,
	mtr_t*			mtr)
{
	const trx_ulogf_t*	log_hdr;
	ulint			next_page_no;
	page_t*			next_page;
	ulint			next;

	if (page_no == page_get_page_no(undo_page)) {

		log_hdr = undo_page + offset;
		next = mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG);

		if (next != 0) {

			return(NULL);
		}
	}

	next_page_no = flst_get_next_addr(undo_page + TRX_UNDO_PAGE_HDR
					  + TRX_UNDO_PAGE_NODE, mtr)
		.page;
	if (next_page_no == FIL_NULL) {

		return(NULL);
	}

	const page_id_t	next_page_id(space, next_page_no);

	if (mode == RW_S_LATCH) {
		next_page = trx_undo_page_get_s_latched(
			next_page_id, mtr);
	} else {
		ut_ad(mode == RW_X_LATCH);
		next_page = trx_undo_page_get(next_page_id, mtr);
	}

	return(trx_undo_page_get_first_rec(next_page, page_no, offset));
}

/***********************************************************************//**
Gets the next record in an undo log.
@return undo log record, the page s-latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_next_rec(
/*==================*/
	trx_undo_rec_t*	rec,	/*!< in: undo record */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset,	/*!< in: undo log header offset on page */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ulint		space;
	trx_undo_rec_t*	next_rec;

	next_rec = trx_undo_page_get_next_rec(rec, page_no, offset);

	if (next_rec) {
		return(next_rec);
	}

	space = page_get_space_id(page_align(rec));

	return(trx_undo_get_next_rec_from_next_page(space,
						    page_align(rec),
						    page_no, offset,
						    RW_S_LATCH, mtr));
}

/** Gets the first record in an undo log.
@param[in]	space		undo log header space
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset on page
@param[in]	mode		latching mode: RW_S_LATCH or RW_X_LATCH
@param[in,out]	mtr		mini-transaction
@return undo log record, the page latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_first_rec(
	fil_space_t*		space,
	ulint			page_no,
	ulint			offset,
	ulint			mode,
	mtr_t*			mtr)
{
	page_t*		undo_page;
	trx_undo_rec_t*	rec;

	const page_id_t	page_id(space->id, page_no);

	if (mode == RW_S_LATCH) {
		undo_page = trx_undo_page_get_s_latched(page_id, mtr);
	} else {
		undo_page = trx_undo_page_get(page_id, mtr);
	}

	rec = trx_undo_page_get_first_rec(undo_page, page_no, offset);

	if (rec) {
		return(rec);
	}

	return(trx_undo_get_next_rec_from_next_page(space->id,
						    undo_page, page_no, offset,
						    mode, mtr));
}

/*============== UNDO LOG FILE COPY CREATION AND FREEING ==================*/

/** Parse MLOG_UNDO_INIT.
@param[in]	ptr	log record
@param[in]	end_ptr	end of log record buffer
@param[in,out]	page	page or NULL
@return	end of log record
@retval	NULL	if the log record is incomplete */
byte*
trx_undo_parse_page_init(const byte* ptr, const byte* end_ptr, page_t* page)
{
	if (end_ptr <= ptr) {
		return NULL;
	}

	const ulint type = *ptr++;

	if (type > TRX_UNDO_UPDATE) {
		recv_sys.found_corrupt_log = true;
	} else if (page) {
		/* Starting with MDEV-12288 in MariaDB 10.3.1, we use
		type=0 for the combined insert/update undo log
		pages. MariaDB 10.2 would use TRX_UNDO_INSERT or
		TRX_UNDO_UPDATE. */
		mach_write_to_2(FIL_PAGE_TYPE + page, FIL_PAGE_UNDO_LOG);
		mach_write_to_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE + page,
				type);
		mach_write_to_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START + page,
				TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
		mach_write_to_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE + page,
				TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
	}

	return(const_cast<byte*>(ptr));
}

/** Parse MLOG_UNDO_HDR_REUSE for crash-upgrade from MariaDB 10.2.
@param[in]	ptr	redo log record
@param[in]	end_ptr	end of log buffer
@param[in,out]	page	undo log page or NULL
@return end of log record or NULL */
byte*
trx_undo_parse_page_header_reuse(
	const byte*	ptr,
	const byte*	end_ptr,
	page_t*		undo_page)
{
	trx_id_t	trx_id = mach_u64_parse_compressed(&ptr, end_ptr);

	if (!ptr || !undo_page) {
		return(const_cast<byte*>(ptr));
	}

	compile_time_assert(TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE
			    + TRX_UNDO_LOG_XA_HDR_SIZE
			    < UNIV_PAGE_SIZE_MIN - 100);

	const ulint new_free = TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE
		+ TRX_UNDO_LOG_OLD_HDR_SIZE;

	/* Insert undo data is not needed after commit: we may free all
	the space on the page */

	ut_ad(mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE
			       + undo_page) == 1);

	byte*	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);
	mach_write_to_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE + undo_page,
			TRX_UNDO_ACTIVE);

	byte* log_hdr = undo_page + TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE;

	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);

	mach_write_to_1(log_hdr + TRX_UNDO_XID_EXISTS, FALSE);
	mach_write_to_1(log_hdr + TRX_UNDO_DICT_TRANS, FALSE);

	return(const_cast<byte*>(ptr));
}

/** Initialize the fields in an undo log segment page.
@param[in,out]	undo_block	undo page
@param[in,out]	mtr		mini-transaction */
static void trx_undo_page_init(buf_block_t* undo_block, mtr_t* mtr)
{
	page_t* page = undo_block->frame;
	mach_write_to_2(FIL_PAGE_TYPE + page, FIL_PAGE_UNDO_LOG);
	mach_write_to_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE + page, 0);
	mach_write_to_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START + page,
			TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
	mach_write_to_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE + page,
			TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);

	mtr->set_modified();
	switch (mtr->get_log_mode()) {
	case MTR_LOG_NONE:
	case MTR_LOG_NO_REDO:
		return;
	case MTR_LOG_SHORT_INSERTS:
		ut_ad(0);
		/* fall through */
	case MTR_LOG_ALL:
		break;
	}

	byte* log_ptr = mtr->get_log()->open(11 + 1);
	log_ptr = mlog_write_initial_log_record_low(
		MLOG_UNDO_INIT,
		undo_block->page.id.space(),
		undo_block->page.id.page_no(),
		log_ptr, mtr);
	*log_ptr++ = 0;
	mlog_close(mtr, log_ptr);
}

/** Create an undo log segment.
@param[in,out]	space		tablespace
@param[in,out]	rseg_hdr	rollback segment header (x-latched)
@param[out]	id		undo slot number
@param[out]	err		error code
@param[in,out]	mtr		mini-transaction
@return	undo log block
@retval	NULL	on failure */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
buf_block_t*
trx_undo_seg_create(fil_space_t* space, trx_rsegf_t* rseg_hdr, ulint* id,
		    dberr_t* err, mtr_t* mtr)
{
	ulint		slot_no;
	buf_block_t*	block;
	ulint		n_reserved;
	bool		success;

	slot_no = trx_rsegf_undo_find_free(rseg_hdr);

	if (slot_no == ULINT_UNDEFINED) {
		ib::warn() << "Cannot find a free slot for an undo log. Do"
			" you have too many active transactions running"
			" concurrently?";

		*err = DB_TOO_MANY_CONCURRENT_TRXS;
		return NULL;
	}

	success = fsp_reserve_free_extents(&n_reserved, space, 2, FSP_UNDO,
					   mtr);
	if (!success) {
		*err = DB_OUT_OF_FILE_SPACE;
		return NULL;
	}

	/* Allocate a new file segment for the undo log */
	block = fseg_create(space, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
			    mtr, true);

	space->release_free_extents(n_reserved);

	if (block == NULL) {
		*err = DB_OUT_OF_FILE_SPACE;
		return NULL;
	}

	buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

	trx_undo_page_init(block, mtr);

	mlog_write_ulint(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE + block->frame,
			 TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE,
			 MLOG_2BYTES, mtr);

	mlog_write_ulint(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG + block->frame,
			 0, MLOG_2BYTES, mtr);

	flst_init(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + block->frame, mtr);

	flst_add_last(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + block->frame,
		      TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + block->frame,
		      mtr);

	*id = slot_no;
	trx_rsegf_set_nth_undo(rseg_hdr, slot_no, block->page.id.page_no(),
			       mtr);

	MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);

	*err = DB_SUCCESS;
	return block;
}

/**********************************************************************//**
Writes the mtr log entry of an undo log header initialization. */
UNIV_INLINE
void
trx_undo_header_create_log(
/*=======================*/
	const page_t*	undo_page,	/*!< in: undo log header page */
	trx_id_t	trx_id,		/*!< in: transaction id */
	mtr_t*		mtr)		/*!< in: mtr */
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_CREATE, mtr);

	mlog_catenate_ull_compressed(mtr, trx_id);
}

/***************************************************************//**
Creates a new undo log header in file. NOTE that this function has its own
log record type MLOG_UNDO_HDR_CREATE. You must NOT change the operation of
this function!
@return header byte offset on page */
static
ulint
trx_undo_header_create(
/*===================*/
	page_t*		undo_page,	/*!< in/out: undo log segment
					header page, x-latched; it is
					assumed that there is
					TRX_UNDO_LOG_XA_HDR_SIZE bytes
					free space on it */
	trx_id_t	trx_id,		/*!< in: transaction id */
	mtr_t*		mtr)		/*!< in: mtr */
{
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	log_hdr;
	ulint		prev_log;
	ulint		free;
	ulint		new_free;

	ut_ad(mtr && undo_page);

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);

	log_hdr = undo_page + free;

	new_free = free + TRX_UNDO_LOG_OLD_HDR_SIZE;

	ut_a(free + TRX_UNDO_LOG_XA_HDR_SIZE < srv_page_size - 100);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);

	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

	prev_log = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);

	if (prev_log != 0) {
		trx_ulogf_t*	prev_log_hdr;

		prev_log_hdr = undo_page + prev_log;

		mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, free);
	}

	mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, free);

	log_hdr = undo_page + free;

	mach_write_to_2(log_hdr + TRX_UNDO_NEEDS_PURGE, 1);

	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);

	mach_write_to_1(log_hdr + TRX_UNDO_XID_EXISTS, FALSE);
	mach_write_to_1(log_hdr + TRX_UNDO_DICT_TRANS, FALSE);

	mach_write_to_2(log_hdr + TRX_UNDO_NEXT_LOG, 0);
	mach_write_to_2(log_hdr + TRX_UNDO_PREV_LOG, prev_log);

	/* Write the log record about the header creation */
	trx_undo_header_create_log(undo_page, trx_id, mtr);

	return(free);
}

/********************************************************************//**
Write X/Open XA Transaction Identification (XID) to undo log header */
static
void
trx_undo_write_xid(
/*===============*/
	trx_ulogf_t*	log_hdr,/*!< in: undo log header */
	const XID*	xid,	/*!< in: X/Open XA Transaction Identification */
	mtr_t*		mtr)	/*!< in: mtr */
{
	DBUG_ASSERT(xid->gtrid_length >= 0);
	DBUG_ASSERT(xid->bqual_length >= 0);
	DBUG_ASSERT(xid->gtrid_length + xid->bqual_length < XIDDATASIZE);

	mlog_write_ulint(log_hdr + TRX_UNDO_XA_FORMAT,
			 static_cast<ulint>(xid->formatID),
			 MLOG_4BYTES, mtr);

	mlog_write_ulint(log_hdr + TRX_UNDO_XA_TRID_LEN,
			 static_cast<ulint>(xid->gtrid_length),
			 MLOG_4BYTES, mtr);

	mlog_write_ulint(log_hdr + TRX_UNDO_XA_BQUAL_LEN,
			 static_cast<ulint>(xid->bqual_length),
			 MLOG_4BYTES, mtr);
	const ulint xid_length = static_cast<ulint>(xid->gtrid_length
						    + xid->bqual_length);
	mlog_write_string(log_hdr + TRX_UNDO_XA_XID,
			  reinterpret_cast<const byte*>(xid->data),
			  xid_length, mtr);
	if (UNIV_LIKELY(xid_length < XIDDATASIZE)) {
		mlog_memset(log_hdr + TRX_UNDO_XA_XID + xid_length,
			    XIDDATASIZE - xid_length, 0, mtr);
	}
}

/********************************************************************//**
Read X/Open XA Transaction Identification (XID) from undo log header */
static
void
trx_undo_read_xid(const trx_ulogf_t* log_hdr, XID* xid)
{
	xid->formatID=static_cast<long>(mach_read_from_4(
		log_hdr + TRX_UNDO_XA_FORMAT));

	xid->gtrid_length=static_cast<long>(mach_read_from_4(
		log_hdr + TRX_UNDO_XA_TRID_LEN));

	xid->bqual_length=static_cast<long>(mach_read_from_4(
		log_hdr + TRX_UNDO_XA_BQUAL_LEN));

	memcpy(xid->data, log_hdr + TRX_UNDO_XA_XID, XIDDATASIZE);
}

/***************************************************************//**
Adds space for the XA XID after an undo log old-style header. */
static
void
trx_undo_header_add_space_for_xid(
/*==============================*/
	page_t*		undo_page,/*!< in: undo log segment header page */
	trx_ulogf_t*	log_hdr,/*!< in: undo log header */
	mtr_t*		mtr)	/*!< in: mtr */
{
	trx_upagef_t*	page_hdr;
	ulint		free;
	ulint		new_free;

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);

	/* free is now the end offset of the old style undo log header */

	ut_a(free == (ulint)(log_hdr - undo_page) + TRX_UNDO_LOG_OLD_HDR_SIZE);

	new_free = free + (TRX_UNDO_LOG_XA_HDR_SIZE
			   - TRX_UNDO_LOG_OLD_HDR_SIZE);

	/* Add space for a XID after the header, update the free offset
	fields on the undo log page and in the undo log header */

	mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_START, new_free,
			 MLOG_2BYTES, mtr);

	mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_FREE, new_free,
			 MLOG_2BYTES, mtr);

	mlog_write_ulint(log_hdr + TRX_UNDO_LOG_START, new_free,
			 MLOG_2BYTES, mtr);
}

/** Parse the redo log entry of an undo log page header create.
@param[in]	ptr	redo log record
@param[in]	end_ptr	end of log buffer
@param[in,out]	page	page frame or NULL
@param[in,out]	mtr	mini-transaction or NULL
@return end of log record or NULL */
byte*
trx_undo_parse_page_header(
	const byte*	ptr,
	const byte*	end_ptr,
	page_t*		page,
	mtr_t*		mtr)
{
	trx_id_t	trx_id = mach_u64_parse_compressed(&ptr, end_ptr);

	if (ptr != NULL && page != NULL) {
		trx_undo_header_create(page, trx_id, mtr);
		return(const_cast<byte*>(ptr));
	}

	return(const_cast<byte*>(ptr));
}

/** Allocate an undo log page.
@param[in,out]	undo	undo log
@param[in,out]	mtr	mini-transaction that does not hold any page latch
@return	X-latched block if success
@retval	NULL	on failure */
buf_block_t* trx_undo_add_page(trx_undo_t* undo, mtr_t* mtr)
{
	trx_rseg_t*	rseg		= undo->rseg;
	buf_block_t*	new_block	= NULL;
	ulint		n_reserved;
	page_t*		header_page;

	/* When we add a page to an undo log, this is analogous to
	a pessimistic insert in a B-tree, and we must reserve the
	counterpart of the tree latch, which is the rseg mutex. */

	mutex_enter(&rseg->mutex);

	header_page = trx_undo_page_get(
		page_id_t(undo->rseg->space->id, undo->hdr_page_no), mtr);

	if (!fsp_reserve_free_extents(&n_reserved, undo->rseg->space, 1,
				      FSP_UNDO, mtr)) {
		goto func_exit;
	}

	new_block = fseg_alloc_free_page_general(
		TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER
		+ header_page,
		undo->top_page_no + 1, FSP_UP, TRUE, mtr, mtr);

	rseg->space->release_free_extents(n_reserved);

	if (!new_block) {
		goto func_exit;
	}

	ut_ad(rw_lock_get_x_lock_count(&new_block->lock) == 1);
	buf_block_dbg_add_level(new_block, SYNC_TRX_UNDO_PAGE);
	undo->last_page_no = new_block->page.id.page_no();

	trx_undo_page_init(new_block, mtr);

	flst_add_last(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST
		      + header_page,
		      TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE
		      + new_block->frame,
		      mtr);
	undo->size++;
	rseg->curr_size++;

func_exit:
	mutex_exit(&rseg->mutex);
	return(new_block);
}

/********************************************************************//**
Frees an undo log page that is not the header page.
@return last page number in remaining log */
static
ulint
trx_undo_free_page(
/*===============*/
	trx_rseg_t* rseg,	/*!< in: rollback segment */
	bool	in_history,	/*!< in: TRUE if the undo log is in the history
				list */
	ulint	hdr_page_no,	/*!< in: header page number */
	ulint	page_no,	/*!< in: page number to free: must not be the
				header page */
	mtr_t*	mtr)		/*!< in: mtr which does not have a latch to any
				undo log page; the caller must have reserved
				the rollback segment mutex */
{
	const ulint	space = rseg->space->id;

	ut_a(hdr_page_no != page_no);
	ut_ad(mutex_own(&(rseg->mutex)));

	page_t*	undo_page = trx_undo_page_get(page_id_t(space, page_no), mtr);
	page_t* header_page = trx_undo_page_get(page_id_t(space, hdr_page_no),
						mtr);

	flst_remove(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + header_page,
		    TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + undo_page, mtr);

	fseg_free_page(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER + header_page,
		       rseg->space, page_no, true, mtr);

	const fil_addr_t last_addr = flst_get_last(
		TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + header_page, mtr);
	rseg->curr_size--;

	if (in_history) {
		trx_rsegf_t* rseg_header = trx_rsegf_get(
			rseg->space, rseg->page_no, mtr);
		uint32_t hist_size = mach_read_from_4(
			rseg_header + TRX_RSEG_HISTORY_SIZE);
		ut_ad(hist_size > 0);
		mlog_write_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE,
				 hist_size - 1, MLOG_4BYTES, mtr);
	}

	return(last_addr.page);
}

/** Free the last undo log page. The caller must hold the rseg mutex.
@param[in,out]	undo	undo log
@param[in,out]	mtr	mini-transaction that does not hold any undo log page
			or that has allocated the undo log page */
void
trx_undo_free_last_page(trx_undo_t* undo, mtr_t* mtr)
{
	ut_ad(undo->hdr_page_no != undo->last_page_no);
	ut_ad(undo->size > 0);

	undo->last_page_no = trx_undo_free_page(
		undo->rseg, false, undo->hdr_page_no, undo->last_page_no, mtr);

	undo->size--;
}

/** Truncate the tail of an undo log during rollback.
@param[in,out]	undo	undo log
@param[in]	limit	all undo logs after this limit will be discarded
@param[in]	is_temp	whether this is temporary undo log */
void trx_undo_truncate_end(trx_undo_t& undo, undo_no_t limit, bool is_temp)
{
	mtr_t mtr;
	ut_ad(is_temp == !undo.rseg->is_persistent());

	for (;;) {
		mtr.start();
		if (is_temp) {
			mtr.set_log_mode(MTR_LOG_NO_REDO);
		}

		trx_undo_rec_t* trunc_here = NULL;
		mutex_enter(&undo.rseg->mutex);
		page_t*		undo_page = trx_undo_page_get(
			page_id_t(undo.rseg->space->id, undo.last_page_no),
			&mtr);
		trx_undo_rec_t* rec = trx_undo_page_get_last_rec(
			undo_page, undo.hdr_page_no, undo.hdr_offset);
		while (rec) {
			if (trx_undo_rec_get_undo_no(rec) < limit) {
				goto func_exit;
			}
			/* Truncate at least this record off, maybe more */
			trunc_here = rec;

			rec = trx_undo_page_get_prev_rec(rec,
							 undo.hdr_page_no,
							 undo.hdr_offset);
		}

		if (undo.last_page_no != undo.hdr_page_no) {
			trx_undo_free_last_page(&undo, &mtr);
			mutex_exit(&undo.rseg->mutex);
			mtr.commit();
			continue;
		}

func_exit:
		mutex_exit(&undo.rseg->mutex);

		if (trunc_here) {
			mlog_write_ulint(undo_page + TRX_UNDO_PAGE_HDR
					 + TRX_UNDO_PAGE_FREE,
					 ulint(trunc_here - undo_page),
					 MLOG_2BYTES, &mtr);
		}

		mtr.commit();
		return;
	}
}

/** Truncate the head of an undo log.
NOTE that only whole pages are freed; the header page is not
freed, but emptied, if all the records there are below the limit.
@param[in,out]	rseg		rollback segment
@param[in]	hdr_page_no	header page number
@param[in]	hdr_offset	header offset on the page
@param[in]	limit		first undo number to preserve
(everything below the limit will be truncated) */
void
trx_undo_truncate_start(
	trx_rseg_t*	rseg,
	ulint		hdr_page_no,
	ulint		hdr_offset,
	undo_no_t	limit)
{
	page_t*		undo_page;
	trx_undo_rec_t* rec;
	trx_undo_rec_t* last_rec;
	ulint		page_no;
	mtr_t		mtr;

	ut_ad(mutex_own(&(rseg->mutex)));

	if (!limit || log_is_in_distress()) {
		return;
	}

loop:
	mtr_start(&mtr);

	if (!rseg->is_persistent()) {
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	}

	rec = trx_undo_get_first_rec(rseg->space, hdr_page_no, hdr_offset,
				     RW_X_LATCH, &mtr);
	if (rec == NULL) {
		/* Already empty */

		mtr_commit(&mtr);

		return;
	}

	undo_page = page_align(rec);

	last_rec = trx_undo_page_get_last_rec(undo_page, hdr_page_no,
					      hdr_offset);
	if (trx_undo_rec_get_undo_no(last_rec) >= limit || log_is_in_distress()) {

		mtr_commit(&mtr);

		return;
	}

	page_no = page_get_page_no(undo_page);

	if (page_no == hdr_page_no) {
		uint16_t end = mach_read_from_2(hdr_offset + TRX_UNDO_NEXT_LOG
						+ undo_page);
		if (end == 0) {
			end = mach_read_from_2(TRX_UNDO_PAGE_HDR
					       + TRX_UNDO_PAGE_FREE
					       + undo_page);
		}

		mlog_write_ulint(undo_page + hdr_offset + TRX_UNDO_LOG_START,
				 end, MLOG_2BYTES, &mtr);
	} else {
		trx_undo_free_page(rseg, true, hdr_page_no, page_no, &mtr);
	}

	mtr_commit(&mtr);

	goto loop;
}

/** Frees an undo log segment which is not in the history list.
@param undo	temporary undo log */
static void trx_undo_seg_free(const trx_undo_t *undo)
{
	trx_rseg_t*	rseg;
	fseg_header_t*	file_seg;
	trx_rsegf_t*	rseg_header;
	trx_usegf_t*	seg_header;
	ibool		finished;
	mtr_t		mtr;

	rseg = undo->rseg;

	do {
		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);

		mutex_enter(&(rseg->mutex));

		seg_header = trx_undo_page_get(page_id_t(SRV_TMP_SPACE_ID,
							 undo->hdr_page_no),
					       &mtr)
			+ TRX_UNDO_SEG_HDR;

		file_seg = seg_header + TRX_UNDO_FSEG_HEADER;

		finished = fseg_free_step(file_seg, &mtr);

		if (finished) {
			/* Update the rseg header */
			rseg_header = trx_rsegf_get(
				rseg->space, rseg->page_no, &mtr);
			trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL,
					       &mtr);

			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);
		}

		mutex_exit(&(rseg->mutex));
		mtr_commit(&mtr);
	} while (!finished);
}

/*========== UNDO LOG MEMORY COPY INITIALIZATION =====================*/

/** Read an undo log when starting up the database.
@param[in,out]	rseg		rollback segment
@param[in]	id		rollback segment slot
@param[in]	page_no		undo log segment page number
@param[in,out]	max_trx_id	the largest observed transaction ID
@return	the undo log
@retval nullptr on error */
trx_undo_t *
trx_undo_mem_create_at_db_start(trx_rseg_t *rseg, ulint id, uint32_t page_no,
                                trx_id_t &max_trx_id)
{
	mtr_t		mtr;
	XID		xid;

	ut_ad(id < TRX_RSEG_N_SLOTS);

	mtr.start();
	const page_t* undo_page = trx_undo_page_get(
		page_id_t(rseg->space->id, page_no), &mtr);
	const uint16_t type = mach_read_from_2(TRX_UNDO_PAGE_HDR
					       + TRX_UNDO_PAGE_TYPE
					       + undo_page);
	if (UNIV_UNLIKELY(type > 2)) {
corrupted_type:
		sql_print_error("InnoDB: unsupported undo header type %u",
				type);
corrupted:
		mtr.commit();
		return nullptr;
	}

	uint16_t offset = mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG
					   + undo_page);
	if (offset < TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE ||
	    offset >= srv_page_size - TRX_UNDO_LOG_OLD_HDR_SIZE) {
		sql_print_error("InnoDB: invalid undo header offset %u",
				offset);
		goto corrupted;
	}

	const trx_ulogf_t* const undo_header = undo_page + offset;
	uint16_t state = mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE
					  + undo_page);
	switch (state) {
	case TRX_UNDO_ACTIVE:
	case TRX_UNDO_PREPARED:
		if (UNIV_LIKELY(type != 1)) {
			break;
		}
		sql_print_error("InnoDB: upgrade from older version than"
				" MariaDB 10.3 requires clean shutdown");
		goto corrupted;
	default:
		sql_print_error("InnoDB: unsupported undo header state %u",
				state);
		goto corrupted;
	case TRX_UNDO_TO_PURGE:
		if (UNIV_UNLIKELY(type == 1)) {
			goto corrupted_type;
		}
		/* fall through */
	case TRX_UNDO_CACHED:
		trx_id_t id = mach_read_from_8(TRX_UNDO_TRX_NO + undo_header);
		if (id >> 48) {
			sql_print_error("InnoDB: corrupted TRX_NO %llx", id);
			goto corrupted;
		}
		if (id > max_trx_id) {
			max_trx_id = id;
		}
	}

	/* Read X/Open XA transaction identification if it exists, or
	set it to NULL. */

	if (undo_header[TRX_UNDO_XID_EXISTS]) {
		trx_undo_read_xid(undo_header, &xid);
	} else {
		xid.null();
	}

	trx_id_t trx_id = mach_read_from_8(undo_header + TRX_UNDO_TRX_ID);
	if (trx_id >> 48) {
		sql_print_error("InnoDB: corrupted TRX_ID %llx", trx_id);
		goto corrupted;
	}
	if (trx_id > max_trx_id) {
		max_trx_id = trx_id;
	}

	mutex_enter(&rseg->mutex);
	trx_undo_t* undo = trx_undo_mem_create(
		rseg, id, trx_id, &xid, page_no, offset);
	mutex_exit(&rseg->mutex);
	if (!undo) {
		return undo;
	}

	undo->dict_operation = undo_header[TRX_UNDO_DICT_TRANS];
	undo->table_id = mach_read_from_8(undo_header + TRX_UNDO_TABLE_ID);
	undo->size = flst_get_len(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST
				  + undo_page);

	fil_addr_t	last_addr = flst_get_last(
		TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + undo_page, &mtr);

	undo->last_page_no = last_addr.page;
	undo->top_page_no = last_addr.page;

	page_t* last_page = trx_undo_page_get(
		page_id_t(rseg->space->id, undo->last_page_no), &mtr);

	if (const trx_undo_rec_t* rec = trx_undo_page_get_last_rec(
		    last_page, page_no, offset)) {
		undo->top_offset = ulint(rec - last_page);
		undo->top_undo_no = trx_undo_rec_get_undo_no(rec);
		ut_ad(!undo->empty());
	} else {
		undo->top_undo_no = IB_ID_MAX;
		ut_ad(undo->empty());
	}

	undo->state = state;

	if (state != TRX_UNDO_CACHED) {
		UT_LIST_ADD_LAST(rseg->undo_list, undo);
	} else {
		UT_LIST_ADD_LAST(rseg->undo_cached, undo);
		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
	}

	mtr.commit();
	return undo;
}

/********************************************************************//**
Creates and initializes an undo log memory object.
@return own: the undo log memory object */
static
trx_undo_t*
trx_undo_mem_create(
/*================*/
	trx_rseg_t*	rseg,	/*!< in: rollback segment memory object */
	ulint		id,	/*!< in: slot index within rseg */
	trx_id_t	trx_id,	/*!< in: id of the trx for which the undo log
				is created */
	const XID*	xid,	/*!< in: X/Open transaction identification */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset)	/*!< in: undo log header byte offset on page */
{
	trx_undo_t*	undo;

	ut_ad(mutex_own(&(rseg->mutex)));

	ut_a(id < TRX_RSEG_N_SLOTS);

	undo = static_cast<trx_undo_t*>(ut_malloc_nokey(sizeof(*undo)));

	if (undo == NULL) {

		return(NULL);
	}

	undo->id = id;
	undo->state = TRX_UNDO_ACTIVE;
	undo->trx_id = trx_id;
	undo->xid = *xid;

	undo->dict_operation = FALSE;

	undo->rseg = rseg;

	undo->hdr_page_no = page_no;
	undo->hdr_offset = offset;
	undo->last_page_no = page_no;
	undo->size = 1;

	undo->top_undo_no = IB_ID_MAX;
	undo->top_page_no = page_no;
	undo->guess_block = NULL;
	ut_ad(undo->empty());

	return(undo);
}

/********************************************************************//**
Initializes a cached undo log object for new use. */
static
void
trx_undo_mem_init_for_reuse(
/*========================*/
	trx_undo_t*	undo,	/*!< in: undo log to init */
	trx_id_t	trx_id,	/*!< in: id of the trx for which the undo log
				is created */
	const XID*	xid,	/*!< in: X/Open XA transaction identification*/
	ulint		offset)	/*!< in: undo log header byte offset on page */
{
	ut_ad(mutex_own(&((undo->rseg)->mutex)));

	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	undo->state = TRX_UNDO_ACTIVE;
	undo->trx_id = trx_id;
	undo->xid = *xid;

	undo->dict_operation = FALSE;

	undo->hdr_offset = offset;
	undo->top_undo_no = IB_ID_MAX;
	ut_ad(undo->empty());
}

/** Create an undo log.
@param[in,out]	trx	transaction
@param[in,out]	rseg	rollback segment
@param[out]	undo	undo log object
@param[out]	err	error code
@param[in,out]	mtr	mini-transaction
@return undo log block
@retval	NULL	on failure */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
buf_block_t*
trx_undo_create(trx_t* trx, trx_rseg_t* rseg, trx_undo_t** undo,
		dberr_t* err, mtr_t* mtr)
{
	ulint		id;

	ut_ad(mutex_own(&(rseg->mutex)));

	buf_block_t*	block = trx_undo_seg_create(
		rseg->space,
		trx_rsegf_get(rseg->space, rseg->page_no, mtr), &id, err, mtr);

	if (!block) {
		return NULL;
	}

	rseg->curr_size++;

	ulint offset = trx_undo_header_create(block->frame, trx->id, mtr);

	trx_undo_header_add_space_for_xid(block->frame, block->frame + offset,
					  mtr);

	*undo = trx_undo_mem_create(rseg, id, trx->id, trx->xid,
				    block->page.id.page_no(), offset);
	if (*undo == NULL) {
		*err = DB_OUT_OF_MEMORY;
		 /* FIXME: this will not free the undo block to the file */
		return NULL;
	} else if (rseg != trx->rsegs.m_redo.rseg) {
		return block;
	}

	switch (trx_get_dict_operation(trx)) {
	case TRX_DICT_OP_NONE:
		break;
	case TRX_DICT_OP_INDEX:
		/* Do not discard the table on recovery. */
		trx->table_id = 0;
		/* fall through */
	case TRX_DICT_OP_TABLE:
		(*undo)->table_id = trx->table_id;
		(*undo)->dict_operation = TRUE;
		mlog_write_ulint(block->frame + offset + TRX_UNDO_DICT_TRANS,
				 TRUE, MLOG_1BYTE, mtr);
		mlog_write_ull(block->frame + offset + TRX_UNDO_TABLE_ID,
			       trx->table_id, mtr);
	}

	*err = DB_SUCCESS;
	return block;
}

/*================ UNDO LOG ASSIGNMENT AND CLEANUP =====================*/

/** Reuse a cached undo log block.
@param[in,out]	trx	transaction
@param[in,out]	rseg	rollback segment
@param[out]	pundo	the undo log memory object
@param[in,out]	mtr	mini-transaction
@return	the undo log block
@retval	NULL	if none cached */
static
buf_block_t*
trx_undo_reuse_cached(trx_t* trx, trx_rseg_t* rseg, trx_undo_t** pundo,
		      mtr_t* mtr)
{
	ut_ad(mutex_own(&rseg->mutex));

	trx_undo_t* undo = UT_LIST_GET_FIRST(rseg->undo_cached);
	if (!undo) {
		return NULL;
	}

	ut_ad(undo->size == 1);
	ut_ad(undo->id < TRX_RSEG_N_SLOTS);

	buf_block_t*	block = buf_page_get(page_id_t(undo->rseg->space->id,
						       undo->hdr_page_no),
					     0, RW_X_LATCH, mtr);
	if (!block) {
		return NULL;
	}

	buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

	UT_LIST_REMOVE(rseg->undo_cached, undo);
	MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);

	*pundo = undo;

	ulint offset = trx_undo_header_create(block->frame, trx->id, mtr);
	/* Reset the TRX_UNDO_PAGE_TYPE in case this page is being
	repurposed after upgrading to MariaDB 10.3. */
	if (ut_d(ulint type =) UNIV_UNLIKELY(
		    mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE
				     + block->frame))) {
		ut_ad(type == 1 || type == 2);
		mlog_write_ulint(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE
				 + block->frame, 0, MLOG_2BYTES, mtr);
	}

	trx_undo_header_add_space_for_xid(block->frame, block->frame + offset,
					  mtr);

	trx_undo_mem_init_for_reuse(undo, trx->id, trx->xid, offset);

	if (rseg != trx->rsegs.m_redo.rseg) {
		return block;
	}

	switch (trx_get_dict_operation(trx)) {
	case TRX_DICT_OP_NONE:
		return block;
	case TRX_DICT_OP_INDEX:
		/* Do not discard the table on recovery. */
		trx->table_id = 0;
		/* fall through */
	case TRX_DICT_OP_TABLE:
		undo->table_id = trx->table_id;
		undo->dict_operation = TRUE;
		mlog_write_ulint(block->frame + offset + TRX_UNDO_DICT_TRANS,
				 TRUE, MLOG_1BYTE, mtr);
		mlog_write_ull(block->frame + offset + TRX_UNDO_TABLE_ID,
			       trx->table_id, mtr);
	}

	return block;
}

/** Assign an undo log for a persistent transaction.
A new undo log is created or a cached undo log reused.
@param[in,out]	trx	transaction
@param[out]	err	error code
@param[in,out]	mtr	mini-transaction
@return	the undo log block
@retval	NULL	on error */
buf_block_t*
trx_undo_assign(trx_t* trx, dberr_t* err, mtr_t* mtr)
{
	ut_ad(mtr->get_log_mode() == MTR_LOG_ALL);

	trx_undo_t* undo = trx->rsegs.m_redo.undo;

	if (undo) {
		return buf_page_get_gen(
			page_id_t(undo->rseg->space->id, undo->last_page_no),
			0, RW_X_LATCH, undo->guess_block,
			BUF_GET, __FILE__, __LINE__, mtr, err);
	}

	trx_rseg_t* rseg = trx->rsegs.m_redo.rseg;

	mutex_enter(&rseg->mutex);
	buf_block_t* block = trx_undo_reuse_cached(
		trx, rseg, &trx->rsegs.m_redo.undo, mtr);

	if (!block) {
		block = trx_undo_create(trx, rseg, &trx->rsegs.m_redo.undo,
					err, mtr);
		ut_ad(!block == (*err != DB_SUCCESS));
		if (!block) {
			goto func_exit;
		}
	} else {
		*err = DB_SUCCESS;
	}

	UT_LIST_ADD_FIRST(rseg->undo_list, trx->rsegs.m_redo.undo);

func_exit:
	mutex_exit(&rseg->mutex);
	return block;
}

/** Assign an undo log for a transaction.
A new undo log is created or a cached undo log reused.
@param[in,out]	trx	transaction
@param[in]	rseg	rollback segment
@param[out]	undo	the undo log
@param[out]	err	error code
@param[in,out]	mtr	mini-transaction
@return	the undo log block
@retval	NULL	on error */
buf_block_t*
trx_undo_assign_low(trx_t* trx, trx_rseg_t* rseg, trx_undo_t** undo,
		    dberr_t* err, mtr_t* mtr)
{
  const bool	is_temp __attribute__((unused)) = rseg == trx->rsegs.m_noredo.rseg;

	ut_ad(rseg == trx->rsegs.m_redo.rseg
	      || rseg == trx->rsegs.m_noredo.rseg);
	ut_ad(undo == (is_temp
		       ? &trx->rsegs.m_noredo.undo
		       : &trx->rsegs.m_redo.undo));
	ut_ad(mtr->get_log_mode()
	      == (is_temp ? MTR_LOG_NO_REDO : MTR_LOG_ALL));

	if (*undo) {
		return buf_page_get_gen(
			page_id_t(rseg->space->id, (*undo)->last_page_no),
			0, RW_X_LATCH, (*undo)->guess_block,
			BUF_GET, __FILE__, __LINE__, mtr, err);
	}

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_too_many_trx",
		*err = DB_TOO_MANY_CONCURRENT_TRXS; return NULL;
	);

	mutex_enter(&rseg->mutex);

	buf_block_t* block = trx_undo_reuse_cached(trx, rseg, undo, mtr);

	if (!block) {
		block = trx_undo_create(trx, rseg, undo, err, mtr);
		ut_ad(!block == (*err != DB_SUCCESS));
		if (!block) {
			goto func_exit;
		}
	} else {
		*err = DB_SUCCESS;
	}

	UT_LIST_ADD_FIRST(rseg->undo_list, *undo);

func_exit:
	mutex_exit(&rseg->mutex);
	return block;
}

/******************************************************************//**
Sets the state of the undo log segment at a transaction finish.
@return undo log segment header page, x-latched */
page_t*
trx_undo_set_state_at_finish(
/*=========================*/
	trx_undo_t*	undo,	/*!< in: undo log memory copy */
	mtr_t*		mtr)	/*!< in: mtr */
{
	trx_usegf_t*	seg_hdr;
	trx_upagef_t*	page_hdr;
	page_t*		undo_page;
	ulint		state;

	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	undo_page = trx_undo_page_get(
		page_id_t(undo->rseg->space->id, undo->hdr_page_no), mtr);

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	if (undo->size == 1
	    && mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE)
	       < TRX_UNDO_PAGE_REUSE_LIMIT) {

		state = TRX_UNDO_CACHED;
	} else {
		state = TRX_UNDO_TO_PURGE;
	}

	undo->state = state;

	mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, state, MLOG_2BYTES, mtr);

	return(undo_page);
}

/** Set the state of the undo log segment at a XA PREPARE or XA ROLLBACK.
@param[in,out]	trx		transaction
@param[in,out]	undo		undo log
@param[in]	rollback	false=XA PREPARE, true=XA ROLLBACK
@param[in,out]	mtr		mini-transaction */
void
trx_undo_set_state_at_prepare(
	trx_t*		trx,
	trx_undo_t*	undo,
	bool		rollback,
	mtr_t*		mtr)
{
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	undo_header;
	page_t*		undo_page;
	ulint		offset;

	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	undo_page = trx_undo_page_get(
		page_id_t(undo->rseg->space->id, undo->hdr_page_no), mtr);

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	if (rollback) {
		ut_ad(undo->state == TRX_UNDO_PREPARED);
		mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE,
				 MLOG_2BYTES, mtr);
		return;
	}

	/*------------------------------*/
	ut_ad(undo->state == TRX_UNDO_ACTIVE);
	undo->state = TRX_UNDO_PREPARED;
	undo->xid   = *trx->xid;
	/*------------------------------*/

	mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, undo->state,
			 MLOG_2BYTES, mtr);

	offset = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
	undo_header = undo_page + offset;

	mlog_write_ulint(undo_header + TRX_UNDO_XID_EXISTS,
			 TRUE, MLOG_1BYTE, mtr);

	trx_undo_write_xid(undo_header, &undo->xid, mtr);
}

/** Free temporary undo log after commit or rollback.
The information is not needed after a commit or rollback, therefore
the data can be discarded.
@param undo     temporary undo log */
void trx_undo_commit_cleanup(trx_undo_t *undo)
{
	trx_rseg_t*	rseg	= undo->rseg;
	ut_ad(rseg->space == fil_system.temp_space);

	mutex_enter(&rseg->mutex);

	UT_LIST_REMOVE(rseg->undo_list, undo);

	if (undo->state == TRX_UNDO_CACHED) {
		UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
	} else {
		ut_ad(undo->state == TRX_UNDO_TO_PURGE);

		/* Delete first the undo log segment in the file */
		mutex_exit(&rseg->mutex);
		trx_undo_seg_free(undo);
		mutex_enter(&rseg->mutex);

		ut_ad(rseg->curr_size > undo->size);
		rseg->curr_size -= undo->size;

		ut_free(undo);
	}

	mutex_exit(&rseg->mutex);
}

/** At shutdown, frees the undo logs of a transaction. */
void trx_undo_free_at_shutdown(trx_t *trx)
{
	if (trx_undo_t*& undo = trx->rsegs.m_redo.undo) {
		switch (undo->state) {
		case TRX_UNDO_PREPARED:
			break;
		case TRX_UNDO_CACHED:
		case TRX_UNDO_TO_PURGE:
			ut_ad(trx_state_eq(trx,
					   TRX_STATE_COMMITTED_IN_MEMORY));
			/* fall through */
		case TRX_UNDO_ACTIVE:
			/* trx_t::commit_state() assigns
			trx->state = TRX_STATE_COMMITTED_IN_MEMORY. */
			ut_a(!srv_was_started
			     || srv_read_only_mode
			     || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO
			     || srv_fast_shutdown);
			break;
		default:
			ut_error;
		}

		UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->undo_list, undo);
		ut_free(undo);
		undo = NULL;
	}
	if (trx_undo_t*& undo = trx->rsegs.m_noredo.undo) {
		ut_a(undo->state == TRX_UNDO_PREPARED);

		UT_LIST_REMOVE(trx->rsegs.m_noredo.rseg->undo_list, undo);
		ut_free(undo);
		undo = NULL;
	}
}
