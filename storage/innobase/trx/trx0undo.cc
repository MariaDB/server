/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2019, MariaDB Corporation.

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
#include "trx0rseg.h"

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
	uint32_t	page_no,/*!< in: undo log header page number */
	uint16_t	offset);/*!< in: undo log header byte offset on page */

/** Determine the start offset of undo log records of an undo log page.
@param[in]	block	undo log page
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset
@return start offset */
static
uint16_t trx_undo_page_get_start(const buf_block_t *block, uint32_t page_no,
                                 uint16_t offset)
{
  return page_no == block->page.id.page_no()
    ? mach_read_from_2(offset + TRX_UNDO_LOG_START + block->frame)
    : TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE;
}

/** Get the first undo log record on a page.
@param[in]	block	undo log page
@param[in]	page_no	undo log header page number
@param[in]	offset	undo log header page offset
@return	pointer to first record
@retval	NULL	if none exists */
static trx_undo_rec_t*
trx_undo_page_get_first_rec(const buf_block_t *block, uint32_t page_no,
                            uint16_t offset)
{
  uint16_t start= trx_undo_page_get_start(block, page_no, offset);
  return start == trx_undo_page_get_end(block, page_no, offset)
    ? nullptr : block->frame + start;
}

/** Get the last undo log record on a page.
@param[in]	page	undo log page
@param[in]	page_no	undo log header page number
@param[in]	offset	undo log header page offset
@return	pointer to last record
@retval	NULL	if none exists */
static
trx_undo_rec_t*
trx_undo_page_get_last_rec(const buf_block_t *block, uint32_t page_no,
                           uint16_t offset)
{
  uint16_t end= trx_undo_page_get_end(block, page_no, offset);
  return trx_undo_page_get_start(block, page_no, offset) == end
    ? nullptr : block->frame + mach_read_from_2(block->frame + end - 2);
}

/** Get the previous record in an undo log from the previous page.
@param[in,out]  block   undo log page
@param[in]      rec     undo record offset in the page
@param[in]      page_no undo log header page number
@param[in]      offset  undo log header offset on page
@param[in]      shared  latching mode: true=RW_S_LATCH, false=RW_X_LATCH
@param[in,out]  mtr     mini-transaction
@return undo log record, the page latched, NULL if none */
static trx_undo_rec_t*
trx_undo_get_prev_rec_from_prev_page(buf_block_t *&block, uint16_t rec,
                                     uint32_t page_no, uint16_t offset,
                                     bool shared, mtr_t *mtr)
{
  uint32_t prev_page_no= flst_get_prev_addr(TRX_UNDO_PAGE_HDR +
                                            TRX_UNDO_PAGE_NODE +
                                            block->frame).page;

  if (prev_page_no == FIL_NULL)
    return NULL;

  block = buf_page_get(page_id_t(block->page.id.space(), prev_page_no),
                       0, shared ? RW_S_LATCH : RW_X_LATCH, mtr);
  buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

  return trx_undo_page_get_last_rec(block, page_no, offset);
}

/** Get the previous undo log record.
@param[in]	block	undo log page
@param[in]	rec	undo log record
@param[in]	page_no	undo log header page number
@param[in]	offset	undo log header page offset
@return	pointer to record
@retval	NULL if none */
static
trx_undo_rec_t*
trx_undo_page_get_prev_rec(const buf_block_t *block, trx_undo_rec_t *rec,
                           uint32_t page_no, uint16_t offset)
{
  ut_ad(block->frame == page_align(rec));
  return rec == block->frame + trx_undo_page_get_start(block, page_no, offset)
    ? nullptr
    : block->frame + mach_read_from_2(rec - 2);
}

