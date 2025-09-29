/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/trx0purge.h
Purge old versions

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#pragma once

#include "trx0sys.h"
#include "que0types.h"
#include "srw_lock.h"

#include <queue>
#include <unordered_map>

/** Prepend the history list with an undo log.
Remove the undo log segment from the rseg slot if it is too big for reuse.
@param[in]	trx		transaction
@param[in,out]	undo		undo log
@param[in,out]	mtr		mini-transaction */
void
trx_purge_add_undo_to_history(const trx_t* trx, trx_undo_t*& undo, mtr_t* mtr);

/**
Remove unnecessary history data from rollback segments. NOTE that when this
function is called, the caller (purge_coordinator_callback)
must not have any latches on undo log pages!
*/
void trx_purge_truncate_history();

/**
Run a purge batch.
@param n_tasks       number of purge tasks to submit to the queue
@param history_size  trx_sys.history_size()
@return number of undo log pages handled in the batch */
ulint trx_purge(ulint n_tasks, ulint history_size);

/** The control structure used in the purge operation */
class purge_sys_t
{
  /** Min-heap based priority queue of (trx_no, trx_sys.rseg_array index)
  pairs, ordered on trx_no. The highest 64-TRX_NO_SHIFT bits of each element is
  trx_no, the lowest 8 bits is rseg's index in trx_sys.rseg_array. */
  class purge_queue
  {
  public:
    typedef std::vector<uint64_t, ut_allocator<uint64_t>> container_type;
    /** Number of bits reseved to shift trx_no in purge queue element */
    static constexpr unsigned TRX_NO_SHIFT= 8;

    bool empty() const { return m_array.empty(); }
    void clear() { m_array.clear(); }

    /** Push (trx_no, trx_sys.rseg_array index) into min-heap.
    @param trx_no_rseg (trx_no << TRX_NO_SHIFT | (trx_sys.rseg_array index)) */
    void push_trx_no_rseg(container_type::value_type trx_no_rseg)
    {
      m_array.push_back(trx_no_rseg);
      std::push_heap(m_array.begin(), m_array.end(),
                     std::greater<container_type::value_type>());
    }

    /** Push rseg to priority queue.
    @param trx_no trx_no of committed transaction
    @param rseg rseg of committed transaction*/
    void push(trx_id_t trx_no, const trx_rseg_t &rseg)
    {
      ut_ad(trx_no < 1ULL << (DATA_TRX_ID_LEN * CHAR_BIT));
      ut_ad(&rseg >= trx_sys.rseg_array);
      ut_ad(&rseg < trx_sys.rseg_array + TRX_SYS_N_RSEGS);
      push_trx_no_rseg(trx_no << TRX_NO_SHIFT |
          byte(&rseg - trx_sys.rseg_array));
    }

    /** Extracts rseg from (trx_no, trx_sys.rseg_array index) pair.
    @param trx_no_rseg (trx_no << TRX_NO_SHIFT | (trx_sys.rseg_array index)
    @return pointer to rseg in trx_sys.rseg_array */
    static trx_rseg_t *rseg(container_type::value_type trx_no_rseg) {
      byte i= static_cast<byte>(trx_no_rseg);
      ut_ad(i < TRX_SYS_N_RSEGS);
      return &trx_sys.rseg_array[i];
    }

    /** Pop rseg from priority queue.
    @return pointer to popped trx_rseg_t object */
    trx_rseg_t *pop()
    {
      ut_ad(!empty());
      std::pop_heap(m_array.begin(), m_array.end(),
                    std::greater<container_type::value_type>());
      trx_rseg_t *r = rseg(m_array.back());
      m_array.pop_back();
      return r;
    }

    /** Clone m_array.
    @return m_array clone */
    container_type clone_container() const{ return m_array; }

  private:
   /** Array of (trx_no, trx_sys.rseg_array index) pairs. */
    container_type m_array;
  };


public:
  /** latch protecting view, m_enabled */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE)
  mutable IF_DBUG(srw_lock_debug,srw_spin_lock) latch;
private:
  /** Read view at the start of a purge batch. Any encountered index records
  that are older than view will be removed. */
  ReadViewBase view;
  /** whether the subsystem has been initialized */
  bool m_initialized{false};
  /** whether purge is enabled; protected by latch and std::atomic */
  std::atomic<bool> m_enabled{false};
  /** The primary candidate for iterator::free_history() is
  rseg=trx_sys.rseg_array[skipped_rseg]. This field may be changed
  after invoking rseg.set_skip_allocation() and rseg.clear_skip_allocation()
  and while holding the exclusive rseg.latch.

  This may only be 0 if innodb_undo_tablespaces=0, because rollback segment
  0 always resides in the system tablespace and would never be used when
  dedicated undo tablespaces are in use. */
  Atomic_relaxed<uint8_t> skipped_rseg;
