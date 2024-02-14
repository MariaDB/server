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

/** Rollback segements from a given transaction with trx-no
scheduled for purge. */
class TrxUndoRsegs {
private:
	typedef std::vector<trx_rseg_t*, ut_allocator<trx_rseg_t*> >
		trx_rsegs_t;
public:
	typedef trx_rsegs_t::iterator iterator;
	typedef trx_rsegs_t::const_iterator const_iterator;

	TrxUndoRsegs() = default;

	/** Constructor */
	TrxUndoRsegs(trx_rseg_t& rseg)
		: trx_no(rseg.last_trx_no()), m_rsegs(1, &rseg) {}
	/** Constructor */
	TrxUndoRsegs(trx_id_t trx_no, trx_rseg_t& rseg)
		: trx_no(trx_no), m_rsegs(1, &rseg) {}

	bool operator!=(const TrxUndoRsegs& other) const
	{ return trx_no != other.trx_no; }
	bool empty() const { return m_rsegs.empty(); }
	void erase(iterator& it) { m_rsegs.erase(it); }
	iterator begin() { return(m_rsegs.begin()); }
	iterator end() { return(m_rsegs.end()); }
	const_iterator begin() const { return m_rsegs.begin(); }
	const_iterator end() const { return m_rsegs.end(); }

	/** Compare two TrxUndoRsegs based on trx_no.
	@param elem1 first element to compare
	@param elem2 second element to compare
	@return true if elem1 > elem2 else false.*/
	bool operator()(const TrxUndoRsegs& lhs, const TrxUndoRsegs& rhs)
	{
		return(lhs.trx_no > rhs.trx_no);
	}

	/** Copy of trx_rseg_t::last_trx_no() */
	trx_id_t trx_no= 0;
private:
	/** Rollback segments of a transaction, scheduled for purge. */
	trx_rsegs_t m_rsegs{};
};

typedef std::priority_queue<
	TrxUndoRsegs,
	std::vector<TrxUndoRsegs, ut_allocator<TrxUndoRsegs> >,
	TrxUndoRsegs>	purge_pq_t;

/** Chooses the rollback segment with the oldest committed transaction */
struct TrxUndoRsegsIterator {
	/** Constructor */
	TrxUndoRsegsIterator();
	/** Sets the next rseg to purge in purge_sys.
	Executed in the purge coordinator thread.
	@retval false when nothing is to be purged
	@retval true  when purge_sys.rseg->latch was locked */
	inline bool set_next();

private:
	// Disable copying
	TrxUndoRsegsIterator(const TrxUndoRsegsIterator&);
	TrxUndoRsegsIterator& operator=(const TrxUndoRsegsIterator&);

	/** The current element to process */
	TrxUndoRsegs			m_rsegs;
	/** Track the current element in m_rsegs */
	TrxUndoRsegs::const_iterator	m_iter;
};

/** The control structure used in the purge operation */
class purge_sys_t
{
  friend TrxUndoRsegsIterator;
public:
  /** latch protecting view, m_enabled */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) mutable srw_spin_lock latch;
private:
  /** Read view at the start of a purge batch. Any encountered index records
  that are older than view will be removed. */
  ReadViewBase view;
  /** whether the subsystem has been initialized */
  bool m_initialized{false};
  /** whether purge is enabled; protected by latch and std::atomic */
  std::atomic<bool> m_enabled{false};
public:
  /** whether purge is active (may hold table handles) */
  std::atomic<bool> m_active{false};
private:
  /** number of pending stop() calls without resume() */
  Atomic_counter<uint32_t> m_paused;
  /** number of stop_SYS() calls without resume_SYS() */
  Atomic_counter<uint32_t> m_SYS_paused;
  /** number of stop_FTS() calls without resume_FTS() */
  Atomic_counter<uint32_t> m_FTS_paused;

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


	TrxUndoRsegsIterator
			rseg_iter;	/*!< Iterator to get the next rseg
					to process */
public:
	purge_pq_t	purge_queue;	/*!< Binary min-heap, ordered on
					TrxUndoRsegs::trx_no. It is protected
					by the pq_mutex */
	mysql_mutex_t	pq_mutex;	/*!< Mutex protecting purge_queue */

	/** Undo tablespace file truncation (only accessed by the
	srv_purge_coordinator_thread) */
	struct {
		/** The undo tablespace that is currently being truncated */
		fil_space_t*	current;
		/** The undo tablespace that was last truncated */
		fil_space_t*	last;
	} truncate;

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
  void stop_SYS() { m_SYS_paused++; }
  /** Resume purge in data dictionary tables */
  static void resume_SYS(void *);

  /** Pause purge during a DDL operation that could drop FTS_ tables. */
  void stop_FTS();
  /** Resume purge after stop_FTS(). */
  void resume_FTS() { ut_d(const auto p=) m_FTS_paused--; ut_ad(p); }
  /** @return whether stop_SYS() is in effect */
  bool must_wait_FTS() const { return m_FTS_paused; }

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
  past the purged log. */
  void rseg_get_next_history_log();

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
  TRANSACTIONAL_TARGET bool is_purgeable(trx_id_t trx_id) const;

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
    inline view_guard();
    inline ~view_guard();

    /** @return purge_sys.view */
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
};

/** The global data structure coordinating a purge */
extern purge_sys_t	purge_sys;

purge_sys_t::view_guard::view_guard()
{ purge_sys.latch.rd_lock(SRW_LOCK_CALL); }

purge_sys_t::view_guard::~view_guard()
{ purge_sys.latch.rd_unlock(); }

const ReadViewBase &purge_sys_t::view_guard::view() const
{ return purge_sys.view; }

purge_sys_t::end_view_guard::end_view_guard()
{ purge_sys.end_latch.rd_lock(); }

purge_sys_t::end_view_guard::~end_view_guard()
{ purge_sys.end_latch.rd_unlock(); }

const ReadViewBase &purge_sys_t::end_view_guard::view() const
{ return purge_sys.end_view; }
