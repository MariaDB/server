/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file trx/trx0rseg.cc
Rollback segment

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0rseg.h"
#include "trx0undo.h"
#include "fut0lst.h"
#include "srv0srv.h"
#include "trx0purge.h"
#include "srv0mon.h"

/** Get a newly created rollback segment header.
@param page_id   header page location
@param mtr       mini-transaction
@return rollback segment header, page x-latched */
static buf_block_t *trx_rsegf_get_new(page_id_t page_id, mtr_t *mtr)
{
#ifdef UNIV_DEBUG
  if (page_id.space() != SRV_TMP_SPACE_ID)
  {
    ut_ad(page_id.space() <= srv_undo_tablespaces_active || !srv_was_started);
    ut_ad(page_id.space() <= TRX_SYS_MAX_UNDO_SPACES);
  }
#endif
  return buf_page_get(page_id, 0, RW_X_LATCH, mtr);
}

#ifdef WITH_WSREP
#include <mysql/service_wsrep.h>

#ifdef UNIV_DEBUG
/** The latest known WSREP XID sequence number */
static long long wsrep_seqno = -1;
#endif /* UNIV_DEBUG */
/** The latest known WSREP XID UUID */
static unsigned char wsrep_uuid[16];

/** Write the WSREP XID information into rollback segment header.
@param[in,out]	rseg_header	rollback segment header
@param[in]	xid		WSREP XID
@param[in,out]	mtr		mini transaction */
static void
trx_rseg_write_wsrep_checkpoint(
	buf_block_t*	rseg_header,
	const XID*	xid,
	mtr_t*		mtr)
{
	DBUG_ASSERT(xid->gtrid_length >= 0);
	DBUG_ASSERT(xid->bqual_length >= 0);
	DBUG_ASSERT(xid->gtrid_length + xid->bqual_length < XIDDATASIZE);

	mtr->write<4,mtr_t::MAYBE_NOP>(*rseg_header,
				       TRX_RSEG + TRX_RSEG_WSREP_XID_FORMAT
				       + rseg_header->page.frame,
				       uint32_t(xid->formatID));

	mtr->write<4,mtr_t::MAYBE_NOP>(*rseg_header,
				       TRX_RSEG + TRX_RSEG_WSREP_XID_GTRID_LEN
				       + rseg_header->page.frame,
				       uint32_t(xid->gtrid_length));

	mtr->write<4,mtr_t::MAYBE_NOP>(*rseg_header,
				       TRX_RSEG + TRX_RSEG_WSREP_XID_BQUAL_LEN
				       + rseg_header->page.frame,
				       uint32_t(xid->bqual_length));

	const ulint xid_length = static_cast<ulint>(xid->gtrid_length
						    + xid->bqual_length);
	mtr->memcpy<mtr_t::MAYBE_NOP>(*rseg_header,
				      TRX_RSEG + TRX_RSEG_WSREP_XID_DATA
				      + rseg_header->page.frame,
				      xid->data, xid_length);
	if (xid_length < XIDDATASIZE
	    && memcmp(TRX_RSEG + TRX_RSEG_WSREP_XID_DATA
		      + rseg_header->page.frame, field_ref_zero,
		      XIDDATASIZE - xid_length)) {
		mtr->memset(rseg_header,
			    TRX_RSEG + TRX_RSEG_WSREP_XID_DATA + xid_length,
			    XIDDATASIZE - xid_length, 0);
	}
}

