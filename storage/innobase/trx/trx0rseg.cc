/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

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
#include "fsp0sysspace.h"

#include <algorithm>

#ifdef WITH_WSREP

#ifdef UNIV_DEBUG
//static long long trx_sys_cur_xid_seqno = -1;
static long long trx_sys_cur_xid_seqno = -2;
static unsigned char trx_sys_cur_xid_uuid[16];

/** Read WSREP XID seqno */
static inline long long read_wsrep_xid_seqno(const XID* xid)
{
	long long seqno;
	memcpy(&seqno, xid->data + 24, sizeof(long long));
	return seqno;
}

/** Read WSREP XID UUID */
static inline void read_wsrep_xid_uuid(const XID* xid, unsigned char* buf)
{
	memcpy(buf, xid->data + 8, 16);
}

#endif /* UNIV_DEBUG */

/** Update the WSREP XID information in rollback segment header.
@param[in,out]	rseg_header	rollback segment header
@param[in]	xid		WSREP XID
@param[in,out]	mtr		mini-transaction */
void
trx_rseg_update_wsrep_checkpoint(
	trx_rsegf_t*	rseg_header,
	const XID*	xid,
	mtr_t*		mtr)
{
	ut_ad(xid->formatID == 1);

#ifdef UNIV_DEBUG
	/* Check that seqno is monotonically increasing */
	unsigned char xid_uuid[16];
	long long xid_seqno = read_wsrep_xid_seqno(xid);
	read_wsrep_xid_uuid(xid, xid_uuid);

	if (!memcmp(xid_uuid, trx_sys_cur_xid_uuid, 8)) {
		ut_ad(xid_seqno > trx_sys_cur_xid_seqno);
		trx_sys_cur_xid_seqno = xid_seqno;
	} else {
		memcpy(trx_sys_cur_xid_uuid, xid_uuid, 16);
	}
	trx_sys_cur_xid_seqno = xid_seqno;
#endif /* UNIV_DEBUG */

	mlog_write_ulint(TRX_RSEG_WSREP_XID_FORMAT + rseg_header,
			 uint32_t(xid->formatID),
			 MLOG_4BYTES, mtr);

	mlog_write_ulint(TRX_RSEG_WSREP_XID_GTRID_LEN + rseg_header,
			 uint32_t(xid->gtrid_length),
			 MLOG_4BYTES, mtr);

	mlog_write_ulint(TRX_RSEG_WSREP_XID_BQUAL_LEN + rseg_header,
			 uint32_t(xid->bqual_length),
			 MLOG_4BYTES, mtr);

	mlog_write_string(TRX_RSEG_WSREP_XID_DATA + rseg_header,
			  reinterpret_cast<const byte*>(xid->data),
			  XIDDATASIZE, mtr);
}

/** Update WSREP checkpoint XID in first rollback segment header.
@param[in]	xid		WSREP XID */
void trx_rseg_update_wsrep_checkpoint(const XID* xid)
{
	mtr_t	mtr;
	mtr.start();

	const trx_rseg_t* rseg = trx_sys.rseg_array[0];

	trx_rsegf_t* rseg_header = trx_rsegf_get(rseg->space, rseg->page_no,
						 &mtr);
	if (UNIV_UNLIKELY(mach_read_from_4(rseg_header + TRX_RSEG_FORMAT))) {
		trx_rseg_format_upgrade(rseg_header, &mtr);
	}

	mlog_write_ull(rseg_header + TRX_RSEG_MAX_TRX_ID,
		       trx_sys.get_max_trx_id(), &mtr);
	trx_rseg_update_wsrep_checkpoint(rseg_header, xid, &mtr);
	mtr.commit();
}

