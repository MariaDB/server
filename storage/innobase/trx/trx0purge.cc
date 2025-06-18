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
#include "dict0load.h"
#include <mysql/service_thd_mdl.h>
#include <mysql/service_wsrep.h>
#include "log.h"

/** Maximum allowable purge history length.  <=0 means 'infinite'. */
ulong		srv_max_purge_lag = 0;

/** Max DML user threads delay in micro-seconds. */
ulong		srv_max_purge_lag_delay = 0;

/** The global data structure coordinating a purge */
purge_sys_t	purge_sys;

#ifdef UNIV_DEBUG
my_bool		srv_purge_view_update_only_debug;
#endif /* UNIV_DEBUG */

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
  ut_ad(!m_initialized);
  ut_ad(!enabled());
  ut_ad(!m_active);
  /* If innodb_undo_tablespaces>0, the rollback segment 0
  (which always resides in the system tablespace) will
  never be used; @see trx_assign_rseg_low() */
  skipped_rseg= srv_undo_tablespaces > 0;
  m_paused= 0;
  query= purge_graph_build();
  next_stored= false;
  rseg= nullptr;
  page_no= 0;
  offset= 0;
  hdr_page_no= 0;
  hdr_offset= 0;
  latch.SRW_LOCK_INIT(trx_purge_latch_key);
  end_latch.init();
  mysql_mutex_init(purge_sys_pq_mutex_key, &pq_mutex, nullptr);
  truncate_undo_space.current= nullptr;
  truncate_undo_space.last= 0;
  m_initialized= true;
}

/** Close the purge subsystem on shutdown. */
void purge_sys_t::close()
{
  ut_ad(this == &purge_sys);
  if (!m_initialized)
    return;

  ut_ad(!enabled());
  trx_t *trx= query->trx;
  que_graph_free(query);
  ut_ad(!trx->id);
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  trx->state= TRX_STATE_NOT_STARTED;
  trx->free();
  latch.destroy();
  end_latch.destroy();
  mysql_mutex_destroy(&pq_mutex);
  m_initialized= false;
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
  ut_ad(undo->id < TRX_RSEG_N_SLOTS);
  ut_ad(undo == trx->rsegs.m_redo.undo);
  trx_rseg_t *rseg= trx->rsegs.m_redo.rseg;
  ut_ad(undo->rseg == rseg);
  buf_block_t *rseg_header= rseg->get(mtr, nullptr);
  /* We are in transaction commit; we cannot return an error. If the
  database is corrupted, it is better to crash it than to
  intentionally violate ACID by committing something that is known to
  be corrupted. */
  ut_ad(rseg_header);
  buf_block_t *undo_page=
    buf_page_get(page_id_t(rseg->space->id, undo->hdr_page_no), 0,
                 RW_X_LATCH, mtr);
  /* This function is invoked during transaction commit, which is not
  allowed to fail. If we get a corrupted undo header, we will crash here. */
  ut_a(undo_page);
  const uint16_t undo_header_offset= undo->hdr_offset;
  trx_ulogf_t *undo_header= undo_page->page.frame + undo_header_offset;

  ut_ad(rseg->needs_purge > trx->id);
  ut_ad(rseg->last_page_no != FIL_NULL);

  rseg->history_size++;

  if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                                     rseg_header->page.frame)))
    /* This database must have been upgraded from before MariaDB 10.3.5. */
    trx_rseg_format_upgrade(rseg_header, mtr);

  uint16_t undo_state;

  if (undo->size == 1 &&
      TRX_UNDO_PAGE_REUSE_LIMIT >
      mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE +
                       undo_page->page.frame))
  {
    undo->state= undo_state= TRX_UNDO_CACHED;
    UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
  }
  else
  {
    ut_ad(undo->size == flst_get_len(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST +
                                     undo_page->page.frame));
    /* The undo log segment will not be reused */
    static_assert(FIL_NULL == 0xffffffff, "");
    mtr->memset(rseg_header, TRX_RSEG + TRX_RSEG_UNDO_SLOTS +
                undo->id * TRX_RSEG_SLOT_SIZE, 4, 0xff);
    uint32_t hist_size= mach_read_from_4(TRX_RSEG_HISTORY_SIZE + TRX_RSEG +
                                         rseg_header->page.frame);
    mtr->write<4>(*rseg_header, TRX_RSEG + TRX_RSEG_HISTORY_SIZE +
                  rseg_header->page.frame, hist_size + undo->size);
    mtr->write<8>(*rseg_header, TRX_RSEG + TRX_RSEG_MAX_TRX_ID +
                  rseg_header->page.frame, trx_sys.get_max_trx_id());
    ut_free(undo);
    undo_state= TRX_UNDO_TO_PURGE;
  }

  undo= nullptr;

  /*
  Before any transaction-generating background threads or the purge
  have been started, we can start transactions in
  row_merge_drop_temp_indexes(), and roll back recovered transactions.

  Arbitrary user transactions may be executed when all the undo log
  related background processes (including purge) are disabled due to
  innodb_force_recovery=2 or innodb_force_recovery=3.  DROP TABLE may
  be executed at any innodb_force_recovery level.

  During fast shutdown, we may also continue to execute user
  transactions. */
  ut_ad(srv_undo_sources || srv_fast_shutdown ||
        (!purge_sys.enabled() &&
         (srv_is_being_started ||
          srv_force_recovery >= SRV_FORCE_NO_BACKGROUND)));

#ifdef WITH_WSREP
  if (wsrep_is_wsrep_xid(&trx->xid))
    trx_rseg_update_wsrep_checkpoint(rseg_header, &trx->xid, mtr);
