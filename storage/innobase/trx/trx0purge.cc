/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2019, MariaDB Corporation.

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
@file trx/trx0purge.cc
Purge old versions

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0purge.h"
#include "fsp0fsp.h"
#include "fut0fut.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "os0thread.h"
#include "que0que.h"
#include "row0purge.h"
#include "row0upd.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "sync0sync.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include <mysql/service_wsrep.h>

/** Maximum allowable purge history length.  <=0 means 'infinite'. */
ulong		srv_max_purge_lag = 0;

/** Max DML user threads delay in micro-seconds. */
ulong		srv_max_purge_lag_delay = 0;

/** The global data structure coordinating a purge */
purge_sys_t	purge_sys;

/** A dummy undo record used as a return value when we have a whole undo log
which needs no purge */
trx_undo_rec_t	trx_purge_dummy_rec;

#ifdef UNIV_DEBUG
my_bool		srv_purge_view_update_only_debug;
#endif /* UNIV_DEBUG */

/** Sentinel value */
static const TrxUndoRsegs NullElement;

/** Default constructor */
TrxUndoRsegsIterator::TrxUndoRsegsIterator()
	: m_rsegs(NullElement), m_iter(m_rsegs.begin())
{
}

/** Sets the next rseg to purge in purge_sys.
Executed in the purge coordinator thread.
@return whether anything is to be purged */
inline bool TrxUndoRsegsIterator::set_next()
{
	mutex_enter(&purge_sys.pq_mutex);

	/* Only purge consumes events from the priority queue, user
	threads only produce the events. */

	/* Check if there are more rsegs to process in the
	current element. */
	if (m_iter != m_rsegs.end()) {
		/* We are still processing rollback segment from
		the same transaction and so expected transaction
		number shouldn't increase. Undo the increment of
		expected commit done by caller assuming rollback
		segments from given transaction are done. */
		purge_sys.tail.commit = (*m_iter)->last_commit;
	} else if (!purge_sys.purge_queue.empty()) {
		m_rsegs = purge_sys.purge_queue.top();
		purge_sys.purge_queue.pop();
		ut_ad(purge_sys.purge_queue.empty()
		      || purge_sys.purge_queue.top() != m_rsegs);
		m_iter = m_rsegs.begin();
	} else {
		/* Queue is empty, reset iterator. */
		purge_sys.rseg = NULL;
		mutex_exit(&purge_sys.pq_mutex);
		m_rsegs = NullElement;
		m_iter = m_rsegs.begin();
		return false;
	}

	purge_sys.rseg = *m_iter++;
	mutex_exit(&purge_sys.pq_mutex);
	mutex_enter(&purge_sys.rseg->mutex);

	ut_a(purge_sys.rseg->last_page_no != FIL_NULL);
	ut_ad(purge_sys.rseg->last_trx_no() == m_rsegs.trx_no());

	/* We assume in purge of externally stored fields that space id is
	in the range of UNDO tablespace space ids */
	ut_ad(purge_sys.rseg->space->id == TRX_SYS_SPACE
	      || srv_is_undo_tablespace(purge_sys.rseg->space->id));

	ut_a(purge_sys.tail.commit <= purge_sys.rseg->last_commit);

	purge_sys.tail.commit = purge_sys.rseg->last_commit;
	purge_sys.hdr_offset = purge_sys.rseg->last_offset;
	purge_sys.hdr_page_no = purge_sys.rseg->last_page_no;

	mutex_exit(&purge_sys.rseg->mutex);

	return(true);
}

/** Build a purge 'query' graph. The actual purge is performed by executing
this query graph.
@return own: the query graph */
static
que_t*
purge_graph_build()
{
	ut_a(srv_n_purge_threads > 0);

	trx_t* trx = trx_create();
	ut_ad(!trx->id);
	trx->start_time = ut_time();
	trx->state = TRX_STATE_ACTIVE;
	trx->op_info = "purge trx";

	mem_heap_t*	heap = mem_heap_create(512);
	que_fork_t*	fork = que_fork_create(
		NULL, NULL, QUE_FORK_PURGE, heap);
	fork->trx = trx;

	for (ulint i = 0; i < srv_n_purge_threads; ++i) {
		que_thr_t*	thr = que_thr_create(fork, heap, NULL);
		thr->child = new(mem_heap_alloc(heap, sizeof(purge_node_t)))
			purge_node_t(thr);
	}

	return(fork);
}

/** Initialise the purge system. */
void purge_sys_t::create()
{
  ut_ad(this == &purge_sys);
  ut_ad(!enabled());
  ut_ad(!event);
  event= os_event_create(0);
  ut_ad(event);
  m_paused= 0;
  query= purge_graph_build();
  n_submitted= 0;
  n_completed= 0;
  next_stored= false;
  rseg= NULL;
  page_no= 0;
  offset= 0;
  hdr_page_no= 0;
  hdr_offset= 0;
  rw_lock_create(trx_purge_latch_key, &latch, SYNC_PURGE_LATCH);
  mutex_create(LATCH_ID_PURGE_SYS_PQ, &pq_mutex);
  undo_trunc.create();
}

/** Close the purge subsystem on shutdown. */
void purge_sys_t::close()
{
  ut_ad(this == &purge_sys);
  if (!event) return;

  m_enabled= false;
  trx_t* trx = query->trx;
  que_graph_free(query);
  ut_ad(!trx->id);
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  trx->state= TRX_STATE_NOT_STARTED;
  trx_free(trx);
  rw_lock_free(&latch);
  /* rw_lock_free() already called latch.~rw_lock_t(); tame the
  debug assertions when the destructor will be called once more. */
  ut_ad(latch.magic_n == 0);
  ut_d(latch.magic_n= RW_LOCK_MAGIC_N);
  mutex_free(&pq_mutex);
  os_event_destroy(event);
}

/*================ UNDO LOG HISTORY LIST =============================*/