public:
  /** whether purge is active (may hold table handles) */
  std::atomic<bool> m_active{false};
private:
  /** number of pending stop() calls without resume() */
  Atomic_counter<uint32_t> m_paused;
  /** PAUSED_SYS * number of stop_SYS() calls without resume_SYS() +
  number of stop_FTS() calls without resume_FTS() */
  Atomic_relaxed<uint32_t> m_FTS_paused;
  /** The stop_SYS() multiplier in m_FTS_paused */
  static constexpr const uint32_t PAUSED_SYS= 1U << 16;

  /** latch protecting end_view */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) srw_spin_lock_low end_latch;
  /** Read view at the end of a purge batch (copied from view). Any undo pages
  containing records older than end_view may be freed. */
  ReadViewBase end_view;

  struct hasher
  {
    size_t operator()(const page_id_t &id) const { return size_t(id.raw()); }
  };

  using unordered_map =
    std::unordered_map<const page_id_t, buf_block_t*, hasher,
#if defined __GNUC__ && __GNUC__ == 4 && __GNUC_MINOR__ >= 8
                       std::equal_to<page_id_t>
                       /* GCC 4.8.5 would fail to find a matching allocator */
#else
                       std::equal_to<page_id_t>,
                       ut_allocator<std::pair<const page_id_t, buf_block_t*>>
#endif
                       >;
  /** map of buffer-fixed undo log pages processed during a purge batch */
  unordered_map pages;
public:
  /** @return the number of processed undo pages */
  size_t n_pages_handled() const { return pages.size(); }

  /** Look up an undo log page.
  @param id    undo page identifier
  @return undo page
  @retval nullptr in case the page is corrupted */
  buf_block_t *get_page(page_id_t id);

	que_t*		query;		/*!< The query graph which will do the
					parallelized purge operation */

	/** Iterator to the undo log records of committed transactions */
	struct iterator
	{
		bool operator<=(const iterator& other) const
		{
			if (trx_no < other.trx_no) return true;
			if (trx_no > other.trx_no) return false;
			return undo_no <= other.undo_no;
		}

		/** Remove unnecessary history data from a rollback segment.
		@param rseg   rollback segment
		@return error code */
		inline dberr_t free_history_rseg(trx_rseg_t &rseg) const;

		/** Free the undo pages up to this. */
		dberr_t free_history() const;

		/** trx_t::no of the committed transaction */
		trx_id_t	trx_no;
		/** The record number within the committed transaction's undo
		log, increasing, purged from from 0 onwards */
		undo_no_t	undo_no;
	};

	/** The tail of the purge queue; the last parsed undo log of a
	committed transaction. */
	iterator	tail;
	/** The head of the purge queue; any older undo logs of committed
	transactions may be discarded (history list truncation).
	Protected by latch. */
	iterator	head;
	/*-----------------------------*/
	bool		next_stored;	/*!< whether rseg holds the next record
					to purge */
	trx_rseg_t*	rseg;		/*!< Rollback segment for the next undo
					record to purge */
private:
	uint32_t	page_no;	/*!< Page number for the next undo
					record to purge, page number of the
					log header, if dummy record */
	uint32_t	hdr_page_no;	/*!< Header page of the undo log where
					the next record to purge belongs */
	uint16_t	offset;		/*!< Page offset for the next undo
					record to purge, 0 if the dummy
					record */
	uint16_t	hdr_offset;	/*!< Header byte offset on the page */

  /** Binary min-heap of (trx_no, trx_sys.rseg_array index) pairs, ordered on
  trx_no. It is protected by the pq_mutex */
  purge_queue purge_queue;

  /** Mutex protecting purge_queue */
  mysql_mutex_t pq_mutex;