#endif

  if (trx->mysql_log_file_name && *trx->mysql_log_file_name)
    /* Update the latest binlog name and offset if log_bin=ON or this
    is a replica. */
    trx_rseg_update_binlog_offset(rseg_header, trx->mysql_log_file_name,
                                  trx->mysql_log_offset, mtr);

  /* Add the log as the first in the history list */

  /* We are in transaction commit; we cannot return an error
  when detecting corruption. It is better to crash the server
  than to intentionally violate ACID by committing something
  that is known to be corrupted. */
  ut_a(flst_add_first(rseg_header, TRX_RSEG + TRX_RSEG_HISTORY, undo_page,
                      uint16_t(undo_header_offset + TRX_UNDO_HISTORY_NODE),
                      rseg->space->free_limit, mtr) == DB_SUCCESS);

  mtr->write<2>(*undo_page, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE +
                undo_page->page.frame, undo_state);
  mtr->write<8,mtr_t::MAYBE_NOP>(*undo_page, undo_header + TRX_UNDO_TRX_NO,
                                 trx->rw_trx_hash_element->no);
}

/** Free an undo log segment.
@param rseg_hdr  rollback segment header page
@param block     undo segment header page
@param mtr       mini-transaction */
static void trx_purge_free_segment(buf_block_t *rseg_hdr, buf_block_t *block,
                                   mtr_t &mtr)
{
  ut_ad(mtr.memo_contains_flagged(rseg_hdr, MTR_MEMO_PAGE_X_FIX));
  ut_ad(mtr.memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));

  while (!fseg_free_step_not_header(block,
				    TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
				    &mtr))
  {
    rseg_hdr->fix();
    block->fix();
    ut_d(const page_id_t rseg_hdr_id{rseg_hdr->page.id()});
    ut_d(const page_id_t id{block->page.id()});
    mtr.commit();
    /* NOTE: If the server is killed after the log that was produced
    up to this point was written, and before the log from the mtr.commit()
    in our caller is written, then the pages belonging to the
    undo log will become unaccessible garbage.

    This does not matter when using multiple innodb_undo_tablespaces;
    innodb_undo_log_truncate=ON will be able to reclaim the space. */
    mtr.start();
    rseg_hdr->page.lock.x_lock();
    ut_ad(rseg_hdr->page.id() == rseg_hdr_id);
    block->page.lock.x_lock();
    ut_ad(block->page.id() == id);
    mtr.memo_push(rseg_hdr, MTR_MEMO_PAGE_X_FIX);
    mtr.memo_push(block, MTR_MEMO_PAGE_X_FIX);
  }

  while (!fseg_free_step(block, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
                         &mtr));
}

void purge_sys_t::rseg_enable(trx_rseg_t &rseg)
{
  ut_ad(this == &purge_sys);
  ut_ad(rseg.latch.have_wr());
  uint8_t skipped= skipped_rseg;
  ut_ad(skipped < TRX_SYS_N_RSEGS);
  if (&rseg == &trx_sys.rseg_array[skipped])
  {
    /* If this rollback segment is subject to innodb_undo_log_truncate=ON,
    we must not clear the flag. But we will advance purge_sys.skipped_rseg
    to be able to choose another candidate for this soft truncation, and
    to prevent the following scenario:

    (1) purge_sys_t::iterator::free_history_rseg() had invoked
    rseg.set_skip_allocation()
    (2) undo log truncation had completed on this rollback segment
    (3) SET GLOBAL innodb_undo_log_truncate=OFF
    (4) purge_sys_t::iterator::free_history_rseg() would not be able to
    invoke rseg.set_skip_allocation() on any other rollback segment
    before this rseg has grown enough */
    if (truncate_undo_space.current != rseg.space)
      rseg.clear_skip_allocation();
    skipped++;
    /* If innodb_undo_tablespaces>0, the rollback segment 0
    (which always resides in the system tablespace) will
    never be used; @see trx_assign_rseg_low() */
    if (!(skipped&= (TRX_SYS_N_RSEGS - 1)) && srv_undo_tablespaces)
      skipped++;
    skipped_rseg= skipped;
  }
}

/** Remove unnecessary history data from a rollback segment.
@param rseg   rollback segment
@param limit  truncate anything before this
@return error code */
inline dberr_t purge_sys_t::iterator::free_history_rseg(trx_rseg_t &rseg) const
{
  fil_addr_t hdr_addr;
  mtr_t mtr;
  bool freed= false;
  uint32_t rseg_ref= 0;
  const auto last_boffset= srv_page_size - TRX_UNDO_LOG_OLD_HDR_SIZE;
  /* Technically, rseg.space->free_limit is not protected by
  rseg.latch, which we are holding, but rseg.space->latch. The value
  that we are reading may become stale (too small) if other pages are
  being allocated in this tablespace, for other rollback
  segments. Nothing can be added to this rseg without holding
  rseg.latch, and hence we can validate the entire file-based list
  against the limit that we are reading here.

  Note: The read here may look like a data race. On none of our target
  architectures this should be an actual problem, because the uint32_t
  value should always fit in a register and be correctly aligned. */
  const auto last_page= rseg.space->free_limit;

  mtr.start();

  dberr_t err;
  buf_block_t *rseg_hdr= rseg.get(&mtr, &err);
  if (!rseg_hdr)
  {
func_exit:
    mtr.commit();
    if (freed && (rseg.SKIP & rseg_ref))
      purge_sys.rseg_enable(rseg);
    return err;
  }

  hdr_addr= flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + rseg_hdr->page.frame);

  if (hdr_addr.page == FIL_NULL)
    goto func_exit;

  if (hdr_addr.page >= last_page ||
      hdr_addr.boffset < TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE ||
      hdr_addr.boffset >= last_boffset)
  {
  corrupted:
    err= DB_CORRUPTION;
    goto func_exit;
  }

  hdr_addr.boffset= static_cast<uint16_t>(hdr_addr.boffset -
                                          TRX_UNDO_HISTORY_NODE);