/** Get the previous record in an undo log.
@param[in,out]  block   undo log page
@param[in]      rec     undo record offset in the page
@param[in]      page_no undo log header page number
@param[in]      offset  undo log header offset on page
@param[in]      shared  latching mode: true=RW_S_LATCH, false=RW_X_LATCH
@param[in,out]  mtr     mini-transaction
@return undo log record, the page latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_prev_rec(buf_block_t *&block, uint16_t rec, uint32_t page_no,
                      uint16_t offset, bool shared, mtr_t *mtr)
{
  if (trx_undo_rec_t *prev= trx_undo_page_get_prev_rec(block,
                                                       block->frame + rec,
                                                       page_no, offset))
    return prev;

  /* We have to go to the previous undo log page to look for the
  previous record */

  return trx_undo_get_prev_rec_from_prev_page(block, rec, page_no, offset,
                                              shared, mtr);
}

/** Get the next record in an undo log from the next page.
@param[in,out]  block   undo log page
@param[in]      page_no undo log header page number
@param[in]      offset  undo log header offset on page
@param[in]      mode    latching mode: RW_S_LATCH or RW_X_LATCH
@param[in,out]  mtr     mini-transaction
@return undo log record, the page latched, NULL if none */
static trx_undo_rec_t*
trx_undo_get_next_rec_from_next_page(buf_block_t *&block, uint32_t page_no,
                                     uint16_t offset, ulint mode, mtr_t *mtr)
{
  if (page_no == block->page.id.page_no() &&
      mach_read_from_2(block->frame + offset + TRX_UNDO_NEXT_LOG))
    return NULL;

  ulint next= flst_get_next_addr(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE +
                                 block->frame).page;
  if (next == FIL_NULL)
    return NULL;

  block= buf_page_get(page_id_t(block->page.id.space(), next), 0, mode, mtr);
  buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

  return trx_undo_page_get_first_rec(block, page_no, offset);
}

/** Get the next record in an undo log.
@param[in,out]  block   undo log page
@param[in]      rec     undo record offset in the page
@param[in]      page_no undo log header page number
@param[in]      offset  undo log header offset on page
@param[in,out]  mtr     mini-transaction
@return undo log record, the page latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_next_rec(buf_block_t *&block, uint16_t rec, uint32_t page_no,
                      uint16_t offset, mtr_t *mtr)
{
  if (trx_undo_rec_t *next= trx_undo_page_get_next_rec(block, rec, page_no,
                                                       offset))
    return next;

  return trx_undo_get_next_rec_from_next_page(block, page_no, offset,
                                              RW_S_LATCH, mtr);
}

/** Get the first record in an undo log.
@param[in]      space   undo log header space
@param[in]      page_no undo log header page number
@param[in]      offset  undo log header offset on page
@param[in]      mode    latching mode: RW_S_LATCH or RW_X_LATCH
@param[out]     block   undo log page
@param[in,out]  mtr     mini-transaction
@return undo log record, the page latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_first_rec(const fil_space_t &space, uint32_t page_no,
                       uint16_t offset, ulint mode, buf_block_t*& block,
                       mtr_t *mtr)
{
  block = buf_page_get(page_id_t(space.id, page_no), 0, mode, mtr);
  buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

  if (trx_undo_rec_t *rec= trx_undo_page_get_first_rec(block, page_no, offset))
    return rec;

  return trx_undo_get_next_rec_from_next_page(block, page_no, offset, mode,
                                              mtr);
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
			       + undo_page)
	      == TRX_UNDO_INSERT);

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
@param[in,out]	undo_block	undo log segment page
@param[in,out]	mtr		mini-transaction */
static void trx_undo_page_init(const buf_block_t *undo_block, mtr_t *mtr)
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

