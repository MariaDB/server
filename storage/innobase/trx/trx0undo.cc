/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2022, MariaDB Corporation.

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
When a transaction does its first insert or modify in the clustered index, an
undo log is assigned for it. Then we must have an x-latch to the rollback
segment header.
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
  return page_no == block->page.id().page_no()
    ? mach_read_from_2(offset + TRX_UNDO_LOG_START + block->page.frame)
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
    ? nullptr : block->page.frame + start;
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
    ? nullptr
    : block->page.frame + mach_read_from_2(block->page.frame + end - 2);
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
                                            block->page.frame).page;

  if (prev_page_no == FIL_NULL)
    return nullptr;

  block= buf_page_get(page_id_t(block->page.id().space(), prev_page_no),
                      0, shared ? RW_S_LATCH : RW_X_LATCH, mtr);

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
  ut_ad(block->page.frame == page_align(rec));
  return
    rec == block->page.frame + trx_undo_page_get_start(block, page_no, offset)
    ? nullptr
    : block->page.frame + mach_read_from_2(rec - 2);
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
                                                       block->page.frame + rec,
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
  if (page_no == block->page.id().page_no() &&
      mach_read_from_2(block->page.frame + offset + TRX_UNDO_NEXT_LOG))
    return NULL;

  uint32_t next= flst_get_next_addr(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE +
				    block->page.frame).page;
  if (next == FIL_NULL)
    return NULL;

  block= buf_page_get(page_id_t(block->page.id().space(), next), 0, mode, mtr);

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

  if (trx_undo_rec_t *rec= trx_undo_page_get_first_rec(block, page_no, offset))
    return rec;

  return trx_undo_get_next_rec_from_next_page(block, page_no, offset, mode,
                                              mtr);
}

inline void UndorecApplier::assign_rec(const buf_block_t &block,
                                       uint16_t offset)
{
  ut_ad(block.page.lock.have_s());
  this->offset= offset;
  this->undo_rec= trx_undo_rec_copy(block.page.frame + offset, heap);
}

void UndorecApplier::apply_undo_rec()
{
  bool updated_extern= false;
  undo_no_t undo_no= 0;
  table_id_t table_id= 0;
  undo_rec= trx_undo_rec_get_pars(undo_rec, &type,
                                  &cmpl_info,
                                  &updated_extern, &undo_no, &table_id);
  dict_sys.freeze(SRW_LOCK_CALL);
  dict_table_t *table= dict_sys.find_table(table_id);
  dict_sys.unfreeze();

  ut_ad(table);
  if (!table->is_active_ddl())
    return;

  dict_index_t *index= dict_table_get_first_index(table);
  const dtuple_t *undo_tuple;
  switch (type) {
  default:
    ut_ad("invalid type" == 0);
    MY_ASSERT_UNREACHABLE();
  case TRX_UNDO_INSERT_REC:
    undo_rec= trx_undo_rec_get_row_ref(undo_rec, index, &undo_tuple, heap);
  insert:
    log_insert(*undo_tuple, index);
    break;
  case TRX_UNDO_UPD_EXIST_REC:
  case TRX_UNDO_UPD_DEL_REC:
  case TRX_UNDO_DEL_MARK_REC:
    trx_id_t trx_id;
    roll_ptr_t roll_ptr;
    byte info_bits;
    undo_rec= trx_undo_update_rec_get_sys_cols(
      undo_rec, &trx_id, &roll_ptr, &info_bits);

    undo_rec= trx_undo_rec_get_row_ref(undo_rec, index, &undo_tuple, heap);
    undo_rec= trx_undo_update_rec_get_update(undo_rec, index, type, trx_id,
                                             roll_ptr, info_bits,
                                             heap, &update);
    if (type == TRX_UNDO_UPD_DEL_REC)
      goto insert;
    log_update(*undo_tuple, index);
  }

  clear_undo_rec();
}

