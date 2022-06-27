/*****************************************************************************

Copyright (c) 1996, 2022, Oracle and/or its affiliates.
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
@file include/lock0lock.h
The transaction lock system

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#ifndef lock0lock_h
#define lock0lock_h

#include "buf0types.h"
#include "trx0trx.h"
#include "mtr0types.h"
#include "rem0types.h"
#include "hash0hash.h"
#include "srv0srv.h"
#include "ut0vec.h"
#include "gis0rtree.h"
#include "lock0prdt.h"
#include "transactional_lock_guard.h"

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

/** Discard locks for an index when purging DELETE FROM SYS_INDEXES
after an aborted CREATE INDEX operation.
@param index   a stale index on which ADD INDEX operation was aborted */
ATTRIBUTE_COLD void lock_discard_for_index(const dict_index_t &index);

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

/** Update gap locks between the last record of the left_block and the
first record of the right_block when a record is about to be inserted
at the start of the right_block, even though it should "naturally" be
inserted as the last record of the left_block according to the
current node pointer in the parent page.

That is, we assume that the lowest common ancestor of the left_block
and right_block routes the key of the new record to the left_block,
but a heuristic which tries to avoid overflowing left_block has chosen
to insert the record into right_block instead. Said ancestor performs
this routing by comparing the key of the record to a "split point" -
all records greater or equal to than the split point (node pointer)
are in right_block, and smaller ones in left_block.
The split point may be smaller than the smallest key in right_block.

The gap between the last record on the left_block and the first record
on the right_block is represented as a gap lock attached to the supremum
pseudo-record of left_block, and a gap lock attached to the new first
record of right_block.

Thus, inserting the new record, and subsequently adjusting the node
pointers in parent pages to values smaller or equal to the new
records' key, will mean that gap will be sliced at a different place
("moved to the left"): fragment of the 1st gap will now become treated
as 2nd. Therefore, we must copy any GRANTED locks from 1st gap to the
2nd gap. Any WAITING locks must be of INSERT_INTENTION type (as no
other GAP locks ever wait for anything) and can stay at 1st gap, as
their only purpose is to notify the requester they can retry
insertion, and there's no correctness requirement to avoid waking them
up too soon.
@param left_block   left page
@param right_block  right page */
void lock_update_node_pointer(const buf_block_t *left_block,
                              const buf_block_t *right_block);
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