/** Update the WSREP XID information in rollback segment header.
@param[in,out]	rseg_header	rollback segment header
@param[in]	xid		WSREP XID
@param[in,out]	mtr		mini-transaction */
void
trx_rseg_update_wsrep_checkpoint(
	buf_block_t*	rseg_header,
	const XID*	xid,
	mtr_t*		mtr)
{
	ut_ad(wsrep_is_wsrep_xid(xid));

#ifdef UNIV_DEBUG
	/* Check that seqno is monotonically increasing */
	long long xid_seqno = wsrep_xid_seqno(xid);
	const byte* xid_uuid = wsrep_xid_uuid(xid);

	if (xid_seqno != -1
	    && !memcmp(xid_uuid, wsrep_uuid, sizeof wsrep_uuid)) {
		ut_ad(xid_seqno > wsrep_seqno);
	} else {
		memcpy(wsrep_uuid, xid_uuid, sizeof wsrep_uuid);
	}
	wsrep_seqno = xid_seqno;
#endif /* UNIV_DEBUG */
	trx_rseg_write_wsrep_checkpoint(rseg_header, xid, mtr);
}

/** Clear the WSREP XID information from rollback segment header.
@param[in,out]	block	rollback segment header
@param[in,out]	mtr 	mini-transaction */
static void trx_rseg_clear_wsrep_checkpoint(buf_block_t *block, mtr_t *mtr)
{
  mtr->memset(block, TRX_RSEG + TRX_RSEG_WSREP_XID_INFO,
              TRX_RSEG_WSREP_XID_DATA + XIDDATASIZE - TRX_RSEG_WSREP_XID_INFO,
              0);
}

static void
trx_rseg_update_wsrep_checkpoint(const XID* xid, mtr_t* mtr)
{
	const byte* xid_uuid = wsrep_xid_uuid(xid);
	/* We must make check against wsrep_uuid here, the
	trx_rseg_update_wsrep_checkpoint() writes over wsrep_uuid with
	xid contents in debug mode and the memcmp() will never give nonzero
	result. */
	const bool must_clear_rsegs = memcmp(wsrep_uuid, xid_uuid,
					     sizeof wsrep_uuid);
	const trx_rseg_t* rseg = &trx_sys.rseg_array[0];

	buf_block_t* rseg_header = trx_rsegf_get(rseg->space, rseg->page_no,
						 mtr);
	if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT
					   + rseg_header->page.frame))) {
		trx_rseg_format_upgrade(rseg_header, mtr);
	}

	trx_rseg_update_wsrep_checkpoint(rseg_header, xid, mtr);

	if (must_clear_rsegs) {
		/* Because the UUID part of the WSREP XID differed
		from current_xid_uuid, the WSREP group UUID was
		changed, and we must reset the XID in all rollback
		segment headers. */
		for (ulint rseg_id = 1; rseg_id < TRX_SYS_N_RSEGS; ++rseg_id) {
			const trx_rseg_t &rseg = trx_sys.rseg_array[rseg_id];
			if (rseg.space) {
				trx_rseg_clear_wsrep_checkpoint(
					trx_rsegf_get(rseg.space, rseg.page_no,
						      mtr),
				        mtr);
			}
		}
	}
}

/** Update WSREP checkpoint XID in first rollback segment header
as part of wsrep_set_SE_checkpoint() when it is guaranteed that there
are no wsrep transactions committing.
If the UUID part of the WSREP XID does not match to the UUIDs of XIDs already
stored into rollback segments, the WSREP XID in all the remaining rollback
segments will be reset.
@param[in]	xid		WSREP XID */
void trx_rseg_update_wsrep_checkpoint(const XID* xid)
{
	mtr_t	mtr;
	mtr.start();
	trx_rseg_update_wsrep_checkpoint(xid, &mtr);
	mtr.commit();
}

/** Read the WSREP XID information in rollback segment header.
@param[in]	rseg_header	Rollback segment header
@param[out]	xid		Transaction XID
@return	whether the WSREP XID was present */
static
bool trx_rseg_read_wsrep_checkpoint(const buf_block_t *rseg_header, XID &xid)
{
	int formatID = static_cast<int>(
		mach_read_from_4(TRX_RSEG + TRX_RSEG_WSREP_XID_FORMAT
				 + rseg_header->page.frame));
	if (formatID == 0) {
		return false;
	}

	xid.formatID = formatID;
	xid.gtrid_length = static_cast<int>(
		mach_read_from_4(TRX_RSEG + TRX_RSEG_WSREP_XID_GTRID_LEN
				 + rseg_header->page.frame));

	xid.bqual_length = static_cast<int>(
		mach_read_from_4(TRX_RSEG + TRX_RSEG_WSREP_XID_BQUAL_LEN
				 + rseg_header->page.frame));

	memcpy(xid.data, TRX_RSEG + TRX_RSEG_WSREP_XID_DATA
	       + rseg_header->page.frame, XIDDATASIZE);

	return true;
}

