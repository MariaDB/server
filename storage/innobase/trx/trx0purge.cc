/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
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
#include "read0read.h"
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

/** Maximum allowable purge history length.  <=0 means 'infinite'. */
ulong		srv_max_purge_lag = 0;

/** Max DML user threads delay in micro-seconds. */
ulong		srv_max_purge_lag_delay = 0;

/** The global data structure coordinating a purge */
purge_sys_t*	purge_sys;

/** A dummy undo record used as a return value when we have a whole undo log
which needs no purge */
trx_undo_rec_t	trx_purge_dummy_rec;

#ifdef UNIV_DEBUG
my_bool		srv_purge_view_update_only_debug;
#endif /* UNIV_DEBUG */

/** Sentinel value */
const TrxUndoRsegs TrxUndoRsegsIterator::NullElement(UINT64_UNDEFINED);

/** Constructor */
TrxUndoRsegsIterator::TrxUndoRsegsIterator()
	:
	m_trx_undo_rsegs(NullElement),
	m_iter(m_trx_undo_rsegs.end())
{
}

/** Sets the next rseg to purge in purge_sys.
@return whether anything is to be purged */
inline
bool
TrxUndoRsegsIterator::set_next()
{
	mutex_enter(&purge_sys->pq_mutex);

	/* Only purge consumes events from the priority queue, user
	threads only produce the events. */

	/* Check if there are more rsegs to process in the
	current element. */
	if (m_iter != m_trx_undo_rsegs.end()) {

		/* We are still processing rollback segment from
		the same transaction and so expected transaction
		number shouldn't increase. Undo increment of
		expected trx_no done by caller assuming rollback
		segments from given transaction are done. */
		purge_sys->iter.trx_no = (*m_iter)->last_trx_no;

	} else if (!purge_sys->purge_queue.empty()) {

		/* Read the next element from the queue.
		Combine elements if they have same transaction number.
		This can happen if a transaction shares redo rollback segment
		with another transaction that has already added it to purge
		queue and former transaction also needs to schedule non-redo
		rollback segment for purge. */
		m_trx_undo_rsegs = NullElement;

		purge_pq_t&	purge_queue = purge_sys->purge_queue;

		while (!purge_queue.empty()) {

			if (m_trx_undo_rsegs.get_trx_no() == UINT64_UNDEFINED) {
				m_trx_undo_rsegs = purge_queue.top();
			} else if (purge_queue.top().get_trx_no() ==
					m_trx_undo_rsegs.get_trx_no()) {
				m_trx_undo_rsegs.append(
					purge_queue.top());
			} else {
				break;
			}

			purge_queue.pop();
		}

		m_iter = m_trx_undo_rsegs.begin();

	} else {
		/* Queue is empty, reset iterator. */
		m_trx_undo_rsegs = NullElement;
		m_iter = m_trx_undo_rsegs.end();

		mutex_exit(&purge_sys->pq_mutex);

		purge_sys->rseg = NULL;
		return false;
	}

	purge_sys->rseg = *m_iter++;

	mutex_exit(&purge_sys->pq_mutex);

	ut_a(purge_sys->rseg != NULL);

	mutex_enter(&purge_sys->rseg->mutex);

	ut_a(purge_sys->rseg->last_page_no != FIL_NULL);
	ut_ad(purge_sys->rseg->last_trx_no == m_trx_undo_rsegs.get_trx_no());

	/* We assume in purge of externally stored fields that space id is
	in the range of UNDO tablespace space ids */
	ut_a(purge_sys->rseg->space == TRX_SYS_SPACE
	     || srv_is_undo_tablespace(purge_sys->rseg->space));

	ut_a(purge_sys->iter.trx_no <= purge_sys->rseg->last_trx_no);

	purge_sys->iter.trx_no = purge_sys->rseg->last_trx_no;
	purge_sys->hdr_offset = purge_sys->rseg->last_offset;
	purge_sys->hdr_page_no = purge_sys->rseg->last_page_no;

	mutex_exit(&purge_sys->rseg->mutex);

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

	trx_t* trx = trx_allocate_for_background();
	ut_ad(!trx->id);
	trx->start_time = time(NULL);
	trx->start_time_micro = microsecond_interval_timer();
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

/** Construct the purge system. */
purge_sys_t::purge_sys_t()
	: latch(), event(os_event_create(0)),
	  n_stop(0), running(false), state(PURGE_STATE_INIT),
	  query(purge_graph_build()),
	  view(), n_submitted(0), n_completed(0),
	  iter(), limit(),
#ifdef UNIV_DEBUG
	  done(),
#endif /* UNIV_DEBUG */
	  next_stored(false), rseg(NULL),
	  page_no(0), offset(0), hdr_page_no(0), hdr_offset(0),
	  rseg_iter(), purge_queue(), pq_mutex(), undo_trunc()
{
	ut_ad(!purge_sys);
	rw_lock_create(trx_purge_latch_key, &latch, SYNC_PURGE_LATCH);
	mutex_create(LATCH_ID_PURGE_SYS_PQ, &pq_mutex);
}

/** Destruct the purge system. */
purge_sys_t::~purge_sys_t()
{
	ut_ad(this == purge_sys);

	trx_t* trx = query->trx;
	que_graph_free(query);
	ut_ad(!trx->id);
	ut_ad(trx->state == TRX_STATE_ACTIVE);
	trx->state = TRX_STATE_NOT_STARTED;
	trx_free_for_background(trx);
	view.close();
	rw_lock_free(&latch);
	mutex_free(&pq_mutex);
	os_event_destroy(event);
}

/*================ UNDO LOG HISTORY LIST =============================*/

/********************************************************************//**
Adds the update undo log as the first log in the history list. Removes the
update undo log segment from the rseg slot if it is too big for reuse. */
void
trx_purge_add_update_undo_to_history(
/*=================================*/
	trx_t*		trx,		/*!< in: transaction */
	page_t*		undo_page,	/*!< in: update undo log header page,
					x-latched */
	mtr_t*		mtr)		/*!< in: mtr */
{
	trx_undo_t*	undo		= trx->rsegs.m_redo.update_undo;
	trx_rseg_t*	rseg		= undo->rseg;
	trx_rsegf_t*	rseg_header	= trx_rsegf_get(
		rseg->space, rseg->page_no, mtr);
	trx_ulogf_t*	undo_header	= undo_page + undo->hdr_offset;

	if (undo->state != TRX_UNDO_CACHED) {
		ulint		hist_size;
#ifdef UNIV_DEBUG
		trx_usegf_t*	seg_header = undo_page + TRX_UNDO_SEG_HDR;
#endif /* UNIV_DEBUG */

		/* The undo log segment will not be reused */

		if (UNIV_UNLIKELY(undo->id >= TRX_RSEG_N_SLOTS)) {
			ib::fatal() << "undo->id is " << undo->id;
		}

		trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL, mtr);

		MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);

		hist_size = mtr_read_ulint(
			rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, mtr);

		ut_ad(undo->size == flst_get_len(
			      seg_header + TRX_UNDO_PAGE_LIST));

		mlog_write_ulint(
			rseg_header + TRX_RSEG_HISTORY_SIZE,
			hist_size + undo->size, MLOG_4BYTES, mtr);
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
	      || ((srv_is_being_started
		   || trx_rollback_or_clean_is_active)
		  && purge_sys->state == PURGE_STATE_INIT)
	      || (srv_force_recovery >= SRV_FORCE_NO_BACKGROUND
		  && purge_sys->state == PURGE_STATE_DISABLED)
	      || ((trx->in_mysql_trx_list || trx->internal)
		  && srv_fast_shutdown));

	/* Add the log as the first in the history list */
	flst_add_first(rseg_header + TRX_RSEG_HISTORY,
		       undo_header + TRX_UNDO_HISTORY_NODE, mtr);

	my_atomic_addlint(&trx_sys->rseg_history_len, 1);

	/* Write the trx number to the undo log header */
	mlog_write_ull(undo_header + TRX_UNDO_TRX_NO, trx->no, mtr);

	/* Write information about delete markings to the undo log header */

	if (!undo->del_marks) {
		mlog_write_ulint(undo_header + TRX_UNDO_DEL_MARKS, FALSE,
				 MLOG_2BYTES, mtr);
	}

	if (rseg->last_page_no == FIL_NULL) {
		rseg->last_page_no = undo->hdr_page_no;
		rseg->last_offset = undo->hdr_offset;
		rseg->last_trx_no = trx->no;
		rseg->last_del_marks = undo->del_marks;
	}
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
	my_atomic_addlint(&trx_sys->rseg_history_len, -1);
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
	trx_ulogf_t*	log_hdr;
	trx_usegf_t*	seg_hdr;
	ulint		seg_size;
	ulint		hist_size;
	bool		marked		= false;

	for (;;) {
		page_t*	undo_page;

		mtr_start(&mtr);

		mutex_enter(&rseg->mutex);

		rseg_hdr = trx_rsegf_get(rseg->space, rseg->page_no, &mtr);

		undo_page = trx_undo_page_get(
			page_id_t(rseg->space, hdr_addr.page), &mtr);

		seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
		log_hdr = undo_page + hdr_addr.boffset;

		/* Mark the last undo log totally purged, so that if the
		system crashes, the tail of the undo log will not get accessed
		again. The list of pages in the undo log tail gets inconsistent
		during the freeing of the segment, and therefore purge should
		not try to access them again. */

		if (!marked) {
			marked = true;
			mlog_write_ulint(
				log_hdr + TRX_UNDO_DEL_MARKS, FALSE,
				MLOG_2BYTES, &mtr);
		}

		if (fseg_free_step_not_header(
			    seg_hdr + TRX_UNDO_FSEG_HEADER, &mtr)) {

			break;
		}

		mutex_exit(&rseg->mutex);

		mtr_commit(&mtr);
	}

	/* The page list may now be inconsistent, but the length field
	stored in the list base node tells us how big it was before we
	started the freeing. */

	seg_size = flst_get_len(seg_hdr + TRX_UNDO_PAGE_LIST);

	/* We may free the undo log segment header page; it must be freed
	within the same mtr as the undo log header is removed from the
	history list: otherwise, in case of a database crash, the segment
	could become inaccessible garbage in the file space. */

	trx_purge_remove_log_hdr(rseg_hdr, log_hdr, &mtr);

	do {

		/* Here we assume that a file segment with just the header
		page can be freed in a few steps, so that the buffer pool
		is not flooded with bufferfixed pages: see the note in
		fsp0fsp.cc. */

	} while (!fseg_free_step(seg_hdr + TRX_UNDO_FSEG_HEADER, &mtr));

	hist_size = mtr_read_ulint(rseg_hdr + TRX_RSEG_HISTORY_SIZE,
				   MLOG_4BYTES, &mtr);
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
@param[in]	limit		truncate offset */
static
void
trx_purge_truncate_rseg_history(trx_rseg_t* rseg, const purge_iter_t* limit)
{
	fil_addr_t	hdr_addr;
	fil_addr_t	prev_hdr_addr;
	trx_rsegf_t*	rseg_hdr;
	page_t*		undo_page;
	trx_ulogf_t*	log_hdr;
	trx_usegf_t*	seg_hdr;
	mtr_t		mtr;
	trx_id_t	undo_trx_no;

	mtr_start(&mtr);
	ut_ad(rseg->is_persistent());
	mutex_enter(&(rseg->mutex));

	rseg_hdr = trx_rsegf_get(rseg->space, rseg->page_no, &mtr);

	hdr_addr = trx_purge_get_log_from_hist(
		flst_get_last(rseg_hdr + TRX_RSEG_HISTORY, &mtr));
loop:
	if (hdr_addr.page == FIL_NULL) {

		mutex_exit(&(rseg->mutex));

		mtr_commit(&mtr);

		return;
	}

	undo_page = trx_undo_page_get(page_id_t(rseg->space, hdr_addr.page),
				      &mtr);

	log_hdr = undo_page + hdr_addr.boffset;

	undo_trx_no = mach_read_from_8(log_hdr + TRX_UNDO_TRX_NO);

	if (undo_trx_no >= limit->trx_no) {

		/* limit space_id should match the rollback segment
		space id to avoid freeing of the page belongs to
		different rollback segment for the same trx_no. */
		if (undo_trx_no == limit->trx_no
		    && rseg->space == limit->undo_rseg_space) {

			trx_undo_truncate_start(
				rseg, hdr_addr.page,
				hdr_addr.boffset, limit->undo_no);
		}

		mutex_exit(&(rseg->mutex));
		mtr_commit(&mtr);

		return;
	}

	prev_hdr_addr = trx_purge_get_log_from_hist(
		flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE, &mtr));

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	if ((mach_read_from_2(seg_hdr + TRX_UNDO_STATE) == TRX_UNDO_TO_PURGE)
	    && (mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG) == 0)) {

		/* We can free the whole log segment */

		mutex_exit(&(rseg->mutex));
		mtr_commit(&mtr);

		/* calls the trx_purge_remove_log_hdr()
		inside trx_purge_free_segment(). */
		trx_purge_free_segment(rseg, hdr_addr);
	} else {
		/* Remove the log hdr from the rseg history. */
		trx_purge_remove_log_hdr(rseg_hdr, log_hdr, &mtr);

		mutex_exit(&(rseg->mutex));
		mtr_commit(&mtr);
	}

	mtr_start(&mtr);
	mutex_enter(&(rseg->mutex));

	rseg_hdr = trx_rsegf_get(rseg->space, rseg->page_no, &mtr);

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

		ulint	sz = UNIV_PAGE_SIZE;
		void*	buf = ut_zalloc_nokey(sz + UNIV_PAGE_SIZE);
		if (buf == NULL) {
			os_file_close(handle);
			os_file_delete(innodb_log_file_key, log_file_name);
			delete[] log_file_name;
			return;
		}

		byte*	log_buf = static_cast<byte*>(
			ut_align(buf, UNIV_PAGE_SIZE));

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

			ulint	sz = UNIV_PAGE_SIZE;
			void*	buf = ut_zalloc_nokey(sz + UNIV_PAGE_SIZE);
			if (buf == NULL) {
				os_file_close(handle);
				os_file_delete(innodb_log_file_key,
					       log_file_name);
				delete[] log_file_name;
				return(false);
			}

			byte*	log_buf = static_cast<byte*>(
				ut_align(buf, UNIV_PAGE_SIZE));

			IORequest	request(IORequest::READ);

			dberr_t	err;

			err = os_file_read(request, handle, log_buf, 0, sz);

			os_file_close(handle);

			if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
				ib::info()
					<< "Unable to read '"
					<< log_file_name << "' : "
					<< err;

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
		    > (srv_max_undo_log_size / srv_page_size)) {
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
		if (trx_rseg_t* rseg = trx_sys->rseg_array[i]) {
			ut_ad(rseg->is_persistent());
			if (rseg->space == undo_trunc->get_marked_space_id()) {

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
	mutex_enter(&purge_sys->pq_mutex);
	typedef	std::vector<TrxUndoRsegs>	purge_elem_list_t;
	purge_elem_list_t			purge_elem_list;

	/* Remove rseg instances that are in the purge queue before we start
	truncate of corresponding UNDO truncate. */
	while (!purge_sys->purge_queue.empty()) {
		purge_elem_list.push_back(purge_sys->purge_queue.top());
		purge_sys->purge_queue.pop();
	}
	ut_ad(purge_sys->purge_queue.empty());

	for (purge_elem_list_t::iterator it = purge_elem_list.begin();
	     it != purge_elem_list.end();
	     ++it) {

		for (TrxUndoRsegs::iterator it2 = it->begin();
		     it2 != it->end();
		     ++it2) {

			if ((*it2)->space
				== undo_trunc->get_marked_space_id()) {
				it->erase(it2);
				break;
			}
		}

		if (it->size()) {
			/* size != 0 suggest that there exist other rsegs that
			needs processing so add this element to purge queue.
			Note: Other rseg could be non-redo rsegs. */
			purge_sys->purge_queue.push(*it);
		}
	}
	mutex_exit(&purge_sys->pq_mutex);
}

/** Iterate over selected UNDO tablespace and check if all the rsegs
that resides in the tablespace are free.
@param[in]	limit		truncate_limit
@param[in,out]	undo_trunc	undo truncate tracker */
static
void
trx_purge_initiate_truncate(
	purge_iter_t*	limit,
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
				UT_LIST_GET_FIRST(rseg->update_undo_cached);
			     undo != NULL && all_free;
			     undo = UT_LIST_GET_NEXT(undo_list, undo)) {

				if (limit->trx_no < undo->trx_id) {
					all_free = false;
				} else {
					cached_undo_size += undo->size;
				}
			}

			for (trx_undo_t* undo =
				UT_LIST_GET_FIRST(rseg->insert_undo_cached);
			     undo != NULL && all_free;
			     undo = UT_LIST_GET_NEXT(undo_list, undo)) {

				if (limit->trx_no < undo->trx_id) {
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
			space_id,
			UT_LIST_GET_FIRST(purge_sys->query->thrs)->graph->trx,
			NULL);
		buf_LRU_flush_or_remove_pages(space_id, &observer);
	}

	log_free_check();

	/* Adjust the tablespace metadata. */
	fil_space_t* space = fil_truncate_prepare(space_id);

	if (!space) {
		ib::error() << "Failed to find UNDO tablespace " << space_id;
		return;
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
	mtr.x_lock_space(space, __FILE__, __LINE__);
	fil_truncate_log(space, size, &mtr);
	fsp_header_init(space_id, size, &mtr);
	mutex_enter(&fil_system->mutex);
	space->size = file->size = size;
	mutex_exit(&fil_system->mutex);

	for (ulint i = 0; i < undo_trunc->rsegs_size(); ++i) {
		trx_rseg_t*	rseg = undo_trunc->get_ith_rseg(i);

		buf_block_t* rblock = trx_rseg_header_create(
			space_id, ULINT_MAX, rseg->id, &mtr);
		ut_ad(rblock);
		rseg->page_no = rblock ? rblock->page.id.page_no() : FIL_NULL;

		/* Before re-initialization ensure that we free the existing
		structure. There can't be any active transactions. */
		ut_a(UT_LIST_GET_LEN(rseg->update_undo_list) == 0);
		ut_a(UT_LIST_GET_LEN(rseg->insert_undo_list) == 0);

		trx_undo_t*	next_undo;

		for (trx_undo_t* undo =
			UT_LIST_GET_FIRST(rseg->update_undo_cached);
		     undo != NULL;
		     undo = next_undo) {

			next_undo = UT_LIST_GET_NEXT(undo_list, undo);
			UT_LIST_REMOVE(rseg->update_undo_cached, undo);
			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
			trx_undo_mem_free(undo);
		}

		for (trx_undo_t* undo =
			UT_LIST_GET_FIRST(rseg->insert_undo_cached);
		     undo != NULL;
		     undo = next_undo) {

			next_undo = UT_LIST_GET_NEXT(undo_list, undo);
			UT_LIST_REMOVE(rseg->insert_undo_cached, undo);
			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
			trx_undo_mem_free(undo);
		}

		UT_LIST_INIT(rseg->update_undo_list, &trx_undo_t::undo_list);
		UT_LIST_INIT(rseg->update_undo_cached, &trx_undo_t::undo_list);
		UT_LIST_INIT(rseg->insert_undo_list, &trx_undo_t::undo_list);
		UT_LIST_INIT(rseg->insert_undo_cached, &trx_undo_t::undo_list);

		/* These were written by trx_rseg_header_create(). */
		ut_ad(mach_read_from_4(TRX_RSEG + TRX_RSEG_MAX_SIZE
				       + rblock->frame)
		      == uint32_t(rseg->max_size));
		ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_HISTORY_SIZE
					+ rblock->frame));

		rseg->max_size = ULINT_MAX;

		/* Initialize the undo log lists according to the rseg header */
		rseg->curr_size = 1;
		rseg->trx_ref_count = 0;
		rseg->last_page_no = FIL_NULL;
		rseg->last_offset = 0;
		rseg->last_trx_no = 0;
		rseg->last_del_marks = FALSE;
	}

	mtr.commit_shrink(*space);

	/* No mutex; this is only updated by the purge coordinator. */
	export_vars.innodb_undo_truncations++;

	if (purge_sys->rseg != NULL
	    && purge_sys->rseg->last_page_no == FIL_NULL) {
		/* If purge_sys->rseg is pointing to rseg that was recently
		truncated then move to next rseg element.
		Note: Ideally purge_sys->rseg should be NULL because purge
		should complete processing of all the records but there is
		purge_batch_size that can force the purge loop to exit before
		all the records are purged and in this case purge_sys->rseg
		could point to a valid rseg waiting for next purge cycle. */
		purge_sys->next_stored = false;
		purge_sys->rseg = NULL;
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

/********************************************************************//**
Removes unnecessary history data from rollback segments. NOTE that when this
function is called, the caller must not have any latches on undo log pages! */
static
void
trx_purge_truncate_history(
/*========================*/
	purge_iter_t*		limit,		/*!< in: truncate limit */
	const ReadView*		view)		/*!< in: purge view */
{
	ut_ad(trx_purge_check_limit());

	/* We play safe and set the truncate limit at most to the purge view
	low_limit number, though this is not necessary */

	if (limit->trx_no >= view->low_limit_no()) {
		limit->trx_no = view->low_limit_no();
		limit->undo_no = 0;
		limit->undo_rseg_space = ULINT_UNDEFINED;
	}

	ut_ad(limit->trx_no <= purge_sys->view.low_limit_no());

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; ++i) {
		trx_rseg_t*	rseg = trx_sys->rseg_array[i];

		if (rseg != NULL) {
			ut_a(rseg->id == i);
			trx_purge_truncate_rseg_history(rseg, limit);
		}
	}

	/* UNDO tablespace truncate. We will try to truncate as much as we
	can (greedy approach). This will ensure when the server is idle we
	try and truncate all the UNDO tablespaces. */
	for (ulint i = srv_undo_tablespaces_active; i--; ) {
		trx_purge_mark_undo_for_truncate(&purge_sys->undo_trunc);
		trx_purge_initiate_truncate(limit, &purge_sys->undo_trunc);
	}
}

/***********************************************************************//**
Updates the last not yet purged history log info in rseg when we have purged
a whole undo log. Advances also purge_sys->purge_trx_no past the purged log. */
static
void
trx_purge_rseg_get_next_history_log(
/*================================*/
	trx_rseg_t*	rseg,		/*!< in: rollback segment */
	ulint*		n_pages_handled)/*!< in/out: number of UNDO pages
					handled */
{
	page_t*		undo_page;
	trx_ulogf_t*	log_hdr;
	fil_addr_t	prev_log_addr;
	trx_id_t	trx_no;
	ibool		del_marks;
	mtr_t		mtr;

	mutex_enter(&(rseg->mutex));

	ut_a(rseg->last_page_no != FIL_NULL);

	purge_sys->iter.trx_no = rseg->last_trx_no + 1;
	purge_sys->iter.undo_no = 0;
	purge_sys->iter.undo_rseg_space = ULINT_UNDEFINED;
	purge_sys->next_stored = false;

	mtr_start(&mtr);

	undo_page = trx_undo_page_get_s_latched(
		page_id_t(rseg->space, rseg->last_page_no), &mtr);

	log_hdr = undo_page + rseg->last_offset;

	/* Increase the purge page count by one for every handled log */

	(*n_pages_handled)++;

	prev_log_addr = trx_purge_get_log_from_hist(
		flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE, &mtr));

	if (prev_log_addr.page == FIL_NULL) {
		/* No logs left in the history list */

		rseg->last_page_no = FIL_NULL;

		mutex_exit(&(rseg->mutex));
		mtr_commit(&mtr);
		return;
	}

	mutex_exit(&rseg->mutex);

	mtr_commit(&mtr);

	/* Read the trx number and del marks from the previous log header */
	mtr_start(&mtr);

	log_hdr = trx_undo_page_get_s_latched(page_id_t(rseg->space,
							prev_log_addr.page),
					      &mtr)
		+ prev_log_addr.boffset;

	trx_no = mach_read_from_8(log_hdr + TRX_UNDO_TRX_NO);

	del_marks = mach_read_from_2(log_hdr + TRX_UNDO_DEL_MARKS);

	mtr_commit(&mtr);

	mutex_enter(&(rseg->mutex));

	rseg->last_page_no = prev_log_addr.page;
	rseg->last_offset = prev_log_addr.boffset;
	rseg->last_trx_no = trx_no;
	rseg->last_del_marks = del_marks;

	TrxUndoRsegs elem(rseg->last_trx_no);
	elem.push_back(rseg);

	/* Purge can also produce events, however these are already ordered
	in the rollback segment and any user generated event will be greater
	than the events that Purge produces. ie. Purge can never produce
	events from an empty rollback segment. */

	mutex_enter(&purge_sys->pq_mutex);

	purge_sys->purge_queue.push(elem);

	mutex_exit(&purge_sys->pq_mutex);

	mutex_exit(&rseg->mutex);
}