/** Update the locks when a page is split and merged to two pages,
in defragmentation. */
void lock_update_split_and_merge(
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

/**
Create a table lock, without checking for deadlocks or lock compatibility.
@param table      table on which the lock is created
@param type_mode  lock type and mode
@param trx        transaction
@param c_lock     conflicting lock
@return the created lock object */
lock_t *lock_table_create(dict_table_t *table, unsigned type_mode, trx_t *trx,
                          lock_t *c_lock= nullptr);

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

/** Acquire a table lock.
@param table   table to be locked
@param fktable pointer to table, in case of a FOREIGN key check
@param mode    lock mode
@param thr     SQL execution thread
@retval DB_SUCCESS    if the lock was acquired
@retval DB_DEADLOCK   if a deadlock occurred, or fktable && *fktable != table
@retval DB_LOCK_WAIT  if lock_wait() must be invoked */
dberr_t lock_table(dict_table_t *table, dict_table_t *const*fktable,
                   lock_mode mode, que_thr_t *thr)
  MY_ATTRIBUTE((warn_unused_result));

/** Create a table lock object for a resurrected transaction.
@param table    table to be X-locked
@param trx      transaction
@param mode     LOCK_X or LOCK_IX */
void lock_table_resurrect(dict_table_t *table, trx_t *trx, lock_mode mode);

/** Sets a lock on a table based on the given mode.
@param table	table to lock
@param trx	transaction
@param mode	LOCK_X or LOCK_S
@param no_wait  whether to skip handling DB_LOCK_WAIT
@return error code */
dberr_t lock_table_for_trx(dict_table_t *table, trx_t *trx, lock_mode mode,
                           bool no_wait= false)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Exclusively lock the data dictionary tables.
@param trx  dictionary transaction
@return error code
@retval DB_SUCCESS on success */
dberr_t lock_sys_tables(trx_t *trx);

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

/** Release the explicit locks of a committing transaction while
dict_sys.latch is exclusively locked,
and release possible other transactions waiting because of these locks. */
void lock_release_on_drop(trx_t *trx);

/** Release non-exclusive locks on XA PREPARE,
and release possible other transactions waiting because of these locks. */
void lock_release_on_prepare(trx_t *trx);

/** Release locks on a table whose creation is being rolled back */
ATTRIBUTE_COLD void lock_release_on_rollback(trx_t *trx, dict_table_t *table);

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
  friend struct TMLockGuard;
  friend struct TMLockMutexGuard;
  friend struct TMLockTrxGuard;

  /** Hash table latch */
  struct hash_latch
#ifdef SUX_LOCK_GENERIC
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
    /** @return whether any lock is being held or waited for by any thread */
    bool is_locked_or_waiting() const
    { return rw_lock::is_locked_or_waiting(); }
    /** @return whether this latch is possibly held by any thread */
    bool is_locked() const { return rw_lock::is_locked(); }
#else
  {
  private:
    srw_spin_lock_low lock;
  public:
    /** Try to acquire a lock */
    bool try_acquire() { return lock.wr_lock_try(); }
    /** Acquire a lock */
    void acquire() { lock.wr_lock(); }
    /** Release a lock */
    void release() { lock.wr_unlock(); }
    /** @return whether any lock may be held by any thread */
    bool is_locked_or_waiting() const noexcept
    { return lock.is_locked_or_waiting(); }
    /** @return whether this latch is possibly held by any thread */
    bool is_locked() const noexcept { return lock.is_locked(); }
#endif
  };

public:
  struct hash_table
  {
    /** Number of consecutive array[] elements occupied by a hash_latch */
    static constexpr size_t LATCH= sizeof(void*) >= sizeof(hash_latch) ? 1 : 2;
    static_assert(sizeof(hash_latch) <= LATCH * sizeof(void*), "allocation");

    /** Number of array[] elements per hash_latch.
    Must be LATCH less than a power of 2. */
    static constexpr size_t ELEMENTS_PER_LATCH= (64 / sizeof(void*)) - LATCH;
    static constexpr size_t EMPTY_SLOTS_PER_LATCH=
      ((CPU_LEVEL1_DCACHE_LINESIZE / 64) - 1) * (64 / sizeof(void*));

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
    static ulint pad(ulint h)
    {
      ulint latches= LATCH * (h / ELEMENTS_PER_LATCH);
      ulint empty_slots= (h / ELEMENTS_PER_LATCH) * EMPTY_SLOTS_PER_LATCH;
      return LATCH + latches + empty_slots + h;
    }

    /** Get a latch. */
    static hash_latch *latch(hash_cell_t *cell)
    {
      void *l= ut_align_down(cell, sizeof *cell *
                             (ELEMENTS_PER_LATCH + LATCH));
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
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) srw_spin_lock latch;
#ifdef UNIV_DEBUG
  /** The owner of exclusive latch (0 if none); protected by latch */
  std::atomic<pthread_t> writer{0};
  /** Number of shared latches */
  std::atomic<ulint> readers{0};
#endif
#ifdef SUX_LOCK_GENERIC
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
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t wait_mutex;
private:
  /** The increment of wait_count for a wait. Anything smaller is a
  pending wait count. */
  static constexpr uint64_t WAIT_COUNT_STEP= 1U << 19;
  /** waits and total number of lock waits; protected by wait_mutex */
  uint64_t wait_count;
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
    mysql_mutex_assert_not_owner(&wait_mutex);
    ut_ad(!is_writer());
    latch.wr_lock();
    ut_ad(!writer.exchange(pthread_self(),
                           std::memory_order_relaxed));
  }
  /** Release exclusive lock_sys.latch */
  void wr_unlock()
  {
    ut_ad(writer.exchange(0, std::memory_order_relaxed) ==
          pthread_self());
    latch.wr_unlock();
  }
  /** Acquire shared lock_sys.latch */
  void rd_lock()
  {
    mysql_mutex_assert_not_owner(&wait_mutex);
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
    ut_ad(!writer.exchange(pthread_self(),
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
  {
# ifdef SUX_LOCK_GENERIC
    return writer.load(std::memory_order_relaxed) == pthread_self();
# else
    return writer.load(std::memory_order_relaxed) == pthread_self() ||
      (xtest() && !latch.is_locked_or_waiting());
# endif
  }
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
  @tparam check_victim  whether to check for DB_DEADLOCK
  @param lock           waiting lock request
  @param trx            active transaction
  @retval DB_SUCCESS    if no lock existed
  @retval DB_DEADLOCK   if trx->lock.was_chosen_as_deadlock_victim was set
  @retval DB_LOCK_WAIT  if the lock was canceled */
  template<bool check_victim>
  static dberr_t cancel(trx_t *trx, lock_t *lock);
  /** Cancel a waiting lock request (if any) when killing a transaction */
  static void cancel(trx_t *trx);

  /** Note that a record lock wait started */
  inline void wait_start();

  /** Note that a record lock wait resumed */
  inline void wait_resume(THD *thd, my_hrtime_t start, my_hrtime_t now);

  /** @return pending number of lock waits */
  ulint get_wait_pending() const
  {
    return static_cast<ulint>(wait_count & (WAIT_COUNT_STEP - 1));
  }
  /** @return cumulative number of lock waits */
  ulint get_wait_cumulative() const
  { return static_cast<ulint>(wait_count / WAIT_COUNT_STEP); }
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

  /** Cancel possible lock waiting for a transaction */
  static void cancel_lock_wait_for_trx(trx_t *trx);
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

/** lock_sys.latch exclusive guard using transactional memory */
struct TMLockMutexGuard
{
  TRANSACTIONAL_INLINE
  TMLockMutexGuard(SRW_LOCK_ARGS(const char *file, unsigned line))
  {
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
    if (xbegin())
    {
      if (was_elided())
        return;
      xabort();
    }
#endif
    lock_sys.wr_lock(SRW_LOCK_ARGS(file, line));
  }
  TRANSACTIONAL_INLINE
  ~TMLockMutexGuard()
  {
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
    if (was_elided()) xend(); else
#endif
    lock_sys.wr_unlock();
  }

#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  bool was_elided() const noexcept
  { return !lock_sys.latch.is_locked_or_waiting(); }
#else
  bool was_elided() const noexcept { return false; }
#endif
};

/** lock_sys latch guard for 1 page_id_t, using transactional memory */
struct TMLockGuard
{
  TRANSACTIONAL_TARGET
  TMLockGuard(lock_sys_t::hash_table &hash, const page_id_t id);
  TRANSACTIONAL_INLINE ~TMLockGuard()
  {
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
    if (elided)
    {
      xend();
      return;
    }
#endif
    lock_sys_t::hash_table::latch(cell_)->release();
    /* Must be last, to avoid a race with lock_sys_t::hash_table::resize() */
    lock_sys.rd_unlock();
  }
  /** @return the hash array cell */
  hash_cell_t &cell() const { return *cell_; }
private:
  /** The hash array cell */
  hash_cell_t *cell_;
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  /** whether the latches were elided */
  bool elided;
#endif
};

/** guard for shared lock_sys.latch and trx_t::mutex using
transactional memory */
struct TMLockTrxGuard
{
  trx_t &trx;

  TRANSACTIONAL_INLINE
#ifndef UNIV_PFS_RWLOCK
  TMLockTrxGuard(trx_t &trx) : trx(trx)
# define TMLockTrxArgs(trx) trx
#else
  TMLockTrxGuard(const char *file, unsigned line, trx_t &trx) : trx(trx)
# define TMLockTrxArgs(trx) SRW_LOCK_CALL, trx
#endif
  {
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
    if (xbegin())
    {
      if (!lock_sys.latch.is_write_locked() && was_elided())
        return;
      xabort();
    }
#endif
    lock_sys.rd_lock(SRW_LOCK_ARGS(file, line));
    trx.mutex_lock();
  }
  TRANSACTIONAL_INLINE
  ~TMLockTrxGuard()
  {
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
    if (was_elided())
    {
      xend();
      return;
    }
#endif
    lock_sys.rd_unlock();
    trx.mutex_unlock();
  }
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  bool was_elided() const noexcept { return !trx.mutex_is_locked(); }
#else
  bool was_elided() const noexcept { return false; }
#endif
};

/** guard for trx_t::mutex using transactional memory */
struct TMTrxGuard
{
  trx_t &trx;

  TRANSACTIONAL_INLINE TMTrxGuard(trx_t &trx) : trx(trx)
  {
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
    if (xbegin())
    {
      if (was_elided())
        return;
      xabort();
    }
#endif
    trx.mutex_lock();
  }
  TRANSACTIONAL_INLINE ~TMTrxGuard()
  {
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
    if (was_elided())
    {
      xend();
      return;
    }
#endif
    trx.mutex_unlock();
  }
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  bool was_elided() const noexcept { return !trx.mutex_is_locked(); }
#else
  bool was_elided() const noexcept { return false; }
#endif
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

#include "lock0lock.inl"

#endif