/** Apply any changes to tables for which online DDL is in progress. */
ATTRIBUTE_COLD void trx_t::apply_log()
{
  if (undo_no == 0 || apply_online_log == false)
    return;
  const trx_undo_t *undo= rsegs.m_redo.undo;
  if (!undo)
    return;
  page_id_t page_id{rsegs.m_redo.rseg->space->id, undo->hdr_page_no};
  page_id_t next_page_id(page_id);
  mtr_t mtr;
  mtr.start();
  buf_block_t *block= buf_page_get(page_id, 0, RW_S_LATCH, &mtr);
  ut_ad(block);

  UndorecApplier log_applier(page_id, id);

  for (;;)
  {
    trx_undo_rec_t *rec= trx_undo_page_get_first_rec(block, page_id.page_no(),
                                                     undo->hdr_offset);
    while (rec)
    {
      log_applier.assign_rec(*block, page_offset(rec));
      mtr.commit();
      log_applier.apply_undo_rec();
      mtr.start();
      block= buf_page_get(log_applier.get_page_id(), 0, RW_S_LATCH, &mtr);
      rec= trx_undo_page_get_next_rec(block, log_applier.get_offset(),
                                      page_id.page_no(), undo->hdr_offset);
    }

    uint32_t next= mach_read_from_4(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE +
                                    FLST_NEXT + FIL_ADDR_PAGE +
                                    block->page.frame);
    if (next == FIL_NULL)
      break;
    next_page_id.set_page_no(next);
    mtr.commit();
    mtr.start();
    block= buf_page_get_gen(next_page_id, 0, RW_S_LATCH, block, BUF_GET, &mtr);
    log_applier.assign_next(next_page_id);
    ut_ad(block);
  }
  mtr.commit();
  apply_online_log= false;
}

/*============== UNDO LOG FILE COPY CREATION AND FREEING ==================*/

/** Initialize an undo log page.
NOTE: This corresponds to a redo log record and must not be changed!
@see mtr_t::undo_create()
@param block   undo log page */
void trx_undo_page_init(const buf_block_t &block)
{
  mach_write_to_2(my_assume_aligned<2>(FIL_PAGE_TYPE + block.page.frame),
                  FIL_PAGE_UNDO_LOG);
  static_assert(TRX_UNDO_PAGE_HDR == FIL_PAGE_DATA, "compatibility");
  memset_aligned<2>(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE + block.page.frame,
                    0, 2);
  mach_write_to_2(my_assume_aligned<2>
                  (TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START + block.page.frame),
                  TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
  memcpy_aligned<2>(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE + block.page.frame,
                    TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START + block.page.frame,
                    2);
  /* The following corresponds to flst_zero_both(), but without writing log. */
  memset_aligned<4>(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + FLST_PREV +
                    FIL_ADDR_PAGE + block.page.frame, 0xff, 4);
  memset_aligned<2>(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + FLST_PREV +
                    FIL_ADDR_BYTE + block.page.frame, 0, 2);
  memset_aligned<2>(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + FLST_NEXT +
                    FIL_ADDR_PAGE + block.page.frame, 0xff, 4);
  memset_aligned<2>(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + FLST_NEXT +
                    FIL_ADDR_BYTE + block.page.frame, 0, 2);
  static_assert(TRX_UNDO_PAGE_NODE + FLST_NEXT + FIL_ADDR_BYTE + 2 ==
                TRX_UNDO_PAGE_HDR_SIZE, "compatibility");
  /* Preserve TRX_UNDO_SEG_HDR, but clear the rest of the page. */
  memset_aligned<2>(TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE +
                    block.page.frame, 0,
                    srv_page_size - (TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE +
                                     FIL_PAGE_DATA_END));
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
	uint32_t	n_reserved;
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
	block = fseg_create(space, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
			    mtr, true);

	space->release_free_extents(n_reserved);

	if (block == NULL) {
		*err = DB_OUT_OF_FILE_SPACE;
		return NULL;
	}

	mtr->undo_create(*block);
	trx_undo_page_init(*block);

	mtr->write<2>(*block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
		      + block->page.frame,
		      TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE);
	mtr->write<2,mtr_t::MAYBE_NOP>(*block,
				       TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG
				       + block->page.frame, 0U);

	flst_init(*block, TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST
		  + block->page.frame, mtr);

	flst_add_last(block, TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		      block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);

	*id = slot_no;
	mtr->write<4>(*rseg_hdr, TRX_RSEG + TRX_RSEG_UNDO_SLOTS
		      + slot_no * TRX_RSEG_SLOT_SIZE + rseg_hdr->page.frame,
		      block->page.id().page_no());

	MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);

	*err = DB_SUCCESS;
	return block;
}

