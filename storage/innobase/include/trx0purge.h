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
@file include/trx0purge.h
Purge old versions

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#ifndef trx0purge_h
#define trx0purge_h

#include "trx0rseg.h"
#include "que0types.h"

#include <queue>

/** A dummy undo record used as a return value when we have a whole undo log
which needs no purge */
extern trx_undo_rec_t	trx_purge_dummy_rec;

/********************************************************************//**
Calculates the file address of an undo log header when we have the file
address of its history list node.
@return file address of the log */
UNIV_INLINE
fil_addr_t
trx_purge_get_log_from_hist(
/*========================*/
	fil_addr_t	node_addr);	/*!< in: file address of the history
					list node of the log */
/** Prepend the history list with an undo log.
Remove the undo log segment from the rseg slot if it is too big for reuse.
@param[in]	trx		transaction
@param[in,out]	undo		undo log
@param[in,out]	mtr		mini-transaction */
void
trx_purge_add_undo_to_history(const trx_t* trx, trx_undo_t*& undo, mtr_t* mtr);
/*******************************************************************//**
This function runs a purge batch.
@return number of undo log pages handled in the batch */
ulint
trx_purge(
/*======*/
	ulint	n_purge_threads,	/*!< in: number of purge tasks to
					submit to task queue. */
	bool	truncate		/*!< in: truncate history if true */
#ifdef UNIV_DEBUG
	, srv_slot_t *slot		/*!< in/out: purge coordinator
					thread slot */
#endif
);

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
	@return whether anything is to be purged */
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
public:
	/** signal state changes; os_event_reset() and os_event_set()
	are protected by rw_lock_x_lock(latch) */
	MY_ALIGNED(CACHE_LINE_SIZE)
	os_event_t	event;
	/** latch protecting view, m_enabled */
	MY_ALIGNED(CACHE_LINE_SIZE)
	rw_lock_t	latch;
private:
	/** whether purge is enabled; protected by latch and std::atomic */
	std::atomic<bool>		m_enabled;
	/** number of pending stop() calls without resume() */
	Atomic_counter<int32_t>		m_paused;
public:
	que_t*		query;		/*!< The query graph which will do the
					parallelized purge operation */
	MY_ALIGNED(CACHE_LINE_SIZE)
	ReadView	view;		/*!< The purge will not remove undo logs
					which are >= this view (purge view) */
	/** Number of not completed tasks. Accessed by srv_purge_coordinator
	and srv_worker_thread by std::atomic. */
	std::atomic<ulint>	n_tasks;

	/** Iterator to the undo log records of committed transactions */
	struct iterator
	{
		bool operator<=(const iterator& other) const
		{
			if (trx_no < other.trx_no) return true;
			if (trx_no > other.trx_no) return false;
			return undo_no <= other.undo_no;
		}

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
	transactions may be discarded (history list truncation). */
	iterator	head;
	/*-----------------------------*/
	bool		next_stored;	/*!< whether rseg holds the next record
					to purge */
	trx_rseg_t*	rseg;		/*!< Rollback segment for the next undo
					record to purge */
	ulint		page_no;	/*!< Page number for the next undo
					record to purge, page number of the
					log header, if dummy record */
	ulint		offset;		/*!< Page offset for the next undo
					record to purge, 0 if the dummy
					record */
	ulint		hdr_page_no;	/*!< Header page of the undo log where
					the next record to purge belongs */
	ulint		hdr_offset;	/*!< Header byte offset on the page */


	TrxUndoRsegsIterator
			rseg_iter;	/*!< Iterator to get the next rseg
					to process */

	purge_pq_t	purge_queue;	/*!< Binary min-heap, ordered on
					TrxUndoRsegs::trx_no. It is protected
					by the pq_mutex */
	PQMutex		pq_mutex;	/*!< Mutex protecting purge_queue */

	/** Undo tablespace file truncation (only accessed by the
	srv_purge_coordinator_thread) */
	struct {
		/** The undo tablespace that is currently being truncated */
		fil_space_t*	current;
		/** The undo tablespace that was last truncated */
		fil_space_t*	last;
	} truncate;

  /**
    Constructor.

    Some members may require late initialisation, thus we just mark object as
    uninitialised. Real initialisation happens in create().
  */

  purge_sys_t() : event(NULL), m_enabled(false), n_tasks(0) {}


  /** Create the instance */
  void create();

  /** Close the purge system on shutdown */
  void close();

  /** @return whether purge is enabled */
  bool enabled() { return m_enabled.load(std::memory_order_relaxed); }
  /** @return whether the purge coordinator is paused */
  bool paused()
  { return m_paused != 0; }

  /** Enable purge at startup. Not protected by latch; the main thread
  will wait for purge_sys.enabled() in srv_start() */
  void coordinator_startup()
  {
    ut_ad(!enabled());
    m_enabled.store(true, std::memory_order_relaxed);
  }

  /** Disable purge at shutdown */
  void coordinator_shutdown()
  {
    ut_ad(enabled());
    m_enabled.store(false, std::memory_order_relaxed);
  }

  /** @return whether the purge coordinator thread is active */
  bool running();
  /** Stop purge during FLUSH TABLES FOR EXPORT */
  void stop();
  /** Resume purge at UNLOCK TABLES after FLUSH TABLES FOR EXPORT */
  void resume();
};

/** The global data structure coordinating a purge */
extern purge_sys_t	purge_sys;

/** Info required to purge a record */
struct trx_purge_rec_t {
	trx_undo_rec_t*	undo_rec;	/*!< Record to purge */
	roll_ptr_t	roll_ptr;	/*!< File pointr to UNDO record */
};

#include "trx0purge.inl"

#endif /* trx0purge_h */