/** Read the WSREP XID from the TRX_SYS page (in case of upgrade).
@param[in]	page	TRX_SYS page
@param[out]	xid	WSREP XID (if present)
@return	whether the WSREP XID is present */
static bool trx_rseg_init_wsrep_xid(const page_t* page, XID& xid)
{
	if (mach_read_from_4(TRX_SYS + TRX_SYS_WSREP_XID_INFO
			     + TRX_SYS_WSREP_XID_MAGIC_N_FLD
			     + page)
	    != TRX_SYS_WSREP_XID_MAGIC_N) {
		return false;
	}

	xid.formatID = static_cast<int>(
		mach_read_from_4(
			TRX_SYS + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_FORMAT + page));
	xid.gtrid_length = static_cast<int>(
		mach_read_from_4(
			TRX_SYS + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_GTRID_LEN + page));
	xid.bqual_length = static_cast<int>(
		mach_read_from_4(
			TRX_SYS + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_BQUAL_LEN + page));
	memcpy(xid.data,
	       TRX_SYS + TRX_SYS_WSREP_XID_INFO
	       + TRX_SYS_WSREP_XID_DATA + page, XIDDATASIZE);
	return true;
}

/** Recover the latest WSREP checkpoint XID.
@param[out]	xid	WSREP XID
@return	whether the WSREP XID was found */
bool trx_rseg_read_wsrep_checkpoint(XID& xid)
{
	mtr_t		mtr;
	long long       max_xid_seqno = -1;
	bool		found = false;

	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS;
	     rseg_id++, mtr.commit()) {
		mtr.start();
		const buf_block_t* sys = trx_sysf_get(&mtr, false);
		const uint32_t page_no = trx_sysf_rseg_get_page_no(
			sys, rseg_id);

		if (page_no == FIL_NULL) {
			continue;
		}

		const buf_block_t* rseg_header = trx_rsegf_get_new(
			page_id_t(trx_sysf_rseg_get_space(sys, rseg_id),
				  page_no), &mtr);

		if (mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT
				     + rseg_header->page.frame)) {
			continue;
		}

		XID tmp_xid;
		long long tmp_seqno = 0;
		if (trx_rseg_read_wsrep_checkpoint(rseg_header, tmp_xid)
		    && (tmp_seqno = wsrep_xid_seqno(&tmp_xid))
		    > max_xid_seqno) {
			found = true;
			max_xid_seqno = tmp_seqno;
			xid = tmp_xid;
			memcpy(wsrep_uuid, wsrep_xid_uuid(&tmp_xid),
			       sizeof wsrep_uuid);
		}
	}

	return found;
}
#endif /* WITH_WSREP */

/** Upgrade a rollback segment header page to MariaDB 10.3 format.
@param[in,out]	rseg_header	rollback segment header page
@param[in,out]	mtr		mini-transaction */
void trx_rseg_format_upgrade(buf_block_t *rseg_header, mtr_t *mtr)
{
  mtr->memset(rseg_header, TRX_RSEG + TRX_RSEG_FORMAT, 4, 0);
  /* Clear also possible garbage at the end of the page. Old
  InnoDB versions did not initialize unused parts of pages. */
  mtr->memset(rseg_header, TRX_RSEG + TRX_RSEG_MAX_TRX_ID + 8,
              srv_page_size
              - (FIL_PAGE_DATA_END + TRX_RSEG + TRX_RSEG_MAX_TRX_ID + 8),
              0);
}

