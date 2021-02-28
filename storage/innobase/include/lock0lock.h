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
extern my_bool innodb_deadlock_detect;
/** The value of innodb_deadlock_report */
extern ulong innodb_deadlock_report;

namespace Deadlock
{
  /** The allowed values of innodb_deadlock_report */
  enum report { REPORT_OFF, REPORT_BASIC, REPORT_FULL };
}

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

/** Check if there are any locks on a table.
@return true if table has either table or record locks. */
bool lock_table_has_locks(dict_table_t *table);

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

/** Handle a pending lock wait (DB_LOCK_WAIT) in a semi-consistent read
while holding a clustered index leaf page latch.
@param trx           transaction that is or was waiting for a lock
@retval DB_SUCCESS   if the lock was granted
@retval DB_DEADLOCK  if the transaction must be aborted due to a deadlock
@retval DB_LOCK_WAIT if a lock wait would be necessary; the pending
                     lock request was released */
dberr_t lock_trx_handle_wait(trx_t *trx);

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
  friend struct LockGuard;
  friend struct LockMultiGuard;

  /** Hash table latch */
  struct hash_latch
#if defined SRW_LOCK_DUMMY && !defined _WIN32
  : private rw_lock
  {
    /** Wait for an exclusive lock */
    void wait();
    /** Try to acquire a lock */
    bool try_acquire() { return write_trylock(); }
    /** Acquire a lock */
    void acquire() { if (!try_acquire()) wait(); }
    /** Release a lock */
    void release();
#else
  {
  private:
    srw_lock_low lock;
  public:
    /** Try to acquire a lock */
    bool try_acquire() { return lock.wr_lock_try(); }
    /** Acquire a lock */
    void acquire() { lock.wr_lock(); }
    /** Release a lock */
    void release() { lock.wr_unlock(); }
#endif
#ifdef UNIV_DEBUG
    /** @return whether this latch is possibly held by any thread */
    bool is_locked() const
    { return memcmp(this, field_ref_zero, sizeof *this); }
#endif
  };
  static_assert(sizeof(hash_latch) <= sizeof(void*), "compatibility");

public:
  struct hash_table
  {
    /** Number of array[] elements per hash_latch.
    Must be one less than a power of 2. */
    static constexpr size_t ELEMENTS_PER_LATCH= CPU_LEVEL1_DCACHE_LINESIZE /
      sizeof(void*) - 1;

    /** number of payload elements in array[]. Protected by lock_sys.latch. */
    ulint n_cells;
    /** the hash table, with pad(n_cells) elements, aligned to L1 cache size;
    in any hash chain, lock_t::is_waiting() entries must not precede
    granted locks */
    hash_cell_t *array;

    /** Create the hash table.
    @param n  the lower bound of n_cells */
    void create(ulint n);

    /** Resize the hash table.
    @param n  the lower bound of n_cells */
    void resize(ulint n);

    /** Free the hash table. */
    void free() { aligned_free(array); array= nullptr; }

    /** @return the index of an array element */
    inline ulint calc_hash(ulint fold) const;
    /** @return raw array index converted to padded index */
    static ulint pad(ulint h) { return 1 + (h / ELEMENTS_PER_LATCH) + h; }
    /** Get a latch. */
    static hash_latch *latch(hash_cell_t *cell)
    {
      void *l= ut_align_down(cell, (ELEMENTS_PER_LATCH + 1) * sizeof *cell);
      return static_cast<hash_latch*>(l);
    }
    /** Get a hash table cell. */
    inline hash_cell_t *cell_get(ulint fold) const;

#ifdef UNIV_DEBUG
    void assert_locked(const page_id_t id) const;
#else
    void assert_locked(const page_id_t) const {}
#endif

  private:
    /** @return the hash value before any ELEMENTS_PER_LATCH padding */
    static ulint hash(ulint fold, ulint n) { return ut_hash_ulint(fold, n); }

    /** @return the index of an array element */
    static ulint calc_hash(ulint fold, ulint n_cells)
    {
      return pad(hash(fold, n_cells));
    }
  };

private:
  bool m_initialised;

  /** mutex proteting the locks */
  MY_ALIGNED(CPU_LEVEL1_DCACHE_LINESIZE) srw_lock latch;
#ifdef UNIV_DEBUG
  /** The owner of exclusive latch (0 if none); protected by latch */
  std::atomic<os_thread_id_t> writer{0};
  /** Number of shared latches */
  std::atomic<ulint> readers{0};
#endif
#if defined SRW_LOCK_DUMMY && !defined _WIN32
protected:
  /** mutex for hash_latch::wait() */
  pthread_mutex_t hash_mutex;
  /** condition variable for hash_latch::wait() */
  pthread_cond_t hash_cond;
#endif
public:
  /** record locks */
  hash_table rec_hash;
  /** predicate locks for SPATIAL INDEX */
  hash_table prdt_hash;
  /** page locks for SPATIAL INDEX */
  hash_table prdt_page_hash;

  /** mutex covering lock waits; @see trx_lock_t::wait_lock */
  MY_ALIGNED(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t wait_mutex;
private:
  /** Cumulative number of lock waits; protected by wait_mutex */
  ulint wait_count;
  /** Pending number of lock waits; protected by wait_mutex */
  uint32_t wait_pending;
  /** Cumulative wait time; protected by wait_mutex */
  uint32_t wait_time;
  /** Longest wait time; protected by wait_mutex */
  uint32_t wait_time_max;
public:
  /** number of deadlocks detected; protected by wait_mutex */
  ulint deadlocks;
  /** number of lock wait timeouts; protected by wait_mutex */
  ulint timeouts;
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
  /** Try to acquire shared lock_sys.latch
  @return whether the latch was acquired */
  bool rd_lock_try()
  {
    ut_ad(!is_writer());
    if (!latch.rd_lock_try()) return false;
    ut_ad(!writer.load(std::memory_order_relaxed));
    ut_d(readers.fetch_add(1, std::memory_order_relaxed));
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
  /** Assert that a lock shard is exclusively latched (by some thread) */
  void assert_locked(const lock_t &lock) const;
  /** Assert that a table lock shard is exclusively latched by this thread */
  void assert_locked(const dict_table_t &table) const;
  /** Assert that a hash table cell is exclusively latched (by some thread) */
  void assert_locked(const hash_cell_t &cell) const;
#else
  void assert_locked(const lock_t &) const {}
  void assert_locked(const dict_table_t &) const {}
  void assert_locked(const hash_cell_t &) const {}
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


  /** Check for deadlocks while holding only lock_sys.wait_mutex. */
  void deadlock_check();

  /** Cancel a waiting lock request.
  @param lock   waiting lock request
  @param trx    active transaction */
  static void cancel(trx_t *trx, lock_t *lock);
  /** Cancel a waiting lock request (if any) when killing a transaction */
  static void cancel(trx_t *trx);

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

  /** Get the lock hash table for a mode */
  hash_table &hash_get(ulint mode)
  {
    if (UNIV_LIKELY(!(mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE))))
      return rec_hash;
    return (mode & LOCK_PREDICATE) ? prdt_hash : prdt_page_hash;
  }

  /** Get the lock hash table for predicate a mode */
  hash_table &prdt_hash_get(bool page)
  { return page ? prdt_page_hash : prdt_hash; }

  /** Get the first lock on a page.
  @param cell        hash table cell
  @param id          page number
  @return first lock
  @retval nullptr if none exists */
  static inline lock_t *get_first(const hash_cell_t &cell, page_id_t id);

  /** Get the first explicit lock request on a record.
  @param cell     first lock hash table cell
  @param id       page identifier
  @param heap_no  record identifier in page
  @return first lock
  @retval nullptr if none exists */
  static inline lock_t *get_first(const hash_cell_t &cell, page_id_t id,
                                  ulint heap_no);

  /** Remove locks on a discarded SPATIAL INDEX page.
  @param id   page to be discarded
  @param page whether to discard also from lock_sys.prdt_hash */
  void prdt_page_free_from_discard(const page_id_t id, bool all= false);
};

/** The lock system */
extern lock_sys_t lock_sys;

/** @return the index of an array element */
inline ulint lock_sys_t::hash_table::calc_hash(ulint fold) const
{
  ut_ad(lock_sys.is_writer() || lock_sys.readers);
  return calc_hash(fold, n_cells);
}

/** Get a hash table cell. */
inline hash_cell_t *lock_sys_t::hash_table::cell_get(ulint fold) const
{
  ut_ad(lock_sys.is_writer() || lock_sys.readers);
  return &array[calc_hash(fold)];
}

/** Get the first lock on a page.
@param cell        hash table cell
@param id          page number
@return first lock
@retval nullptr if none exists */
inline lock_t *lock_sys_t::get_first(const hash_cell_t &cell, page_id_t id)
{
  lock_sys.assert_locked(cell);
  for (auto lock= static_cast<lock_t*>(cell.node); lock; lock= lock->hash)
  {
    ut_ad(!lock->is_table());
    if (lock->un_member.rec_lock.page_id == id)
      return lock;
  }
  return nullptr;
}

/** lock_sys.latch exclusive guard */
struct LockMutexGuard
{
  LockMutexGuard(SRW_LOCK_ARGS(const char *file, unsigned line))
  { lock_sys.wr_lock(SRW_LOCK_ARGS(file, line)); }
  ~LockMutexGuard() { lock_sys.wr_unlock(); }
};

/** lock_sys latch guard for 1 page_id_t */
struct LockGuard
{
  LockGuard(lock_sys_t::hash_table &hash, const page_id_t id);
  ~LockGuard()
  {
    lock_sys_t::hash_table::latch(cell_)->release();
    /* Must be last, to avoid a race with lock_sys_t::hash_table::resize() */
    lock_sys.rd_unlock();
  }
  /** @return the hash array cell */
  hash_cell_t &cell() const { return *cell_; }
private:
  /** The hash array cell */
  hash_cell_t *cell_;
};

/** lock_sys latch guard for 2 page_id_t */
struct LockMultiGuard
{
  LockMultiGuard(lock_sys_t::hash_table &hash,
                 const page_id_t id1, const page_id_t id2);
  ~LockMultiGuard();

  /** @return the first hash array cell */
  hash_cell_t &cell1() const { return *cell1_; }
  /** @return the second hash array cell */
  hash_cell_t &cell2() const { return *cell2_; }
private:
  /** The first hash array cell */
  hash_cell_t *cell1_;
  /** The second hash array cell */
  hash_cell_t *cell2_;
};

/*********************************************************************//**
Creates a new record lock and inserts it to the lock queue. Does NOT check
for deadlocks or lock compatibility!
@return created lock */
UNIV_INLINE
lock_t*
lock_rec_create(
/*============*/
	lock_t*			c_lock,	/*!< conflicting lock */
	unsigned		type_mode,/*!< in: lock mode and wait flag */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of the record */
	dict_index_t*		index,	/*!< in: index of record */
	trx_t*			trx,	/*!< in,out: transaction */
	bool			caller_owns_trx_mutex);
					/*!< in: true if caller owns
					trx mutex */

/** Remove a record lock request, waiting or granted, on a discarded page
@param hash     hash table
@param in_lock  lock object */
void lock_rec_discard(lock_sys_t::hash_table &lock_hash, lock_t *in_lock);

/** Create a new record lock and inserts it to the lock queue,
without checking for deadlocks or conflicts.
@param[in]	c_lock		conflicting lock, or NULL
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
	lock_t*		c_lock,
	unsigned	type_mode,
	const page_id_t	page_id,
	const page_t*	page,
	ulint		heap_no,
	dict_index_t*	index,
	trx_t*		trx,
	bool		holds_trx_mutex);

/** Enqueue a waiting request for a lock which cannot be granted immediately.
Check for deadlocks.
@param[in]	c_lock		conflicting lock
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
	lock_t*			c_lock,
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

#include "lock0lock.ic"

#endif