/** Read the WSREP XID information in rollback segment header.
@param[in]	rseg_header	Rollback segment header
@param[out]	xid		Transaction XID
@return	whether the WSREP XID was present */
bool trx_rseg_read_wsrep_checkpoint(const trx_rsegf_t* rseg_header, XID& xid)
{
	xid.formatID = (int)mach_read_from_4(
		TRX_RSEG_WSREP_XID_FORMAT + rseg_header);

	if (xid.formatID == 0) {
		memset(&xid, 0, sizeof(xid));
		long long seqno= -1;
		memcpy(xid.data + 24, &seqno, sizeof(long long));
		xid.formatID = -1;
		return false;
	}

	xid.gtrid_length = (int)mach_read_from_4(
		TRX_RSEG_WSREP_XID_GTRID_LEN + rseg_header);

	xid.bqual_length = (int)mach_read_from_4(
		TRX_RSEG_WSREP_XID_BQUAL_LEN + rseg_header);

	memcpy(xid.data, TRX_RSEG_WSREP_XID_DATA + rseg_header, XIDDATASIZE);

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

	xid.formatID = (int)mach_read_from_4(
		TRX_SYS + TRX_SYS_WSREP_XID_INFO
		+ TRX_SYS_WSREP_XID_FORMAT + page);
	xid.gtrid_length = (int)mach_read_from_4(
		TRX_SYS + TRX_SYS_WSREP_XID_INFO
		+ TRX_SYS_WSREP_XID_GTRID_LEN + page);
	xid.bqual_length = (int)mach_read_from_4(
		TRX_SYS + TRX_SYS_WSREP_XID_INFO
		+ TRX_SYS_WSREP_XID_BQUAL_LEN + page);
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
	trx_id_t	max_id = 0;
	bool		found = false;

	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS;
	     rseg_id++, mtr.commit()) {
		mtr.start();
		const buf_block_t* sys = trx_sysf_get(&mtr, false);
		if (rseg_id == 0) {
			found = trx_rseg_init_wsrep_xid(sys->frame, xid);
		}

		const uint32_t page_no = trx_sysf_rseg_get_page_no(
			sys, rseg_id);

		if (page_no == FIL_NULL) {
			continue;
		}

		const trx_rsegf_t* rseg_header = trx_rsegf_get_new(
			trx_sysf_rseg_get_space(sys, rseg_id), page_no, &mtr);

		if (mach_read_from_4(rseg_header + TRX_RSEG_FORMAT)) {
			continue;
		}

		trx_id_t id = mach_read_from_8(rseg_header
					       + TRX_RSEG_MAX_TRX_ID);

		if (id < max_id) {
			continue;
		}

		max_id = id;
		found = trx_rseg_read_wsrep_checkpoint(rseg_header, xid)
			|| found;
	}

	return found;
}
#endif /* WITH_WSREP */

/** Upgrade a rollback segment header page to MariaDB 10.3 format.
@param[in,out]	rseg_header	rollback segment header page
@param[in,out]	mtr		mini-transaction */
void trx_rseg_format_upgrade(trx_rsegf_t* rseg_header, mtr_t* mtr)
{
	ut_ad(page_offset(rseg_header) == TRX_RSEG);
	byte* rseg_format = TRX_RSEG_FORMAT + rseg_header;
	mlog_write_ulint(rseg_format, 0, MLOG_4BYTES, mtr);
	/* Clear also possible garbage at the end of the page. Old
	InnoDB versions did not initialize unused parts of pages. */
	byte* b = rseg_header + TRX_RSEG_MAX_TRX_ID + 8;
	ulint len = UNIV_PAGE_SIZE
		- (FIL_PAGE_DATA_END
		   + TRX_RSEG + TRX_RSEG_MAX_TRX_ID + 8);
	memset(b, 0, len);
	mlog_log_string(b, len, mtr);
}

