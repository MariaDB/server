/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
#include "mach0data.h"
#include "mtr0log.h"
#include "que0que.h"
#include "row0purge.h"
#include "row0upd.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include <mysql/service_wsrep.h>

#include <unordered_map>

/** Maximum allowable purge history length.  <=0 means 'infinite'. */
ulong		srv_max_purge_lag = 0;

/** Max DML user threads delay in micro-seconds. */
ulong		srv_max_purge_lag_delay = 0;

/** The global data structure coordinating a purge */
purge_sys_t	purge_sys;

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
TRANSACTIONAL_INLINE inline bool TrxUndoRsegsIterator::set_next()
{
	mysql_mutex_lock(&purge_sys.pq_mutex);

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
		purge_sys.tail.trx_no = (*m_iter)->last_trx_no();
	} else if (!purge_sys.purge_queue.empty()) {
		m_rsegs = purge_sys.purge_queue.top();
		purge_sys.purge_queue.pop();
		ut_ad(purge_sys.purge_queue.empty()
		      || purge_sys.purge_queue.top() != m_rsegs);
		m_iter = m_rsegs.begin();
	} else {
		/* Queue is empty, reset iterator. */
		purge_sys.rseg = NULL;
		mysql_mutex_unlock(&purge_sys.pq_mutex);
		m_rsegs = NullElement;
		m_iter = m_rsegs.begin();
		return false;
	}

	purge_sys.rseg = *m_iter++;
	mysql_mutex_unlock(&purge_sys.pq_mutex);

	/* We assume in purge of externally stored fields that space
	id is in the range of UNDO tablespace space ids */
	ut_ad(purge_sys.rseg->space->id == TRX_SYS_SPACE
	      || srv_is_undo_tablespace(purge_sys.rseg->space->id));

	trx_id_t last_trx_no;
	{
#ifdef SUX_LOCK_GENERIC
		purge_sys.rseg->latch.rd_lock(SRW_LOCK_CALL);
#else
		transactional_shared_lock_guard<srw_spin_lock> rg
			{purge_sys.rseg->latch};
#endif
		last_trx_no = purge_sys.rseg->last_trx_no();

		purge_sys.hdr_offset = purge_sys.rseg->last_offset();
		purge_sys.hdr_page_no = purge_sys.rseg->last_page_no;

#ifdef SUX_LOCK_GENERIC
		purge_sys.rseg->latch.rd_unlock();
#endif
	}

	/* Only the purge coordinator task will access this object
	purge_sys.rseg_iter, or any of purge_sys.hdr_page_no,
	purge_sys.tail, purge_sys.head, or modify purge_sys.view. */
	ut_ad(last_trx_no == m_rsegs.trx_no);
	ut_a(purge_sys.hdr_page_no != FIL_NULL);
	ut_a(purge_sys.tail.trx_no <= last_trx_no);
	purge_sys.tail.trx_no = last_trx_no;

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
	trx->start_time = time(NULL);
	trx->start_time_micro = microsecond_interval_timer();
	trx->state = TRX_STATE_ACTIVE;
	trx->op_info = "purge trx";

	mem_heap_t*	heap = mem_heap_create(512);
	que_fork_t*	fork = que_fork_create(heap);
	fork->trx = trx;

	for (auto i = innodb_purge_threads_MAX; i; i--) {
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
  ut_ad(!heap);
  ut_ad(!enabled());
  m_paused= 0;
  m_SYS_paused= 0;
  query= purge_graph_build();
  next_stored= false;
  rseg= NULL;
  page_no= 0;
  offset= 0;
  hdr_page_no= 0;
  hdr_offset= 0;
  latch.SRW_LOCK_INIT(trx_purge_latch_key);
  end_latch.init();
  mysql_mutex_init(purge_sys_pq_mutex_key, &pq_mutex, nullptr);
  truncate.current= NULL;
  truncate.last= NULL;
  heap= mem_heap_create(4096);
}

/** Close the purge subsystem on shutdown. */
void purge_sys_t::close()
{
  ut_ad(this == &purge_sys);
  if (!heap)
    return;

  ut_ad(!enabled());
  trx_t* trx = query->trx;
  que_graph_free(query);
  ut_ad(!trx->id);
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  trx->state= TRX_STATE_NOT_STARTED;
  trx->free();
  latch.destroy();
  end_latch.destroy();
  mysql_mutex_destroy(&pq_mutex);
  mem_heap_free(heap);
  heap= nullptr;
}