/** Initialize an undo log header.
@param[in,out]  undo_page   undo log segment header page
@param[in]      trx_id      transaction identifier
@param[in,out]  mtr         mini-transaction
@return header byte offset on page */
static uint16_t trx_undo_header_create(buf_block_t *undo_page, trx_id_t trx_id,
                                       mtr_t* mtr)
{
  /* Reset the TRX_UNDO_PAGE_TYPE in case this page is being
  repurposed after upgrading to MariaDB 10.3. */
  byte *undo_type= my_assume_aligned<2>
    (TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE + undo_page->page.frame);
  ut_ad(mach_read_from_2(undo_type) <= 2);
  mtr->write<2,mtr_t::MAYBE_NOP>(*undo_page, undo_type, 0U);
  byte *start= my_assume_aligned<4>(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START +
                                    undo_page->page.frame);
  const uint16_t free= mach_read_from_2(start + 2);
  static_assert(TRX_UNDO_PAGE_START + 2 == TRX_UNDO_PAGE_FREE,
                "compatibility");
  ut_a(free + TRX_UNDO_LOG_XA_HDR_SIZE < srv_page_size - 100);

  mach_write_to_2(start, free + TRX_UNDO_LOG_XA_HDR_SIZE);
  /* A WRITE of 2 bytes is never longer than a MEMMOVE.
  So, WRITE 2+2 bytes is better than WRITE+MEMMOVE.
  But, a MEMSET will only be 1+2 bytes, that is, 1 byte shorter! */
  memcpy_aligned<2>(start + 2, start, 2);
  mtr->memset(*undo_page, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START, 4,
              start, 2);
  uint16_t prev_log= mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG +
                                      undo_page->page.frame);
  alignas(4) byte buf[4];
  mach_write_to_2(buf, TRX_UNDO_ACTIVE);
  mach_write_to_2(buf + 2, free);
  static_assert(TRX_UNDO_STATE + 2 == TRX_UNDO_LAST_LOG, "compatibility");
  static_assert(!((TRX_UNDO_SEG_HDR + TRX_UNDO_STATE) % 4), "alignment");
  mtr->memcpy(*undo_page, my_assume_aligned<4>
              (TRX_UNDO_SEG_HDR + TRX_UNDO_STATE + undo_page->page.frame),
              buf, 4);
  if (prev_log)
    mtr->write<2>(*undo_page, prev_log + TRX_UNDO_NEXT_LOG +
                  undo_page->page.frame, free);
  mtr->write<8,mtr_t::MAYBE_NOP>(*undo_page, free + TRX_UNDO_TRX_ID +
                                 undo_page->page.frame, trx_id);
  if (UNIV_UNLIKELY(mach_read_from_8(free + TRX_UNDO_TRX_NO +
                                     undo_page->page.frame) != 0))
    mtr->memset(undo_page, free + TRX_UNDO_TRX_NO, 8, 0);

  /* Write TRX_UNDO_NEEDS_PURGE=1 and TRX_UNDO_LOG_START. */
  mach_write_to_2(buf, 1);
  memcpy_aligned<2>(buf + 2, start, 2);
  static_assert(TRX_UNDO_NEEDS_PURGE + 2 == TRX_UNDO_LOG_START,
                "compatibility");
  mtr->memcpy<mtr_t::MAYBE_NOP>(*undo_page, free + TRX_UNDO_NEEDS_PURGE +
                                undo_page->page.frame, buf, 4);
  /* Initialize all fields TRX_UNDO_XID_EXISTS to TRX_UNDO_HISTORY_NODE. */
  if (prev_log)
  {
    mtr->memset(undo_page, free + TRX_UNDO_XID_EXISTS,
                TRX_UNDO_PREV_LOG - TRX_UNDO_XID_EXISTS, 0);
    mtr->write<2,mtr_t::MAYBE_NOP>(*undo_page, free + TRX_UNDO_PREV_LOG +
                                   undo_page->page.frame, prev_log);
    static_assert(TRX_UNDO_PREV_LOG + 2 == TRX_UNDO_HISTORY_NODE,
                  "compatibility");
    mtr->memset(undo_page, free + TRX_UNDO_HISTORY_NODE, FLST_NODE_SIZE, 0);
    static_assert(TRX_UNDO_LOG_OLD_HDR_SIZE == TRX_UNDO_HISTORY_NODE +
                  FLST_NODE_SIZE, "compatibility");
  }
  else
    mtr->memset(undo_page, free + TRX_UNDO_XID_EXISTS,
                TRX_UNDO_LOG_OLD_HDR_SIZE - TRX_UNDO_XID_EXISTS, 0);
  return free;
}