/** Prepend the history list with an undo log.
Remove the undo log segment from the rseg slot if it is too big for reuse.
@param[in]	trx		transaction
@param[in,out]	undo		undo log
@param[in,out]	mtr		mini-transaction */
void
trx_purge_add_undo_to_history(const trx_t* trx, trx_undo_t*& undo, mtr_t* mtr)
{
	DBUG_PRINT("trx", ("commit(" TRX_ID_FMT "," TRX_ID_FMT ")",
			   trx->id, trx->no));
	ut_ad(undo == trx->rsegs.m_redo.undo
	      || undo == trx->rsegs.m_redo.old_insert);
	trx_rseg_t*	rseg		= trx->rsegs.m_redo.rseg;
	ut_ad(undo->rseg == rseg);
	trx_rsegf_t*	rseg_header	= trx_rsegf_get(
		rseg->space, rseg->page_no, mtr);
	page_t*		undo_page	= trx_undo_set_state_at_finish(
		undo, mtr);
	trx_ulogf_t*	undo_header	= undo_page + undo->hdr_offset;

	ut_ad(mach_read_from_2(undo_header + TRX_UNDO_NEEDS_PURGE) <= 1);

	if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG_FORMAT + rseg_header))) {
		/* This database must have been upgraded from
		before MariaDB 10.3.5. */
		trx_rseg_format_upgrade(rseg_header, mtr);
	}

	if (undo->state != TRX_UNDO_CACHED) {
		/* The undo log segment will not be reused */
		ut_a(undo->id < TRX_RSEG_N_SLOTS);
		trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL, mtr);

		MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);

		uint32_t hist_size = mach_read_from_4(TRX_RSEG_HISTORY_SIZE
						      + rseg_header);

		ut_ad(undo->size == flst_get_len(TRX_UNDO_SEG_HDR
						 + TRX_UNDO_PAGE_LIST
						 + undo_page));

		mlog_write_ulint(
			rseg_header + TRX_RSEG_HISTORY_SIZE,
			hist_size + undo->size, MLOG_4BYTES, mtr);

		mlog_write_ull(rseg_header + TRX_RSEG_MAX_TRX_ID,
			       trx_sys.get_max_trx_id(), mtr);
	}

	/* After the purge thread has been given permission to exit,
	we may roll back transactions (trx->undo_no==0)
	in THD::cleanup() invoked from unlink_thd() in fast shutdown,
	or in trx_rollback_resurrected() in slow shutdown.

	Before any transaction-generating background threads or the
	purge have been started, recv_recovery_rollback_active() can
	start transactions in row_merge_drop_temp_indexes() and
	fts_drop_orphaned_tables(), and roll back recovered transactions.

	Arbitrary user transactions may be executed when all the undo log
	related background processes (including purge) are disabled due to
	innodb_force_recovery=2 or innodb_force_recovery=3.
	DROP TABLE may be executed at any innodb_force_recovery	level.

	During fast shutdown, we may also continue to execute
	user transactions. */
	ut_ad(srv_undo_sources
	      || trx->undo_no == 0
	      || (!purge_sys.enabled()
		  && (srv_is_being_started
		      || trx_rollback_is_active
		      || srv_force_recovery >= SRV_FORCE_NO_BACKGROUND))
	      || ((trx->mysql_thd || trx->internal)
		  && srv_fast_shutdown));

#ifdef	WITH_WSREP
	if (wsrep_is_wsrep_xid(trx->xid)) {
		trx_rseg_update_wsrep_checkpoint(rseg_header, trx->xid, mtr);
	}
#endif

	if (trx->mysql_log_file_name && *trx->mysql_log_file_name) {
		/* Update the latest MySQL binlog name and offset info
		in rollback segment header if MySQL binlogging is on
		or the database server is a MySQL replication save. */
		trx_rseg_update_binlog_offset(rseg_header, trx, mtr);
	}

	/* Add the log as the first in the history list */
	flst_add_first(rseg_header + TRX_RSEG_HISTORY,
		       undo_header + TRX_UNDO_HISTORY_NODE, mtr);

	mlog_write_ull(undo_header + TRX_UNDO_TRX_NO, trx->no, mtr);
	/* This is needed for upgrading old undo log pages from
	before MariaDB 10.3.1. */
	if (UNIV_UNLIKELY(!mach_read_from_2(undo_header
					    + TRX_UNDO_NEEDS_PURGE))) {
		mlog_write_ulint(undo_header + TRX_UNDO_NEEDS_PURGE, 1,
				 MLOG_2BYTES, mtr);
	}

	if (rseg->last_page_no == FIL_NULL) {
		rseg->last_page_no = undo->hdr_page_no;
		rseg->last_offset = undo->hdr_offset;
		rseg->set_last_trx_no(trx->no, undo == trx->rsegs.m_redo.undo);
		rseg->needs_purge = true;
	}

	trx_sys.history_insert();

	if (undo->state == TRX_UNDO_CACHED) {
		UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
	} else {
		ut_ad(undo->state == TRX_UNDO_TO_PURGE);
		ut_free(undo);
	}

	undo = NULL;
}

/** Remove undo log header from the history list.
@param[in,out]	rseg_hdr	rollback segment header
@param[in]	log_hdr		undo log segment header
@param[in,out]	mtr		mini transaction. */
static
void
trx_purge_remove_log_hdr(
	trx_rsegf_t*	rseg_hdr,
	trx_ulogf_t*	log_hdr,
	mtr_t*		mtr)
{
	flst_remove(rseg_hdr + TRX_RSEG_HISTORY,
		    log_hdr + TRX_UNDO_HISTORY_NODE, mtr);
	trx_sys.history_remove();
}

/** Free an undo log segment, and remove the header from the history list.
@param[in,out]	rseg		rollback segment
@param[in]	hdr_addr	file address of log_hdr */
static
void
trx_purge_free_segment(trx_rseg_t* rseg, fil_addr_t hdr_addr)
{
	mtr_t		mtr;
	trx_rsegf_t*	rseg_hdr;
	page_t*		undo_page;

	mtr.start();
	mutex_enter(&rseg->mutex);

	rseg_hdr = trx_rsegf_get(rseg->space, rseg->page_no, &mtr);
	undo_page = trx_undo_page_get(
		page_id_t(rseg->space->id, hdr_addr.page), &mtr);

	/* Mark the last undo log totally purged, so that if the
	system crashes, the tail of the undo log will not get accessed
	again. The list of pages in the undo log tail gets
	inconsistent during the freeing of the segment, and therefore
	purge should not try to access them again. */
	mlog_write_ulint(undo_page + hdr_addr.boffset + TRX_UNDO_NEEDS_PURGE,
			 0, MLOG_2BYTES, &mtr);

	while (!fseg_free_step_not_header(
		       TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER
		       + undo_page, false, &mtr)) {
		mutex_exit(&rseg->mutex);

		mtr.commit();
		mtr.start();

		mutex_enter(&rseg->mutex);

		rseg_hdr = trx_rsegf_get(rseg->space, rseg->page_no, &mtr);

		undo_page = trx_undo_page_get(
			page_id_t(rseg->space->id, hdr_addr.page), &mtr);
	}

	/* The page list may now be inconsistent, but the length field
	stored in the list base node tells us how big it was before we
	started the freeing. */

	const ulint seg_size = flst_get_len(
		TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + undo_page);

	/* We may free the undo log segment header page; it must be freed
	within the same mtr as the undo log header is removed from the
	history list: otherwise, in case of a database crash, the segment
	could become inaccessible garbage in the file space. */

	trx_purge_remove_log_hdr(rseg_hdr, undo_page + hdr_addr.boffset, &mtr);

	do {

		/* Here we assume that a file segment with just the header
		page can be freed in a few steps, so that the buffer pool
		is not flooded with bufferfixed pages: see the note in
		fsp0fsp.cc. */

	} while (!fseg_free_step(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER
				 + undo_page, false, &mtr));

	const ulint hist_size = mach_read_from_4(rseg_hdr
						 + TRX_RSEG_HISTORY_SIZE);
	ut_ad(hist_size >= seg_size);

	mlog_write_ulint(rseg_hdr + TRX_RSEG_HISTORY_SIZE,
			 hist_size - seg_size, MLOG_4BYTES, &mtr);

	ut_ad(rseg->curr_size >= seg_size);

	rseg->curr_size -= seg_size;

	mutex_exit(&(rseg->mutex));

	mtr_commit(&mtr);
}