/** Create a rollback segment header.
@param[in,out]	space		system, undo, or temporary tablespace
@param[in]	rseg_id		rollback segment identifier
@param[in]	max_trx_id	new value of TRX_RSEG_MAX_TRX_ID
@param[in,out]	sys_header	the TRX_SYS page (NULL for temporary rseg)
@param[in,out]	mtr		mini-transaction
@return the created rollback segment
@retval	NULL	on failure */
buf_block_t*
trx_rseg_header_create(
	fil_space_t*	space,
	ulint		rseg_id,
	trx_id_t	max_trx_id,
	buf_block_t*	sys_header,
	mtr_t*		mtr)
{
	buf_block_t*	block;

	ut_ad(mtr->memo_contains(*space));
	ut_ad(!sys_header == (space == fil_system.temp_space));

	/* Allocate a new file segment for the rollback segment */
	block = fseg_create(space, TRX_RSEG + TRX_RSEG_FSEG_HEADER, mtr);

	if (block == NULL) {
		/* No space left */
		return block;
	}

	ut_ad(0 == mach_read_from_4(TRX_RSEG_FORMAT + TRX_RSEG
				    + block->page.frame));
	ut_ad(0 == mach_read_from_4(TRX_RSEG_HISTORY_SIZE + TRX_RSEG
				    + block->page.frame));
	ut_ad(0 == mach_read_from_4(TRX_RSEG_MAX_TRX_ID + TRX_RSEG
				    + block->page.frame));

	/* Initialize the history list */
	flst_init(block, TRX_RSEG_HISTORY + TRX_RSEG, mtr);

	mtr->write<8,mtr_t::MAYBE_NOP>(*block,
				       TRX_RSEG + TRX_RSEG_MAX_TRX_ID
				       + block->page.frame, max_trx_id);

	/* Reset the undo log slots */
	mtr->memset(block, TRX_RSEG_UNDO_SLOTS + TRX_RSEG,
		    TRX_RSEG_N_SLOTS * 4, 0xff);

	if (sys_header) {
		/* Add the rollback segment info to the free slot in
		the trx system header */

		mtr->write<4,mtr_t::MAYBE_NOP>(
			*sys_header,
			TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_SPACE
			+ rseg_id * TRX_SYS_RSEG_SLOT_SIZE
			+ sys_header->page.frame, space->id);
		mtr->write<4,mtr_t::MAYBE_NOP>(
			*sys_header,
			TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_PAGE_NO
			+ rseg_id * TRX_SYS_RSEG_SLOT_SIZE
			+ sys_header->page.frame, block->page.id().page_no());
	}

	return block;
}

void trx_rseg_t::destroy()
{
  latch.destroy();

  /* There can't be any active transactions. */
  ut_a(!UT_LIST_GET_LEN(undo_list));

  for (trx_undo_t *next, *undo= UT_LIST_GET_FIRST(undo_cached); undo;
       undo= next)
  {
    next= UT_LIST_GET_NEXT(undo_list, undo);
    UT_LIST_REMOVE(undo_cached, undo);
    ut_free(undo);
  }
}

void trx_rseg_t::init(fil_space_t *space, uint32_t page)
{
  latch.SRW_LOCK_INIT(trx_rseg_latch_key);
  ut_ad(!this->space);
  this->space= space;
  page_no= page;
  last_page_no= FIL_NULL;
  curr_size= 1;

  UT_LIST_INIT(undo_list, &trx_undo_t::undo_list);
  UT_LIST_INIT(undo_cached, &trx_undo_t::undo_list);
}

void trx_rseg_t::reinit(uint32_t page)
{
  ut_ad(is_persistent());
  ut_ad(page_no == page);
  ut_a(!UT_LIST_GET_LEN(undo_list));
  ut_ad(!history_size || UT_LIST_GET_FIRST(undo_cached));

  history_size= 0;
  page_no= page;

  for (trx_undo_t *next, *undo= UT_LIST_GET_FIRST(undo_cached); undo;
       undo= next)
  {
    next= UT_LIST_GET_NEXT(undo_list, undo);
    UT_LIST_REMOVE(undo_cached, undo);
    MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
    ut_free(undo);
  }

  ut_ad(!is_referenced());
  clear_needs_purge();
  last_commit_and_offset= 0;
  last_page_no= FIL_NULL;
  curr_size= 1;
}