/** Look for a free slot for an undo log segment.
@param rseg_header   rollback segment header
@return slot index
@retval ULINT_UNDEFINED if not found */
static ulint trx_rsegf_undo_find_free(const buf_block_t *rseg_header)
{
  ulint max_slots= TRX_RSEG_N_SLOTS;

#ifdef UNIV_DEBUG
  if (trx_rseg_n_slots_debug)
    max_slots= std::min<ulint>(trx_rseg_n_slots_debug, TRX_RSEG_N_SLOTS);
#endif

  for (ulint i= 0; i < max_slots; i++)
    if (trx_rsegf_get_nth_undo(rseg_header, i) == FIL_NULL)
      return i;

  return ULINT_UNDEFINED;
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
trx_undo_seg_create(fil_space_t *space, buf_block_t *rseg_hdr, ulint *id,
                    dberr_t *err, mtr_t *mtr)
{
	buf_block_t*	block;
	ulint		n_reserved;
	bool		success;

	const ulint slot_no = trx_rsegf_undo_find_free(rseg_hdr);

	if (slot_no == ULINT_UNDEFINED) {
		ib::warn() << "Cannot find a free slot for an undo log. Do"
			" you have too many active transactions running"
			" concurrently?";

		*err = DB_TOO_MANY_CONCURRENT_TRXS;
		return NULL;
	}

	ut_ad(slot_no < TRX_RSEG_N_SLOTS);

	success = fsp_reserve_free_extents(&n_reserved, space, 2, FSP_UNDO,
					   mtr);
	if (!success) {
		*err = DB_OUT_OF_FILE_SPACE;
		return NULL;
	}

	/* Allocate a new file segment for the undo log */
	block = fseg_create(space, 0, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
			    mtr, true);

	space->release_free_extents(n_reserved);

	if (block == NULL) {
		*err = DB_OUT_OF_FILE_SPACE;
		return NULL;
	}

	buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

	trx_undo_page_init(block, mtr);

	mtr->write<2,mtr_t::OPT>(*block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
				 + block->frame,
				 TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE);
	mtr->write<2,mtr_t::OPT>(*block, TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG
				 + block->frame, 0U);

	flst_init(*block, TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + block->frame,
		  mtr);

	flst_add_last(block, TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		      block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);

	*id = slot_no;
	mtr->write<4>(*rseg_hdr, TRX_RSEG + TRX_RSEG_UNDO_SLOTS
		      + slot_no * TRX_RSEG_SLOT_SIZE + rseg_hdr->frame,
		      block->page.id.page_no());

	MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);

	*err = DB_SUCCESS;
	return block;
}

/***************************************************************//**
Creates a new undo log header in file. NOTE that this function has its own
log record type MLOG_UNDO_HDR_CREATE. You must NOT change the operation of
this function!
@param[in,out]  undo_page   undo log segment header page
@param[in]      trx_id      transaction identifier
@param[in,out]  mtr         mini-transaction
@return header byte offset on page */
static uint16_t trx_undo_header_create(buf_block_t *undo_page, trx_id_t trx_id,
                                       mtr_t* mtr)
{
	byte* page_free = TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
		+ undo_page->frame;
	uint16_t free = mach_read_from_2(page_free);
	uint16_t new_free = free + TRX_UNDO_LOG_OLD_HDR_SIZE;

	ut_a(free + TRX_UNDO_LOG_XA_HDR_SIZE < srv_page_size - 100);

	mach_write_to_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START
			+ undo_page->frame, new_free);
	mach_write_to_2(page_free, new_free);

	mach_write_to_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE + undo_page->frame,
			TRX_UNDO_ACTIVE);
	byte* last_log = TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG
		+ undo_page->frame;

	uint16_t prev_log = mach_read_from_2(last_log);

	if (prev_log != 0) {
		mach_write_to_2(prev_log + TRX_UNDO_NEXT_LOG + undo_page->frame,
				free);
	}

	mach_write_to_2(last_log, free);

	trx_ulogf_t* log_hdr = undo_page->frame + free;

	mach_write_to_2(log_hdr + TRX_UNDO_NEEDS_PURGE, 1);

	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);

	mach_write_to_1(log_hdr + TRX_UNDO_XID_EXISTS, FALSE);
	mach_write_to_1(log_hdr + TRX_UNDO_DICT_TRANS, FALSE);

	mach_write_to_2(log_hdr + TRX_UNDO_NEXT_LOG, 0);
	mach_write_to_2(log_hdr + TRX_UNDO_PREV_LOG, prev_log);

	/* Write the log record about the header creation */
	mtr->set_modified();
	if (mtr->get_log_mode() != MTR_LOG_ALL) {
		ut_ad(mtr->get_log_mode() == MTR_LOG_NONE
		      || mtr->get_log_mode() == MTR_LOG_NO_REDO);
		return free;
	}

	byte* log_ptr = mtr->get_log()->open(11 + 15);
	log_ptr = mlog_write_initial_log_record_low(
		MLOG_UNDO_HDR_CREATE,
		undo_page->page.id.space(),
		undo_page->page.id.page_no(),
		log_ptr, mtr);
	log_ptr += mach_u64_write_compressed(log_ptr, trx_id);
	mlog_close(mtr, log_ptr);

	return(free);
}