/** Remove unnecessary history data from a rollback segment.
@param[in,out]	rseg		rollback segment
@param[in]	limit		truncate anything before this */
static
void
trx_purge_truncate_rseg_history(
	trx_rseg_t&			rseg,
	const purge_sys_t::iterator&	limit)
{
	fil_addr_t	hdr_addr;
	fil_addr_t	prev_hdr_addr;
	trx_rsegf_t*	rseg_hdr;
	page_t*		undo_page;
	trx_ulogf_t*	log_hdr;
	trx_usegf_t*	seg_hdr;
	mtr_t		mtr;
	trx_id_t	undo_trx_no;

	mtr.start();
	ut_ad(rseg.is_persistent());
	mutex_enter(&rseg.mutex);

	rseg_hdr = trx_rsegf_get(rseg.space, rseg.page_no, &mtr);

	hdr_addr = trx_purge_get_log_from_hist(
		flst_get_last(rseg_hdr + TRX_RSEG_HISTORY, &mtr));
loop:
	if (hdr_addr.page == FIL_NULL) {
func_exit:
		mutex_exit(&rseg.mutex);
		mtr.commit();
		return;
	}

	undo_page = trx_undo_page_get(page_id_t(rseg.space->id, hdr_addr.page),
				      &mtr);

	log_hdr = undo_page + hdr_addr.boffset;

	undo_trx_no = mach_read_from_8(log_hdr + TRX_UNDO_TRX_NO);

	if (undo_trx_no >= limit.trx_no()) {
		if (undo_trx_no == limit.trx_no()) {
			trx_undo_truncate_start(
				&rseg, hdr_addr.page,
				hdr_addr.boffset, limit.undo_no);
		}

		goto func_exit;
	}

	prev_hdr_addr = trx_purge_get_log_from_hist(
		flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE, &mtr));

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	if ((mach_read_from_2(seg_hdr + TRX_UNDO_STATE) == TRX_UNDO_TO_PURGE)
	    && (mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG) == 0)) {

		/* We can free the whole log segment */

		mutex_exit(&rseg.mutex);
		mtr.commit();

		/* calls the trx_purge_remove_log_hdr()
		inside trx_purge_free_segment(). */
		trx_purge_free_segment(&rseg, hdr_addr);
	} else {
		/* Remove the log hdr from the rseg history. */
		trx_purge_remove_log_hdr(rseg_hdr, log_hdr, &mtr);

		mutex_exit(&rseg.mutex);
		mtr.commit();
	}

	mtr.start();
	mutex_enter(&rseg.mutex);

	rseg_hdr = trx_rsegf_get(rseg.space, rseg.page_no, &mtr);

	hdr_addr = prev_hdr_addr;

	goto loop;
}

/** UNDO log truncate logger. Needed to track state of truncate during crash.
An auxiliary redo log file undo_<space_id>_trunc.log will created while the
truncate of the UNDO is in progress. This file is required during recovery
to complete the truncate. */

namespace undo {
	/** Magic Number to indicate truncate action is complete. */
	static const ib_uint32_t		s_magic = 76845412;

	/** Populate log file name based on space_id
	@param[in]	space_id	id of the undo tablespace.
	@return DB_SUCCESS or error code */
	static dberr_t populate_log_file_name(
		ulint	space_id,
		char*&	log_file_name)
	{
		static const char s_log_prefix[] = "undo_";
		static const char s_log_ext[] = "trunc.log";

		ulint log_file_name_sz = strlen(srv_log_group_home_dir)
			+ (22 - 1 /* NUL */
			   + sizeof s_log_prefix + sizeof s_log_ext);

		log_file_name = new (std::nothrow) char[log_file_name_sz];
		if (log_file_name == 0) {
			return(DB_OUT_OF_MEMORY);
		}

		memset(log_file_name, 0, log_file_name_sz);

		strcpy(log_file_name, srv_log_group_home_dir);
		ulint	log_file_name_len = strlen(log_file_name);

		if (log_file_name[log_file_name_len - 1]
				!= OS_PATH_SEPARATOR) {

			log_file_name[log_file_name_len]
				= OS_PATH_SEPARATOR;
			log_file_name_len = strlen(log_file_name);
		}

		snprintf(log_file_name + log_file_name_len,
			 log_file_name_sz - log_file_name_len,
			 "%s" ULINTPF "_%s", s_log_prefix,
			 space_id, s_log_ext);

		return(DB_SUCCESS);
	}

	/** Mark completion of undo truncate action by writing magic number to
	the log file and then removing it from the disk.
	If we are going to remove it from disk then why write magic number ?
	This is to safeguard from unlink (file-system) anomalies that will keep
	the link to the file even after unlink action is successfull and
	ref-count = 0.
	@param[in]	space_id	id of the undo tablespace to truncate.*/
	void done(
		ulint	space_id)
	{
		dberr_t		err;
		char*		log_file_name;

		/* Step-1: Create the log file name using the pre-decided
		prefix/suffix and table id of undo tablepsace to truncate. */
		err = populate_log_file_name(space_id, log_file_name);
		if (err != DB_SUCCESS) {
			return;
		}

		/* Step-2: Open log file and write magic number to
		indicate done phase. */
		bool    ret;
		os_file_t	handle =
			os_file_create_simple_no_error_handling(
				innodb_log_file_key, log_file_name,
				OS_FILE_OPEN, OS_FILE_READ_WRITE,
				srv_read_only_mode, &ret);

		if (!ret) {
			os_file_delete(innodb_log_file_key, log_file_name);
			delete[] log_file_name;
			return;
		}

		ulint	sz = srv_page_size;
		void*	buf = ut_zalloc_nokey(sz + srv_page_size);
		if (buf == NULL) {
			os_file_close(handle);
			os_file_delete(innodb_log_file_key, log_file_name);
			delete[] log_file_name;
			return;
		}

		byte*	log_buf = static_cast<byte*>(
			ut_align(buf, srv_page_size));

		mach_write_to_4(log_buf, undo::s_magic);

		IORequest	request(IORequest::WRITE);

		err = os_file_write(
			request, log_file_name, handle, log_buf, 0, sz);

		ut_ad(err == DB_SUCCESS);

		os_file_flush(handle);
		os_file_close(handle);

		ut_free(buf);
		os_file_delete(innodb_log_file_key, log_file_name);
		delete[] log_file_name;
	}