/** Read the undo log lists.
@param[in,out]  rseg            rollback segment
@param[in,out]  max_trx_id      maximum observed transaction identifier
@param[in]      rseg_header     rollback segment header
@return error code */
static dberr_t trx_undo_lists_init(trx_rseg_t *rseg, trx_id_t &max_trx_id,
                                   const buf_block_t *rseg_header)
{
  ut_ad(srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN);

  for (ulint i= 0; i < TRX_RSEG_N_SLOTS; i++)
  {
    uint32_t page_no= trx_rsegf_get_nth_undo(rseg_header, i);
    if (page_no != FIL_NULL)
    {
      const trx_undo_t *undo= trx_undo_mem_create_at_db_start(rseg, i, page_no,
                                                              max_trx_id);
      if (!undo)
        return DB_CORRUPTION;
      rseg->curr_size+= undo->size;
      MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);
    }
  }

  return DB_SUCCESS;
}

/** Restore the state of a persistent rollback segment.
@param[in,out]	rseg		persistent rollback segment
@param[in,out]	max_trx_id	maximum observed transaction identifier
@param[in,out]	mtr		mini-transaction
@return error code */
static dberr_t trx_rseg_mem_restore(trx_rseg_t *rseg, trx_id_t &max_trx_id,
                                    mtr_t *mtr)
{
	buf_block_t* rseg_hdr = trx_rsegf_get_new(
		page_id_t(rseg->space->id, rseg->page_no), mtr);

	if (!mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT
			      + rseg_hdr->page.frame)) {
		trx_id_t id = mach_read_from_8(TRX_RSEG + TRX_RSEG_MAX_TRX_ID
					       + rseg_hdr->page.frame);

		if (id > max_trx_id) {
			max_trx_id = id;
		}

		const byte* binlog_name = TRX_RSEG + TRX_RSEG_BINLOG_NAME
			+ rseg_hdr->page.frame;
		if (*binlog_name) {
			lsn_t lsn = mach_read_from_8(my_assume_aligned<8>(
							     FIL_PAGE_LSN
							     + rseg_hdr
							     ->page.frame));
			compile_time_assert(TRX_RSEG_BINLOG_NAME_LEN == sizeof
					    trx_sys.recovered_binlog_filename);
			if (lsn > trx_sys.recovered_binlog_lsn) {
				trx_sys.recovered_binlog_lsn = lsn;
				trx_sys.recovered_binlog_offset
					= mach_read_from_8(
						TRX_RSEG
						+ TRX_RSEG_BINLOG_OFFSET
						+ rseg_hdr->page.frame);
				memcpy(trx_sys.recovered_binlog_filename,
				       binlog_name,
				       TRX_RSEG_BINLOG_NAME_LEN);
			}

#ifdef WITH_WSREP
			trx_rseg_read_wsrep_checkpoint(
				rseg_hdr, trx_sys.recovered_wsrep_xid);
#endif
		}
	}

	if (srv_operation == SRV_OPERATION_RESTORE) {
		/* mariabackup --prepare only deals with
		the redo log and the data files, not with
		transactions or the data dictionary. */
		return DB_SUCCESS;
	}

	/* Initialize the undo log lists according to the rseg header */

	rseg->curr_size = mach_read_from_4(TRX_RSEG + TRX_RSEG_HISTORY_SIZE
					   + rseg_hdr->page.frame)
		+ 1;
	if (dberr_t err = trx_undo_lists_init(rseg, max_trx_id, rseg_hdr)) {
		return err;
	}

	if (auto len = flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY
				    + rseg_hdr->page.frame)) {
		rseg->history_size += len;

		fil_addr_t node_addr = flst_get_last(TRX_RSEG
						     + TRX_RSEG_HISTORY
						     + rseg_hdr->page.frame);
		node_addr.boffset = static_cast<uint16_t>(
			node_addr.boffset - TRX_UNDO_HISTORY_NODE);

		rseg->last_page_no = node_addr.page;

		const buf_block_t* block = trx_undo_page_get(
			page_id_t(rseg->space->id, node_addr.page), mtr);

		trx_id_t id = mach_read_from_8(block->page.frame
					       + node_addr.boffset
					       + TRX_UNDO_TRX_ID);
		if (id > max_trx_id) {
			max_trx_id = id;
		}
		id = mach_read_from_8(block->page.frame + node_addr.boffset
				      + TRX_UNDO_TRX_NO);
		if (id > max_trx_id) {
			max_trx_id = id;
		}

		rseg->set_last_commit(node_addr.boffset, id);
		unsigned purge = mach_read_from_2(block->page.frame
						  + node_addr.boffset
						  + TRX_UNDO_NEEDS_PURGE);
		ut_ad(purge <= 1);
		if (purge != 0) {
			rseg->set_needs_purge();
		}

		if (rseg->last_page_no != FIL_NULL) {

			/* There is no need to cover this operation by the purge
			mutex because we are still bootstrapping. */
			purge_sys.purge_queue.push(*rseg);
		}
	}

	return DB_SUCCESS;
}