/** Write X/Open XA Transaction Identifier (XID) to undo log header
@param[in,out]  block   undo header page
@param[in]      offset  undo header record offset
@param[in]      xid     distributed transaction identifier
@param[in,out]  mtr     mini-transaction */
static void trx_undo_write_xid(buf_block_t *block, uint16_t offset,
                               const XID &xid, mtr_t *mtr)
{
  DBUG_ASSERT(xid.gtrid_length >= 0);
  DBUG_ASSERT(xid.bqual_length >= 0);
  DBUG_ASSERT(xid.gtrid_length + xid.bqual_length < XIDDATASIZE);
  DBUG_ASSERT(mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG +
                               block->frame) == offset);

  trx_ulogf_t* log_hdr= block->frame + offset;

  mtr->write<4,mtr_t::OPT>(*block, log_hdr + TRX_UNDO_XA_FORMAT,
                           static_cast<uint32_t>(xid.formatID));
  mtr->write<4,mtr_t::OPT>(*block, log_hdr + TRX_UNDO_XA_TRID_LEN,
                           static_cast<uint32_t>(xid.gtrid_length));
  mtr->write<4,mtr_t::OPT>(*block, log_hdr + TRX_UNDO_XA_BQUAL_LEN,
                           static_cast<uint32_t>(xid.bqual_length));
  const ulint xid_length= static_cast<ulint>(xid.gtrid_length
                                             + xid.bqual_length);
  mtr->memcpy(block, offset + TRX_UNDO_XA_XID, xid.data, xid_length);
  if (UNIV_LIKELY(xid_length < XIDDATASIZE))
    mtr->memset(block, offset + TRX_UNDO_XA_XID + xid_length,
                XIDDATASIZE - xid_length, 0);
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

/** Add space for the XA XID after an undo log old-style header.
@param[in,out]  block   undo page
@param[in]      offset  offset of the undo log header
@param[in,out]  mtr     mini-transaction */
static void trx_undo_header_add_space_for_xid(buf_block_t *block, ulint offset,
                                              mtr_t *mtr)
{
  uint16_t free= mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE +
                                   block->frame);
  /* free is now the end offset of the old style undo log header */
  ut_a(free == offset + TRX_UNDO_LOG_OLD_HDR_SIZE);
  free += TRX_UNDO_LOG_XA_HDR_SIZE - TRX_UNDO_LOG_OLD_HDR_SIZE;
  /* Add space for a XID after the header, update the free offset
  fields on the undo log page and in the undo log header */

  mtr->write<2>(*block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START + block->frame,
                free);
  /* MDEV-12353 TODO: use MEMMOVE record */
  mtr->write<2>(*block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE + block->frame,
                free);
  mtr->write<2>(*block, offset + TRX_UNDO_LOG_START + block->frame, free);
}