loop:
  buf_block_t *b=
    buf_page_get_gen(page_id_t(rseg.space->id, hdr_addr.page),
                     0, RW_X_LATCH, nullptr, BUF_GET_POSSIBLY_FREED,
                     &mtr, &err);
  if (!b)
    goto func_exit;

  const trx_id_t undo_trx_no=
    mach_read_from_8(b->page.frame + hdr_addr.boffset + TRX_UNDO_TRX_NO);

  if (undo_trx_no >= trx_no)
  {
    if (undo_trx_no == trx_no)
      err= trx_undo_truncate_start(&rseg, hdr_addr.page,
                                   hdr_addr.boffset, undo_no);
    goto func_exit;
  }
  else
  {
    rseg_ref= rseg.ref_load();
    if (rseg_ref >= rseg.REF || !purge_sys.sees(rseg.needs_purge))
    {
      /* We cannot clear this entire rseg because trx_assign_rseg_low()
      has already chosen it for a future trx_undo_assign(), or
      because some recently started transaction needs purging.

      If this invocation could not reduce rseg.history_size at all
      (!freed), we will try to ensure progress and prevent our
      starvation by disabling one rollback segment for future
      trx_assign_rseg_low() invocations until a future invocation has
      made progress and invoked purge_sys_t::rseg_enable(rseg) on that
      rollback segment. */

      if (!(rseg.SKIP & rseg_ref) && !freed &&
          ut_d(!trx_rseg_n_slots_debug &&)
          &rseg == &trx_sys.rseg_array[purge_sys.skipped_rseg])
        /* If rseg.space == purge_sys.truncate_undo_space.current
        the following will be a no-op. A possible conflict
        with innodb_undo_log_truncate=ON will be handled in
        purge_sys_t::rseg_enable(). */
        rseg.set_skip_allocation();
      goto func_exit;
    }
  }

  fil_addr_t prev_hdr_addr=
    flst_get_prev_addr(b->page.frame + hdr_addr.boffset +
                       TRX_UNDO_HISTORY_NODE);
  if (prev_hdr_addr.page == FIL_NULL);
  else if (prev_hdr_addr.page >= last_page ||
           prev_hdr_addr.boffset < TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE ||
           prev_hdr_addr.boffset >= last_boffset)
    goto corrupted;

  prev_hdr_addr.boffset= static_cast<uint16_t>(prev_hdr_addr.boffset -
                                               TRX_UNDO_HISTORY_NODE);

  err= flst_remove(rseg_hdr, TRX_RSEG + TRX_RSEG_HISTORY, b,
                   uint16_t(hdr_addr.boffset + TRX_UNDO_HISTORY_NODE),
                   last_page, &mtr);
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
      DBUG_EXECUTE_IF("undo_segment_leak", goto skip_purge_free;);
      trx_purge_free_segment(rseg_hdr, b, mtr);
      break;
    case TRX_UNDO_CACHED:
      /* rseg.undo_cached must point to this page */
      trx_undo_t *undo= UT_LIST_GET_FIRST(rseg.undo_cached);
      for (; undo; undo= UT_LIST_GET_NEXT(undo_list, undo))
        if (undo->hdr_page_no == hdr_addr.page)
          goto found_cached;
      ut_ad("inconsistent undo logs" == 0);
    found_cached:
      static_assert(FIL_NULL == 0xffffffff, "");
      if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                                         rseg_hdr->page.frame)))
        trx_rseg_format_upgrade(rseg_hdr, &mtr);
      DBUG_EXECUTE_IF("skip_cached_undo", goto skip_purge_free;);
      if (UNIV_LIKELY(undo != nullptr))
      {
        UT_LIST_REMOVE(rseg.undo_cached, undo);
        mtr.memset(rseg_hdr, TRX_RSEG + TRX_RSEG_UNDO_SLOTS +
                   undo->id * TRX_RSEG_SLOT_SIZE, 4, 0xff);
        ut_free(undo);
      }
      mtr.write<8,mtr_t::MAYBE_NOP>(*rseg_hdr, TRX_RSEG + TRX_RSEG_MAX_TRX_ID +
                                    rseg_hdr->page.frame,
                                    trx_sys.get_max_trx_id() - 1);
      goto free_segment;
    }
  }
#ifndef DBUG_OFF
skip_purge_free:
#endif /* !DBUG_OFF */
  hdr_addr= prev_hdr_addr;

  mtr.commit();
  ut_ad(rseg.history_size > 0);
  rseg.history_size--;
  freed= true;
  mtr.start();
  rseg_hdr->page.lock.x_lock();
  ut_ad(rseg_hdr->page.id() == rseg.page_id());
  mtr.memo_push(rseg_hdr, MTR_MEMO_PAGE_X_FIX);

  if (hdr_addr.page == FIL_NULL)
    goto func_exit;

  goto loop;
}

void purge_sys_t::cleanse_purge_queue(const fil_space_t &space)
{
  mysql_mutex_lock(&pq_mutex);
  auto purge_elem_list= clone_queue_container();
  purge_queue.clear();
  for (auto elem : purge_elem_list)
    if (purge_queue::rseg(elem)->space != &space)
      purge_queue.push_trx_no_rseg(elem);
  mysql_mutex_unlock(&pq_mutex);
}