/** Determine if the history of a transaction is purgeable.
@param trx_id  transaction identifier
@return whether the history is purgeable */
TRANSACTIONAL_TARGET bool purge_sys_t::is_purgeable(trx_id_t trx_id) const
{
  bool purgeable;
#if !defined SUX_LOCK_GENERIC && !defined NO_ELISION
  purgeable= false;
  if (xbegin())
  {
    if (!latch.is_write_locked())
    {
      purgeable= view.changes_visible(trx_id);
      xend();
    }
    else
      xabort();
  }
  else
#endif
  {
    latch.rd_lock(SRW_LOCK_CALL);
    purgeable= view.changes_visible(trx_id);
    latch.rd_unlock();
  }
  return purgeable;
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
			   trx->id, trx_id_t{trx->rw_trx_hash_element->no}));
	ut_ad(undo == trx->rsegs.m_redo.undo);
	trx_rseg_t*	rseg		= trx->rsegs.m_redo.rseg;
	ut_ad(undo->rseg == rseg);
	buf_block_t*	rseg_header	= rseg->get(mtr, nullptr);
	/* We are in transaction commit; we cannot return an error. If the
	database is corrupted, it is better to crash it than to
	intentionally violate ACID by committing something that is known to
	be corrupted. */
	ut_ad(rseg_header);
	buf_block_t*	undo_page	= trx_undo_set_state_at_finish(
		undo, mtr);
	trx_ulogf_t*	undo_header	= undo_page->page.frame
		+ undo->hdr_offset;

	ut_ad(rseg->needs_purge > trx->id);

	if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT
					   + rseg_header->page.frame))) {
		/* This database must have been upgraded from
		before MariaDB 10.3.5. */
		trx_rseg_format_upgrade(rseg_header, mtr);
	}

	if (undo->state != TRX_UNDO_CACHED) {
		/* The undo log segment will not be reused */
		ut_a(undo->id < TRX_RSEG_N_SLOTS);
		static_assert(FIL_NULL == 0xffffffff, "");
		mtr->memset(rseg_header,
			    TRX_RSEG + TRX_RSEG_UNDO_SLOTS
			    + undo->id * TRX_RSEG_SLOT_SIZE, 4, 0xff);

		uint32_t hist_size = mach_read_from_4(
			TRX_RSEG_HISTORY_SIZE + TRX_RSEG
			+ rseg_header->page.frame);

		ut_ad(undo->size == flst_get_len(TRX_UNDO_SEG_HDR
						 + TRX_UNDO_PAGE_LIST
						 + undo_page->page.frame));

		mtr->write<4>(*rseg_header, TRX_RSEG + TRX_RSEG_HISTORY_SIZE
			      + rseg_header->page.frame,
			      hist_size + undo->size);
		mtr->write<8>(*rseg_header, TRX_RSEG + TRX_RSEG_MAX_TRX_ID
			      + rseg_header->page.frame,
			      trx_sys.get_max_trx_id());
	}

	/* After the purge thread has been given permission to exit,
	we may roll back transactions (trx->undo_no==0)
	in THD::cleanup() invoked from unlink_thd() in fast shutdown,
	or in trx_rollback_recovered() in slow shutdown.

	Before any transaction-generating background threads or the
	purge have been started, we can
	start transactions in row_merge_drop_temp_indexes(),
	and roll back recovered transactions.

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
	      || srv_fast_shutdown);

#ifdef	WITH_WSREP
	if (wsrep_is_wsrep_xid(&trx->xid)) {
		trx_rseg_update_wsrep_checkpoint(rseg_header, &trx->xid, mtr);
	}
#endif

	if (trx->mysql_log_file_name && *trx->mysql_log_file_name) {
		/* Update the latest MySQL binlog name and offset info
		in rollback segment header if MySQL binlogging is on
		or the database server is a MySQL replication save. */
		trx_rseg_update_binlog_offset(
			rseg_header, trx->mysql_log_file_name,
			trx->mysql_log_offset, mtr);
	}

	/* Add the log as the first in the history list */

	/* We are in transaction commit; we cannot return an error
	when detecting corruption. It is better to crash the server
	than to intentionally violate ACID by committing something
	that is known to be corrupted. */
	ut_a(flst_add_first(rseg_header, TRX_RSEG + TRX_RSEG_HISTORY, undo_page,
			    static_cast<uint16_t>(undo->hdr_offset
						  + TRX_UNDO_HISTORY_NODE),
			    mtr) == DB_SUCCESS);

	mtr->write<8,mtr_t::MAYBE_NOP>(*undo_page,
				       undo_header + TRX_UNDO_TRX_NO,
				       trx->rw_trx_hash_element->no);

	if (rseg->last_page_no == FIL_NULL) {
		rseg->last_page_no = undo->hdr_page_no;
		rseg->set_last_commit(undo->hdr_offset,
				      trx->rw_trx_hash_element->no);
	}

	rseg->history_size++;

	if (undo->state == TRX_UNDO_CACHED) {
		UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
	} else {
		ut_ad(undo->state == TRX_UNDO_TO_PURGE);
		ut_free(undo);
	}

	undo = NULL;
}