/** Read binlog metadata from the TRX_SYS page, in case we are upgrading
from MySQL or a MariaDB version older than 10.3.5. */
static void trx_rseg_init_binlog_info(const page_t* page)
{
	if (mach_read_from_4(TRX_SYS + TRX_SYS_MYSQL_LOG_INFO
			     + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
			     + page)
	    == TRX_SYS_MYSQL_LOG_MAGIC_N) {
		memcpy(trx_sys.recovered_binlog_filename,
		       TRX_SYS_MYSQL_LOG_INFO + TRX_SYS_MYSQL_LOG_NAME
		       + TRX_SYS + page, TRX_SYS_MYSQL_LOG_NAME_LEN);
		trx_sys.recovered_binlog_offset = mach_read_from_8(
			TRX_SYS_MYSQL_LOG_INFO + TRX_SYS_MYSQL_LOG_OFFSET
			+ TRX_SYS + page);
	}

#ifdef WITH_WSREP
	trx_rseg_init_wsrep_xid(page, trx_sys.recovered_wsrep_xid);
#endif
}

/** Initialize or recover the rollback segments at startup. */
dberr_t trx_rseg_array_init()
{
	trx_id_t max_trx_id = 0;

	*trx_sys.recovered_binlog_filename = '\0';
	trx_sys.recovered_binlog_offset = 0;
#ifdef WITH_WSREP
	trx_sys.recovered_wsrep_xid.null();
	XID wsrep_sys_xid;
	wsrep_sys_xid.null();
	bool wsrep_xid_in_rseg_found = false;
#endif
	mtr_t mtr;
	dberr_t err = DB_SUCCESS;

	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
		mtr.start();
		if (const buf_block_t* sys = trx_sysf_get(&mtr, false)) {
			if (rseg_id == 0) {
				/* In case this is an upgrade from
				before MariaDB 10.3.5, fetch the base
				information from the TRX_SYS page. */
				max_trx_id = mach_read_from_8(
					TRX_SYS + TRX_SYS_TRX_ID_STORE
					+ sys->page.frame);
				trx_rseg_init_binlog_info(sys->page.frame);
#ifdef WITH_WSREP
				wsrep_sys_xid.set(&trx_sys.recovered_wsrep_xid);
#endif
			}

			const uint32_t	page_no = trx_sysf_rseg_get_page_no(
				sys, rseg_id);
			if (page_no != FIL_NULL) {
				trx_rseg_t& rseg = trx_sys.rseg_array[rseg_id];
				rseg.init(fil_space_get(
						  trx_sysf_rseg_get_space(
							  sys, rseg_id)),
					  page_no);
				ut_ad(rseg.is_persistent());
				if ((err = trx_rseg_mem_restore(
					     &rseg, max_trx_id, &mtr))
				    != DB_SUCCESS) {
					mtr.commit();
					break;
				}
#ifdef WITH_WSREP
				if (!wsrep_sys_xid.is_null() &&
				    !wsrep_sys_xid.eq(&trx_sys.recovered_wsrep_xid)) {
					wsrep_xid_in_rseg_found = true;
					ut_ad(memcmp(wsrep_xid_uuid(&wsrep_sys_xid),
						     wsrep_xid_uuid(&trx_sys.recovered_wsrep_xid),
						     sizeof wsrep_uuid)
					      || wsrep_xid_seqno(
						      &wsrep_sys_xid)
					      <= wsrep_xid_seqno(
						      &trx_sys.recovered_wsrep_xid));
				}
#endif
			}
		}

		mtr.commit();
	}

	if (err != DB_SUCCESS) {
		for (auto& rseg : trx_sys.rseg_array) {
			while (auto u = UT_LIST_GET_FIRST(rseg.undo_list)) {
				UT_LIST_REMOVE(rseg.undo_list, u);
				ut_free(u);
			}
		}
		return err;
	}