	/** Check if TRUNCATE_DDL_LOG file exist.
	@param[in]	space_id	id of the undo tablespace.
	@return true if exist else false. */
	bool is_log_present(
		ulint	space_id)
	{
		dberr_t		err;
		char*		log_file_name;

		/* Step-1: Populate log file name. */
		err = populate_log_file_name(space_id, log_file_name);
		if (err != DB_SUCCESS) {
			return(false);
		}

		/* Step-2: Check for existence of the file. */
		bool		exist;
		os_file_type_t	type;
		os_file_status(log_file_name, &exist, &type);

		/* Step-3: If file exists, check it for presence of magic
		number.  If found, then delete the file and report file
		doesn't exist as presence of magic number suggest that
		truncate action was complete. */

		if (exist) {
			bool    ret;
			os_file_t	handle =
				os_file_create_simple_no_error_handling(
					innodb_log_file_key, log_file_name,
					OS_FILE_OPEN, OS_FILE_READ_WRITE,
					srv_read_only_mode, &ret);
			if (!ret) {
				os_file_delete(innodb_log_file_key,
					       log_file_name);
				delete[] log_file_name;
				return(false);
			}

			ulint	sz = srv_page_size;
			void*	buf = ut_zalloc_nokey(sz + srv_page_size);
			if (buf == NULL) {
				os_file_close(handle);
				os_file_delete(innodb_log_file_key,
					       log_file_name);
				delete[] log_file_name;
				return(false);
			}

			byte*	log_buf = static_cast<byte*>(
				ut_align(buf, srv_page_size));

			IORequest	request(IORequest::READ);

			dberr_t	err;

			err = os_file_read(request, handle, log_buf, 0, sz);

			os_file_close(handle);

			if (err != DB_SUCCESS) {

				ib::info()
					<< "Unable to read '"
					<< log_file_name << "' : "
					<< ut_strerr(err);

				os_file_delete(
					innodb_log_file_key, log_file_name);

				ut_free(buf);

				delete[] log_file_name;

				return(false);
			}

			ulint	magic_no = mach_read_from_4(log_buf);

			ut_free(buf);

			if (magic_no == undo::s_magic) {
				/* Found magic number. */
				os_file_delete(innodb_log_file_key,
					       log_file_name);
				delete[] log_file_name;
				return(false);
			}
		}

		delete[] log_file_name;

		return(exist);
	}
};

/** Iterate over all the UNDO tablespaces and check if any of the UNDO
tablespace qualifies for TRUNCATE (size > threshold).
@param[in,out]	undo_trunc	undo truncate tracker */
static
void
trx_purge_mark_undo_for_truncate(
	undo::Truncate*	undo_trunc)
{
	/* Step-1: If UNDO Tablespace
		- already marked for truncate (OR)
		- truncate disabled
	return immediately else search for qualifying tablespace. */
	if (undo_trunc->is_marked() || !srv_undo_log_truncate) {
		return;
	}

	/* Step-2: Validation/Qualification checks
	a. At-least 2 UNDO tablespaces so even if one UNDO tablespace
	   is being truncated server can continue to operate.
	b. At-least 2 persistent UNDO logs (besides the default rseg-0)
	b. At-least 1 UNDO tablespace size > threshold. */
	if (srv_undo_tablespaces_active < 2 || srv_undo_logs < 3) {
		return;
	}

	/* Avoid bias selection and so start the scan from immediate next
	of last selected UNDO tablespace for truncate. */
	ulint space_id = undo_trunc->get_scan_start();

	for (ulint i = 1; i <= srv_undo_tablespaces_active; i++) {

		if (fil_space_get_size(space_id)
		    > (srv_max_undo_log_size >> srv_page_size_shift)) {
			/* Tablespace qualifies for truncate. */
			undo_trunc->mark(space_id);
			undo::Truncate::add_space_to_trunc_list(space_id);
			break;
		}

		space_id = ((space_id + 1) % (srv_undo_tablespaces_active + 1));
		if (space_id == 0) {
			/* Note: UNDO tablespace ids starts from 1. */
			++space_id;
		}
	}

	/* Couldn't make any selection. */
	if (!undo_trunc->is_marked()) {
		return;
	}

	DBUG_LOG("undo",
		 "marking for truncate UNDO tablespace "
		 << undo_trunc->get_marked_space_id());

	/* Step-3: Iterate over all the rsegs of selected UNDO tablespace
	and mark them temporarily unavailable for allocation.*/
	for (ulint i = 0; i < TRX_SYS_N_RSEGS; ++i) {
		if (trx_rseg_t* rseg = trx_sys.rseg_array[i]) {
			ut_ad(rseg->is_persistent());
			if (rseg->space->id
			    == undo_trunc->get_marked_space_id()) {

				/* Once set this rseg will not be allocated
				to new booting transaction but we will wait
				for existing active transaction to finish. */
				rseg->skip_allocation = true;
				undo_trunc->add_rseg_to_trunc(rseg);
			}
		}
	}
}

undo::undo_spaces_t	undo::Truncate::s_spaces_to_truncate;

/** Cleanse purge queue to remove the rseg that reside in undo-tablespace
marked for truncate.
@param[in,out]	undo_trunc	undo truncate tracker */
static
void
trx_purge_cleanse_purge_queue(
	undo::Truncate*	undo_trunc)
{
	mutex_enter(&purge_sys.pq_mutex);
	typedef	std::vector<TrxUndoRsegs>	purge_elem_list_t;
	purge_elem_list_t			purge_elem_list;

	/* Remove rseg instances that are in the purge queue before we start
	truncate of corresponding UNDO truncate. */
	while (!purge_sys.purge_queue.empty()) {
		purge_elem_list.push_back(purge_sys.purge_queue.top());
		purge_sys.purge_queue.pop();
	}
	ut_ad(purge_sys.purge_queue.empty());

	for (purge_elem_list_t::iterator it = purge_elem_list.begin();
	     it != purge_elem_list.end();
	     ++it) {

		for (TrxUndoRsegs::iterator it2 = it->begin();
		     it2 != it->end();
		     ++it2) {

			if ((*it2)->space->id
				== undo_trunc->get_marked_space_id()) {
				it->erase(it2);
				break;
			}
		}

		if (!it->empty()) {
			purge_sys.purge_queue.push(*it);
		}
	}
	mutex_exit(&purge_sys.pq_mutex);
}

