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
@file include/lock0lock.h
The transaction lock system

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#ifndef lock0lock_h
#define lock0lock_h

#include "buf0types.h"
#include "trx0types.h"
#include "mtr0types.h"
#include "rem0types.h"
#include "que0types.h"
#include "lock0types.h"
#include "hash0hash.h"
#include "srv0srv.h"
#include "ut0vec.h"
#include "gis0rtree.h"
#include "lock0prdt.h"

// Forward declaration
class ReadView;

/** The value of innodb_deadlock_detect */
extern my_bool	innobase_deadlock_detect;

/*********************************************************************//**
Gets the heap_no of the smallest user record on a page.
@return heap_no of smallest user record, or PAGE_HEAP_NO_SUPREMUM */
UNIV_INLINE
ulint
lock_get_min_heap_no(
/*=================*/
	const buf_block_t*	block);	/*!< in: buffer block */
/*************************************************************//**
Updates the lock table when we have reorganized a page. NOTE: we copy
also the locks set on the infimum of the page; the infimum may carry
locks if an update of a record is occurring on the page, and its locks
were temporarily stored on the infimum. */
void
lock_move_reorganize_page(
/*======================*/
	const buf_block_t*	block,	/*!< in: old index page, now
					reorganized */
	const buf_block_t*	oblock);/*!< in: copy of the old, not
					reorganized page */
/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list end is moved to another page. */
void
lock_move_rec_list_end(
/*===================*/
	const buf_block_t*	new_block,	/*!< in: index page to move to */
	const buf_block_t*	block,		/*!< in: index page */
	const rec_t*		rec);		/*!< in: record on page: this
						is the first record moved */
/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
void
lock_move_rec_list_start(
/*=====================*/
	const buf_block_t*	new_block,	/*!< in: index page to move to */
	const buf_block_t*	block,		/*!< in: index page */
	const rec_t*		rec,		/*!< in: record on page:
						this is the first
						record NOT copied */
	const rec_t*		old_end);	/*!< in: old
						previous-to-last
						record on new_page
						before the records
						were copied */
/*************************************************************//**
Updates the lock table when a page is split to the right. */
void
lock_update_split_right(
/*====================*/
	const buf_block_t*	right_block,	/*!< in: right page */
	const buf_block_t*	left_block);	/*!< in: left page */
/*************************************************************//**
Updates the lock table when a page is merged to the right. */
void
lock_update_merge_right(
/*====================*/
	const buf_block_t*	right_block,	/*!< in: right page to
						which merged */
	const rec_t*		orig_succ,	/*!< in: original
						successor of infimum
						on the right page
						before merge */
	const buf_block_t*	left_block);	/*!< in: merged index
						page which will be
						discarded */
/** Update locks when the root page is copied to another in
btr_root_raise_and_insert(). Note that we leave lock structs on the
root page, even though they do not make sense on other than leaf
pages: the reason is that in a pessimistic update the infimum record
of the root page will act as a dummy carrier of the locks of the record
to be updated. */
void lock_update_root_raise(const buf_block_t &block, const page_id_t root);
/** Update the lock table when a page is copied to another.
@param new_block  the target page
@param old        old page (not index root page) */
void lock_update_copy_and_discard(const buf_block_t &new_block, page_id_t old);
/*************************************************************//**
Updates the lock table when a page is split to the left. */
void
lock_update_split_left(
/*===================*/
	const buf_block_t*	right_block,	/*!< in: right page */
	const buf_block_t*	left_block);	/*!< in: left page */
/** Update the lock table when a page is merged to the left.
@param left      left page
@param orig_pred original predecessor of supremum on the left page before merge
@param right     merged, to-be-discarded right page */
void lock_update_merge_left(const buf_block_t& left, const rec_t *orig_pred,
                            const page_id_t right);