dberr_t purge_sys_t::iterator::free_history() const
{
  for (auto &rseg : trx_sys.rseg_array)
    if (rseg.space)
    {
      ut_ad(rseg.is_persistent());
      log_free_check();
      rseg.latch.wr_lock(SRW_LOCK_CALL);
      dberr_t err= free_history_rseg(rseg);
      rseg.latch.wr_unlock();
      if (err)
        return err;
    }
  return DB_SUCCESS;
}

inline void trx_sys_t::undo_truncate_start(fil_space_t &space)
{
  ut_ad(this == &trx_sys);
  /* Undo tablespace always are a single file. */
  ut_a(UT_LIST_GET_LEN(space.chain) == 1);
  fil_node_t *file= UT_LIST_GET_FIRST(space.chain);
  /* The undo tablespace files are never closed. */
  ut_ad(file->is_open());
  sql_print_information("InnoDB: Starting to truncate %s", file->name);

  for (auto &rseg : rseg_array)
    if (rseg.space == &space)
    {
      /* Prevent a race with purge_sys_t::iterator::free_history_rseg() */
      rseg.latch.rd_lock(SRW_LOCK_CALL);
      /* Once set, this rseg will not be allocated to subsequent
      transactions, but we will wait for existing active
      transactions to finish. */
      rseg.set_skip_allocation();
      rseg.latch.rd_unlock();
    }
}

inline fil_space_t *purge_sys_t::undo_truncate_try(uint32_t id, uint32_t size)
{
  ut_ad(srv_is_undo_tablespace(id));
  fil_space_t *space= fil_space_get(id);
  if (space && space->get_size() > size)
  {
    truncate_undo_space.current= space;
    trx_sys.undo_truncate_start(*space);
    return space;
  }
  return nullptr;
}

fil_space_t *purge_sys_t::truncating_tablespace()
{
  ut_ad(this == &purge_sys);

  fil_space_t *space= truncate_undo_space.current;
  if (space || srv_undo_tablespaces_active < 2 || !srv_undo_log_truncate)
    return space;

  const uint32_t size=
    uint32_t(std::min(ulonglong{std::numeric_limits<uint32_t>::max()},
                      srv_max_undo_log_size >> srv_page_size_shift));
  for (uint32_t i= truncate_undo_space.last, j= i;; )
  {
    if (fil_space_t *s= undo_truncate_try(srv_undo_space_id_start + i, size))
      return s;
    ++i;
    i%= srv_undo_tablespaces_active;
    if (i == j)
      return nullptr;
  }
}

#if defined __GNUC__ && __GNUC__ == 4 && !defined __clang__
# if defined __arm__ || defined __aarch64__
/* Work around an internal compiler error in GCC 4.8.5 */
__attribute__((optimize(0)))
# endif
#endif
/**
Remove unnecessary history data from rollback segments. NOTE that when this
function is called, the caller
(purge_coordinator_callback or purge_truncation_callback)
must not have any latches on undo log pages!
*/
TRANSACTIONAL_TARGET void trx_purge_truncate_history()
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

  if (head.free_history() != DB_SUCCESS)
    return;

  while (fil_space_t *space= purge_sys.truncating_tablespace())
  {
    for (auto &rseg : trx_sys.rseg_array)
    {
      if (rseg.space != space)
        continue;

      rseg.latch.rd_lock(SRW_LOCK_CALL);
      ut_ad(rseg.skip_allocation());
      if (rseg.is_referenced() || !purge_sys.sees(rseg.needs_purge))
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
        if (head.trx_no && head.trx_no < undo->trx_id)
          goto not_free;
        else
          cached+= undo->size;
      }

      ut_ad(rseg.curr_size > cached);
      if (rseg.curr_size > cached + 1 &&
          (rseg.history_size || srv_fast_shutdown || srv_undo_sources))
        goto not_free;

      rseg.latch.rd_unlock();
    }

    const char *file_name= UT_LIST_GET_FIRST(space->chain)->name;
    sql_print_information("InnoDB: Truncating %s", file_name);
    purge_sys.cleanse_purge_queue(*space);

    /* Lock all modified pages of the tablespace.

    During truncation, we do not want any writes to the file.

    If a log checkpoint was completed at LSN earlier than our
    mini-transaction commit and the server was killed, then
    discarding the to-be-trimmed pages without flushing would
    break crash recovery. */

    if (UNIV_UNLIKELY(srv_shutdown_state != SRV_SHUTDOWN_NONE) &&
        srv_fast_shutdown)
      return;

    /* Adjust the tablespace metadata. */
    mysql_mutex_lock(&fil_system.mutex);
    if (space->crypt_data)
    {
      space->reacquire();
      mysql_mutex_unlock(&fil_system.mutex);
      fil_space_crypt_close_tablespace(space);
      space->release();
    }
    else
      mysql_mutex_unlock(&fil_system.mutex);

    /* Re-initialize tablespace, in a single mini-transaction. */
    const uint32_t size= SRV_UNDO_TABLESPACE_SIZE_IN_PAGES;

    log_free_check();

    mtr_t mtr;
    mtr.start();
    mtr.x_lock_space(space);
    /* Associate the undo tablespace with mtr.
    During mtr::commit_shrink(), InnoDB can use the undo
    tablespace object to clear all freed ranges */
    mtr.set_named_space(space);
    mtr.trim_pages(page_id_t(space->id, size));
    ut_a(fsp_header_init(space, size, &mtr) == DB_SUCCESS);

    for (auto &rseg : trx_sys.rseg_array)
    {
      if (rseg.space != space)
        continue;

      ut_ad(!rseg.is_referenced());
      /* We may actually have rseg.needs_purge > head.trx_no here
      if trx_t::commit_empty() had been executed in the past,
      possibly before this server had been started up. */

      dberr_t err;
      buf_block_t *rblock= trx_rseg_header_create(space,
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

    mtr.commit_shrink(*space, size);

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

    DBUG_EXECUTE_IF("ib_undo_trunc",
                    sql_print_information("InnoDB: ib_undo_trunc");
                    log_buffer_flush_to_disk();
                    DBUG_SUICIDE(););

    sql_print_information("InnoDB: Truncated %s", file_name);
    ut_ad(space == purge_sys.truncate_undo_space.current);
    purge_sys.truncate_undo_space.current= nullptr;
    purge_sys.truncate_undo_space.last= space->id - srv_undo_space_id_start;
  }
}