/** Iterate over selected UNDO tablespace and check if all the rsegs
that resides in the tablespace are free.
@param[in]	limit		truncate_limit
@param[in,out]	undo_trunc	undo truncate tracker */
static
void
trx_purge_initiate_truncate(
	const purge_sys_t::iterator& limit,
	undo::Truncate*	undo_trunc)
{
	/* Step-1: Early check to findout if any of the the UNDO tablespace
	is marked for truncate. */
	if (!undo_trunc->is_marked()) {
		/* No tablespace marked for truncate yet. */
		return;
	}

	/* Step-2: Scan over each rseg and ensure that it doesn't hold any
	active undo records. */
	bool all_free = true;

	for (ulint i = 0; i < undo_trunc->rsegs_size() && all_free; ++i) {

		trx_rseg_t*	rseg = undo_trunc->get_ith_rseg(i);

		mutex_enter(&rseg->mutex);

		if (rseg->trx_ref_count > 0) {
			/* This rseg is still being held by an active
			transaction. */
			all_free = false;
			mutex_exit(&rseg->mutex);
			continue;
		}

		ut_ad(rseg->trx_ref_count == 0);
		ut_ad(rseg->skip_allocation);

		ulint	size_of_rsegs = rseg->curr_size;

		if (size_of_rsegs == 1) {
			mutex_exit(&rseg->mutex);
			continue;
		} else {

			/* There could be cached undo segment. Check if records
			in these segments can be purged. Normal purge history
			will not touch these cached segment. */
			ulint		cached_undo_size = 0;

			for (trx_undo_t* undo =
				     UT_LIST_GET_FIRST(rseg->undo_cached);
			     undo != NULL && all_free;
			     undo = UT_LIST_GET_NEXT(undo_list, undo)) {

				if (limit.trx_no() < undo->trx_id) {
					all_free = false;
				} else {
					cached_undo_size += undo->size;
				}
			}

			ut_ad(size_of_rsegs >= (cached_undo_size + 1));

			if (size_of_rsegs > (cached_undo_size + 1)) {
				/* There are pages besides cached pages that
				still hold active data. */
				all_free = false;
			}
		}

		mutex_exit(&rseg->mutex);
	}

	if (!all_free) {
		/* rseg still holds active data.*/
		return;
	}


	/* Step-3: Start the actual truncate.
	a. Remove rseg instance if added to purge queue before we
	   initiate truncate.
	b. Execute actual truncate */

	const ulint space_id = undo_trunc->get_marked_space_id();

	ib::info() << "Truncating UNDO tablespace " << space_id;

	trx_purge_cleanse_purge_queue(undo_trunc);

	ut_a(srv_is_undo_tablespace(space_id));

	fil_space_t* space = fil_space_get(space_id);

	if (!space) {
not_found:
		ib::error() << "Failed to find UNDO tablespace " << space_id;
		return;
	}

	/* Flush all to-be-discarded pages of the tablespace.

	During truncation, we do not want any writes to the
	to-be-discarded area, because we must set the space->size
	early in order to have deterministic page allocation.

	If a log checkpoint was completed at LSN earlier than our
	mini-transaction commit and the server was killed, then
	discarding the to-be-trimmed pages without flushing would
	break crash recovery. So, we cannot avoid the write. */
	{
		FlushObserver observer(
			space,
			UT_LIST_GET_FIRST(purge_sys.query->thrs)->graph->trx,
			NULL);
		buf_LRU_flush_or_remove_pages(space_id, &observer);
	}

	log_free_check();

	/* Adjust the tablespace metadata. */
	space = fil_truncate_prepare(space_id);

	if (!space) {
		goto not_found;
	}

	/* Undo tablespace always are a single file. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);
	fil_node_t* file = UT_LIST_GET_FIRST(space->chain);
	/* The undo tablespace files are never closed. */
	ut_ad(file->is_open());

	/* Re-initialize tablespace, in a single mini-transaction. */
	mtr_t mtr;
	const ulint size = SRV_UNDO_TABLESPACE_SIZE_IN_PAGES;
	mtr.start();
	mtr_x_lock(&space->latch, &mtr);
	fil_truncate_log(space, size, &mtr);
	fsp_header_init(space, size, &mtr);
	mutex_enter(&fil_system.mutex);
	space->size = file->size = size;
	mutex_exit(&fil_system.mutex);

	buf_block_t* sys_header = trx_sysf_get(&mtr);

	for (ulint i = 0; i < undo_trunc->rsegs_size(); ++i) {
		trx_rseg_t*	rseg = undo_trunc->get_ith_rseg(i);
		buf_block_t* rblock = trx_rseg_header_create(
			space, rseg->id, sys_header, &mtr);
		ut_ad(rblock);
		rseg->page_no = rblock ? rblock->page.id.page_no() : FIL_NULL;

		/* Before re-initialization ensure that we free the existing
		structure. There can't be any active transactions. */
		ut_a(UT_LIST_GET_LEN(rseg->undo_list) == 0);
		ut_a(UT_LIST_GET_LEN(rseg->old_insert_list) == 0);

		trx_undo_t*	next_undo;

		for (trx_undo_t* undo = UT_LIST_GET_FIRST(rseg->undo_cached);
		     undo != NULL;
		     undo = next_undo) {

			next_undo = UT_LIST_GET_NEXT(undo_list, undo);
			UT_LIST_REMOVE(rseg->undo_cached, undo);
			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
			ut_free(undo);
		}

		UT_LIST_INIT(rseg->undo_list, &trx_undo_t::undo_list);
		UT_LIST_INIT(rseg->undo_cached, &trx_undo_t::undo_list);
		UT_LIST_INIT(rseg->old_insert_list, &trx_undo_t::undo_list);

		/* These were written by trx_rseg_header_create(). */
		ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT
					+ rblock->frame));
		ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_HISTORY_SIZE
					+ rblock->frame));

		/* Initialize the undo log lists according to the rseg header */
		rseg->curr_size = 1;
		rseg->trx_ref_count = 0;
		rseg->last_page_no = FIL_NULL;
		rseg->last_offset = 0;
		rseg->last_commit = 0;
		rseg->needs_purge = false;
	}

	mtr.commit();
	/* Write-ahead the redo log record. */
	log_write_up_to(mtr.commit_lsn(), true);

	/* Trim the file size. */
	os_file_truncate(file->name, file->handle,
			 os_offset_t(size) << srv_page_size_shift, true);

	/* This is only executed by the srv_purge_coordinator_thread. */
	export_vars.innodb_undo_truncations++;

	/* TODO: PUNCH_HOLE the garbage (with write-ahead logging) */

	mutex_enter(&fil_system.mutex);
	ut_ad(space->stop_new_ops);
	ut_ad(space->is_being_truncated);
	space->stop_new_ops = false;
	space->is_being_truncated = false;
	mutex_exit(&fil_system.mutex);

	if (purge_sys.rseg != NULL
	    && purge_sys.rseg->last_page_no == FIL_NULL) {
		/* If purge_sys.rseg is pointing to rseg that was recently
		truncated then move to next rseg element.
		Note: Ideally purge_sys.rseg should be NULL because purge
		should complete processing of all the records but there is
		purge_batch_size that can force the purge loop to exit before
		all the records are purged and in this case purge_sys.rseg
		could point to a valid rseg waiting for next purge cycle. */
		purge_sys.next_stored = false;
		purge_sys.rseg = NULL;
	}

	DBUG_EXECUTE_IF("ib_undo_trunc",
			ib::info() << "ib_undo_trunc";
			log_write_up_to(LSN_MAX, true);
			DBUG_SUICIDE(););

	/* Completed truncate. Now it is safe to re-use the tablespace. */
	for (ulint i = 0; i < undo_trunc->rsegs_size(); ++i) {
		trx_rseg_t*	rseg = undo_trunc->get_ith_rseg(i);
		rseg->skip_allocation = false;
	}

	ib::info() << "Truncated UNDO tablespace " << space_id;

	undo_trunc->reset();
	undo::Truncate::clear_trunc_list();
}