/** Creates a rollback segment header.
This function is called only when a new rollback segment is created in
the database.
@param[in]	space		space id
@param[in]	rseg_id		rollback segment identifier
@param[in,out]	sys_header	the TRX_SYS page (NULL for temporary rseg)
@param[in,out]	mtr		mini-transaction
@return page number of the created segment, FIL_NULL if fail */
ulint
trx_rseg_header_create(
	ulint			space,
	ulint			rseg_id,
	buf_block_t*		sys_header,
	mtr_t*			mtr)
{
	ulint		page_no;
	trx_rsegf_t*	rsegf;
	buf_block_t*	block;

	ut_ad(mtr);
	ut_ad(mtr_memo_contains(mtr, fil_space_get_latch(space, NULL),
				MTR_MEMO_X_LOCK));
	ut_ad(!sys_header == (space == SRV_TMP_SPACE_ID));

	/* Allocate a new file segment for the rollback segment */
	block = fseg_create(space, 0, TRX_RSEG + TRX_RSEG_FSEG_HEADER, mtr);

	if (block == NULL) {
		/* No space left */

		return(FIL_NULL);
	}

	buf_block_dbg_add_level(block, SYNC_RSEG_HEADER_NEW);

	page_no = block->page.id.page_no();

	/* Get the rollback segment file page */
	rsegf = trx_rsegf_get_new(space, page_no, mtr);

	mlog_write_ulint(rsegf + TRX_RSEG_FORMAT, 0, MLOG_4BYTES, mtr);

	/* Initialize the history list */

	mlog_write_ulint(rsegf + TRX_RSEG_HISTORY_SIZE, 0, MLOG_4BYTES, mtr);
	flst_init(rsegf + TRX_RSEG_HISTORY, mtr);

	/* Reset the undo log slots */
	for (ulint i = 0; i < TRX_RSEG_N_SLOTS; i++) {

		trx_rsegf_set_nth_undo(rsegf, i, FIL_NULL, mtr);
	}

	if (sys_header) {
		/* Add the rollback segment info to the free slot in
		the trx system header */

		mlog_write_ulint(TRX_SYS + TRX_SYS_RSEGS
				 + TRX_SYS_RSEG_SPACE
				 + rseg_id * TRX_SYS_RSEG_SLOT_SIZE
				 + sys_header->frame,
				 space, MLOG_4BYTES, mtr);
		mlog_write_ulint(TRX_SYS + TRX_SYS_RSEGS
				 + TRX_SYS_RSEG_PAGE_NO
				 + rseg_id * TRX_SYS_RSEG_SLOT_SIZE
				 + sys_header->frame,
				 page_no, MLOG_4BYTES, mtr);
	}

	return(page_no);
}

/** Free a rollback segment in memory. */
void
trx_rseg_mem_free(trx_rseg_t* rseg)
{
	trx_undo_t*	undo;
	trx_undo_t*	next_undo;

	mutex_free(&rseg->mutex);

	/* There can't be any active transactions. */
	ut_a(UT_LIST_GET_LEN(rseg->undo_list) == 0);
	ut_a(UT_LIST_GET_LEN(rseg->old_insert_list) == 0);

	for (undo = UT_LIST_GET_FIRST(rseg->undo_cached);
	     undo != NULL;
	     undo = next_undo) {

		next_undo = UT_LIST_GET_NEXT(undo_list, undo);

		UT_LIST_REMOVE(rseg->undo_cached, undo);

		MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);

		ut_free(undo);
	}

	ut_free(rseg);
}

/** Create a rollback segment object.
@param[in]	id		rollback segment id
@param[in]	space		space where the segment is placed
@param[in]	page_no		page number of the segment header */
static
trx_rseg_t*
trx_rseg_mem_create(ulint id, ulint space, ulint page_no)
{
	trx_rseg_t* rseg = static_cast<trx_rseg_t*>(
		ut_zalloc_nokey(sizeof *rseg));

	rseg->id = id;
	rseg->space = space;
	rseg->page_no = page_no;
	rseg->last_page_no = FIL_NULL;
	rseg->curr_size = 1;

	mutex_create(rseg->is_persistent()
		     ? LATCH_ID_REDO_RSEG : LATCH_ID_NOREDO_RSEG,
		     &rseg->mutex);

	UT_LIST_INIT(rseg->undo_list, &trx_undo_t::undo_list);
	UT_LIST_INIT(rseg->old_insert_list, &trx_undo_t::undo_list);
	UT_LIST_INIT(rseg->undo_cached, &trx_undo_t::undo_list);

	return(rseg);
}