#ifdef WITH_WSREP
	if (!wsrep_sys_xid.is_null()) {
		/* Upgrade from a version prior to 10.3.5,
		where WSREP XID was stored in TRX_SYS page.
		If no rollback segment has a WSREP XID set,
		we must copy the XID found in TRX_SYS page
		to rollback segments. */
		mtr.start();

		if (!wsrep_xid_in_rseg_found) {
			trx_rseg_update_wsrep_checkpoint(&wsrep_sys_xid, &mtr);
		}

		/* Finally, clear WSREP XID in TRX_SYS page. */
		mtr.memset(trx_sysf_get(&mtr),
			   TRX_SYS + TRX_SYS_WSREP_XID_INFO,
			   TRX_SYS_WSREP_XID_LEN, 0);
		mtr.commit();
	}
#endif

	trx_sys.init_max_trx_id(max_trx_id + 1);
	return DB_SUCCESS;
}

/** Create the temporary rollback segments. */
void trx_temp_rseg_create()
{
	mtr_t		mtr;

	for (ulong i = 0; i < array_elements(trx_sys.temp_rsegs); i++) {
		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);
		mtr.x_lock_space(fil_system.temp_space);

		buf_block_t* rblock = trx_rseg_header_create(
			fil_system.temp_space, i, 0, NULL, &mtr);
		trx_sys.temp_rsegs[i].init(fil_system.temp_space,
					   rblock->page.id().page_no());
		mtr.commit();
	}
}

/** Update the offset information about the end of the binlog entry
which corresponds to the transaction just being committed.
In a replication slave, this updates the master binlog position
up to which replication has proceeded.
@param[in,out]	rseg_header	rollback segment header
@param[in]	trx		committing transaction
@param[in,out]	mtr		mini-transaction */
void trx_rseg_update_binlog_offset(buf_block_t *rseg_header, const trx_t *trx,
                                   mtr_t *mtr)
{
	DBUG_LOG("trx", "trx_mysql_binlog_offset: " << trx->mysql_log_offset);

	const size_t len = strlen(trx->mysql_log_file_name) + 1;

	ut_ad(len > 1);

	if (UNIV_UNLIKELY(len > TRX_RSEG_BINLOG_NAME_LEN)) {
		return;
	}

	mtr->write<8,mtr_t::MAYBE_NOP>(*rseg_header,
				       TRX_RSEG + TRX_RSEG_BINLOG_OFFSET
				       + rseg_header->page.frame,
				       trx->mysql_log_offset);

	void* name = TRX_RSEG + TRX_RSEG_BINLOG_NAME + rseg_header->page.frame;

	if (memcmp(trx->mysql_log_file_name, name, len)) {
		mtr->memcpy(*rseg_header, name, trx->mysql_log_file_name, len);
	}
}