/**
Removes unnecessary history data from rollback segments. NOTE that when this
function is called, the caller must not have any latches on undo log pages!
*/
static void trx_purge_truncate_history()
{
	ut_ad(purge_sys.head <= purge_sys.tail);
	purge_sys_t::iterator& head = purge_sys.head.commit
		? purge_sys.head : purge_sys.tail;

	if (head.trx_no() >= purge_sys.view.low_limit_no()) {
		/* This is sometimes necessary. TODO: find out why. */
		head.reset_trx_no(purge_sys.view.low_limit_no());
		head.undo_no = 0;
	}

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; ++i) {
		if (trx_rseg_t* rseg = trx_sys.rseg_array[i]) {
			ut_ad(rseg->id == i);
			trx_purge_truncate_rseg_history(*rseg, head);
		}
	}

	/* UNDO tablespace truncate. We will try to truncate as much as we
	can (greedy approach). This will ensure when the server is idle we
	try and truncate all the UNDO tablespaces. */
	for (ulint i = srv_undo_tablespaces_active; i--; ) {
		trx_purge_mark_undo_for_truncate(&purge_sys.undo_trunc);
		trx_purge_initiate_truncate(head, &purge_sys.undo_trunc);
	}
}

/***********************************************************************//**
Updates the last not yet purged history log info in rseg when we have purged
a whole undo log. Advances also purge_sys.purge_trx_no past the purged log. */
static void trx_purge_rseg_get_next_history_log(
	ulint*		n_pages_handled)/*!< in/out: number of UNDO pages
					handled */
{
	page_t*		undo_page;
	trx_ulogf_t*	log_hdr;
	fil_addr_t	prev_log_addr;
	trx_id_t	trx_no;
	mtr_t		mtr;

	mutex_enter(&purge_sys.rseg->mutex);

	ut_a(purge_sys.rseg->last_page_no != FIL_NULL);

	purge_sys.tail.commit = purge_sys.rseg->last_commit + 1;
	purge_sys.tail.undo_no = 0;
	purge_sys.next_stored = false;

	mtr.start();

	undo_page = trx_undo_page_get_s_latched(
		page_id_t(purge_sys.rseg->space->id,
			  purge_sys.rseg->last_page_no), &mtr);

	log_hdr = undo_page + purge_sys.rseg->last_offset;

	/* Increase the purge page count by one for every handled log */

	(*n_pages_handled)++;

	prev_log_addr = trx_purge_get_log_from_hist(
		flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE, &mtr));

	const bool empty = prev_log_addr.page == FIL_NULL;

	if (empty) {
		/* No logs left in the history list */
		purge_sys.rseg->last_page_no = FIL_NULL;
	}

	mutex_exit(&purge_sys.rseg->mutex);
	mtr.commit();

	if (empty) {
		return;
	}

	/* Read the previous log header. */
	mtr.start();

	log_hdr = trx_undo_page_get_s_latched(
		page_id_t(purge_sys.rseg->space->id, prev_log_addr.page),
		&mtr)
		+ prev_log_addr.boffset;

	trx_no = mach_read_from_8(log_hdr + TRX_UNDO_TRX_NO);
	unsigned purge = mach_read_from_2(log_hdr + TRX_UNDO_NEEDS_PURGE);
	ut_ad(purge <= 1);

	mtr_commit(&mtr);

	mutex_enter(&purge_sys.rseg->mutex);

	purge_sys.rseg->last_page_no = prev_log_addr.page;
	purge_sys.rseg->last_offset = prev_log_addr.boffset;
	purge_sys.rseg->set_last_trx_no(trx_no, purge != 0);
	purge_sys.rseg->needs_purge = purge != 0;

	/* Purge can also produce events, however these are already ordered
	in the rollback segment and any user generated event will be greater
	than the events that Purge produces. ie. Purge can never produce
	events from an empty rollback segment. */

	mutex_enter(&purge_sys.pq_mutex);

	purge_sys.purge_queue.push(*purge_sys.rseg);

	mutex_exit(&purge_sys.pq_mutex);

	mutex_exit(&purge_sys.rseg->mutex);
}

/** Position the purge sys "iterator" on the undo record to use for purging. */
static
void
trx_purge_read_undo_rec()
{
	ulint		offset;
	ulint		page_no;
	ib_uint64_t	undo_no;

	purge_sys.hdr_offset = purge_sys.rseg->last_offset;
	page_no = purge_sys.hdr_page_no = purge_sys.rseg->last_page_no;

	if (purge_sys.rseg->needs_purge) {
		mtr_t		mtr;
		mtr.start();
		if (trx_undo_rec_t* undo_rec = trx_undo_get_first_rec(
			    purge_sys.rseg->space, purge_sys.hdr_page_no,
			    purge_sys.hdr_offset, RW_S_LATCH, &mtr)) {

			offset = page_offset(undo_rec);
			undo_no = trx_undo_rec_get_undo_no(undo_rec);
			page_no = page_get_page_no(page_align(undo_rec));
		} else {
			offset = 0;
			undo_no = 0;
		}

		mtr.commit();
	} else {
		offset = 0;
		undo_no = 0;
	}

	purge_sys.offset = offset;
	purge_sys.page_no = page_no;
	purge_sys.tail.undo_no = undo_no;

	purge_sys.next_stored = true;
}

/***********************************************************************//**
Chooses the next undo log to purge and updates the info in purge_sys. This
function is used to initialize purge_sys when the next record to purge is
not known, and also to update the purge system info on the next record when
purge has handled the whole undo log for a transaction. */
static
void
trx_purge_choose_next_log(void)
/*===========================*/
{
	ut_ad(!purge_sys.next_stored);

	if (purge_sys.rseg_iter.set_next()) {
		trx_purge_read_undo_rec();
	} else {
		/* There is nothing to do yet. */
		os_thread_yield();
	}
}