buf_block_t *purge_sys_t::get_page(page_id_t id)
{
  ut_ad(!recv_sys.recovery_on);

  buf_block_t *&h= pages[id];
  buf_block_t *undo_page= h;

  if (!undo_page)
  {
    undo_page= buf_pool.page_fix(id); // batch_cleanup() will unfix()
    if (!undo_page)
      pages.erase(id);
    else
      h= undo_page;
  }

  return undo_page;
}

bool purge_sys_t::rseg_get_next_history_log()
{
  fil_addr_t prev_log_addr;

  ut_ad(rseg->latch.have_wr());
  ut_a(rseg->last_page_no != FIL_NULL);

  tail.trx_no= rseg->last_trx_no() + 1;
  tail.undo_no= 0;
  next_stored= false;

  if (buf_block_t *undo_page=
      get_page(page_id_t(rseg->space->id, rseg->last_page_no)))
  {
    const byte *log_hdr= undo_page->page.frame + rseg->last_offset();
    prev_log_addr= flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE);
    if (prev_log_addr.boffset < TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE ||
        prev_log_addr.boffset >= srv_page_size - TRX_UNDO_LOG_OLD_HDR_SIZE)
      goto corrupted;
    prev_log_addr.boffset = static_cast<uint16_t>(prev_log_addr.boffset -
                                                  TRX_UNDO_HISTORY_NODE);
  }
  else
    goto corrupted;

  if (prev_log_addr.page >= rseg->space->free_limit)
  corrupted:
    rseg->last_page_no= FIL_NULL;
  else
  {
    /* Read the previous log header. */
    trx_id_t trx_no= 0;
    if (const buf_block_t* undo_page=
        get_page(page_id_t(rseg->space->id, prev_log_addr.page)))
    {
      const byte *log_hdr= undo_page->page.frame + prev_log_addr.boffset;
      trx_no= mach_read_from_8(log_hdr + TRX_UNDO_TRX_NO);
    }

    if (UNIV_LIKELY(trx_no != 0))
    {
      rseg->last_page_no= prev_log_addr.page;
      rseg->set_last_commit(prev_log_addr.boffset, trx_no);

      /* Purge can also produce events, however these are already
      ordered in the rollback segment and any user generated event
      will be greater than the events that Purge produces. ie. Purge
      can never produce events from an empty rollback segment. */

      mysql_mutex_lock(&pq_mutex);
      enqueue(*rseg);
      mysql_mutex_unlock(&pq_mutex);
    }
  }

  rseg->latch.wr_unlock();
  return choose_next_log();
}

/** Position the purge sys "iterator" on the undo record to use for purging.
@retval false when nothing is to be purged
@retval true  when purge_sys.rseg->latch was locked */
bool purge_sys_t::choose_next_log()
{
  ut_ad(!next_stored);

  mysql_mutex_lock(&pq_mutex);
  if (purge_queue.empty()) {
    rseg = nullptr;
    mysql_mutex_unlock(&purge_sys.pq_mutex);
    return false;
  }
  rseg= purge_queue.pop();
  mysql_mutex_unlock(&purge_sys.pq_mutex);

  /* We assume in purge of externally stored fields that space
  id is in the range of UNDO tablespace space ids */
  ut_ad(rseg->space == fil_system.sys_space ||
        srv_is_undo_tablespace(rseg->space->id));

  rseg->latch.wr_lock(SRW_LOCK_CALL);
  trx_id_t last_trx_no = rseg->last_trx_no();
  hdr_offset = rseg->last_offset();
  hdr_page_no = rseg->last_page_no;

  /* Only the purge_coordinator_task will access this any of
  purge_sys.hdr_page_no, purge_sys.tail. The field purge_sys.head and
  purge_sys.view are modified by clone_end_view() in the
  purge_coordinator_task while holding exclusive purge_sys.latch. The
  purge_sys.view may also be modified by wake_if_not_active() while holding
  exclusive purge_sys.latch. The purge_sys.head may be read by
  purge_truncation_callback(). */
  ut_a(hdr_page_no != FIL_NULL);
  ut_a(tail.trx_no <= last_trx_no);
  tail.trx_no = last_trx_no;

  if (!rseg->needs_purge)
  {
  purge_nothing:
    page_no= hdr_page_no;
    offset= 0;
    tail.undo_no= 0;
  }
  else
  {
    page_id_t id{rseg->space->id, hdr_page_no};
    buf_block_t *b= get_page(id);
    if (!b)
      goto purge_nothing;
    const trx_undo_rec_t *undo_rec=
      trx_undo_page_get_first_rec(b, hdr_page_no, hdr_offset);
    if (!undo_rec)
    {
      if (mach_read_from_2(b->page.frame + hdr_offset + TRX_UNDO_NEXT_LOG))
        goto purge_nothing;
      const uint32_t next=
        mach_read_from_4(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE +
                         FLST_NEXT + FIL_ADDR_PAGE + b->page.frame);
      if (next == FIL_NULL)
        goto purge_nothing;
      id.set_page_no(next);
      b= get_page(id);
      if (!b)
        goto purge_nothing;
      undo_rec=
        trx_undo_page_get_first_rec(b, hdr_page_no, hdr_offset);
      if (!undo_rec)
        goto purge_nothing;
    }

    offset= uint16_t(undo_rec - b->page.frame);
    ut_ad(undo_rec - b->page.frame == page_offset(undo_rec));
    tail.undo_no= trx_undo_rec_get_undo_no(undo_rec);
    page_no= id.page_no();
  }

  next_stored= true;
  return true;
}