/** Parse the redo log entry of an undo log page header create.
@param[in]	ptr	redo log record
@param[in]	end_ptr	end of log buffer
@param[in,out]	block	page frame or NULL
@param[in,out]	mtr	mini-transaction or NULL
@return end of log record or NULL */
byte*
trx_undo_parse_page_header(
	const byte*	ptr,
	const byte*	end_ptr,
	buf_block_t*	block,
	mtr_t*		mtr)
{
	trx_id_t	trx_id = mach_u64_parse_compressed(&ptr, end_ptr);

	if (ptr && block) {
		trx_undo_header_create(block, trx_id, mtr);

	}

	return const_cast<byte*>(ptr);
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

	/* When we add a page to an undo log, this is analogous to
	a pessimistic insert in a B-tree, and we must reserve the
	counterpart of the tree latch, which is the rseg mutex. */

	mutex_enter(&rseg->mutex);

	buf_block_t* header_block = trx_undo_page_get(
		page_id_t(undo->rseg->space->id, undo->hdr_page_no), mtr);

	if (!fsp_reserve_free_extents(&n_reserved, undo->rseg->space, 1,
				      FSP_UNDO, mtr)) {
		goto func_exit;
	}

	new_block = fseg_alloc_free_page_general(
		TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER
		+ header_block->frame,
		undo->top_page_no + 1, FSP_UP, TRUE, mtr, mtr);

	rseg->space->release_free_extents(n_reserved);

	if (!new_block) {
		goto func_exit;
	}

	ut_ad(rw_lock_get_x_lock_count(&new_block->lock) == 1);
	buf_block_dbg_add_level(new_block, SYNC_TRX_UNDO_PAGE);
	undo->last_page_no = new_block->page.id.page_no();

	trx_undo_page_init(new_block, mtr);

	flst_add_last(header_block, TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		      new_block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);
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
uint32_t
trx_undo_free_page(
/*===============*/
	trx_rseg_t* rseg,	/*!< in: rollback segment */
	bool	in_history,	/*!< in: TRUE if the undo log is in the history
				list */
	uint32_t hdr_page_no,	/*!< in: header page number */
	uint32_t page_no,	/*!< in: page number to free: must not be the
				header page */
	mtr_t*	mtr)		/*!< in: mtr which does not have a latch to any
				undo log page; the caller must have reserved
				the rollback segment mutex */
{
	const ulint	space = rseg->space->id;

	ut_a(hdr_page_no != page_no);
	ut_ad(mutex_own(&(rseg->mutex)));

	buf_block_t* undo_block = trx_undo_page_get(page_id_t(space, page_no),
						    mtr);
	buf_block_t* header_block = trx_undo_page_get(page_id_t(space,
								hdr_page_no),
						      mtr);

	flst_remove(header_block, TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		    undo_block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);

	fseg_free_page(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER
		       + header_block->frame,
		       rseg->space, page_no, false, true, mtr);

	const fil_addr_t last_addr = flst_get_last(
		TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + header_block->frame);
	rseg->curr_size--;

	if (in_history) {
		buf_block_t* rseg_header = trx_rsegf_get(
			rseg->space, rseg->page_no, mtr);
		byte* rseg_hist_size = TRX_RSEG + TRX_RSEG_HISTORY_SIZE
			+ rseg_header->frame;
		uint32_t hist_size = mach_read_from_4(rseg_hist_size);
		ut_ad(hist_size > 0);
		mtr->write<4>(*rseg_header, rseg_hist_size, hist_size - 1);
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
		buf_block_t* undo_block = trx_undo_page_get(
			page_id_t(undo.rseg->space->id, undo.last_page_no),
			&mtr);
		trx_undo_rec_t* rec = trx_undo_page_get_last_rec(
			undo_block, undo.hdr_page_no, undo.hdr_offset);
		while (rec) {
			if (trx_undo_rec_get_undo_no(rec) < limit) {
				goto func_exit;
			}
			/* Truncate at least this record off, maybe more */
			trunc_here = rec;

			rec = trx_undo_page_get_prev_rec(undo_block, rec,
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
			mtr.write<2>(*undo_block,
				     TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
				     + undo_block->frame,
				     ulint(trunc_here - undo_block->frame));
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
	uint32_t	hdr_page_no,
	uint16_t	hdr_offset,
	undo_no_t	limit)
{
	trx_undo_rec_t* rec;
	trx_undo_rec_t* last_rec;
	mtr_t		mtr;

	ut_ad(mutex_own(&(rseg->mutex)));

	if (!limit) {
		return;
	}
loop:
	mtr_start(&mtr);

	if (!rseg->is_persistent()) {
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	}

	buf_block_t* undo_page;
	rec = trx_undo_get_first_rec(*rseg->space, hdr_page_no, hdr_offset,
				     RW_X_LATCH, undo_page, &mtr);
	if (rec == NULL) {
		/* Already empty */
done:
		mtr.commit();
		return;
	}

	last_rec = trx_undo_page_get_last_rec(undo_page, hdr_page_no,
					      hdr_offset);
	if (trx_undo_rec_get_undo_no(last_rec) >= limit) {
		goto done;
	}

	if (undo_page->page.id.page_no() == hdr_page_no) {
		uint16_t end = mach_read_from_2(hdr_offset + TRX_UNDO_NEXT_LOG
						+ undo_page->frame);
		if (end == 0) {
			end = mach_read_from_2(TRX_UNDO_PAGE_HDR
					       + TRX_UNDO_PAGE_FREE
					       + undo_page->frame);
		}

		mtr.write<2>(*undo_page, undo_page->frame + hdr_offset
			     + TRX_UNDO_LOG_START, end);
	} else {
		trx_undo_free_page(rseg, true, hdr_page_no,
				   undo_page->page.id.page_no(), &mtr);
	}

	mtr_commit(&mtr);

	goto loop;
}

/** Frees an undo log segment which is not in the history list.
@param[in]	undo	undo log
@param[in]	noredo	whether the undo tablespace is redo logged */
static void trx_undo_seg_free(const trx_undo_t* undo, bool noredo)
{
	ut_ad(undo->id < TRX_RSEG_N_SLOTS);

	trx_rseg_t* const rseg = undo->rseg;
	bool		finished;
	mtr_t		mtr;

	do {
		mtr.start();

		if (noredo) {
			mtr.set_log_mode(MTR_LOG_NO_REDO);
		}

		mutex_enter(&rseg->mutex);

		buf_block_t* block = trx_undo_page_get(
			page_id_t(rseg->space->id, undo->hdr_page_no), &mtr);

		fseg_header_t* file_seg = TRX_UNDO_SEG_HDR
			+ TRX_UNDO_FSEG_HEADER + block->frame;

		finished = fseg_free_step(file_seg, false, &mtr);

		if (finished) {
			/* Update the rseg header */
			buf_block_t* rseg_header = trx_rsegf_get(
				rseg->space, rseg->page_no, &mtr);
			compile_time_assert(FIL_NULL == 0xffffffff);
			mtr.memset(rseg_header, TRX_RSEG + TRX_RSEG_UNDO_SLOTS
				   + undo->id * TRX_RSEG_SLOT_SIZE, 4, 0xff);
			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);
		}

		mutex_exit(&rseg->mutex);
		mtr.commit();
	} while (!finished);
}

/*========== UNDO LOG MEMORY COPY INITIALIZATION =====================*/

/** Read an undo log when starting up the database.
@param[in,out]	rseg		rollback segment
@param[in]	id		rollback segment slot
@param[in]	page_no		undo log segment page number
@param[in,out]	max_trx_id	the largest observed transaction ID
@return	size of the undo log in pages */
uint32_t
trx_undo_mem_create_at_db_start(trx_rseg_t *rseg, ulint id, uint32_t page_no,
                                trx_id_t &max_trx_id)
{
	mtr_t		mtr;
	XID		xid;

	ut_ad(id < TRX_RSEG_N_SLOTS);

	mtr.start();
	const buf_block_t* block = trx_undo_page_get(
		page_id_t(rseg->space->id, page_no), &mtr);
	const ulint type = mach_read_from_2(
		TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE + block->frame);
	ut_ad(type == 0 || type == TRX_UNDO_INSERT || type == TRX_UNDO_UPDATE);

	uint16_t state = mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE
					  + block->frame);
	uint16_t offset = mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG
					   + block->frame);

	const trx_ulogf_t*	undo_header = block->frame + offset;

	/* Read X/Open XA transaction identification if it exists, or
	set it to NULL. */

	if (undo_header[TRX_UNDO_XID_EXISTS]) {
		trx_undo_read_xid(undo_header, &xid);
	} else {
		xid.null();
	}

	trx_id_t trx_id = mach_read_from_8(undo_header + TRX_UNDO_TRX_ID);
	if (trx_id > max_trx_id) {
		max_trx_id = trx_id;
	}

	mutex_enter(&rseg->mutex);
	trx_undo_t* undo = trx_undo_mem_create(
		rseg, id, trx_id, &xid, page_no, offset);
	mutex_exit(&rseg->mutex);

	undo->dict_operation = undo_header[TRX_UNDO_DICT_TRANS];
	undo->table_id = mach_read_from_8(undo_header + TRX_UNDO_TABLE_ID);
	undo->size = flst_get_len(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST
				  + block->frame);

	if (UNIV_UNLIKELY(state == TRX_UNDO_TO_FREE)) {
		/* This is an old-format insert_undo log segment that
		is being freed. The page list is inconsistent. */
		ut_ad(type == TRX_UNDO_INSERT);
		state = TRX_UNDO_TO_PURGE;
	} else {
		if (state == TRX_UNDO_TO_PURGE
		    || state == TRX_UNDO_CACHED) {
			trx_id_t id = mach_read_from_8(TRX_UNDO_TRX_NO
						       + undo_header);
			if (id > max_trx_id) {
				max_trx_id = id;
			}
		}

		fil_addr_t	last_addr = flst_get_last(
			TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + block->frame);

		undo->last_page_no = last_addr.page;
		undo->top_page_no = last_addr.page;

		const buf_block_t* last = trx_undo_page_get(
			page_id_t(rseg->space->id, undo->last_page_no), &mtr);

		if (const trx_undo_rec_t* rec = trx_undo_page_get_last_rec(
			    last, page_no, offset)) {
			undo->top_offset = uint16_t(rec - last->frame);
			undo->top_undo_no = trx_undo_rec_get_undo_no(rec);
			ut_ad(!undo->empty());
		} else {
			undo->top_undo_no = IB_ID_MAX;
			ut_ad(undo->empty());
		}
	}

	undo->state = state;

	if (state != TRX_UNDO_CACHED) {
		UT_LIST_ADD_LAST(type == TRX_UNDO_INSERT
				 ? rseg->old_insert_list
				 : rseg->undo_list, undo);
	} else {
		UT_LIST_ADD_LAST(rseg->undo_cached, undo);
		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
	}

	mtr.commit();
	return undo->size;
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
	uint32_t	page_no,/*!< in: undo log header page number */
	uint16_t	offset)	/*!< in: undo log header byte offset on page */
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
	undo->withdraw_clock = 0;
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
	uint16_t	offset)	/*!< in: undo log header byte offset on page */
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

	uint16_t offset = trx_undo_header_create(block, trx->id, mtr);

	trx_undo_header_add_space_for_xid(block, offset, mtr);

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
		mtr->write<1,mtr_t::OPT>(*block, block->frame + offset
					 + TRX_UNDO_DICT_TRANS, 1U);
		mtr->write<8,mtr_t::OPT>(*block, block->frame + offset
					 + TRX_UNDO_TABLE_ID, trx->table_id);
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

	uint16_t offset = trx_undo_header_create(block, trx->id, mtr);
	/* Reset the TRX_UNDO_PAGE_TYPE in case this page is being
	repurposed after upgrading to MariaDB 10.3. */
	if (ut_d(ulint type =) UNIV_UNLIKELY(
		    mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE
				     + block->frame))) {
		ut_ad(type == TRX_UNDO_INSERT || type == TRX_UNDO_UPDATE);
		mtr->write<2>(*block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE
			      + block->frame, 0U);
	}

	trx_undo_header_add_space_for_xid(block, offset, mtr);

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
		mtr->write<1,mtr_t::OPT>(*block, block->frame + offset
					 + TRX_UNDO_DICT_TRANS, 1U);
		mtr->write<8,mtr_t::OPT>(*block, block->frame + offset
					 + TRX_UNDO_TABLE_ID, trx->table_id);
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
			0, RW_X_LATCH,
			buf_pool_is_obsolete(undo->withdraw_clock)
			? NULL : undo->guess_block,
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
			0, RW_X_LATCH,
			buf_pool_is_obsolete((*undo)->withdraw_clock)
			? NULL : (*undo)->guess_block,
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
buf_block_t*
trx_undo_set_state_at_finish(
/*=========================*/
	trx_undo_t*	undo,	/*!< in: undo log memory copy */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	buf_block_t* block = trx_undo_page_get(
		page_id_t(undo->rseg->space->id, undo->hdr_page_no), mtr);

	const uint16_t state = undo->size == 1
		&& TRX_UNDO_PAGE_REUSE_LIMIT
		> mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
				   + block->frame)
		? TRX_UNDO_CACHED
		: TRX_UNDO_TO_PURGE;

	undo->state = state;
	mtr->write<2>(*block, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE
		      + block->frame, state);
	return block;
}