/** Free an undo log segment.
@param block     rollback segment header page
@param mtr       mini-transaction */
static void trx_purge_free_segment(buf_block_t *block, mtr_t &mtr)
{
  while (!fseg_free_step_not_header(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER +
                                    block->page.frame, &mtr))
  {
    block->fix();
    ut_d(const page_id_t id{block->page.id()});
    mtr.commit();
    /* NOTE: If the server is killed after the log that was produced
    up to this point was written, and before the log from the mtr.commit()
    in our caller is written, then the pages belonging to the
    undo log will become unaccessible garbage.

    This does not matter when using multiple innodb_undo_tablespaces;
    innodb_undo_log_truncate=ON will be able to reclaim the space. */
    log_free_check();
    mtr.start();
    block->page.lock.x_lock();
    ut_ad(block->page.id() == id);
    mtr.memo_push(block, MTR_MEMO_PAGE_X_MODIFY);
  }

  while (!fseg_free_step(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER +
                         block->page.frame, &mtr));
}

/** Remove unnecessary history data from a rollback segment.
@param rseg   rollback segment
@param limit  truncate anything before this
@param all    whether everything can be truncated
@return error code */
static dberr_t
trx_purge_truncate_rseg_history(trx_rseg_t &rseg,
                                const purge_sys_t::iterator &limit, bool all)
{
  fil_addr_t hdr_addr;
  mtr_t mtr;

  log_free_check();
  mtr.start();

  dberr_t err;
  buf_block_t *rseg_hdr= rseg.get(&mtr, &err);
  if (!rseg_hdr)
  {
func_exit:
    mtr.commit();
    return err;
  }

  hdr_addr= flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + rseg_hdr->page.frame);
  hdr_addr.boffset= static_cast<uint16_t>(hdr_addr.boffset -
                                          TRX_UNDO_HISTORY_NODE);

loop:
  if (hdr_addr.page == FIL_NULL)
    goto func_exit;

  buf_block_t *b=
    buf_page_get_gen(page_id_t(rseg.space->id, hdr_addr.page),
                     0, RW_X_LATCH, nullptr, BUF_GET_POSSIBLY_FREED,
                     &mtr, &err);
  if (!b)
    goto func_exit;

  const trx_id_t undo_trx_no=
    mach_read_from_8(b->page.frame + hdr_addr.boffset + TRX_UNDO_TRX_NO);

  if (undo_trx_no >= limit.trx_no)
  {
    if (undo_trx_no == limit.trx_no)
      err = trx_undo_truncate_start(&rseg, hdr_addr.page,
                                    hdr_addr.boffset, limit.undo_no);
    goto func_exit;
  }

  if (!all)
    goto func_exit;

  fil_addr_t prev_hdr_addr=
    flst_get_prev_addr(b->page.frame + hdr_addr.boffset +
                       TRX_UNDO_HISTORY_NODE);
  prev_hdr_addr.boffset= static_cast<uint16_t>(prev_hdr_addr.boffset -
                                               TRX_UNDO_HISTORY_NODE);

  err= flst_remove(rseg_hdr, TRX_RSEG + TRX_RSEG_HISTORY, b,
                   uint16_t(hdr_addr.boffset + TRX_UNDO_HISTORY_NODE), &mtr);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
    goto func_exit;

  rseg_hdr->fix();

  if (mach_read_from_2(b->page.frame + hdr_addr.boffset + TRX_UNDO_NEXT_LOG))
    /* We cannot free the entire undo log segment. */;
  else
  {
    const uint32_t seg_size=
      flst_get_len(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + b->page.frame);
    switch (mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE +
                             b->page.frame)) {
    case TRX_UNDO_TO_PURGE:
      {
        byte *hist= TRX_RSEG + TRX_RSEG_HISTORY_SIZE + rseg_hdr->page.frame;
        ut_ad(mach_read_from_4(hist) >= seg_size);
        mtr.write<4>(*rseg_hdr, hist, mach_read_from_4(hist) - seg_size);
      }
    free_segment:
      ut_ad(rseg.curr_size >= seg_size);
      rseg.curr_size-= seg_size;
      trx_purge_free_segment(b, mtr);
      break;
    case TRX_UNDO_CACHED:
      /* rseg.undo_cached must point to this page */
      trx_undo_t *undo= UT_LIST_GET_FIRST(rseg.undo_cached);
      for (; undo; undo= UT_LIST_GET_NEXT(undo_list, undo))
        if (undo->hdr_page_no == hdr_addr.page)
          goto found_cached;
      ut_ad("inconsistent undo logs" == 0);
      break;
    found_cached:
      UT_LIST_REMOVE(rseg.undo_cached, undo);
      static_assert(FIL_NULL == 0xffffffff, "");
      if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                                         rseg_hdr->page.frame)))
        trx_rseg_format_upgrade(rseg_hdr, &mtr);
      mtr.memset(rseg_hdr, TRX_RSEG + TRX_RSEG_UNDO_SLOTS +
                 undo->id * TRX_RSEG_SLOT_SIZE, 4, 0xff);
      ut_free(undo);
      mtr.write<8,mtr_t::MAYBE_NOP>(*rseg_hdr, TRX_RSEG + TRX_RSEG_MAX_TRX_ID +
                                    rseg_hdr->page.frame,
                                    trx_sys.get_max_trx_id() - 1);
      goto free_segment;
    }
  }

  hdr_addr= prev_hdr_addr;

  mtr.commit();
  ut_ad(rseg.history_size > 0);
  rseg.history_size--;
  log_free_check();
  mtr.start();
  rseg_hdr->page.lock.x_lock();
  ut_ad(rseg_hdr->page.id() == rseg.page_id());
  mtr.memo_push(rseg_hdr, MTR_MEMO_PAGE_X_MODIFY);

  goto loop;
}