/**
Get the next record to purge and update the info in the purge system.
@param roll_ptr           undo log pointer to the record
@return buffer-fixed reference to undo log record
@retval {nullptr,1} if the whole undo log can skipped in purge
@retval {nullptr,0} if nothing is left, or on corruption */
inline trx_purge_rec_t purge_sys_t::get_next_rec(roll_ptr_t roll_ptr)
{
  ut_ad(next_stored);
  ut_ad(tail.trx_no < low_limit_no());
  ut_ad(rseg->latch.have_wr());

  if (!offset)
  {
    /* It is the dummy undo log record, which means that there is no need to
    purge this undo log. Look for the next undo log and record to purge */
    if (rseg_get_next_history_log())
      rseg->latch.wr_unlock();
    return {nullptr, 1};
  }

  ut_ad(offset == uint16_t(roll_ptr));

  page_id_t page_id{rseg->space->id, page_no};
  bool locked= true;
  buf_block_t *b= get_page(page_id);
  if (UNIV_UNLIKELY(!b))
  {
    if (locked)
      rseg->latch.wr_unlock();
    return {nullptr, 0};
  }

  buf_block_t *rec2_page= b;
  if (const trx_undo_rec_t *rec2=
      trx_undo_page_get_next_rec(b, offset, hdr_page_no, hdr_offset))
  {
  got_rec:
    ut_ad(page_no == page_id.page_no());
    ut_ad(page_offset(rec2) == rec2 - rec2_page->page.frame);
    offset= uint16_t(rec2 - rec2_page->page.frame);
    tail.undo_no= trx_undo_rec_get_undo_no(rec2);
  }
  else if (hdr_page_no != page_no ||
           !mach_read_from_2(b->page.frame + hdr_offset + TRX_UNDO_NEXT_LOG))
  {
    uint32_t next= mach_read_from_4(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE +
                                    FLST_NEXT + FIL_ADDR_PAGE + b->page.frame);
    if (next != FIL_NULL)
    {
      page_id.set_page_no(next);
      if (buf_block_t *next_page= get_page(page_id))
      {
        rec2= trx_undo_page_get_first_rec(next_page, hdr_page_no, hdr_offset);
        if (rec2)
        {
          rec2_page= next_page;
          page_no= next;
          goto got_rec;
        }
      }
    }
    goto got_no_rec;
  }
  else
  {
  got_no_rec:
    /* Look for the next undo log and record to purge */
    locked= rseg_get_next_history_log();
  }

  if (locked)
    rseg->latch.wr_unlock();

  return {b->page.frame + uint16_t(roll_ptr), roll_ptr};
}

inline trx_purge_rec_t purge_sys_t::fetch_next_rec()
{
  roll_ptr_t roll_ptr;

  if (!next_stored)
  {
    bool locked= choose_next_log();
    ut_ad(locked == next_stored);
    if (!locked)
      goto got_nothing;
    if (tail.trx_no >= low_limit_no())
    {
      rseg->latch.wr_unlock();
      goto got_nothing;
    }
    /* row_purge_record_func() will later set ROLL_PTR_INSERT_FLAG for
    TRX_UNDO_INSERT_REC */
    roll_ptr= trx_undo_build_roll_ptr(false, trx_sys.rseg_id(rseg, true),
                                      page_no, offset);
  }
  else if (tail.trx_no >= low_limit_no())
  got_nothing:
    return {nullptr, 0};
  else
  {
    roll_ptr= trx_undo_build_roll_ptr(false, trx_sys.rseg_id(rseg, true),
                                      page_no, offset);
    rseg->latch.wr_lock(SRW_LOCK_CALL);
  }

  /* The following will advance the purge iterator. */
  return get_next_rec(roll_ptr);
}

/** Close all tables that were opened in a purge batch for a worker.
@param node   purge task context
@param thd    purge coordinator thread handle */
static void trx_purge_close_tables(purge_node_t *node, THD *thd) noexcept
{
  for (auto &t : node->tables)
  {
    dict_table_t *table= t.second.first;
    if (table != nullptr && table != reinterpret_cast<dict_table_t*>(-1))
      table->release();
  }

  MDL_context *mdl_context= static_cast<MDL_context*>(thd_mdl_context(thd));

  for (auto &t : node->tables)
  {
    dict_table_t *table= t.second.first;
    if (table != nullptr && table != reinterpret_cast<dict_table_t*>(-1))
    {
      t.second.first= reinterpret_cast<dict_table_t*>(-1);
      if (mdl_context != nullptr && t.second.second != nullptr)
        mdl_context->release_lock(t.second.second);
    }
  }
}