/** Position the purge sys "iterator" on the undo record to use for purging. */
static
void
trx_purge_read_undo_rec()
{
	ulint		offset;
	ulint		page_no;
	ib_uint64_t	undo_no;
	ulint		undo_rseg_space;

	purge_sys->hdr_offset = purge_sys->rseg->last_offset;
	page_no = purge_sys->hdr_page_no = purge_sys->rseg->last_page_no;

	if (purge_sys->rseg->last_del_marks) {
		mtr_t		mtr;
		trx_undo_rec_t*	undo_rec = NULL;

		mtr_start(&mtr);

		undo_rec = trx_undo_get_first_rec(
			purge_sys->rseg->space,
			purge_sys->hdr_page_no,
			purge_sys->hdr_offset, RW_S_LATCH, &mtr);

		if (undo_rec != NULL) {
			offset = page_offset(undo_rec);
			undo_no = trx_undo_rec_get_undo_no(undo_rec);
			undo_rseg_space = purge_sys->rseg->space;
			page_no = page_get_page_no(page_align(undo_rec));
		} else {
			offset = 0;
			undo_no = 0;
			undo_rseg_space = ULINT_UNDEFINED;
		}

		mtr_commit(&mtr);
	} else {
		offset = 0;
		undo_no = 0;
		undo_rseg_space = ULINT_UNDEFINED;
	}

	purge_sys->offset = offset;
	purge_sys->page_no = page_no;
	purge_sys->iter.undo_no = undo_no;
	purge_sys->iter.undo_rseg_space = undo_rseg_space;

	purge_sys->next_stored = true;
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
	ut_ad(!purge_sys->next_stored);

	if (purge_sys->rseg_iter.set_next()) {
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

	ut_ad(purge_sys->next_stored);
	ut_ad(purge_sys->iter.trx_no < purge_sys->view.low_limit_no());

	space = purge_sys->rseg->space;
	page_no = purge_sys->page_no;
	offset = purge_sys->offset;

	if (offset == 0) {
		/* It is the dummy undo log record, which means that there is
		no need to purge this undo log */

		trx_purge_rseg_get_next_history_log(
			purge_sys->rseg, n_pages_handled);

		/* Look for the next undo log and record to purge */

		trx_purge_choose_next_log();

		return(&trx_purge_dummy_rec);
	}

	mtr_start(&mtr);

	undo_page = trx_undo_page_get_s_latched(page_id_t(space, page_no),
						&mtr);

	rec = undo_page + offset;

	rec2 = rec;

	for (;;) {
		ulint		type;
		trx_undo_rec_t*	next_rec;
		ulint		cmpl_info;

		/* Try first to find the next record which requires a purge
		operation from the same page of the same undo log */

		next_rec = trx_undo_page_get_next_rec(
			rec2, purge_sys->hdr_page_no, purge_sys->hdr_offset);

		if (next_rec == NULL) {
			rec2 = trx_undo_get_next_rec(
				rec2, purge_sys->hdr_page_no,
				purge_sys->hdr_offset, &mtr);
			break;
		}

		rec2 = next_rec;

		type = trx_undo_rec_get_type(rec2);

		if (type == TRX_UNDO_DEL_MARK_REC) {

			break;
		}

		cmpl_info = trx_undo_rec_get_cmpl_info(rec2);

		if (trx_undo_rec_get_extern_storage(rec2)) {
			break;
		}

		if ((type == TRX_UNDO_UPD_EXIST_REC)
		    && !(cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {
			break;
		}
	}

	if (rec2 == NULL) {
		mtr_commit(&mtr);

		trx_purge_rseg_get_next_history_log(
			purge_sys->rseg, n_pages_handled);

		/* Look for the next undo log and record to purge */

		trx_purge_choose_next_log();

		mtr_start(&mtr);

		undo_page = trx_undo_page_get_s_latched(
			page_id_t(space, page_no), &mtr);

		rec = undo_page + offset;
	} else {
		page = page_align(rec2);

		purge_sys->offset = rec2 - page;
		purge_sys->page_no = page_get_page_no(page);
		purge_sys->iter.undo_no = trx_undo_rec_get_undo_no(rec2);
		purge_sys->iter.undo_rseg_space = space;

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
	if (!purge_sys->next_stored) {
		trx_purge_choose_next_log();

		if (!purge_sys->next_stored) {
			DBUG_PRINT("ib_purge",
				   ("no logs left in the history list"));
			return(NULL);
		}
	}

	if (purge_sys->iter.trx_no >= purge_sys->view.low_limit_no()) {

		return(NULL);
	}

	/* fprintf(stderr, "Thread %lu purging trx %llu undo record %llu\n",
	os_thread_get_curr_id(), iter->trx_no, iter->undo_no); */

	*roll_ptr = trx_undo_build_roll_ptr(
		FALSE, purge_sys->rseg->id,
		purge_sys->page_no, purge_sys->offset);

	/* The following call will advance the stored values of the
	purge iterator. */

	return(trx_purge_get_next_rec(n_pages_handled, heap));
}

/*******************************************************************//**
This function runs a purge batch.
@return number of undo log pages handled in the batch */
static
ulint
trx_purge_attach_undo_recs(
/*=======================*/
	ulint		n_purge_threads,/*!< in: number of purge threads */
	purge_sys_t*	purge_sys,	/*!< in/out: purge instance */
	ulint		batch_size)	/*!< in: no. of pages to purge */
{
	que_thr_t*	thr;
	ulint		i;
	ulint		n_pages_handled = 0;
	ulint		n_thrs = UT_LIST_GET_LEN(purge_sys->query->thrs);

	ut_a(n_purge_threads > 0);

	purge_sys->limit = purge_sys->iter;

#ifdef UNIV_DEBUG
	i = 0;
	/* Debug code to validate some pre-requisites and reset done flag. */
	for (thr = UT_LIST_GET_FIRST(purge_sys->query->thrs);
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
	thr = UT_LIST_GET_FIRST(purge_sys->query->thrs);
	ut_a(n_thrs > 0 && thr != NULL);

	ut_ad(trx_purge_check_limit());

	i = 0;

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

		if (trx_purge_check_limit()) {
			purge_sys->limit = purge_sys->iter;
		}

		/* Fetch the next record, and advance the purge_sys->iter. */
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
			thr = UT_LIST_GET_FIRST(purge_sys->query->thrs);
		}

		ut_a(thr != NULL);
	}

	ut_ad(trx_purge_check_limit());

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
	without holding trx_sys->mutex. */

	if (srv_max_purge_lag > 0) {
		float	ratio;

		ratio = float(trx_sys->rseg_history_len) / srv_max_purge_lag;

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

/*******************************************************************//**
Wait for pending purge jobs to complete. */
static
void
trx_purge_wait_for_workers_to_complete(
/*===================================*/
	purge_sys_t*	purge_sys)	/*!< in: purge instance */
{
	ulint		n_submitted = purge_sys->n_submitted;

	/* Ensure that the work queue empties out. */
	while ((ulint) my_atomic_loadlint(&purge_sys->n_completed) != n_submitted) {

		if (srv_get_task_queue_length() > 0) {
			srv_release_threads(SRV_WORKER, 1);
		}

		os_thread_yield();
	}

	/* None of the worker threads should be doing any work. */
	ut_a(purge_sys->n_submitted == purge_sys->n_completed);

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
	ulint	batch_size,		/*!< in: the maximum number of records
					to purge in one batch */
	bool	truncate		/*!< in: truncate history if true */
#ifdef UNIV_DEBUG
	, srv_slot_t *slot		/*!< in/out: purge coordinator
					thread slot */
#endif
)
{
	que_thr_t*	thr = NULL;
	ulint		n_pages_handled;

	ut_a(n_purge_threads > 0);

	srv_dml_needed_delay = trx_purge_dml_delay();

	/* The number of tasks submitted should be completed. */
	ut_a(purge_sys->n_submitted == purge_sys->n_completed);

	rw_lock_x_lock(&purge_sys->latch);
	trx_sys->mvcc->clone_oldest_view(&purge_sys->view);
	rw_lock_x_unlock(&purge_sys->latch);

#ifdef UNIV_DEBUG
	if (srv_purge_view_update_only_debug) {
		return(0);
	}
#endif /* UNIV_DEBUG */

	/* Fetch the UNDO recs that need to be purged. */
	n_pages_handled = trx_purge_attach_undo_recs(
		n_purge_threads, purge_sys, batch_size);

	/* Do we do an asynchronous purge or not ? */
	if (n_purge_threads > 1) {
		ulint	i = 0;

		/* Submit the tasks to the work queue. */
		for (i = 0; i < n_purge_threads - 1; ++i) {
			thr = que_fork_scheduler_round_robin(
				purge_sys->query, thr);

			ut_a(thr != NULL);

			srv_que_task_enqueue_low(thr);
		}

		thr = que_fork_scheduler_round_robin(purge_sys->query, thr);
		ut_a(thr != NULL);

		purge_sys->n_submitted += n_purge_threads - 1;

		goto run_synchronously;

	/* Do it synchronously. */
	} else {
		thr = que_fork_scheduler_round_robin(purge_sys->query, NULL);
		ut_ad(thr);

run_synchronously:
		++purge_sys->n_submitted;

		ut_d(thr->thread_slot = slot);
		que_run_threads(thr);

		my_atomic_addlint(
			&purge_sys->n_completed, 1);

		if (n_purge_threads > 1) {
			trx_purge_wait_for_workers_to_complete(purge_sys);
		}
	}

	ut_a(purge_sys->n_submitted == purge_sys->n_completed);

#ifdef UNIV_DEBUG
	rw_lock_x_lock(&purge_sys->latch);
	if (purge_sys->limit.trx_no == 0) {
		purge_sys->done = purge_sys->iter;
	} else {
		purge_sys->done = purge_sys->limit;
	}
	rw_lock_x_unlock(&purge_sys->latch);
#endif /* UNIV_DEBUG */

	if (truncate) {
		trx_purge_truncate_history(
			purge_sys->limit.trx_no
			? &purge_sys->limit
			: &purge_sys->iter,
			&purge_sys->view);
	}

	MONITOR_INC_VALUE(MONITOR_PURGE_INVOKED, 1);
	MONITOR_INC_VALUE(MONITOR_PURGE_N_PAGE_HANDLED, n_pages_handled);

	return(n_pages_handled);
}

/*******************************************************************//**
Get the purge state.
@return purge state. */
purge_state_t
trx_purge_state(void)
/*=================*/
{
	purge_state_t	state;

	rw_lock_x_lock(&purge_sys->latch);

	state = purge_sys->state;

	rw_lock_x_unlock(&purge_sys->latch);

	return(state);
}

/*******************************************************************//**
Stop purge and wait for it to stop, move to PURGE_STATE_STOP. */
void
trx_purge_stop(void)
/*================*/
{
	rw_lock_x_lock(&purge_sys->latch);

	switch (purge_sys->state) {
	case PURGE_STATE_INIT:
	case PURGE_STATE_DISABLED:
		ut_error;
	case PURGE_STATE_EXIT:
		/* Shutdown must have been initiated during
		FLUSH TABLES FOR EXPORT. */
		ut_ad(!srv_undo_sources);
unlock:
		rw_lock_x_unlock(&purge_sys->latch);
		break;
	case PURGE_STATE_STOP:
		ut_ad(srv_n_purge_threads > 0);
		++purge_sys->n_stop;
		purge_sys->state = PURGE_STATE_STOP;
		if (!purge_sys->running) {
			goto unlock;
		}
		ib::info() << "Waiting for purge to stop";
		do {
			rw_lock_x_unlock(&purge_sys->latch);
			os_thread_sleep(10000);
			rw_lock_x_lock(&purge_sys->latch);
		} while (purge_sys->running);
		goto unlock;
	case PURGE_STATE_RUN:
		ut_ad(srv_n_purge_threads > 0);
		++purge_sys->n_stop;
		ib::info() << "Stopping purge";

		/* We need to wakeup the purge thread in case it is suspended,
		so that it can acknowledge the state change. */

		const int64_t sig_count = os_event_reset(purge_sys->event);
		purge_sys->state = PURGE_STATE_STOP;
		rw_lock_x_unlock(&purge_sys->latch);
		srv_purge_wakeup();
		/* Wait for purge coordinator to signal that it
		is suspended. */
		os_event_wait_low(purge_sys->event, sig_count);
	}

	MONITOR_INC_VALUE(MONITOR_PURGE_STOP_COUNT, 1);
}

/*******************************************************************//**
Resume purge, move to PURGE_STATE_RUN. */
void
trx_purge_run(void)
/*===============*/
{
	rw_lock_x_lock(&purge_sys->latch);

	switch (purge_sys->state) {
	case PURGE_STATE_EXIT:
		/* Shutdown must have been initiated during
		FLUSH TABLES FOR EXPORT. */
		ut_ad(!srv_undo_sources);
		break;
	case PURGE_STATE_INIT:
	case PURGE_STATE_DISABLED:
		ut_error;

	case PURGE_STATE_RUN:
		ut_a(!purge_sys->n_stop);
		break;
	case PURGE_STATE_STOP:
		ut_a(purge_sys->n_stop);
		if (--purge_sys->n_stop == 0) {

			ib::info() << "Resuming purge";

			purge_sys->state = PURGE_STATE_RUN;
		}

		MONITOR_INC_VALUE(MONITOR_PURGE_RESUME_COUNT, 1);
	}

	rw_lock_x_unlock(&purge_sys->latch);

	srv_purge_wakeup();
}