/** Cleanse purge queue to remove the rseg that reside in undo-tablespace
marked for truncate.
@param[in]	space	undo tablespace being truncated */
static void trx_purge_cleanse_purge_queue(const fil_space_t& space)
{
	typedef	std::vector<TrxUndoRsegs>	purge_elem_list_t;
	purge_elem_list_t			purge_elem_list;

	mysql_mutex_lock(&purge_sys.pq_mutex);

	/* Remove rseg instances that are in the purge queue before we start
	truncate of corresponding UNDO truncate. */
	while (!purge_sys.purge_queue.empty()) {
		purge_elem_list.push_back(purge_sys.purge_queue.top());
		purge_sys.purge_queue.pop();
	}

	for (purge_elem_list_t::iterator it = purge_elem_list.begin();
	     it != purge_elem_list.end();
	     ++it) {

		for (TrxUndoRsegs::iterator it2 = it->begin();
		     it2 != it->end();
		     ++it2) {
			if ((*it2)->space == &space) {
				it->erase(it2);
				break;
			}
		}

		if (!it->empty()) {
			purge_sys.purge_queue.push(*it);
		}
	}

	mysql_mutex_unlock(&purge_sys.pq_mutex);
}

#if defined __GNUC__ && __GNUC__ == 4 && !defined __clang__
# if defined __arm__ || defined __aarch64__
/* Work around an internal compiler error in GCC 4.8.5 */
__attribute__((optimize(0)))
# endif
#endif
/**
Removes unnecessary history data from rollback segments. NOTE that when this
function is called, the caller must not have any latches on undo log pages!
*/
TRANSACTIONAL_TARGET static void trx_purge_truncate_history()
{
  ut_ad(purge_sys.head <= purge_sys.tail);
  purge_sys_t::iterator &head= purge_sys.head.trx_no
    ? purge_sys.head : purge_sys.tail;

  if (head.trx_no >= purge_sys.low_limit_no())
  {
    /* This is sometimes necessary. TODO: find out why. */
    head.trx_no= purge_sys.low_limit_no();
    head.undo_no= 0;
  }

  dberr_t err= DB_SUCCESS;
  for (auto &rseg : trx_sys.rseg_array)
    if (rseg.space)
    {
      ut_ad(rseg.is_persistent());
      rseg.latch.wr_lock(SRW_LOCK_CALL);
      if (dberr_t e=
          trx_purge_truncate_rseg_history(rseg, head,
                                          !rseg.is_referenced() &&
                                          rseg.needs_purge <= head.trx_no))
        err= e;
      rseg.latch.wr_unlock();
    }

  if (err != DB_SUCCESS || srv_undo_tablespaces_active < 2)
    return;

  while (srv_undo_log_truncate)
  {
    if (!purge_sys.truncate.current)
    {
      const ulint threshold=
        ulint(srv_max_undo_log_size >> srv_page_size_shift);
      for (uint32_t i= purge_sys.truncate.last
           ? purge_sys.truncate.last->id - srv_undo_space_id_start : 0,
           j= i;; )
      {
        const uint32_t space_id= srv_undo_space_id_start + i;
        ut_ad(srv_is_undo_tablespace(space_id));
        fil_space_t *space= fil_space_get(space_id);
        ut_a(UT_LIST_GET_LEN(space->chain) == 1);

        if (space && space->get_size() > threshold)
        {
          purge_sys.truncate.current= space;
          break;
        }

        ++i;
        i %= srv_undo_tablespaces_active;
        if (i == j)
          return;
      }
    }

    fil_space_t &space= *purge_sys.truncate.current;
    /* Undo tablespace always are a single file. */
    fil_node_t *file= UT_LIST_GET_FIRST(space.chain);
    /* The undo tablespace files are never closed. */
    ut_ad(file->is_open());

    DBUG_LOG("undo", "marking for truncate: " << file->name);

    for (auto &rseg : trx_sys.rseg_array)
      if (rseg.space == &space)
        /* Once set, this rseg will not be allocated to subsequent
        transactions, but we will wait for existing active
        transactions to finish. */
        rseg.set_skip_allocation();

    for (auto &rseg : trx_sys.rseg_array)
    {
      if (rseg.space != &space)
        continue;

      rseg.latch.rd_lock(SRW_LOCK_CALL);
      ut_ad(rseg.skip_allocation());
      if (rseg.is_referenced() || rseg.needs_purge > head.trx_no)
      {
not_free:
        rseg.latch.rd_unlock();
        return;
      }

      ut_ad(UT_LIST_GET_LEN(rseg.undo_list) == 0);
      /* Check if all segments are cached and safe to remove. */
      ulint cached= 0;

      for (const trx_undo_t *undo= UT_LIST_GET_FIRST(rseg.undo_cached); undo;
           undo= UT_LIST_GET_NEXT(undo_list, undo))
      {
        if (head.trx_no < undo->trx_id)
          goto not_free;
        else
          cached+= undo->size;
      }

      ut_ad(rseg.curr_size > cached);
      if (rseg.curr_size > cached + 1)
        goto not_free;

      rseg.latch.rd_unlock();
    }

    ib::info() << "Truncating " << file->name;
    trx_purge_cleanse_purge_queue(space);

    log_free_check();

    mtr_t mtr;
    mtr.start();
    mtr.x_lock_space(&space);

    /* Lock all modified pages of the tablespace.

    During truncation, we do not want any writes to the file.

    If a log checkpoint was completed at LSN earlier than our
    mini-transaction commit and the server was killed, then
    discarding the to-be-trimmed pages without flushing would
    break crash recovery. */
    mysql_mutex_lock(&buf_pool.flush_list_mutex);

    for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list); bpage; )
    {
      ut_ad(bpage->oldest_modification());
      ut_ad(bpage->in_file());

      buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);

      if (bpage->id().space() == space.id &&
          bpage->oldest_modification() != 1)
      {
        ut_ad(bpage->frame);
        auto block= reinterpret_cast<buf_block_t*>(bpage);
        if (!bpage->lock.x_lock_try())
        {
        rescan:
          /* Let buf_pool_t::release_freed_page() proceed. */
          mysql_mutex_unlock(&buf_pool.flush_list_mutex);
          mysql_mutex_lock(&buf_pool.mutex);
          mysql_mutex_lock(&buf_pool.flush_list_mutex);
          mysql_mutex_unlock(&buf_pool.mutex);
          bpage= UT_LIST_GET_LAST(buf_pool.flush_list);
          continue;
        }
        buf_pool.flush_hp.set(prev);
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);