/** Write X/Open XA Transaction Identifier (XID) to undo log header
@param[in,out]  block   undo header page
@param[in]      offset  undo header record offset
@param[in]      xid     distributed transaction identifier
@param[in,out]  mtr     mini-transaction */
static void trx_undo_write_xid(buf_block_t *block, uint16_t offset,
                               const XID &xid, mtr_t *mtr)
{
  DBUG_ASSERT(xid.gtrid_length > 0);
  DBUG_ASSERT(xid.bqual_length >= 0);
  DBUG_ASSERT(xid.gtrid_length <= MAXGTRIDSIZE);
  DBUG_ASSERT(xid.bqual_length <= MAXBQUALSIZE);
  static_assert(MAXGTRIDSIZE + MAXBQUALSIZE == XIDDATASIZE,
                "gtrid and bqual don't fit xid data");
  DBUG_ASSERT(mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG +
                               block->page.frame) == offset);

  trx_ulogf_t* log_hdr= block->page.frame + offset;

  mtr->write<4,mtr_t::MAYBE_NOP>(*block, log_hdr + TRX_UNDO_XA_FORMAT,
                                 static_cast<uint32_t>(xid.formatID));
  mtr->write<4,mtr_t::MAYBE_NOP>(*block, log_hdr + TRX_UNDO_XA_TRID_LEN,
                                 static_cast<uint32_t>(xid.gtrid_length));
  mtr->write<4,mtr_t::MAYBE_NOP>(*block, log_hdr + TRX_UNDO_XA_BQUAL_LEN,
                                 static_cast<uint32_t>(xid.bqual_length));
  const ulint xid_length= static_cast<ulint>(xid.gtrid_length
                                             + xid.bqual_length);
  mtr->memcpy(*block, &block->page.frame[offset + TRX_UNDO_XA_XID],
              xid.data, xid_length);
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