/*************************************************************//**
Updates the lock table when a page is split and merged to
two pages. */
UNIV_INTERN
void
lock_update_split_and_merge(
	const buf_block_t* left_block,	/*!< in: left page to which merged */
	const rec_t* orig_pred,		/*!< in: original predecessor of
					supremum on the left page before merge*/
	const buf_block_t* right_block);/*!< in: right page from which merged */
/*************************************************************//**
Resets the original locks on heir and replaces them with gap type locks
inherited from rec. */
void
lock_rec_reset_and_inherit_gap_locks(
/*=================================*/
	const buf_block_t&	heir_block,	/*!< in: block containing the
						record which inherits */
	const page_id_t		donor,		/*!< in: page containing the
						record from which inherited;
						does NOT reset the locks on
						this record */
	ulint			heir_heap_no,	/*!< in: heap_no of the
						inheriting record */
	ulint			heap_no);	/*!< in: heap_no of the
						donating record */
/*************************************************************//**
Updates the lock table when a page is discarded. */
void
lock_update_discard(
/*================*/
	const buf_block_t*	heir_block,	/*!< in: index page
						which will inherit the locks */
	ulint			heir_heap_no,	/*!< in: heap_no of the record
						which will inherit the locks */
	const buf_block_t*	block);		/*!< in: index page
						which will be discarded */