/** Read the undo log lists.
@param[in,out]	rseg		rollback segment
@param[in,out]	max_trx_id	maximum observed transaction identifier
@param[in]	rseg_header	rollback segment header
@return the combined size of undo log segments in pages */
static
ulint
trx_undo_lists_init(trx_rseg_t* rseg, trx_id_t& max_trx_id,
		    const trx_rsegf_t* rseg_header)
{
	ut_ad(srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN);

	ulint size = 0;

	for (ulint i = 0; i < TRX_RSEG_N_SLOTS; i++) {
		ulint	page_no = trx_rsegf_get_nth_undo(rseg_header, i);
		if (page_no != FIL_NULL) {
			size += trx_undo_mem_create_at_db_start(
				rseg, i, page_no, max_trx_id);
			MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);
		}
	}

	return(size);
}

/** Restore the state of a persistent rollback segment.
@param[in,out]	rseg		persistent rollback segment
@param[in,out]	max_trx_id	maximum observed transaction identifier
@param[in,out]	max_rseg_trx_id	maximum observed TRX_RSEG_MAX_TRX_ID
@param[in,out]	mtr		mini-transaction */
static
void
trx_rseg_mem_restore(
	trx_rseg_t*	rseg,
	trx_id_t&	max_trx_id,
	trx_id_t&	max_rseg_trx_id,
	mtr_t*		mtr)
{
	trx_rsegf_t*	rseg_header = trx_rsegf_get_new(
		rseg->space, rseg->page_no, mtr);

	if (mach_read_from_4(rseg_header + TRX_RSEG_FORMAT) == 0) {
		trx_id_t id = mach_read_from_8(rseg_header
					       + TRX_RSEG_MAX_TRX_ID);

		if (id > max_trx_id) {
			max_trx_id = id;
		}

		if (id > max_rseg_trx_id) {
			max_rseg_trx_id = id;

			if (rseg_header[TRX_RSEG_BINLOG_NAME]) {
				memcpy(trx_sys.recovered_binlog_filename,
				       rseg_header + TRX_RSEG_BINLOG_NAME,
				       TRX_RSEG_BINLOG_NAME_LEN);
				trx_sys.recovered_binlog_offset = mach_read_from_8(
						rseg_header
						+ TRX_RSEG_BINLOG_OFFSET);
			}

#ifdef WITH_WSREP
			trx_rseg_read_wsrep_checkpoint(
				rseg_header, trx_sys.recovered_wsrep_xid);
#endif
		}
	}

	if (srv_operation == SRV_OPERATION_RESTORE) {
		/* mariabackup --prepare only deals with
		the redo log and the data files, not with
		transactions or the data dictionary. */
		return;
	}

	/* Initialize the undo log lists according to the rseg header */

	rseg->curr_size = mach_read_from_4(rseg_header + TRX_RSEG_HISTORY_SIZE)
		+ 1 + trx_undo_lists_init(rseg, max_trx_id, rseg_header);

	if (ulint len = flst_get_len(rseg_header + TRX_RSEG_HISTORY)) {
		trx_sys.history_add(int32(len));

		fil_addr_t	node_addr = trx_purge_get_log_from_hist(
			flst_get_last(rseg_header + TRX_RSEG_HISTORY, mtr));

		rseg->last_page_no = node_addr.page;
		rseg->last_offset = node_addr.boffset;

		const trx_ulogf_t*	undo_log_hdr = trx_undo_page_get(
			page_id_t(rseg->space, node_addr.page), mtr)
			+ node_addr.boffset;

		trx_id_t id = mach_read_from_8(undo_log_hdr + TRX_UNDO_TRX_ID);
		if (id > max_trx_id) {
			max_trx_id = id;
		}
		id = mach_read_from_8(undo_log_hdr + TRX_UNDO_TRX_NO);
		rseg->last_trx_no = id;
		if (id > max_trx_id) {
			max_trx_id = id;
		}
		unsigned purge = mach_read_from_2(
			undo_log_hdr + TRX_UNDO_NEEDS_PURGE);
		ut_ad(purge <= 1);
		rseg->needs_purge = purge != 0;

		TrxUndoRsegs elem(rseg->last_trx_no);
		elem.push_back(rseg);

		if (rseg->last_page_no != FIL_NULL) {

			/* There is no need to cover this operation by the purge
			mutex because we are still bootstrapping. */

			purge_sys->purge_queue.push(elem);
		}
	}
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

/** Initialize the rollback segments in memory at database startup. */
void
trx_rseg_array_init()
{
	trx_id_t max_trx_id = 0, max_rseg_trx_id = 0;

	*trx_sys.recovered_binlog_filename = '\0';
	trx_sys.recovered_binlog_offset = -1;
#ifdef WITH_WSREP
	memset(&trx_sys.recovered_wsrep_xid, 0,
	       sizeof trx_sys.recovered_wsrep_xid);
	trx_sys.recovered_wsrep_xid.formatID = -1;
#endif

	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
		mtr_t mtr;
		mtr.start();
		if (const buf_block_t* sys = trx_sysf_get(&mtr, false)) {
			if (rseg_id == 0) {
				/* In case this is an upgrade from
				before MariaDB 10.3.5, fetch the base
				information from the TRX_SYS page. */
				max_trx_id = mach_read_from_8(
					TRX_SYS + TRX_SYS_TRX_ID_STORE
					+ sys->frame);
				trx_rseg_init_binlog_info(sys->frame);
			}

			const uint32_t	page_no = trx_sysf_rseg_get_page_no(
				sys, rseg_id);
			if (page_no != FIL_NULL) {
				trx_rseg_t* rseg = trx_rseg_mem_create(
					rseg_id, trx_sysf_rseg_get_space(
						sys, rseg_id),
					page_no);
				ut_ad(rseg->is_persistent());
				ut_ad(rseg->id == rseg_id);
				ut_ad(!trx_sys.rseg_array[rseg_id]);
				trx_sys.rseg_array[rseg_id] = rseg;
				trx_rseg_mem_restore(
					rseg, max_trx_id, max_rseg_trx_id,
					&mtr);
			}
		}

		mtr.commit();
	}

	trx_sys.init_max_trx_id(max_trx_id + 1);
}