#ifdef BTR_CUR_HASH_ADAPT
        ut_ad(!block->index); /* There is no AHI on undo tablespaces. */
#endif
        bpage->fix();
        ut_ad(!bpage->is_io_fixed());
        mysql_mutex_lock(&buf_pool.flush_list_mutex);

        if (bpage->oldest_modification() > 1)
        {
          bpage->reset_oldest_modification();
          mtr.memo_push(block, MTR_MEMO_PAGE_X_FIX);
        }
        else
        {
          bpage->unfix();
          bpage->lock.x_unlock();
        }

        if (prev != buf_pool.flush_hp.get())
          /* Rescan, because we may have lost the position. */
          goto rescan;
      }

      bpage= prev;
    }

    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    /* Re-initialize tablespace, in a single mini-transaction. */
    const ulint size= SRV_UNDO_TABLESPACE_SIZE_IN_PAGES;

    /* Adjust the tablespace metadata. */
    mysql_mutex_lock(&fil_system.mutex);
    space.set_stopping();
    space.is_being_truncated= true;
    if (space.crypt_data)
    {
      space.reacquire();
      mysql_mutex_unlock(&fil_system.mutex);
      fil_space_crypt_close_tablespace(&space);
      space.release();
    }
    else
      mysql_mutex_unlock(&fil_system.mutex);

    for (auto i= 6000; space.referenced();
         std::this_thread::sleep_for(std::chrono::milliseconds(10)))
    {
      if (!--i)
      {
        mtr.commit();
        ib::error() << "Failed to freeze UNDO tablespace " << file->name;
        return;
      }
    }

    /* Associate the undo tablespace with mtr.
    During mtr::commit_shrink(), InnoDB can use the undo
    tablespace object to clear all freed ranges */
    mtr.set_named_space(&space);
    mtr.trim_pages(page_id_t(space.id, size));
    ut_a(fsp_header_init(&space, size, &mtr) == DB_SUCCESS);
    mysql_mutex_lock(&fil_system.mutex);
    space.size= file->size= size;
    mysql_mutex_unlock(&fil_system.mutex);

    for (auto &rseg : trx_sys.rseg_array)
    {
      if (rseg.space != &space)
        continue;

      ut_ad(!rseg.is_referenced());
      ut_ad(rseg.needs_purge <= head.trx_no);

      buf_block_t *rblock= trx_rseg_header_create(&space,
                                                  &rseg - trx_sys.rseg_array,
                                                  trx_sys.get_max_trx_id(),
                                                  &mtr, &err);
      ut_a(rblock);
      /* These were written by trx_rseg_header_create(). */
      ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                              rblock->page.frame));
      ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_HISTORY_SIZE +
                              rblock->page.frame));
      rseg.reinit(rblock->page.id().page_no());
    }

    mtr.commit_shrink(space);

    /* No mutex; this is only updated by the purge coordinator. */
    export_vars.innodb_undo_truncations++;

    if (purge_sys.rseg && purge_sys.rseg->last_page_no == FIL_NULL)
    {
      /* If purge_sys.rseg is pointing to rseg that was recently
      truncated then move to next rseg element.

      Note: Ideally purge_sys.rseg should be NULL because purge should
      complete processing of all the records but srv_purge_batch_size
      can force the purge loop to exit before all the records are purged. */
      purge_sys.rseg= nullptr;
      purge_sys.next_stored= false;
    }

    DBUG_EXECUTE_IF("ib_undo_trunc", ib::info() << "ib_undo_trunc";
                    log_buffer_flush_to_disk();
                    DBUG_SUICIDE(););

    ib::info() << "Truncated " << file->name;
    purge_sys.truncate.last= purge_sys.truncate.current;
    ut_ad(&space == purge_sys.truncate.current);
    purge_sys.truncate.current= nullptr;
  }
}