/***********************************************************************//**
Gets the next record to purge and updates the info in the purge system.
@return copy of an undo log record or pointer to the dummy undo log record */
static
trx_undo_rec_t*
trx_purge_get_next_rec(
/*===================*/
	ulint*		n_pages_handled,/*!< in/out: number of UNDO pages
					handled */
	mem_heap_t*	heap)		/*!< in: memory heap where copied */
{
	trx_undo_rec_t*	rec;
	trx_undo_rec_t*	rec_copy;
	trx_undo_rec_t*	rec2;
	page_t*		undo_page;
	page_t*		page;
	ulint		offset;
	ulint		page_no;
	ulint		space;
	mtr_t		mtr;

	ut_ad(purge_sys.next_stored);
	ut_ad(purge_sys.tail.trx_no() < purge_sys.view.low_limit_no());

	space = purge_sys.rseg->space->id;
	page_no = purge_sys.page_no;
	offset = purge_sys.offset;

	if (offset == 0) {
		/* It is the dummy undo log record, which means that there is
		no need to purge this undo log */

		trx_purge_rseg_get_next_history_log(n_pages_handled);

		/* Look for the next undo log and record to purge */

		trx_purge_choose_next_log();

		return(&trx_purge_dummy_rec);
	}

	mtr_start(&mtr);

	undo_page = trx_undo_page_get_s_latched(page_id_t(space, page_no),
						&mtr);

	rec = undo_page + offset;

	rec2 = trx_undo_page_get_next_rec(rec, purge_sys.hdr_page_no,
					  purge_sys.hdr_offset);

	if (rec2 == NULL) {
		rec2 = trx_undo_get_next_rec(rec, purge_sys.hdr_page_no,
					     purge_sys.hdr_offset, &mtr);
	}

	if (rec2 == NULL) {
		mtr_commit(&mtr);

		trx_purge_rseg_get_next_history_log(n_pages_handled);

		/* Look for the next undo log and record to purge */

		trx_purge_choose_next_log();

		mtr_start(&mtr);

		undo_page = trx_undo_page_get_s_latched(
			page_id_t(space, page_no), &mtr);

		rec = undo_page + offset;
	} else {
		page = page_align(rec2);

		purge_sys.offset = ulint(rec2 - page);
		purge_sys.page_no = page_get_page_no(page);
		purge_sys.tail.undo_no = trx_undo_rec_get_undo_no(rec2);

		if (undo_page != page) {
			/* We advance to a new page of the undo log: */
			(*n_pages_handled)++;
		}
	}

	rec_copy = trx_undo_rec_copy(rec, heap);

	mtr_commit(&mtr);

	return(rec_copy);
}

/********************************************************************//**
Fetches the next undo log record from the history list to purge. It must be
released with the corresponding release function.
@return copy of an undo log record or pointer to trx_purge_dummy_rec,
if the whole undo log can skipped in purge; NULL if none left */
static MY_ATTRIBUTE((warn_unused_result))
trx_undo_rec_t*
trx_purge_fetch_next_rec(
/*=====================*/
	roll_ptr_t*	roll_ptr,	/*!< out: roll pointer to undo record */
	ulint*		n_pages_handled,/*!< in/out: number of UNDO log pages
					handled */
	mem_heap_t*	heap)		/*!< in: memory heap where copied */
{
	if (!purge_sys.next_stored) {
		trx_purge_choose_next_log();

		if (!purge_sys.next_stored) {
			DBUG_PRINT("ib_purge",
				   ("no logs left in the history list"));
			return(NULL);
		}
	}

	if (purge_sys.tail.trx_no() >= purge_sys.view.low_limit_no()) {

		return(NULL);
	}

	/* fprintf(stderr, "Thread %lu purging trx %llu undo record %llu\n",
	os_thread_get_curr_id(), iter->trx_no, iter->undo_no); */

	*roll_ptr = trx_undo_build_roll_ptr(
		/* row_purge_record_func() will later set
		ROLL_PTR_INSERT_FLAG for TRX_UNDO_INSERT_REC */
		false,
		purge_sys.rseg->id,
		purge_sys.page_no, purge_sys.offset);

	/* The following call will advance the stored values of the
	purge iterator. */

	return(trx_purge_get_next_rec(n_pages_handled, heap));
}

/** Run a purge batch.
@param n_purge_threads	number of purge threads
@return number of undo log pages handled in the batch */
static
ulint
trx_purge_attach_undo_recs(ulint n_purge_threads)
{
	que_thr_t*	thr;
	ulint		i;
	ulint		n_pages_handled = 0;
	ulint		n_thrs = UT_LIST_GET_LEN(purge_sys.query->thrs);

	ut_a(n_purge_threads > 0);

	purge_sys.head = purge_sys.tail;

#ifdef UNIV_DEBUG
	i = 0;
	/* Debug code to validate some pre-requisites and reset done flag. */
	for (thr = UT_LIST_GET_FIRST(purge_sys.query->thrs);
	     thr != NULL && i < n_purge_threads;
	     thr = UT_LIST_GET_NEXT(thrs, thr), ++i) {

		purge_node_t*		node;

		/* Get the purge node. */
		node = (purge_node_t*) thr->child;

		ut_ad(que_node_get_type(node) == QUE_NODE_PURGE);
		ut_ad(node->undo_recs == NULL);
		ut_ad(!node->in_progress);
		ut_d(node->in_progress = true);
	}

	/* There should never be fewer nodes than threads, the inverse
	however is allowed because we only use purge threads as needed. */
	ut_ad(i == n_purge_threads);
#endif

	/* Fetch and parse the UNDO records. The UNDO records are added
	to a per purge node vector. */
	thr = UT_LIST_GET_FIRST(purge_sys.query->thrs);
	ut_a(n_thrs > 0 && thr != NULL);

	ut_ad(purge_sys.head <= purge_sys.tail);

	i = 0;

	const ulint batch_size = srv_purge_batch_size;

	while (UNIV_LIKELY(srv_undo_sources) || !srv_fast_shutdown) {
		purge_node_t*		node;
		trx_purge_rec_t*	purge_rec;

		ut_a(!thr->is_active);

		/* Get the purge node. */
		node = (purge_node_t*) thr->child;
		ut_a(que_node_get_type(node) == QUE_NODE_PURGE);

		purge_rec = static_cast<trx_purge_rec_t*>(
			mem_heap_zalloc(node->heap, sizeof(*purge_rec)));

		/* Track the max {trx_id, undo_no} for truncating the
		UNDO logs once we have purged the records. */

		if (purge_sys.head <= purge_sys.tail) {
			purge_sys.head = purge_sys.tail;
		}

		/* Fetch the next record, and advance the purge_sys.tail. */
		purge_rec->undo_rec = trx_purge_fetch_next_rec(
			&purge_rec->roll_ptr, &n_pages_handled, node->heap);

		if (purge_rec->undo_rec != NULL) {

			if (node->undo_recs == NULL) {
				node->undo_recs = ib_vector_create(
					ib_heap_allocator_create(node->heap),
					sizeof(trx_purge_rec_t),
					batch_size);
			} else {
				ut_a(!ib_vector_is_empty(node->undo_recs));
			}

			ib_vector_push(node->undo_recs, purge_rec);

			if (n_pages_handled >= batch_size) {

				break;
			}
		} else {
			break;
		}

		thr = UT_LIST_GET_NEXT(thrs, thr);

		if (!(++i % n_purge_threads)) {
			thr = UT_LIST_GET_FIRST(purge_sys.query->thrs);
		}

		ut_a(thr != NULL);
	}

	ut_ad(purge_sys.head <= purge_sys.tail);

	return(n_pages_handled);
}