/** Create a persistent rollback segment.
@param[in]	space_id	system or undo tablespace id
@return pointer to new rollback segment
@retval	NULL	on failure */
trx_rseg_t*
trx_rseg_create(ulint space_id)
{
	trx_rseg_t*		rseg = NULL;
	mtr_t			mtr;

	mtr.start();

	/* To obey the latching order, acquire the file space
	x-latch before the trx_sys.mutex. */
#ifdef UNIV_DEBUG
	const fil_space_t*	space =
#endif /* UNIV_DEBUG */
		mtr_x_lock_space(space_id, &mtr);
	ut_ad(space->purpose == FIL_TYPE_TABLESPACE);

	if (buf_block_t* sys_header = trx_sysf_get(&mtr)) {
		ulint	rseg_id = trx_sys_rseg_find_free(sys_header);
		ulint	page_no = rseg_id == ULINT_UNDEFINED
			? FIL_NULL
			: trx_rseg_header_create(space_id, rseg_id, sys_header,
						 &mtr);
		if (page_no != FIL_NULL) {
			ut_ad(trx_sysf_rseg_get_space(sys_header, rseg_id)
			      == space_id);
			rseg = trx_rseg_mem_create(rseg_id, space_id, page_no);
			ut_ad(rseg->id == rseg_id);
			ut_ad(rseg->is_persistent());
			ut_ad(!trx_sys.rseg_array[rseg->id]);
			trx_sys.rseg_array[rseg->id] = rseg;
		}
	}

	mtr.commit();

	return(rseg);
}