/***********************************************************************//**
Updates the last not yet purged history log info in rseg when we have purged
a whole undo log. Advances also purge_sys.purge_trx_no past the purged log. */
static void trx_purge_rseg_get_next_history_log(
	ulint*		n_pages_handled)/*!< in/out: number of UNDO pages
					handled */
{
  fil_addr_t prev_log_addr;
  mtr_t mtr;

  mtr.start();

  purge_sys.rseg->latch.wr_lock(SRW_LOCK_CALL);

  ut_a(purge_sys.rseg->last_page_no != FIL_NULL);

  purge_sys.tail.trx_no= purge_sys.rseg->last_trx_no() + 1;
  purge_sys.tail.undo_no= 0;
  purge_sys.next_stored= false;

  if (const buf_block_t* undo_page=
      buf_page_get_gen(page_id_t(purge_sys.rseg->space->id,
                                 purge_sys.rseg->last_page_no),
                       0, RW_S_LATCH, nullptr,
                       BUF_GET_POSSIBLY_FREED, &mtr))
  {
    const trx_ulogf_t *log_hdr=
      undo_page->page.frame + purge_sys.rseg->last_offset();
    /* Increase the purge page count by one for every handled log */
    ++*n_pages_handled;
    prev_log_addr= flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE);
    prev_log_addr.boffset = static_cast<uint16_t>(prev_log_addr.boffset -
                                                  TRX_UNDO_HISTORY_NODE);
  }
  else
    prev_log_addr.page= FIL_NULL;

  const bool empty= prev_log_addr.page == FIL_NULL;

  if (empty)
    /* No logs left in the history list */
    purge_sys.rseg->last_page_no= FIL_NULL;

  purge_sys.rseg->latch.wr_unlock();
  mtr.commit();

  if (empty)
    return;

  /* Read the previous log header. */
  mtr.start();

  trx_id_t trx_no= 0;

  if (const buf_block_t* undo_page=
      buf_page_get_gen(page_id_t(purge_sys.rseg->space->id, prev_log_addr.page),
                       0, RW_S_LATCH, nullptr, BUF_GET_POSSIBLY_FREED, &mtr))
    trx_no= mach_read_from_8(undo_page->page.frame + prev_log_addr.boffset +
                             TRX_UNDO_TRX_NO);

  mtr.commit();

  if (UNIV_UNLIKELY(!trx_no))
    return;

  purge_sys.rseg->latch.wr_lock(SRW_LOCK_CALL);
  purge_sys.rseg->last_page_no= prev_log_addr.page;
  purge_sys.rseg->set_last_commit(prev_log_addr.boffset, trx_no);

  /* Purge can also produce events, however these are already ordered
  in the rollback segment and any user generated event will be greater
  than the events that Purge produces. ie. Purge can never produce
  events from an empty rollback segment. */

  mysql_mutex_lock(&purge_sys.pq_mutex);
  purge_sys.purge_queue.push(*purge_sys.rseg);
  mysql_mutex_unlock(&purge_sys.pq_mutex);
  purge_sys.rseg->latch.wr_unlock();
}