/*******************************************************************//**
Calculate the DML delay required.
@return delay in microseconds or ULINT_MAX */
static
ulint
trx_purge_dml_delay(void)
/*=====================*/
{
	/* Determine how much data manipulation language (DML) statements
	need to be delayed in order to reduce the lagging of the purge
	thread. */
	ulint	delay = 0; /* in microseconds; default: no delay */

	/* If purge lag is set (ie. > 0) then calculate the new DML delay.
	Note: we do a dirty read of the trx_sys_t data structure here,
	without holding trx_sys.mutex. */

	if (srv_max_purge_lag > 0) {
		float	ratio;

		ratio = float(trx_sys.history_size()) / srv_max_purge_lag;

		if (ratio > 1.0) {
			/* If the history list length exceeds the
			srv_max_purge_lag, the data manipulation
			statements are delayed by at least 5000
			microseconds. */
			delay = (ulint) ((ratio - .5) * 10000);
		}

		if (delay > srv_max_purge_lag_delay) {
			delay = srv_max_purge_lag_delay;
		}

		MONITOR_SET(MONITOR_DML_PURGE_DELAY, delay);
	}

	return(delay);
}

/** Wait for pending purge jobs to complete. */
static
void
trx_purge_wait_for_workers_to_complete()
{
	/* Ensure that the work queue empties out. */
	while (my_atomic_loadlint(&purge_sys.n_completed)
	       != purge_sys.n_submitted) {

		if (srv_get_task_queue_length() > 0) {
			srv_release_threads(SRV_WORKER, 1);
		}

		os_thread_yield();
	}

	/* There should be no outstanding tasks as long
	as the worker threads are active. */
	ut_a(srv_get_task_queue_length() == 0);
}

/*******************************************************************//**
This function runs a purge batch.
@return number of undo log pages handled in the batch */
ulint
trx_purge(
/*======*/
	ulint	n_purge_threads,	/*!< in: number of purge tasks
					to submit to the work queue */
	bool	truncate)		/*!< in: truncate history if true */
{
	que_thr_t*	thr = NULL;
	ulint		n_pages_handled;

	ut_a(n_purge_threads > 0);

	srv_dml_needed_delay = trx_purge_dml_delay();

	/* The number of tasks submitted should be completed. */
	ut_a(purge_sys.n_submitted
	     == my_atomic_loadlint(&purge_sys.n_completed));

	rw_lock_x_lock(&purge_sys.latch);
	trx_sys.clone_oldest_view();
	rw_lock_x_unlock(&purge_sys.latch);

#ifdef UNIV_DEBUG
	if (srv_purge_view_update_only_debug) {
		return(0);
	}
#endif /* UNIV_DEBUG */

	/* Fetch the UNDO recs that need to be purged. */
	n_pages_handled = trx_purge_attach_undo_recs(n_purge_threads);
	purge_sys.n_submitted += n_purge_threads;

	/* Submit tasks to workers queue if using multi-threaded purge. */
	for (ulint i = n_purge_threads; --i; ) {
		thr = que_fork_scheduler_round_robin(purge_sys.query, thr);
		ut_a(thr);
		srv_que_task_enqueue_low(thr);
	}

	thr = que_fork_scheduler_round_robin(purge_sys.query, thr);

	que_run_threads(thr);

	my_atomic_addlint(&purge_sys.n_completed, 1);

	if (n_purge_threads > 1) {
		trx_purge_wait_for_workers_to_complete();
	}

	ut_a(purge_sys.n_submitted
	     == my_atomic_loadlint(&purge_sys.n_completed));

	if (truncate) {
		trx_purge_truncate_history();
	}

	MONITOR_INC_VALUE(MONITOR_PURGE_INVOKED, 1);
	MONITOR_INC_VALUE(MONITOR_PURGE_N_PAGE_HANDLED, n_pages_handled);

	return(n_pages_handled);
}

/** Stop purge during FLUSH TABLES FOR EXPORT */
void purge_sys_t::stop()
{
  rw_lock_x_lock(&latch);

  if (!enabled_latched())
  {
    /* Shutdown must have been initiated during FLUSH TABLES FOR EXPORT. */
    ut_ad(!srv_undo_sources);
    rw_lock_x_unlock(&latch);
    return;
  }

  ut_ad(srv_n_purge_threads > 0);

  if (0 == my_atomic_add32_explicit(&m_paused, 1, MY_MEMORY_ORDER_RELAXED))
  {
    /* We need to wakeup the purge thread in case it is suspended, so
    that it can acknowledge the state change. */
    const int64_t sig_count = os_event_reset(event);
    rw_lock_x_unlock(&latch);
    ib::info() << "Stopping purge";
    srv_purge_wakeup();
    /* Wait for purge coordinator to signal that it is suspended. */
    os_event_wait_low(event, sig_count);
    MONITOR_ATOMIC_INC(MONITOR_PURGE_STOP_COUNT);
    return;
  }

  rw_lock_x_unlock(&latch);

  if (running())
  {
    ib::info() << "Waiting for purge to stop";
    while (running())
      os_thread_sleep(10000);
  }
}

/** Resume purge at UNLOCK TABLES after FLUSH TABLES FOR EXPORT */
void purge_sys_t::resume()
{
   if (!enabled())
   {
     /* Shutdown must have been initiated during FLUSH TABLES FOR EXPORT. */
     ut_ad(!srv_undo_sources);
     return;
   }

   int32_t paused= my_atomic_add32_explicit(&m_paused, -1,
                                            MY_MEMORY_ORDER_RELAXED);
   ut_a(paused);

   if (paused == 1)
   {
     ib::info() << "Resuming purge";
     srv_purge_wakeup();
     MONITOR_ATOMIC_INC(MONITOR_PURGE_RESUME_COUNT);
   }
}