public:

  void enqueue(trx_id_t trx_no, const trx_rseg_t &rseg) {
    mysql_mutex_assert_owner(&pq_mutex);
    purge_queue.push(trx_no, rseg);
  }

  /** Push to purge queue without acquiring pq_mutex.
  @param rseg rseg to push */
  void enqueue(const trx_rseg_t &rseg) { enqueue(rseg.last_trx_no(), rseg); }

  /** Clone purge queue container.
  @return purge queue container clone */
  purge_queue::container_type clone_queue_container() const {
    mysql_mutex_assert_owner(&pq_mutex);
    return purge_queue.clone_container();
  }

  /** Acquare purge_queue_mutex */
  void queue_lock() { mysql_mutex_lock(&pq_mutex); }

  /** Release purge queue mutex */
  void queue_unlock() { mysql_mutex_unlock(&pq_mutex); }

  /** innodb_undo_log_truncate=ON state;
  only modified by purge_coordinator_callback() */
  struct {
    /** The undo tablespace that is currently being truncated */
    Atomic_relaxed<fil_space_t*> current;
    /** The number of the undo tablespace that was last truncated,
    relative from srv_undo_space_id_start */
    uint32_t last;
  } truncate_undo_space;

  /** Create the instance */
  void create();

  /** Close the purge system on shutdown */
  void close();

  /** @return whether purge is enabled */
  bool enabled() { return m_enabled.load(std::memory_order_relaxed); }
  /** @return whether the purge coordinator is paused */
  bool paused()
  { return m_paused != 0; }

  /** Enable purge at startup. */
  void coordinator_startup()
  {
    ut_ad(!enabled());
    m_enabled.store(true, std::memory_order_relaxed);
    wake_if_not_active();
  }

  /** Disable purge at shutdown */
  void coordinator_shutdown()
  {
    ut_ad(enabled());
    m_enabled.store(false, std::memory_order_relaxed);
  }

  /** @return whether the purge tasks are active */
  static bool running();

  /** Stop purge during FLUSH TABLES FOR EXPORT. */
  void stop();
  /** Resume purge at UNLOCK TABLES after FLUSH TABLES FOR EXPORT */
  void resume();

  /** Close and reopen all tables in case of a MDL conflict with DDL */
  dict_table_t *close_and_reopen(table_id_t id, THD *thd, MDL_ticket **mdl);
private:
  /** Suspend purge during a DDL operation on FULLTEXT INDEX tables */
  void wait_FTS(bool also_sys);
public:
  /** Suspend purge in data dictionary tables */
  void stop_SYS()
  {
    ut_d(const auto p=) m_FTS_paused.fetch_add(PAUSED_SYS);
    ut_ad(p < p + PAUSED_SYS);
  }
  /** Resume purge in data dictionary tables */
  static void resume_SYS(void *);

  /** Pause purge during a DDL operation that could drop FTS_ tables. */
  void stop_FTS();
  /** Resume purge after stop_FTS(). */
  void resume_FTS()
  { ut_d(const auto p=) m_FTS_paused.fetch_sub(1); ut_ad(p & ~PAUSED_SYS); }
  /** @return whether stop_SYS() is in effect */
  bool must_wait_FTS() const { return m_FTS_paused & ~PAUSED_SYS; }

private:
  /**
  Get the next record to purge and update the info in the purge system.
  @param roll_ptr           undo log pointer to the record
  @return buffer-fixed reference to undo log record
  @retval {nullptr,1} if the whole undo log can skipped in purge
  @retval {nullptr,0} if nothing is left, or on corruption */
  inline trx_purge_rec_t get_next_rec(roll_ptr_t roll_ptr);

  /** Choose the next undo log to purge.
  @return whether anything is to be purged */
  bool choose_next_log();

  /** Update the last not yet purged history log info in rseg when
  we have purged a whole undo log. Advances also purge_trx_no
  past the purged log.
  @return whether anything is to be purged */
  bool rseg_get_next_history_log();

public:
  /**
  Fetch the next undo log record from the history list to purge.
  @return buffer-fixed reference to undo log record
  @retval {nullptr,1} if the whole undo log can skipped in purge
  @retval {nullptr,0} if nothing is left, or on corruption */
  inline trx_purge_rec_t fetch_next_rec();

  /** Determine if the history of a transaction is purgeable.
  @param trx_id  transaction identifier
  @return whether the history is purgeable */
  bool is_purgeable(trx_id_t trx_id) const noexcept;

  /** A wrapper around ReadView::low_limit_no(). */
  trx_id_t low_limit_no() const
  {
    /* This function may only be called by purge_coordinator_callback().

    The purge coordinator task may call this without holding any latch,
    because it is the only thread that may modify purge_sys.view.

    Any other threads that access purge_sys.view must hold purge_sys.latch,
    typically via purge_sys_t::view_guard. */
    return view.low_limit_no();
  }
  /** A wrapper around ReadView::sees(). */
  trx_id_t sees(trx_id_t id) const
  {
    /* This function may only be called by purge_coordinator_callback().

    The purge coordinator task may call this without holding any latch,
    because it is the only thread that may modify purge_sys.view.

    Any other threads that access purge_sys.view must hold purge_sys.latch,
    typically via purge_sys_t::view_guard. */
    return view.sees(id);
  }