/** Allocate an undo log page.
@param[in,out]	undo	undo log
@param[in,out]	mtr	mini-transaction that does not hold any page latch
@return	X-latched block if success
@retval	NULL	on failure */
buf_block_t* trx_undo_add_page(trx_undo_t* undo, mtr_t* mtr)
{
	trx_rseg_t*	rseg		= undo->rseg;
	buf_block_t*	new_block	= NULL;
	uint32_t	n_reserved;

	/* When we add a page to an undo log, this is analogous to
	a pessimistic insert in a B-tree, and we must reserve the
	counterpart of the tree latch, which is the rseg mutex. */

	rseg->latch.wr_lock(SRW_LOCK_CALL);

	buf_block_t* header_block = trx_undo_page_get(
		page_id_t(undo->rseg->space->id, undo->hdr_page_no), mtr);

	if (!fsp_reserve_free_extents(&n_reserved, undo->rseg->space, 1,
				      FSP_UNDO, mtr)) {
		goto func_exit;
	}

	new_block = fseg_alloc_free_page_general(
		TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER
		+ header_block->page.frame,
		undo->top_page_no + 1, FSP_UP, true, mtr, mtr);

	rseg->space->release_free_extents(n_reserved);

	if (!new_block) {
		goto func_exit;
	}

	undo->last_page_no = new_block->page.id().page_no();

	mtr->undo_create(*new_block);
	trx_undo_page_init(*new_block);

	flst_add_last(header_block, TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		      new_block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);
	undo->size++;
	rseg->curr_size++;

func_exit:
	rseg->latch.wr_unlock();
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
	const uint32_t space = rseg->space->id;

	ut_a(hdr_page_no != page_no);

	buf_block_t* undo_block = trx_undo_page_get(page_id_t(space, page_no),
						    mtr);
	buf_block_t* header_block = trx_undo_page_get(page_id_t(space,
								hdr_page_no),
						      mtr);

	flst_remove(header_block, TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		    undo_block, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);

	fseg_free_page(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER
		       + header_block->page.frame,
		       rseg->space, page_no, mtr);
	buf_page_free(rseg->space, page_no, mtr);

	const fil_addr_t last_addr = flst_get_last(
		TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST
		+ header_block->page.frame);
	rseg->curr_size--;

	if (in_history) {
		buf_block_t* rseg_header = trx_rsegf_get(
			rseg->space, rseg->page_no, mtr);
		byte* rseg_hist_size = TRX_RSEG + TRX_RSEG_HISTORY_SIZE
			+ rseg_header->page.frame;
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
		undo.rseg->latch.wr_lock(SRW_LOCK_CALL);
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
			undo.rseg->latch.wr_unlock();
			mtr.commit();
			continue;
		}

func_exit:
		undo.rseg->latch.wr_unlock();

		if (trunc_here) {
			mtr.write<2>(*undo_block,
				     TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
				     + undo_block->page.frame,
				     ulint(trunc_here
					   - undo_block->page.frame));
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

	if (undo_page->page.id().page_no() == hdr_page_no) {
		uint16_t end = mach_read_from_2(hdr_offset + TRX_UNDO_NEXT_LOG
						+ undo_page->page.frame);
		if (end == 0) {
			end = mach_read_from_2(TRX_UNDO_PAGE_HDR
					       + TRX_UNDO_PAGE_FREE
					       + undo_page->page.frame);
		}

		mtr.write<2>(*undo_page, undo_page->page.frame + hdr_offset
			     + TRX_UNDO_LOG_START, end);
	} else {
		trx_undo_free_page(rseg, true, hdr_page_no,
				   undo_page->page.id().page_no(), &mtr);
	}

	mtr_commit(&mtr);

	goto loop;
}

/** Frees an undo log segment which is not in the history list.
@param undo	temporary undo log */
static void trx_undo_seg_free(const trx_undo_t *undo)
{
	ut_ad(undo->id < TRX_RSEG_N_SLOTS);

	trx_rseg_t* const rseg = undo->rseg;
	bool		finished;
	mtr_t		mtr;
	ut_ad(rseg->space == fil_system.temp_space);

	do {
		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);

		buf_block_t* block = trx_undo_page_get(
			page_id_t(SRV_TMP_SPACE_ID, undo->hdr_page_no), &mtr);

		fseg_header_t* file_seg = TRX_UNDO_SEG_HDR
			+ TRX_UNDO_FSEG_HEADER + block->page.frame;

		finished = fseg_free_step(file_seg, &mtr);

		if (finished) {
			/* Update the rseg header */
			buf_block_t* rseg_header = trx_rsegf_get(
				rseg->space, rseg->page_no, &mtr);
			compile_time_assert(FIL_NULL == 0xffffffff);
			memset(TRX_RSEG + TRX_RSEG_UNDO_SLOTS
			       + undo->id * TRX_RSEG_SLOT_SIZE +
			       rseg_header->page.frame, 0xff, 4);
			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);
		}

		mtr.commit();
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
	const buf_block_t* block = trx_undo_page_get(
		page_id_t(rseg->space->id, page_no), &mtr);
	const uint16_t type = mach_read_from_2(TRX_UNDO_PAGE_HDR
					       + TRX_UNDO_PAGE_TYPE
					       + block->page.frame);
	if (UNIV_UNLIKELY(type > 2)) {
corrupted_type:
		sql_print_error("InnoDB: unsupported undo header type %u",
				type);
corrupted:
		mtr.commit();
		return nullptr;
	}

	uint16_t offset = mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG
					   + block->page.frame);
	if (offset < TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE ||
	    offset >= srv_page_size - TRX_UNDO_LOG_OLD_HDR_SIZE) {
		sql_print_error("InnoDB: invalid undo header offset %u",
				offset);
		goto corrupted;
	}

	const trx_ulogf_t* const undo_header = block->page.frame + offset;
	uint16_t state = mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE
					  + block->page.frame);
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
	case TRX_UNDO_CACHED:
		if (UNIV_UNLIKELY(type != 0)) {
			/* This undo page was not updated by MariaDB
			10.3 or later. The TRX_UNDO_TRX_NO field may
			contain garbage. */
			break;
		}
		goto read_trx_no;
	case TRX_UNDO_TO_PURGE:
		if (UNIV_UNLIKELY(type == 1)) {
			goto corrupted_type;
		}
	read_trx_no:
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

	trx_undo_t* undo = trx_undo_mem_create(
		rseg, id, trx_id, &xid, page_no, offset);
	if (!undo) {
		return undo;
	}

	undo->dict_operation = undo_header[TRX_UNDO_DICT_TRANS];
	undo->size = flst_get_len(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST
				  + block->page.frame);

	fil_addr_t	last_addr = flst_get_last(
		TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + block->page.frame);

	undo->last_page_no = last_addr.page;
	undo->top_page_no = last_addr.page;

	const buf_block_t* last = trx_undo_page_get(
		page_id_t(rseg->space->id, undo->last_page_no), &mtr);

	if (const trx_undo_rec_t* rec = trx_undo_page_get_last_rec(
		    last, page_no, offset)) {
		undo->top_offset = static_cast<uint16_t>(
			rec - last->page.frame);
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
	uint32_t	page_no,/*!< in: undo log header page number */
	uint16_t	offset)	/*!< in: undo log header byte offset on page */
{
	trx_undo_t*	undo;

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
	uint16_t	offset)	/*!< in: undo log header byte offset on page */
{
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
	buf_block_t*	block = trx_undo_seg_create(
		rseg->space,
		trx_rsegf_get(rseg->space, rseg->page_no, mtr), &id, err, mtr);

	if (!block) {
		return NULL;
	}

	rseg->curr_size++;

	uint16_t offset = trx_undo_header_create(block, trx->id, mtr);

	*undo = trx_undo_mem_create(rseg, id, trx->id, &trx->xid,
				    block->page.id().page_no(), offset);
	if (*undo == NULL) {
		*err = DB_OUT_OF_MEMORY;
		 /* FIXME: this will not free the undo block to the file */
		return NULL;
	} else if (rseg != trx->rsegs.m_redo.rseg) {
		return block;
	}

	if (trx->dict_operation) {
		(*undo)->dict_operation = true;
		mtr->write<1,mtr_t::MAYBE_NOP>(*block,
					       block->page.frame + offset
					       + TRX_UNDO_DICT_TRANS, 1U);
		mtr->write<8,mtr_t::MAYBE_NOP>(*block,
					       block->page.frame + offset
					       + TRX_UNDO_TABLE_ID, 0U);
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

	UT_LIST_REMOVE(rseg->undo_cached, undo);
	MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);

	*pundo = undo;

	uint16_t offset = trx_undo_header_create(block, trx->id, mtr);

	trx_undo_mem_init_for_reuse(undo, trx->id, &trx->xid, offset);

	if (rseg != trx->rsegs.m_redo.rseg) {
		return block;
	}

	if (trx->dict_operation) {
		undo->dict_operation = TRUE;
		mtr->write<1,mtr_t::MAYBE_NOP>(*block,
					       block->page.frame + offset
					       + TRX_UNDO_DICT_TRANS, 1U);
		mtr->write<8,mtr_t::MAYBE_NOP>(*block,
					       block->page.frame + offset
					       + TRX_UNDO_TABLE_ID, 0U);
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
			BUF_GET, mtr, err);
	}

	trx_rseg_t* rseg = trx->rsegs.m_redo.rseg;

	rseg->latch.wr_lock(SRW_LOCK_CALL);
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
	rseg->latch.wr_unlock();
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
	ut_d(const bool	is_temp = rseg == trx->rsegs.m_noredo.rseg);
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
			BUF_GET, mtr, err);
	}

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_too_many_trx",
		*err = DB_TOO_MANY_CONCURRENT_TRXS; return NULL;
	);

	rseg->latch.wr_lock(SRW_LOCK_CALL);

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
	rseg->latch.wr_unlock();
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
				   + block->page.frame)
		? TRX_UNDO_CACHED
		: TRX_UNDO_TO_PURGE;

	undo->state = state;
	mtr->write<2>(*block, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE
		      + block->page.frame, state);
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
			      + block->page.frame, TRX_UNDO_ACTIVE);
		return;
	}

	/*------------------------------*/
	ut_ad(undo->state == TRX_UNDO_ACTIVE);
	undo->state = TRX_UNDO_PREPARED;
	undo->xid   = trx->xid;
	/*------------------------------*/

	mtr->write<2>(*block, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE
		      + block->page.frame, undo->state);
	uint16_t offset = mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG
					   + block->page.frame);
	mtr->write<1>(*block, block->page.frame + offset + TRX_UNDO_XID_EXISTS,
		      1U);

	trx_undo_write_xid(block, offset, undo->xid, mtr);
}

/** Free temporary undo log after commit or rollback.
The information is not needed after a commit or rollback, therefore
the data can be discarded.
@param undo     temporary undo log */
void trx_undo_commit_cleanup(trx_undo_t *undo)
{
	trx_rseg_t*	rseg	= undo->rseg;
	ut_ad(rseg->space == fil_system.temp_space);

	rseg->latch.wr_lock(SRW_LOCK_CALL);

	UT_LIST_REMOVE(rseg->undo_list, undo);

	if (undo->state == TRX_UNDO_CACHED) {
		UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
		undo = nullptr;
	} else {
		ut_ad(undo->state == TRX_UNDO_TO_PURGE);

		/* Delete first the undo log segment in the file */
		trx_undo_seg_free(undo);

		ut_ad(rseg->curr_size > undo->size);
		rseg->curr_size -= undo->size;
	}

	rseg->latch.wr_unlock();
	ut_free(undo);
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