/** Create the temporary rollback segments. */
void
trx_temp_rseg_create()
{
	mtr_t		mtr;

	for (ulong i = 0; i < TRX_SYS_N_RSEGS; i++) {
		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);
#ifdef UNIV_DEBUG
		const fil_space_t*	space =
#endif /* UNIV_DEBUG */
			mtr_x_lock_space(SRV_TMP_SPACE_ID, &mtr);
		ut_ad(space->purpose == FIL_TYPE_TEMPORARY);

		ulint page_no = trx_rseg_header_create(
			SRV_TMP_SPACE_ID, i, NULL, &mtr);
		trx_rseg_t* rseg = trx_rseg_mem_create(
			i, SRV_TMP_SPACE_ID, page_no);
		ut_ad(!rseg->is_persistent());
		ut_ad(!trx_sys.temp_rsegs[i]);
		trx_sys.temp_rsegs[i] = rseg;
		mtr.commit();
	}
}

/********************************************************************
Get the number of unique rollback tablespaces in use except space id 0.
The last space id will be the sentinel value ULINT_UNDEFINED. The array
will be sorted on space id. Note: space_ids should have have space for
TRX_SYS_N_RSEGS + 1 elements.
@return number of unique rollback tablespaces in use. */
ulint
trx_rseg_get_n_undo_tablespaces(
/*============================*/
	ulint*		space_ids)	/*!< out: array of space ids of
					UNDO tablespaces */
{
	mtr_t mtr;
	mtr.start();

	buf_block_t* sys_header = trx_sysf_get(&mtr, false);
	if (!sys_header) {
		mtr.commit();
		return 0;
	}

	ulint* end = space_ids;

	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
		uint32_t page_no = trx_sysf_rseg_get_page_no(sys_header,
							     rseg_id);

		if (page_no == FIL_NULL) {
			continue;
		}

		if (ulint space = trx_sysf_rseg_get_space(sys_header,
							  rseg_id)) {
			if (std::find(space_ids, end, space) == end) {
				*end++ = space;
			}
		}
	}

	mtr.commit();

	ut_a(end - space_ids <= TRX_SYS_N_RSEGS);
	*end = ULINT_UNDEFINED;

	std::sort(space_ids, end);

	return ulint(end - space_ids);
}

/** Update the offset information about the end of the binlog entry
which corresponds to the transaction just being committed.
In a replication slave, this updates the master binlog position
up to which replication has proceeded.
@param[in,out]	rseg_header	rollback segment header
@param[in]	trx		committing transaction
@param[in,out]	mtr		mini-transaction */
void
trx_rseg_update_binlog_offset(byte* rseg_header, const trx_t* trx, mtr_t* mtr)
{
	DBUG_LOG("trx", "trx_mysql_binlog_offset: " << trx->mysql_log_offset);

	const size_t len = strlen(trx->mysql_log_file_name) + 1;

	ut_ad(len > 1);

	if (UNIV_UNLIKELY(len > TRX_RSEG_BINLOG_NAME_LEN)) {
		return;
	}

	mlog_write_ull(rseg_header + TRX_RSEG_BINLOG_OFFSET,
		       trx->mysql_log_offset, mtr);
	byte* p = rseg_header + TRX_RSEG_BINLOG_NAME;
	const byte* binlog_name = reinterpret_cast<const byte*>
		(trx->mysql_log_file_name);

	if (memcmp(binlog_name, p, len)) {
		mlog_write_string(p, binlog_name, len, mtr);
	}
}