/** Set the state of the undo log segment at a XA PREPARE or XA ROLLBACK.
@param[in,out]	trx		transaction
@param[in,out]	undo		undo log
@param[in]	rollback	false=XA PREPARE, true=XA ROLLBACK
@param[in,out]	mtr		mini-transaction
@return undo log segment header page, x-latched */
void trx_undo_set_state_at_prepare(trx_t *trx, trx_undo_t *undo, bool rollback,
				   mtr_t *mtr)
{
	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	buf_block_t* block = trx_undo_page_get(
		page_id_t(undo->rseg->space->id, undo->hdr_page_no), mtr);

	if (rollback) {
		ut_ad(undo->state == TRX_UNDO_PREPARED);
		mtr->write<2>(*block, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE
			      + block->frame, TRX_UNDO_ACTIVE);
		return;
	}

	/*------------------------------*/
	ut_ad(undo->state == TRX_UNDO_ACTIVE);
	undo->state = TRX_UNDO_PREPARED;
	undo->xid   = *trx->xid;
	/*------------------------------*/

	mtr->write<2>(*block, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE + block->frame,
		      undo->state);
	uint16_t offset = mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG
					   + block->frame);
	mtr->write<1>(*block, block->frame + offset + TRX_UNDO_XID_EXISTS, 1U);

	trx_undo_write_xid(block, offset, undo->xid, mtr);
}