/** Position the purge sys "iterator" on the undo record to use for purging. */
static void trx_purge_read_undo_rec()
{
	uint16_t	offset;
	uint32_t	page_no;
	ib_uint64_t	undo_no;

	purge_sys.hdr_offset = purge_sys.rseg->last_offset();
	page_no = purge_sys.hdr_page_no = purge_sys.rseg->last_page_no;

	if (purge_sys.rseg->needs_purge) {
		mtr_t		mtr;
		mtr.start();
		const buf_block_t* undo_page;
		if (trx_undo_rec_t* undo_rec = trx_undo_get_first_rec(
			    *purge_sys.rseg->space, purge_sys.hdr_page_no,
			    purge_sys.hdr_offset, RW_S_LATCH,
			    undo_page, &mtr, nullptr)) {

			offset = page_offset(undo_rec);
			undo_no = trx_undo_rec_get_undo_no(undo_rec);
			page_no = undo_page->page.id().page_no();
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
TRANSACTIONAL_TARGET static void trx_purge_choose_next_log()
{
	ut_ad(!purge_sys.next_stored);

	if (purge_sys.rseg_iter.set_next()) {
		trx_purge_read_undo_rec();
	} else {
		/* There is nothing to do yet. */
		std::this_thread::yield();
	}
}

/***********************************************************************//**
Gets the next record to purge and updates the info in the purge system.
@return copy of an undo log record
@retval -1 if there is nothing to purge
@retval nullptr on corruption */
static
trx_undo_rec_t*
trx_purge_get_next_rec(
/*===================*/
	ulint*		n_pages_handled,/*!< in/out: number of UNDO pages
					handled */
	mem_heap_t*	heap)		/*!< in: memory heap where copied */
{
	mtr_t		mtr;

	ut_ad(purge_sys.next_stored);
	ut_ad(purge_sys.tail.trx_no < purge_sys.low_limit_no());

	const page_id_t page_id{purge_sys.rseg->space->id, purge_sys.page_no};
	const uint16_t offset = purge_sys.offset;

	if (offset == 0) {
		/* It is the dummy undo log record, which means that there is
		no need to purge this undo log */

		trx_purge_rseg_get_next_history_log(n_pages_handled);

		/* Look for the next undo log and record to purge */

		trx_purge_choose_next_log();
		return reinterpret_cast<trx_undo_rec_t*>(-1);
	}

	mtr.start();

	const buf_block_t* undo_page
		= buf_page_get_gen(page_id, 0, RW_S_LATCH, nullptr,
				   BUF_GET_POSSIBLY_FREED, &mtr);
	if (UNIV_UNLIKELY(!undo_page)) {
corrupted:
		mtr.commit();
		return nullptr;
	}

	const buf_block_t* rec2_page = undo_page;

	const trx_undo_rec_t* rec2 = trx_undo_page_get_next_rec(
		undo_page, offset, purge_sys.hdr_page_no, purge_sys.hdr_offset);

	if (rec2 == NULL) {
		rec2 = trx_undo_get_next_rec(rec2_page, offset,
					     purge_sys.hdr_page_no,
					     purge_sys.hdr_offset, &mtr);
	}

	if (rec2 == NULL) {
		mtr_commit(&mtr);

		trx_purge_rseg_get_next_history_log(n_pages_handled);

		/* Look for the next undo log and record to purge */

		trx_purge_choose_next_log();

		mtr_start(&mtr);

		undo_page = buf_page_get_gen(page_id, 0, RW_S_LATCH,
					     nullptr, BUF_GET_POSSIBLY_FREED,
					     &mtr);
		if (UNIV_UNLIKELY(!undo_page)) {
			goto corrupted;
		}
	} else {
		purge_sys.offset = page_offset(rec2);
		purge_sys.page_no = rec2_page->page.id().page_no();
		purge_sys.tail.undo_no = trx_undo_rec_get_undo_no(rec2);

		if (undo_page != rec2_page) {
			/* We advance to a new page of the undo log: */
			(*n_pages_handled)++;
		}
	}

	trx_undo_rec_t*	rec_copy = trx_undo_rec_copy(undo_page->page.frame
						     + offset, heap);

	mtr.commit();
	return rec_copy;
}

/********************************************************************//**
Fetches the next undo log record from the history list to purge. It must be
released with the corresponding release function.
@return copy of an undo log record
@retval -1 if the whole undo log can skipped in purge
@retval nullptr if nothing is left, or on corruption */
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
			return nullptr;
		}
	}

	if (purge_sys.tail.trx_no >= purge_sys.low_limit_no()) {
		return nullptr;
	}

	/* fprintf(stderr, "Thread %lu purging trx %llu undo record %llu\n",
	pthread_self(), iter->trx_no, iter->undo_no); */

	*roll_ptr = trx_undo_build_roll_ptr(
		/* row_purge_record_func() will later set
		ROLL_PTR_INSERT_FLAG for TRX_UNDO_INSERT_REC */
		false,
		trx_sys.rseg_id(purge_sys.rseg, true),
		purge_sys.page_no, purge_sys.offset);

	/* The following call will advance the stored values of the
	purge iterator. */

	return trx_purge_get_next_rec(n_pages_handled, heap);
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
		ut_ad(node->undo_recs.empty());
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

	std::unordered_map<table_id_t, purge_node_t*> table_id_map;
	mem_heap_empty(purge_sys.heap);

	while (UNIV_LIKELY(srv_undo_sources) || !srv_fast_shutdown) {
		purge_node_t*		node;
		trx_purge_rec_t		purge_rec;

		/* Get the purge node. */
		node = (purge_node_t*) thr->child;
		ut_a(que_node_get_type(node) == QUE_NODE_PURGE);

		/* Track the max {trx_id, undo_no} for truncating the
		UNDO logs once we have purged the records. */

		if (purge_sys.head <= purge_sys.tail) {
			purge_sys.head = purge_sys.tail;
		}

		/* Fetch the next record, and advance the purge_sys.tail. */
		purge_rec.undo_rec = trx_purge_fetch_next_rec(
			&purge_rec.roll_ptr, &n_pages_handled,
			purge_sys.heap);

		if (purge_rec.undo_rec == NULL) {
			break;
		} else if (purge_rec.undo_rec
			   == reinterpret_cast<trx_undo_rec_t*>(-1)) {
			continue;
		}

		table_id_t table_id = trx_undo_rec_get_table_id(
			purge_rec.undo_rec);

		purge_node_t *& table_node = table_id_map[table_id];

		if (table_node) {
			node = table_node;
		} else {
			thr = UT_LIST_GET_NEXT(thrs, thr);

			if (!(++i % n_purge_threads)) {
				thr = UT_LIST_GET_FIRST(
					purge_sys.query->thrs);
			}

			ut_a(thr != NULL);
			table_node = node;
		}

		node->undo_recs.push(purge_rec);

		if (n_pages_handled >= srv_purge_batch_size) {
			break;
		}
	}

	ut_ad(purge_sys.head <= purge_sys.tail);

	return(n_pages_handled);
}