/*************************************************************//**
Updates the lock table when a new user record is inserted. */
void
lock_update_insert(
/*===============*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec);	/*!< in: the inserted record */
/*************************************************************//**
Updates the lock table when a record is removed. */
void
lock_update_delete(
/*===============*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec);	/*!< in: the record to be removed */
/*********************************************************************//**
Stores on the page infimum record the explicit locks of another record.
This function is used to store the lock state of a record when it is
updated and the size of the record changes in the update. The record
is in such an update moved, perhaps to another page. The infimum record
acts as a dummy carrier record, taking care of lock releases while the
actual record is being moved. */
void
lock_rec_store_on_page_infimum(
/*===========================*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec);	/*!< in: record whose lock state
					is stored on the infimum
					record of the same page; lock
					bits are reset on the
					record */
/** Restore the explicit lock requests on a single record, where the
state was stored on the infimum of a page.
@param block   buffer block containing rec
@param rec     record whose lock state is restored
@param donator page (rec is not necessarily on this page)
whose infimum stored the lock state; lock bits are reset on the infimum */
void lock_rec_restore_from_page_infimum(const buf_block_t &block,
					const rec_t *rec, page_id_t donator);
/*********************************************************************//**
Checks if locks of other transactions prevent an immediate insert of
a record. If they do, first tests if the query thread should anyway
be suspended for some reason; if not, then puts the transaction and
the query thread to the lock wait state and inserts a waiting request
for a gap x-lock to the lock queue.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_rec_insert_check_and_lock(
/*===========================*/
	const rec_t*	rec,	/*!< in: record after which to insert */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	dict_index_t*	index,	/*!< in: index */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	bool*		inherit)/*!< out: set to true if the new
				inserted record maybe should inherit
				LOCK_GAP type locks from the successor
				record */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate modify (update,
delete mark, or delete unmark) of a clustered index record. If they do,
first tests if the query thread should anyway be suspended for some
reason; if not, then puts the transaction and the query thread to the
lock wait state and inserts a waiting request for a record x-lock to the
lock queue.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_modify_check_and_lock(
/*=================================*/
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: record which should be
					modified */
	dict_index_t*		index,	/*!< in: clustered index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	que_thr_t*		thr)	/*!< in: query thread */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Checks if locks of other transactions prevent an immediate modify
(delete mark or delete unmark) of a secondary index record.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_sec_rec_modify_check_and_lock(
/*===============================*/
	ulint		flags,	/*!< in: if BTR_NO_LOCKING_FLAG
				bit is set, does nothing */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	const rec_t*	rec,	/*!< in: record which should be
				modified; NOTE: as this is a secondary
				index, we always have to modify the
				clustered index record first: see the
				comment below */
	dict_index_t*	index,	/*!< in: secondary index */
	que_thr_t*	thr,	/*!< in: query thread
				(can be NULL if BTR_NO_LOCKING_FLAG) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Like lock_clust_rec_read_check_and_lock(), but reads a
secondary index record.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_sec_rec_read_check_and_lock(
/*=============================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: secondary index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr);	/*!< in: query thread */
/*********************************************************************//**
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_read_check_and_lock(
/*===============================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: clustered index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr);	/*!< in: query thread */
/*********************************************************************//**
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record. This is an alternative version of
lock_clust_rec_read_check_and_lock() that does not require the parameter
"offsets".
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_read_check_and_lock_alt(
/*===================================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: clustered index */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Locks the specified database table in the mode given. If the lock cannot
be granted immediately, the query thread is put to wait.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_table(
/*=======*/
	dict_table_t*	table,	/*!< in/out: database table
				in dictionary cache */
	lock_mode	mode,	/*!< in: lock mode */
	que_thr_t*	thr)	/*!< in: query thread */
	MY_ATTRIBUTE((warn_unused_result));

/** Create a table lock object for a resurrected transaction.
@param table    table to be X-locked
@param trx      transaction
@param mode     LOCK_X or LOCK_IX */
void lock_table_resurrect(dict_table_t *table, trx_t *trx, lock_mode mode);

/** Release a table X lock after rolling back an insert into an empty table
(which was covered by a TRX_UNDO_EMPTY record).
@param table    table to be X-unlocked
@param trx      transaction */
void lock_table_x_unlock(dict_table_t *table, trx_t *trx);

/** Sets a lock on a table based on the given mode.
@param[in]	table	table to lock
@param[in,out]	trx	transaction
@param[in]	mode	LOCK_X or LOCK_S
@return error code or DB_SUCCESS. */
dberr_t
lock_table_for_trx(
	dict_table_t*	table,
	trx_t*		trx,
	enum lock_mode	mode)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/*************************************************************//**
Removes a granted record lock of a transaction from the queue and grants
locks to other transactions waiting in the queue if they now are entitled
to a lock. */
void
lock_rec_unlock(
/*============*/
	trx_t*			trx,	/*!< in/out: transaction that has
					set a record lock */
	const page_id_t		id,	/*!< in: page containing rec */
	const rec_t*		rec,	/*!< in: record */
	lock_mode		lock_mode);/*!< in: LOCK_S or LOCK_X */

/** Release the explicit locks of a committing transaction,
and release possible other transactions waiting because of these locks. */
void lock_release(trx_t* trx);

/*************************************************************//**
Get the lock hash table */
UNIV_INLINE
hash_table_t*
lock_hash_get(
/*==========*/
	ulint	mode);	/*!< in: lock mode */

/**********************************************************************//**
Looks for a set bit in a record lock bitmap. Returns ULINT_UNDEFINED,
if none found.
@return bit index == heap number of the record, or ULINT_UNDEFINED if
none found */
ulint
lock_rec_find_set_bit(
/*==================*/
	const lock_t*	lock);	/*!< in: record lock with at least one
				bit set */

/*********************************************************************//**
Checks if a lock request lock1 has to wait for request lock2.
@return whether lock1 has to wait for lock2 to be removed */
bool
lock_has_to_wait(
/*=============*/
	const lock_t*	lock1,	/*!< in: waiting lock */
	const lock_t*	lock2);	/*!< in: another lock; NOTE that it is
				assumed that this has a lock bit set
				on the same record as in lock1 if the
				locks are record locks */
/*********************************************************************//**
Reports that a transaction id is insensible, i.e., in the future. */
ATTRIBUTE_COLD
void
lock_report_trx_id_insanity(
/*========================*/
	trx_id_t	trx_id,		/*!< in: trx id */
	const rec_t*	rec,		/*!< in: user record */
	dict_index_t*	index,		/*!< in: index */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index) */
	trx_id_t	max_trx_id);	/*!< in: trx_sys.get_max_trx_id() */
/*********************************************************************//**
Prints info of locks for all transactions.
@return FALSE if not able to acquire lock_sys.latch (and display info) */
ibool
lock_print_info_summary(
/*====================*/
	FILE*	file,	/*!< in: file where to print */
	ibool   nowait)	/*!< in: whether to wait for lock_sys.latch */
	MY_ATTRIBUTE((warn_unused_result));

/** Prints transaction lock wait and MVCC state.
@param[in,out]	file	file where to print
@param[in]	trx	transaction
@param[in]	now	current my_hrtime_coarse() */
void lock_trx_print_wait_and_mvcc_state(FILE *file, const trx_t *trx,
                                        my_hrtime_t now);

/*********************************************************************//**
Prints info of locks for each transaction. This function will release
lock_sys.latch, which the caller must be holding in exclusive mode. */
void
lock_print_info_all_transactions(
/*=============================*/
	FILE*	file);	/*!< in: file where to print */

/*********************************************************************//**
Return the number of table locks for a transaction.
The caller must be holding lock_sys.latch. */
ulint
lock_number_of_tables_locked(
/*=========================*/
	const trx_lock_t*	trx_lock)	/*!< in: transaction locks */
	MY_ATTRIBUTE((warn_unused_result));

/*******************************************************************//**
Gets the id of the table on which the lock is.
@return id of the table */
table_id_t
lock_get_table_id(
/*==============*/
	const lock_t*	lock);	/*!< in: lock */

/** Determine which table a lock is associated with.
@param[in]	lock	the lock
@return name of the table */
const table_name_t&
lock_get_table_name(
	const lock_t*	lock);

/*******************************************************************//**
For a record lock, gets the index on which the lock is.
@return index */
const dict_index_t*
lock_rec_get_index(
/*===============*/
	const lock_t*	lock);	/*!< in: lock */

/*******************************************************************//**
Check if there are any locks (table or rec) against table.
@return TRUE if locks exist */
bool
lock_table_has_locks(
/*=================*/
	const dict_table_t*	table);	/*!< in: check if there are any locks
					held on records in this table or on the
					table itself */

/** Wait for a lock to be released.
@retval DB_DEADLOCK if this transaction was chosen as the deadlock victim
@retval DB_INTERRUPTED if the execution was interrupted by the user
@retval DB_LOCK_WAIT_TIMEOUT if the lock wait timed out
@retval DB_SUCCESS if the lock was granted */
dberr_t lock_wait(que_thr_t *thr);
/*********************************************************************//**
Unlocks AUTO_INC type locks that were possibly reserved by a trx. This
function should be called at the the end of an SQL statement, by the
connection thread that owns the transaction (trx->mysql_thd). */
void
lock_unlock_table_autoinc(
/*======================*/
	trx_t*	trx);			/*!< in/out: transaction */
/*********************************************************************//**
Check whether the transaction has already been rolled back because it
was selected as a deadlock victim, or if it has to wait then cancel
the wait lock.
@return DB_DEADLOCK, DB_LOCK_WAIT or DB_SUCCESS */
dberr_t
lock_trx_handle_wait(
/*=================*/
	trx_t*	trx);	/*!< in/out: trx lock state */

/*********************************************************************//**
Checks that a transaction id is sensible, i.e., not in the future.
@return true if ok */
bool
lock_check_trx_id_sanity(
/*=====================*/
	trx_id_t	trx_id,		/*!< in: trx id */
	const rec_t*	rec,		/*!< in: user record */
	dict_index_t*	index,		/*!< in: index */
	const rec_offs*	offsets);	/*!< in: rec_get_offsets(rec, index) */
#ifdef UNIV_DEBUG
/*******************************************************************//**
Check if the transaction holds any locks on the sys tables
or its records.
@return the strongest lock found on any sys table or 0 for none */
const lock_t*
lock_trx_has_sys_table_locks(
/*=========================*/
	const trx_t*	trx)	/*!< in: transaction to check */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Check if the transaction holds an explicit exclusive lock on a record.
@param[in]	trx	transaction
@param[in]	table	table
@param[in]	id	leaf page identifier
@param[in]	heap_no	heap number identifying the record
@return whether an explicit X-lock is held */
bool lock_trx_has_expl_x_lock(const trx_t &trx, const dict_table_t &table,
                              page_id_t id, ulint heap_no);
#endif /* UNIV_DEBUG */

/** Lock operation struct */
struct lock_op_t{
	dict_table_t*	table;	/*!< table to be locked */
	lock_mode	mode;	/*!< lock mode */
};

/** The lock system struct */
class lock_sys_t
{
  bool m_initialised;

  /** mutex proteting the locks */
  MY_ALIGNED(CACHE_LINE_SIZE) srw_lock latch;
#ifdef UNIV_DEBUG
  /** The owner of exclusive latch (0 if none); protected by latch */
  std::atomic<os_thread_id_t> writer{0};
  /** Number of shared latches */
  std::atomic<ulint> readers{0};
#endif
public:
  /** record locks */
  hash_table_t rec_hash;
  /** predicate locks for SPATIAL INDEX */
  hash_table_t prdt_hash;
  /** page locks for SPATIAL INDEX */
  hash_table_t prdt_page_hash;
  /** number of deadlocks detected; protected by mutex */
  ulint deadlocks;

  /** mutex covering lock waits; @see trx_lock_t::wait_lock */
  MY_ALIGNED(CACHE_LINE_SIZE) mysql_mutex_t wait_mutex;
private:
  /** Pending number of lock waits; protected by wait_mutex */
  ulint wait_pending;
  /** Cumulative number of lock waits; protected by wait_mutex */
  ulint wait_count;
  /** Cumulative wait time; protected by wait_mutex */
  ulint wait_time;
  /** Longest wait time; protected by wait_mutex */
  ulint wait_time_max;
public:
  /**
    Constructor.

    Some members may require late initialisation, thus we just mark object as
    uninitialised. Real initialisation happens in create().
  */
  lock_sys_t(): m_initialised(false) {}


  bool is_initialised() const { return m_initialised; }

#ifdef UNIV_PFS_RWLOCK
  /** Acquire exclusive lock_sys.latch */
  ATTRIBUTE_NOINLINE
  void wr_lock(const char *file, unsigned line);
  /** Release exclusive lock_sys.latch */
  ATTRIBUTE_NOINLINE void wr_unlock();
  /** Acquire shared lock_sys.latch */
  ATTRIBUTE_NOINLINE void rd_lock(const char *file, unsigned line);
  /** Release shared lock_sys.latch */
  ATTRIBUTE_NOINLINE void rd_unlock();
#else
  /** Acquire exclusive lock_sys.latch */
  void wr_lock()
  {
    ut_ad(!is_writer());
    latch.wr_lock();
    ut_ad(!writer.exchange(os_thread_get_curr_id(),
                           std::memory_order_relaxed));
  }
  /** Release exclusive lock_sys.latch */
  void wr_unlock()
  {
    ut_ad(writer.exchange(0, std::memory_order_relaxed) ==
          os_thread_get_curr_id());
    latch.wr_unlock();
  }
  /** Acquire shared lock_sys.latch */
  void rd_lock()
  {
    ut_ad(!is_writer());
    latch.rd_lock();
    ut_ad(!writer.load(std::memory_order_relaxed));
    ut_d(readers.fetch_add(1, std::memory_order_relaxed));
  }
  /** Release shared lock_sys.latch */
  void rd_unlock()
  {
    ut_ad(!is_writer());
    ut_ad(readers.fetch_sub(1, std::memory_order_relaxed));
    latch.rd_unlock();
  }
#endif
  /** Try to acquire exclusive lock_sys.latch
  @return whether the latch was acquired */
  bool wr_lock_try()
  {
    ut_ad(!is_writer());
    if (!latch.wr_lock_try()) return false;
    ut_ad(!writer.exchange(os_thread_get_curr_id(),
                           std::memory_order_relaxed));
    return true;
  }

  /** Assert that wr_lock() has been invoked by this thread */
  void assert_locked() const { ut_ad(is_writer()); }
  /** Assert that wr_lock() has not been invoked by this thread */
  void assert_unlocked() const { ut_ad(!is_writer()); }
#ifdef UNIV_DEBUG
  /** @return whether the current thread is the lock_sys.latch writer */
  bool is_writer() const
  { return writer.load(std::memory_order_relaxed) == os_thread_get_curr_id(); }
  /** Assert that a lock shard is exclusively latched by this thread */
  void assert_locked(const lock_t &lock) const;
  /** Assert that a table lock shard is exclusively latched by this thread */
  void assert_locked(const dict_table_t &table) const;
#else
  void assert_locked(const lock_t &) const {}
  void assert_locked(const dict_table_t &) const {}
#endif

  /**
    Creates the lock system at database start.

    @param[in] n_cells number of slots in lock hash table
  */
  void create(ulint n_cells);


  /**
    Resize the lock hash table.

    @param[in] n_cells number of slots in lock hash table
  */
  void resize(ulint n_cells);


  /** Closes the lock system at database shutdown. */
  void close();


  /** Note that a record lock wait started */
  inline void wait_start();

  /** Note that a record lock wait resumed */
  inline void wait_resume(THD *thd, my_hrtime_t start, my_hrtime_t now);

  /** @return pending number of lock waits */
  ulint get_wait_pending() const { return wait_pending; }
  /** @return cumulative number of lock waits */
  ulint get_wait_cumulative() const { return wait_count; }
  /** Cumulative wait time; protected by wait_mutex */
  ulint get_wait_time_cumulative() const { return wait_time; }
  /** Longest wait time; protected by wait_mutex */
  ulint get_wait_time_max() const { return wait_time_max; }

  /** @return the hash value for a page address */
  ulint hash(const page_id_t id) const
  { assert_locked(); return rec_hash.calc_hash(id.fold()); }

  /** Get the first lock on a page.
  @param lock_hash   hash table to look at
  @param id          page number
  @return first lock
  @retval nullptr if none exists */
  lock_t *get_first(const hash_table_t &lock_hash, const page_id_t id) const
  {
    ut_ad(&lock_hash == &rec_hash || &lock_hash == &prdt_hash ||
          &lock_hash == &prdt_page_hash);
    for (lock_t *lock= static_cast<lock_t*>
         (HASH_GET_FIRST(&lock_hash, hash(id)));
         lock; lock= static_cast<lock_t*>(HASH_GET_NEXT(hash, lock)))
      if (lock->un_member.rec_lock.page_id == id)
         return lock;
    return nullptr;
  }

  /** Get the first record lock on a page.
  @param id          page number
  @return first lock
  @retval nullptr if none exists */
  lock_t *get_first(const page_id_t id) const
  { return get_first(rec_hash, id); }
  /** Get the first predicate lock on a SPATIAL INDEX page.
  @param id          page number
  @return first lock
  @retval nullptr if none exists */
  lock_t *get_first_prdt(const page_id_t id) const
  { return get_first(prdt_hash, id); }
  /** Get the first predicate lock on a SPATIAL INDEX page.
  @param id          page number
  @return first lock
  @retval nullptr if none exists */
  lock_t *get_first_prdt_page(const page_id_t id) const
  { return get_first(prdt_page_hash, id); }
};

/** The lock system */
extern lock_sys_t lock_sys;

/** lock_sys.latch guard */
struct LockMutexGuard
{
  LockMutexGuard(SRW_LOCK_ARGS(const char *file, unsigned line))
  { lock_sys.wr_lock(SRW_LOCK_ARGS(file, line)); }
  ~LockMutexGuard() { lock_sys.wr_unlock(); }
};

/*********************************************************************//**
Creates a new record lock and inserts it to the lock queue. Does NOT check
for deadlocks or lock compatibility!
@return created lock */
UNIV_INLINE
lock_t*
lock_rec_create(
/*============*/
#ifdef WITH_WSREP
	lock_t*			c_lock,	/*!< conflicting lock */
	que_thr_t*		thr,	/*!< thread owning trx */
#endif
	unsigned		type_mode,/*!< in: lock mode and wait flag */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of the record */
	dict_index_t*		index,	/*!< in: index of record */
	trx_t*			trx,	/*!< in,out: transaction */
	bool			caller_owns_trx_mutex);
					/*!< in: true if caller owns
					trx mutex */

/*************************************************************//**
Removes a record lock request, waiting or granted, from the queue. */
void
lock_rec_discard(
/*=============*/
	lock_t*		in_lock);	/*!< in: record lock object: all
					record locks which are contained
					in this lock object are removed */

/** Create a new record lock and inserts it to the lock queue,
without checking for deadlocks or conflicts.
@param[in]	type_mode	lock mode and wait flag
@param[in]	page_id		index page number
@param[in]	page		R-tree index page, or NULL
@param[in]	heap_no		record heap number in the index page
@param[in]	index		the index tree
@param[in,out]	trx		transaction
@param[in]	holds_trx_mutex	whether the caller holds trx->mutex
@return created lock */
lock_t*
lock_rec_create_low(
#ifdef WITH_WSREP
	lock_t*		c_lock,	/*!< conflicting lock */
	que_thr_t*	thr,	/*!< thread owning trx */
#endif
	unsigned	type_mode,
	const page_id_t	page_id,
	const page_t*	page,
	ulint		heap_no,
	dict_index_t*	index,
	trx_t*		trx,
	bool		holds_trx_mutex);

/** Enqueue a waiting request for a lock which cannot be granted immediately.
Check for deadlocks.
@param[in]	type_mode	the requested lock mode (LOCK_S or LOCK_X)
				possibly ORed with LOCK_GAP or
				LOCK_REC_NOT_GAP, ORed with
				LOCK_INSERT_INTENTION if this
				waiting lock request is set
				when performing an insert of
				an index record
@param[in]	id		page identifier
@param[in]	page		leaf page in the index
@param[in]	heap_no		record heap number in the block
@param[in]	index		index tree
@param[in,out]	thr		query thread
@param[in]	prdt		minimum bounding box (spatial index)
@retval	DB_LOCK_WAIT		if the waiting lock was enqueued
@retval	DB_DEADLOCK		if this transaction was chosen as the victim */
dberr_t
lock_rec_enqueue_waiting(
#ifdef WITH_WSREP
	lock_t*			c_lock,	/*!< conflicting lock */
#endif
	unsigned		type_mode,
	const page_id_t		id,
	const page_t*		page,
	ulint			heap_no,
	dict_index_t*		index,
	que_thr_t*		thr,
	lock_prdt_t*		prdt);
/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
void
lock_rtr_move_rec_list(
/*===================*/
	const buf_block_t*	new_block,	/*!< in: index page to
						move to */
	const buf_block_t*	block,		/*!< in: index page */
	rtr_rec_move_t*		rec_move,	/*!< in: recording records
						moved */
	ulint			num_move);	/*!< in: num of rec to move */

/** Remove record locks for an index page which is discarded. This
function does not move locks, or check for waiting locks, therefore the
lock bitmaps must already be reset when this function is called. */
void lock_rec_free_all_from_discard_page(const page_id_t page_id);

/** Cancel a waiting lock request and release possibly waiting transactions */
void lock_cancel_waiting_and_release(lock_t *lock);

#include "lock0lock.ic"

#endif