private:
  /** Enable the use of a rollback segment and advance skipped_rseg,
  after iterator::free_history_rseg() had invoked
  rseg.set_skip_allocation(). */
  inline void rseg_enable(trx_rseg_t &rseg);

  /** Try to start truncating a tablespace.
  @param id        undo tablespace identifier
  @param size      the maximum desired undo tablespace size, in pages
  @return undo tablespace whose truncation was started
  @retval nullptr  if truncation is not currently possible */
  inline fil_space_t *undo_truncate_try(uint32_t id, uint32_t size);
public:
  /** Check if innodb_undo_log_truncate=ON needs to be handled.
  This is only to be called by purge_coordinator_callback().
  @return undo tablespace chosen by innodb_undo_log_truncate=ON
  @retval nullptr if truncation is not currently possible */
  fil_space_t *truncating_tablespace();

  /** A wrapper around trx_sys_t::clone_oldest_view(). */
  template<bool also_end_view= false>
  void clone_oldest_view()
  {
    if (!also_end_view)
      wait_FTS(true);
    latch.wr_lock(SRW_LOCK_CALL);
    trx_sys.clone_oldest_view(&view);
    if (also_end_view)
      (end_view= view).
        clamp_low_limit_id(head.trx_no ? head.trx_no : tail.trx_no);
    latch.wr_unlock();
  }

  /** Wake up the purge threads if there is work to do. */
  void wake_if_not_active();

  /** Release undo pages and update end_view at the end of a purge batch.
  @retval false when nothing is to be purged
  @retval true  when purge_sys.rseg->latch was locked  */
  inline void batch_cleanup(const iterator &head);

  struct view_guard
  {
    enum guard { END_VIEW= -1, PURGE= 0, VIEW= 1};
    guard latch;
    inline view_guard(guard latch);
    inline ~view_guard();
    /** Fetch an undo log page.
    @param id   page identifier
    @param mtr  mini-transaction
    @return reference to buffer page, possibly buffer-fixed in mtr */
    inline const buf_block_t *get(const page_id_t id, mtr_t *mtr);

    /** @return purge_sys.view or purge_sys.end_view */
    inline const ReadViewBase &view() const;
  };

  struct end_view_guard
  {
    inline end_view_guard();
    inline ~end_view_guard();

    /** @return purge_sys.end_view */
    inline const ReadViewBase &view() const;
  };

  /** Stop the purge thread and check n_ref_count of all auxiliary
  and common table associated with the fts table.
  @param	table		parent FTS table
  @param	already_stopped	True indicates purge threads were
				already stopped */
  void stop_FTS(const dict_table_t &table, bool already_stopped=false);

  /** Cleanse purge queue to remove the rseg that reside in undo-tablespace
  marked for truncate.
  @param space undo tablespace being truncated */
  void cleanse_purge_queue(const fil_space_t &space);
};

/** The global data structure coordinating a purge */
extern purge_sys_t	purge_sys;

purge_sys_t::view_guard::view_guard(purge_sys_t::view_guard::guard latch) :
  latch(latch)
{
  switch (latch) {
  case VIEW:
    purge_sys.latch.rd_lock(SRW_LOCK_CALL);
    break;
  case END_VIEW:
    purge_sys.end_latch.rd_lock();
    break;
  case PURGE:
    /* the access is within a purge batch; purge_coordinator_task
    will wait for all workers to complete before updating the views */
    break;
  }
}

purge_sys_t::view_guard::~view_guard()
{
  switch (latch) {
  case VIEW:
    purge_sys.latch.rd_unlock();
    break;
  case END_VIEW:
    purge_sys.end_latch.rd_unlock();
    break;
  case PURGE:
    break;
  }
}

const ReadViewBase &purge_sys_t::view_guard::view() const
{ return latch == END_VIEW ? purge_sys.end_view : purge_sys.view; }

purge_sys_t::end_view_guard::end_view_guard()
{ purge_sys.end_latch.rd_lock(); }

purge_sys_t::end_view_guard::~end_view_guard()
{ purge_sys.end_latch.rd_unlock(); }

const ReadViewBase &purge_sys_t::end_view_guard::view() const
{ return purge_sys.end_view; }