extern tpool::waitable_task purge_worker_task;

/** Wait for pending purge jobs to complete. */
static void trx_purge_wait_for_workers_to_complete()
{
  const bool notify_wait{purge_worker_task.is_running()};

  if (notify_wait)
    tpool::tpool_wait_begin();

  purge_worker_task.wait();

  if (notify_wait)
    tpool::tpool_wait_end();

  /* There should be no outstanding tasks as long
  as the worker threads are active. */
  ut_ad(srv_get_task_queue_length() == 0);
}

/** Update end_view at the end of a purge batch. */
TRANSACTIONAL_INLINE void purge_sys_t::clone_end_view()
{
  /* This is only invoked only by the purge coordinator,
  which is the only thread that can modify our inputs head, tail, view.
  Therefore, we only need to protect end_view from concurrent reads. */

  /* Limit the end_view similar to what trx_purge_truncate_history() does. */
  const trx_id_t trx_no= head.trx_no ? head.trx_no : tail.trx_no;
#ifdef SUX_LOCK_GENERIC
  end_latch.wr_lock();
#else
  transactional_lock_guard<srw_spin_lock_low> g(end_latch);
#endif
  end_view= view;
  end_view.clamp_low_limit_id(trx_no);
#ifdef SUX_LOCK_GENERIC
  end_latch.wr_unlock();
#endif
}

/**
Run a purge batch.
@param n_tasks       number of purge tasks to submit to the queue
@param history_size  trx_sys.history_size()
@param truncate      whether to truncate the history at the end of the batch
@return number of undo log pages handled in the batch */
TRANSACTIONAL_TARGET
ulint trx_purge(ulint n_tasks, ulint history_size, bool truncate)
{
	que_thr_t*	thr = NULL;
	ulint		n_pages_handled;

	ut_ad(n_tasks > 0);

	purge_sys.clone_oldest_view();

#ifdef UNIV_DEBUG
	if (srv_purge_view_update_only_debug) {
		return(0);
	}
#endif /* UNIV_DEBUG */

	/* Fetch the UNDO recs that need to be purged. */
	n_pages_handled = trx_purge_attach_undo_recs(n_tasks);

	{
		ulint delay = n_pages_handled ? srv_max_purge_lag : 0;
		if (UNIV_UNLIKELY(delay)) {
			if (delay >= history_size) {
		no_throttle:
				delay = 0;
			} else if (const ulint max_delay =
				   srv_max_purge_lag_delay) {
				delay = std::min(max_delay,
						 10000 * history_size / delay
						 - 5000);
			} else {
				goto no_throttle;
			}
		}
		srv_dml_needed_delay = delay;
	}

	/* Submit tasks to workers queue if using multi-threaded purge. */
	for (ulint i = n_tasks; --i; ) {
		thr = que_fork_scheduler_round_robin(purge_sys.query, thr);
		ut_a(thr);
		srv_que_task_enqueue_low(thr);
		srv_thread_pool->submit_task(&purge_worker_task);
	}

	thr = que_fork_scheduler_round_robin(purge_sys.query, thr);

	que_run_threads(thr);

	trx_purge_wait_for_workers_to_complete();

	purge_sys.clone_end_view();

	if (truncate) {
		trx_purge_truncate_history();
	}

	MONITOR_INC_VALUE(MONITOR_PURGE_INVOKED, 1);
	MONITOR_INC_VALUE(MONITOR_PURGE_N_PAGE_HANDLED, n_pages_handled);

	return(n_pages_handled);
}