void purge_sys_t::wait_FTS(bool also_sys)
{
  std::this_thread::yield();
  for (const uint32_t mask= also_sys ? ~0U : ~PAUSED_SYS; m_FTS_paused & mask;)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

__attribute__((nonnull))
/** Acquire a metadata lock on a table.
@param table        table handle
@param mdl_context  metadata lock acquisition context
@param mdl          metadata lock
@return table handle
@retval nullptr if the table is not found or accessible
@retval -1      if the purge of history must be suspended due to DDL */
static dict_table_t *trx_purge_table_acquire(dict_table_t *table,
                                             MDL_context *mdl_context,
                                             MDL_ticket **mdl) noexcept
{
  ut_ad(dict_sys.frozen_not_locked());
  *mdl= nullptr;

  if (!table->is_readable() || table->corrupted)
    return nullptr;

  size_t db_len= dict_get_db_name_len(table->name.m_name);
  if (db_len == 0)
  {
    /* InnoDB system tables are not covered by MDL */
  got_table:
    table->acquire();
    return table;
  }

  if (purge_sys.must_wait_FTS())
  must_wait:
    return reinterpret_cast<dict_table_t*>(-1);

  char db_buf[NAME_LEN + 1];
  char tbl_buf[NAME_LEN + 1];
  size_t tbl_len;

  if (!table->parse_name<true>(db_buf, tbl_buf, &db_len, &tbl_len))
    /* The name of an intermediate table starts with #sql */
    goto got_table;

  {
    MDL_request request;
    MDL_REQUEST_INIT(&request,MDL_key::TABLE, db_buf, tbl_buf, MDL_SHARED,
                     MDL_EXPLICIT);
    if (mdl_context->try_acquire_lock(&request))
      goto must_wait;
    *mdl= request.ticket;
    if (!*mdl)
      goto must_wait;
  }

  goto got_table;
}

/** Open a table handle for the purge of committed transaction history
@param table_id     InnoDB table identifier
@param mdl_context  metadata lock acquisition context
@param mdl          metadata lock
@return table handle
@retval nullptr if the table is not found or accessible
@retval -1      if the purge of history must be suspended due to DDL */
static dict_table_t *trx_purge_table_open(table_id_t table_id,
                                          MDL_context *mdl_context,
                                          MDL_ticket **mdl) noexcept
{
  dict_table_t *table;

  for (;;)
  {
    dict_sys.freeze(SRW_LOCK_CALL);
    table= dict_sys.find_table(table_id);
    if (table)
      break;
    dict_sys.unfreeze();
    dict_sys.lock(SRW_LOCK_CALL);
    table= dict_load_table_on_id(table_id, DICT_ERR_IGNORE_FK_NOKEY);
    dict_sys.unlock();
    if (!table)
      return nullptr;
    /* At this point, the freshly loaded table may already have been evicted.
    We must look it up again while holding a shared dict_sys.latch.  We keep
    trying this until the table is found in the cache or it cannot be found
    in the dictionary (because the table has been dropped or rebuilt). */
  }

  table= trx_purge_table_acquire(table, mdl_context, mdl);
  dict_sys.unfreeze();
  return table;
}

ATTRIBUTE_COLD
dict_table_t *purge_sys_t::close_and_reopen(table_id_t id, THD *thd,
                                            MDL_ticket **mdl)
{
  MDL_context *mdl_context= static_cast<MDL_context*>(thd_mdl_context(thd));
  ut_ad(mdl_context);
 retry:
  ut_ad(m_active);

  for (que_thr_t *thr= UT_LIST_GET_FIRST(purge_sys.query->thrs); thr;
       thr= UT_LIST_GET_NEXT(thrs, thr))
    trx_purge_close_tables(static_cast<purge_node_t*>(thr->child), thd);

  m_active= false;
  wait_FTS(false);
  m_active= true;

  dict_table_t *table= trx_purge_table_open(id, mdl_context, mdl);
  if (table == reinterpret_cast<dict_table_t*>(-1))
  {
    VALGRIND_YIELD;
    goto retry;
  }

  for (que_thr_t *thr= UT_LIST_GET_FIRST(purge_sys.query->thrs); thr;
       thr= UT_LIST_GET_NEXT(thrs, thr))
  {
    purge_node_t *node= static_cast<purge_node_t*>(thr->child);
    for (auto &t : node->tables)
    {
      if (t.second.first)
      {
        t.second.first= trx_purge_table_open(t.first, mdl_context,
                                             &t.second.second);
        if (t.second.first == reinterpret_cast<dict_table_t*>(-1))
        {
          if (table)
            dict_table_close(table, thd, *mdl);
          VALGRIND_YIELD;
          goto retry;
        }
      }
    }
  }

  return table;
}

/** Run a purge batch.
@param n_purge_threads	number of purge threads
@param thd              purge coordinator thread handle
@param n_work_items     number of work items (currently tables) to process
@return new purge_sys.head */
static purge_sys_t::iterator trx_purge_attach_undo_recs(THD *thd,
                                                        ulint *n_work_items)
{
  que_thr_t *thr;
  purge_sys_t::iterator head= purge_sys.tail;

  /* Fetch and parse the UNDO records. The UNDO records are added
  to a per purge node vector. */
  thr= nullptr;

  std::unordered_map<table_id_t, purge_node_t *>
    table_id_map(TRX_PURGE_TABLE_BUCKETS);
  purge_sys.m_active= true;

  MDL_context *const mdl_context=
    static_cast<MDL_context*>(thd_mdl_context(thd));
  ut_ad(mdl_context);

  while (UNIV_LIKELY(srv_undo_sources) || !srv_fast_shutdown)
  {
    /* Track the max {trx_id, undo_no} for truncating the
    UNDO logs once we have purged the records. */

    if (head <= purge_sys.tail)
      head= purge_sys.tail;

    /* Fetch the next record, and advance the purge_sys.tail. */
    trx_purge_rec_t purge_rec= purge_sys.fetch_next_rec();

    if (!purge_rec.undo_rec)
    {
      if (!purge_rec.roll_ptr)
        break;
      ut_ad(purge_rec.roll_ptr == 1);
      continue;
    }

    table_id_t table_id= trx_undo_rec_get_table_id(purge_rec.undo_rec);

    purge_node_t *&table_node= table_id_map[table_id];
    if (table_node)
      ut_ad(!table_node->in_progress);
    if (!table_node)
    {
      std::pair<dict_table_t *, MDL_ticket *> p;
      p.first= trx_purge_table_open(table_id, mdl_context, &p.second);
      if (p.first == reinterpret_cast<dict_table_t *>(-1))
        p.first= purge_sys.close_and_reopen(table_id, thd, &p.second);

      if (!thr || !(thr= UT_LIST_GET_NEXT(thrs, thr)))
        thr= UT_LIST_GET_FIRST(purge_sys.query->thrs);
      ++*n_work_items;
      table_node= static_cast<purge_node_t *>(thr->child);

      ut_a(que_node_get_type(table_node) == QUE_NODE_PURGE);
      ut_d(auto pair=) table_node->tables.emplace(table_id, p);
      ut_ad(pair.second);
      if (p.first)
        goto enqueue;
    }
    else if (table_node->tables[table_id].first)
    {
    enqueue:
      table_node->undo_recs.push(purge_rec);
      ut_ad(!table_node->in_progress);
    }

    const size_t size{purge_sys.n_pages_handled()};
    if (size >= size_t{srv_purge_batch_size} ||
        size >= buf_pool.usable_size() * 3 / 4)
      break;
  }

#ifdef UNIV_DEBUG
  thr= UT_LIST_GET_FIRST(purge_sys.query->thrs);
  for (ulint i= 0; thr && i < *n_work_items;
       i++, thr= UT_LIST_GET_NEXT(thrs, thr))
  {
    purge_node_t *node= static_cast<purge_node_t*>(thr->child);
    ut_ad(que_node_get_type(node) == QUE_NODE_PURGE);
    ut_ad(!node->in_progress);
    node->in_progress= true;
  }

  for (; thr; thr= UT_LIST_GET_NEXT(thrs, thr))
  {
    purge_node_t *node= static_cast<purge_node_t*>(thr->child);
    ut_ad(que_node_get_type(node) == QUE_NODE_PURGE);
    ut_ad(!node->in_progress);
    ut_ad(node->undo_recs.empty());
  }
#endif

  ut_ad(head <= purge_sys.tail);

  return head;
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

TRANSACTIONAL_INLINE
void purge_sys_t::batch_cleanup(const purge_sys_t::iterator &head)
{
  m_active= false;

  /* Release the undo pages. */
  for (auto p : pages)
    p.second->unfix();
  pages.clear();
  pages.reserve(srv_purge_batch_size);

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
  this->head= head;
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
@return number of undo log pages handled in the batch */
TRANSACTIONAL_TARGET ulint trx_purge(ulint n_tasks, ulint history_size)
{
  ut_ad(n_tasks > 0);

  purge_sys.clone_oldest_view();

  ut_d(if (srv_purge_view_update_only_debug) return 0);

  THD *const thd= current_thd;

  /* Fetch the UNDO recs that need to be purged. */
  ulint n_work= 0;
  const purge_sys_t::iterator head= trx_purge_attach_undo_recs(thd, &n_work);
  const size_t n_pages= purge_sys.n_pages_handled();

  {
    ulint delay= n_pages ? srv_max_purge_lag : 0;
    if (UNIV_UNLIKELY(delay))
    {
      if (delay >= history_size)
      no_throttle:
        delay= 0;
      else if (const ulint max_delay= srv_max_purge_lag_delay)
        delay= std::min(max_delay, 10000 * history_size / delay - 5000);
      else
        goto no_throttle;
    }
    srv_dml_needed_delay= delay;
  }

  ut_ad(n_tasks);
  que_thr_t *thr= nullptr;

  if (n_work)
  {
    for (auto i= n_work; i--; )
    {
      if (!thr)
	thr= UT_LIST_GET_FIRST(purge_sys.query->thrs);
      else
	thr= UT_LIST_GET_NEXT(thrs, thr);

      if (!thr)
	break;

      ut_ad(thr->state == QUE_THR_COMPLETED);
      thr->state= QUE_THR_RUNNING;
      thr->run_node= thr;
      thr->prev_node= thr->common.parent;
      purge_sys.query->state= QUE_FORK_ACTIVE;
      purge_sys.query->last_sel_node= nullptr;
      srv_que_task_enqueue_low(thr);
    }

    /*
      To reduce context switches we only submit at most n_tasks-1 worker task.
      (we can use less tasks, if there is not enough work)

      The coordinator does worker's job, instead of waiting and sitting idle,
      then waits for all others to finish.

      This also means if innodb_purge_threads=1, the coordinator does all
      the work alone.
    */
    const ulint workers{std::min(n_work, n_tasks) - 1};
    for (ulint i= 0; i < workers; i++)
      srv_thread_pool->submit_task(&purge_worker_task);
    srv_purge_worker_task_low();

    if (workers)
      trx_purge_wait_for_workers_to_complete();

    for (thr= UT_LIST_GET_FIRST(purge_sys.query->thrs); thr && n_work--;
         thr= UT_LIST_GET_NEXT(thrs, thr))
    {
      purge_node_t *node= static_cast<purge_node_t*>(thr->child);
      trx_purge_close_tables(node, thd);
      node->tables.clear();
    }
  }

  purge_sys.batch_cleanup(head);

  MONITOR_INC_VALUE(MONITOR_PURGE_INVOKED, 1);
  MONITOR_INC_VALUE(MONITOR_PURGE_N_PAGE_HANDLED, n_pages);

  return n_pages;
}