/** Free an old insert or temporary undo log after commit or rollback.
The information is not needed after a commit or rollback, therefore
the data can be discarded.
@param[in,out]	undo	undo log
@param[in]	is_temp	whether this is temporary undo log */
void
trx_undo_commit_cleanup(trx_undo_t* undo, bool is_temp)
{
	trx_rseg_t*	rseg	= undo->rseg;
	ut_ad(is_temp == !rseg->is_persistent());
	ut_ad(!is_temp || 0 == UT_LIST_GET_LEN(rseg->old_insert_list));

	mutex_enter(&rseg->mutex);

	UT_LIST_REMOVE(is_temp ? rseg->undo_list : rseg->old_insert_list,
		       undo);

	if (undo->state == TRX_UNDO_CACHED) {
		UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
	} else {
		ut_ad(undo->state == TRX_UNDO_TO_PURGE);

		/* Delete first the undo log segment in the file */
		mutex_exit(&rseg->mutex);
		trx_undo_seg_free(undo, is_temp);
		mutex_enter(&rseg->mutex);

		ut_ad(rseg->curr_size > undo->size);
		rseg->curr_size -= undo->size;

		ut_free(undo);
	}

	mutex_exit(&rseg->mutex);
}

/** At shutdown, frees the undo logs of a transaction. */
void
trx_undo_free_at_shutdown(trx_t *trx)
{
	if (trx_undo_t*& undo = trx->rsegs.m_redo.undo) {
		switch (undo->state) {
		case TRX_UNDO_PREPARED:
			break;
		case TRX_UNDO_CACHED:
		case TRX_UNDO_TO_FREE:
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

	if (trx_undo_t*& undo = trx->rsegs.m_redo.old_insert) {
		switch (undo->state) {
		case TRX_UNDO_PREPARED:
			break;
		case TRX_UNDO_CACHED:
		case TRX_UNDO_TO_FREE:
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

		UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->old_insert_list, undo);
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
