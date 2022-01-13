/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2021, MariaDB Corporation.

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
@file include/trx0trx.h
The transaction

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#ifndef trx0trx_h
#define trx0trx_h

#include "trx0types.h"
#include "lock0types.h"
#include "que0types.h"
#include "mem0mem.h"
#include "trx0xa.h"
#include "ut0vec.h"
#include "fts0fts.h"

#include <vector>
#include <set>

// Forward declaration
struct mtr_t;
class ReadView;
class FlushObserver;
class ut_stage_alter_t;

/******************************************************************//**
Set detailed error message for the transaction. */
void
trx_set_detailed_error(
/*===================*/
	trx_t*		trx,	/*!< in: transaction struct */
	const char*	msg);	/*!< in: detailed error message */
/*************************************************************//**
Set detailed error message for the transaction from a file. Note that the
file is rewinded before reading from it. */
void
trx_set_detailed_error_from_file(
/*=============================*/
	trx_t*	trx,	/*!< in: transaction struct */
	FILE*	file);	/*!< in: file to read message from */
/****************************************************************//**
Retrieves the error_info field from a trx.
@return the error info */
UNIV_INLINE
const dict_index_t*
trx_get_error_info(
/*===============*/
	const trx_t*	trx);	/*!< in: trx object */
/********************************************************************//**
Creates a transaction object for MySQL.
@return own: transaction object */
trx_t*
trx_allocate_for_mysql(void);
/*========================*/

/** @return allocated transaction object for internal operations */
trx_t *trx_allocate_for_background();

/** Frees and initialize a transaction object instantinated during recovery.
@param trx trx object to free and initialize during recovery */
void
trx_free_resurrected(trx_t* trx);

/** Free a transaction that was allocated by background or user threads.
@param trx trx object to free */
void
trx_free_for_background(trx_t* trx);

/********************************************************************//**
At shutdown, frees a transaction object that is in the PREPARED state. */
void
trx_free_prepared(
/*==============*/
	trx_t*	trx);	/*!< in, own: trx object */

/** Free a transaction object for MySQL.
@param[in,out]	trx	transaction */
void
trx_free_for_mysql(trx_t*	trx);

/** Disconnect a transaction from MySQL.
@param[in,out]	trx	transaction */
void
trx_disconnect_plain(trx_t*	trx);

/** Disconnect a prepared transaction from MySQL.
@param[in,out]	trx	transaction */
void
trx_disconnect_prepared(trx_t*	trx);

/** Initialize (resurrect) transactions at startup. */
void
trx_lists_init_at_db_start();

/*************************************************************//**
Starts the transaction if it is not yet started. */
void
trx_start_if_not_started_xa_low(
/*============================*/
	trx_t*	trx,		/*!< in/out: transaction */
	bool	read_write);	/*!< in: true if read write transaction */
/*************************************************************//**
Starts the transaction if it is not yet started. */
void
trx_start_if_not_started_low(
/*=========================*/
	trx_t*	trx,		/*!< in/out: transaction */
	bool	read_write);	/*!< in: true if read write transaction */

/*************************************************************//**
Starts a transaction for internal processing. */
void
trx_start_internal_low(
/*===================*/
	trx_t*	trx);		/*!< in/out: transaction */

/** Starts a read-only transaction for internal processing.
@param[in,out] trx	transaction to be started */
void
trx_start_internal_read_only_low(
	trx_t*	trx);

#ifdef UNIV_DEBUG
#define trx_start_if_not_started_xa(t, rw)			\
	do {							\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_if_not_started_xa_low((t), rw);		\
	} while (false)

#define trx_start_if_not_started(t, rw)				\
	do {							\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_if_not_started_low((t), rw);			\
	} while (false)

#define trx_start_internal(t)					\
	do {							\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_internal_low((t));				\
	} while (false)

#define trx_start_internal_read_only(t)				\
	do {							\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_internal_read_only_low(t);			\
	} while (false)
#else
#define trx_start_if_not_started(t, rw)				\
	trx_start_if_not_started_low((t), rw)

#define trx_start_internal(t)					\
	trx_start_internal_low((t))

#define trx_start_internal_read_only(t)				\
	trx_start_internal_read_only_low(t)

#define trx_start_if_not_started_xa(t, rw)			\
	trx_start_if_not_started_xa_low((t), (rw))
#endif /* UNIV_DEBUG */

/*************************************************************//**
Starts the transaction for a DDL operation. */
void
trx_start_for_ddl_low(
/*==================*/
	trx_t*		trx,	/*!< in/out: transaction */
	trx_dict_op_t	op);	/*!< in: dictionary operation type */

#ifdef UNIV_DEBUG
#define trx_start_for_ddl(t, o)					\
	do {							\
	ut_ad((t)->start_file == 0);				\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_for_ddl_low((t), (o));			\
	} while (0)
#else
#define trx_start_for_ddl(t, o)					\
	trx_start_for_ddl_low((t), (o))
#endif /* UNIV_DEBUG */

/****************************************************************//**
Commits a transaction. */
void
trx_commit(
/*=======*/
	trx_t*	trx);	/*!< in/out: transaction */

/****************************************************************//**
Commits a transaction and a mini-transaction. */
void
trx_commit_low(
/*===========*/
	trx_t*	trx,	/*!< in/out: transaction */
	mtr_t*	mtr);	/*!< in/out: mini-transaction (will be committed),
			or NULL if trx made no modifications */
/****************************************************************//**
Cleans up a transaction at database startup. The cleanup is needed if
the transaction already got to the middle of a commit when the database
crashed, and we cannot roll it back. */
void
trx_cleanup_at_db_startup(
/*======================*/
	trx_t*	trx);	/*!< in: transaction */
/**********************************************************************//**
Does the transaction commit for MySQL.
@return DB_SUCCESS or error number */
dberr_t
trx_commit_for_mysql(
/*=================*/
	trx_t*	trx);	/*!< in/out: transaction */
/** XA PREPARE a transaction.
@param[in,out]	trx	transaction to prepare */
void trx_prepare_for_mysql(trx_t* trx);
/**********************************************************************//**
This function is used to find number of prepared transactions and
their transaction objects for a recovery.
@return number of prepared transactions */
int
trx_recover_for_mysql(
/*==================*/
	XID*	xid_list,	/*!< in/out: prepared transactions */
	ulint	len);		/*!< in: number of slots in xid_list */
/** Look up an X/Open distributed transaction in XA PREPARE state.
@param[in]	xid	X/Open XA transaction identifier
@return	transaction on match (the trx_t::xid will be invalidated);
note that the trx may have been committed before the caller acquires
trx_t::mutex
@retval	NULL if no match */
trx_t* trx_get_trx_by_xid(const XID* xid);
/**********************************************************************//**
If required, flushes the log to disk if we called trx_commit_for_mysql()
with trx->flush_log_later == TRUE. */
void
trx_commit_complete_for_mysql(
/*==========================*/
	trx_t*	trx);	/*!< in/out: transaction */
/**********************************************************************//**
Marks the latest SQL statement ended. */
void
trx_mark_sql_stat_end(
/*==================*/
	trx_t*	trx);	/*!< in: trx handle */
/********************************************************************//**
Assigns a read view for a consistent read query. All the consistent reads
within the same transaction will get the same read view, which is created
when this function is first called for a new started transaction. */
ReadView*
trx_assign_read_view(
/*=================*/
	trx_t*	trx);	/*!< in: active transaction */

/****************************************************************//**
@return the transaction's read view or NULL if one not assigned. */
UNIV_INLINE
ReadView*
trx_get_read_view(
/*==============*/
	trx_t*	trx);

/****************************************************************//**
@return the transaction's read view or NULL if one not assigned. */
UNIV_INLINE
const ReadView*
trx_get_read_view(
/*==============*/
	const trx_t*	trx);

/****************************************************************//**
Prepares a transaction for commit/rollback. */
void
trx_commit_or_rollback_prepare(
/*===========================*/
	trx_t*	trx);	/*!< in/out: transaction */
/*********************************************************************//**
Creates a commit command node struct.
@return own: commit node struct */
commit_node_t*
trx_commit_node_create(
/*===================*/
	mem_heap_t*	heap);	/*!< in: mem heap where created */
/***********************************************************//**
Performs an execution step for a commit type node in a query graph.
@return query thread to run next, or NULL */
que_thr_t*
trx_commit_step(
/*============*/
	que_thr_t*	thr);	/*!< in: query thread */

/**********************************************************************//**
Prints info about a transaction.
Caller must hold trx_sys->mutex. */
void
trx_print_low(
/*==========*/
	FILE*		f,
			/*!< in: output stream */
	const trx_t*	trx,
			/*!< in: transaction */
	ulint		max_query_len,
			/*!< in: max query length to print,
			or 0 to use the default max length */
	ulint		n_rec_locks,
			/*!< in: lock_number_of_rows_locked(&trx->lock) */
	ulint		n_trx_locks,
			/*!< in: length of trx->lock.trx_locks */
	ulint		heap_size);
			/*!< in: mem_heap_get_size(trx->lock.lock_heap) */

/**********************************************************************//**
Prints info about a transaction.
The caller must hold lock_sys->mutex and trx_sys->mutex.
When possible, use trx_print() instead. */
void
trx_print_latched(
/*==============*/
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx,		/*!< in: transaction */
	ulint		max_query_len);	/*!< in: max query length to print,
					or 0 to use the default max length */

#ifdef WITH_WSREP
/**********************************************************************//**
Prints info about a transaction.
Transaction information may be retrieved without having trx_sys->mutex acquired
so it may not be completely accurate. The caller must own lock_sys->mutex
and the trx must have some locks to make sure that it does not escape
without locking lock_sys->mutex. */
UNIV_INTERN
void
wsrep_trx_print_locking(
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx,		/*!< in: transaction */
	ulint		max_query_len)	/*!< in: max query length to print,
					or 0 to use the default max length */
	MY_ATTRIBUTE((nonnull));
#endif /* WITH_WSREP */
/**********************************************************************//**
Prints info about a transaction.
Acquires and releases lock_sys->mutex and trx_sys->mutex. */
void
trx_print(
/*======*/
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx,		/*!< in: transaction */
	ulint		max_query_len);	/*!< in: max query length to print,
					or 0 to use the default max length */

/**********************************************************************//**
Determine if a transaction is a dictionary operation.
@return dictionary operation mode */
UNIV_INLINE
enum trx_dict_op_t
trx_get_dict_operation(
/*===================*/
	const trx_t*	trx)	/*!< in: transaction */
	MY_ATTRIBUTE((warn_unused_result));
/**********************************************************************//**
Flag a transaction a dictionary operation. */
UNIV_INLINE
void
trx_set_dict_operation(
/*===================*/
	trx_t*			trx,	/*!< in/out: transaction */
	enum trx_dict_op_t	op);	/*!< in: operation, not
					TRX_DICT_OP_NONE */

/**********************************************************************//**
Determines if a transaction is in the given state.
The caller must hold trx_sys->mutex, or it must be the thread
that is serving a running transaction.
A running RW transaction must be in trx_sys->rw_trx_list.
@return TRUE if trx->state == state */
UNIV_INLINE
bool
trx_state_eq(
/*=========*/
	const trx_t*	trx,	/*!< in: transaction */
	trx_state_t	state,	/*!< in: state;
				if state != TRX_STATE_NOT_STARTED
				asserts that
				trx->state != TRX_STATE_NOT_STARTED */
	bool		relaxed = false)
				/*!< in: whether to allow
				trx->state == TRX_STATE_NOT_STARTED
				after an error has been reported */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
# ifdef UNIV_DEBUG
/**********************************************************************//**
Asserts that a transaction has been started.
The caller must hold trx_sys->mutex.
@return TRUE if started */
ibool
trx_assert_started(
/*===============*/
	const trx_t*	trx)	/*!< in: transaction */
	MY_ATTRIBUTE((warn_unused_result));
# endif /* UNIV_DEBUG */

/**********************************************************************//**
Determines if the currently running transaction has been interrupted.
@return TRUE if interrupted */
ibool
trx_is_interrupted(
/*===============*/
	const trx_t*	trx);	/*!< in: transaction */

/*******************************************************************//**
Calculates the "weight" of a transaction. The weight of one transaction
is estimated as the number of altered rows + the number of locked rows.
@param t transaction
@return transaction weight */
#define TRX_WEIGHT(t)	((t)->undo_no + UT_LIST_GET_LEN((t)->lock.trx_locks))

/*******************************************************************//**
Compares the "weight" (or size) of two transactions. Transactions that
have edited non-transactional tables are considered heavier than ones
that have not.
@return true if weight(a) >= weight(b) */
bool
trx_weight_ge(
/*==========*/
	const trx_t*	a,	/*!< in: the transaction to be compared */
	const trx_t*	b);	/*!< in: the transaction to be compared */
/* Maximum length of a string that can be returned by
trx_get_que_state_str(). */
#define TRX_QUE_STATE_STR_MAX_LEN	12 /* "ROLLING BACK" */

/*******************************************************************//**
Retrieves transaction's que state in a human readable string. The string
should not be free()'d or modified.
@return string in the data segment */
UNIV_INLINE
const char*
trx_get_que_state_str(
/*==================*/
	const trx_t*	trx);	/*!< in: transaction */

/** Retreieves the transaction ID.
In a given point in time it is guaranteed that IDs of the running
transactions are unique. The values returned by this function for readonly
transactions may be reused, so a subsequent RO transaction may get the same ID
as a RO transaction that existed in the past. The values returned by this
function should be used for printing purposes only.
@param[in]	trx	transaction whose id to retrieve
@return transaction id */
UNIV_INLINE
trx_id_t
trx_get_id_for_print(
	const trx_t*	trx);

/** Create the trx_t pool */
void
trx_pool_init();

/** Destroy the trx_t pool */
void
trx_pool_close();

/**
Set the transaction as a read-write transaction if it is not already
tagged as such.
@param[in,out] trx	Transaction that needs to be "upgraded" to RW from RO */
void
trx_set_rw_mode(
	trx_t*		trx);

/**
Transactions that aren't started by the MySQL server don't set
the trx_t::mysql_thd field. For such transactions we set the lock
wait timeout to 0 instead of the user configured value that comes
from innodb_lock_wait_timeout via trx_t::mysql_thd.
@param trx transaction
@return lock wait timeout in seconds */
#define trx_lock_wait_timeout_get(t)					\
	((t)->mysql_thd != NULL						\
	 ? thd_lock_wait_timeout((t)->mysql_thd)			\
	 : 0)

typedef std::vector<ib_lock_t*, ut_allocator<ib_lock_t*> >	lock_list;

/*******************************************************************//**
Latching protocol for trx_lock_t::que_state.  trx_lock_t::que_state
captures the state of the query thread during the execution of a query.
This is different from a transaction state. The query state of a transaction
can be updated asynchronously by other threads.  The other threads can be
system threads, like the timeout monitor thread or user threads executing
other queries. Another thing to be mindful of is that there is a delay between
when a query thread is put into LOCK_WAIT state and before it actually starts
waiting.  Between these two events it is possible that the query thread is
granted the lock it was waiting for, which implies that the state can be changed
asynchronously.

All these operations take place within the context of locking. Therefore state
changes within the locking code must acquire both the lock mutex and the
trx->mutex when changing trx->lock.que_state to TRX_QUE_LOCK_WAIT or
trx->lock.wait_lock to non-NULL but when the lock wait ends it is sufficient
to only acquire the trx->mutex.
To query the state either of the mutexes is sufficient within the locking
code and no mutex is required when the query thread is no longer waiting. */

/** The locks and state of an active transaction. Protected by
lock_sys->mutex, trx->mutex or both. */
struct trx_lock_t {
	ulint		n_active_thrs;	/*!< number of active query threads */

	trx_que_t	que_state;	/*!< valid when trx->state
					== TRX_STATE_ACTIVE: TRX_QUE_RUNNING,
					TRX_QUE_LOCK_WAIT, ... */

	lock_t*		wait_lock;	/*!< if trx execution state is
					TRX_QUE_LOCK_WAIT, this points to
					the lock request, otherwise this is
					NULL; set to non-NULL when holding
					both trx->mutex and lock_sys->mutex;
					set to NULL when holding
					lock_sys->mutex; readers should
					hold lock_sys->mutex, except when
					they are holding trx->mutex and
					wait_lock==NULL */
	ib_uint64_t	deadlock_mark;	/*!< A mark field that is initialized
					to and checked against lock_mark_counter
					by lock_deadlock_recursive(). */
	bool		was_chosen_as_deadlock_victim;
					/*!< when the transaction decides to
					wait for a lock, it sets this to false;
					if another transaction chooses this
					transaction as a victim in deadlock
					resolution, it sets this to true.
					Protected by trx->mutex. */
	time_t		wait_started;	/*!< lock wait started at this time,
					protected only by lock_sys->mutex */

	que_thr_t*	wait_thr;	/*!< query thread belonging to this
					trx that is in QUE_THR_LOCK_WAIT
					state. For threads suspended in a
					lock wait, this is protected by
					lock_sys->mutex. Otherwise, this may
					only be modified by the thread that is
					serving the running transaction. */

	/** Pre-allocated record locks */
	struct {
		ib_lock_t lock; byte pad[256];
	} rec_pool[8];

	/** Pre-allocated table locks */
	ib_lock_t	table_pool[8];

	/** Next available rec_pool[] entry */
	unsigned	rec_cached;

	/** Next available table_pool[] entry */
	unsigned	table_cached;

	mem_heap_t*	lock_heap;	/*!< memory heap for trx_locks;
					protected by lock_sys->mutex */

	trx_lock_list_t trx_locks;	/*!< locks requested by the transaction;
					insertions are protected by trx->mutex
					and lock_sys->mutex; removals are
					protected by lock_sys->mutex */

	lock_list	table_locks;	/*!< All table locks requested by this
					transaction, including AUTOINC locks */

	bool		cancel;		/*!< true if the transaction is being
					rolled back either via deadlock
					detection or due to lock timeout. The
					caller has to acquire the trx_t::mutex
					in order to cancel the locks. In
					lock_trx_table_locks_remove() we
					check for this cancel of a transaction's
					locks and avoid reacquiring the trx
					mutex to prevent recursive deadlocks.
					Protected by both the lock sys mutex
					and the trx_t::mutex. */
	ulint		n_rec_locks;	/*!< number of rec locks in this trx */
};

/** Type used to store the list of tables that are modified by a given
transaction. We store pointers to the table objects in memory because
we know that a table object will not be destroyed while a transaction
that modified it is running. */
typedef std::set<
	dict_table_t*,
	std::less<dict_table_t*>,
	ut_allocator<dict_table_t*> >	trx_mod_tables_t;

/** The transaction handle

Normally, there is a 1:1 relationship between a transaction handle
(trx) and a session (client connection). One session is associated
with exactly one user transaction. There are some exceptions to this:

* For DDL operations, a subtransaction is allocated that modifies the
data dictionary tables. Lock waits and deadlocks are prevented by
acquiring the dict_operation_lock before starting the subtransaction
and releasing it after committing the subtransaction.

* The purge system uses a special transaction that is not associated
with any session.

* If the system crashed or it was quickly shut down while there were
transactions in the ACTIVE or PREPARED state, these transactions would
no longer be associated with a session when the server is restarted.

A session may be served by at most one thread at a time. The serving
thread of a session might change in some MySQL implementations.
Therefore we do not have os_thread_get_curr_id() assertions in the code.

Normally, only the thread that is currently associated with a running
transaction may access (read and modify) the trx object, and it may do
so without holding any mutex. The following are exceptions to this:

* trx_rollback_resurrected() may access resurrected (connectionless)
transactions while the system is already processing new user
transactions. The trx_sys->mutex and trx->is_recovered prevent
a race condition between it and trx_commit().

* trx_print_low() may access transactions not associated with the current
thread. The caller must be holding trx_sys->mutex and lock_sys->mutex.

* When a transaction handle is in the trx_sys->mysql_trx_list or
trx_sys->trx_list, some of its fields must not be modified without
holding trx_sys->mutex exclusively.

* The locking code (in particular, lock_deadlock_recursive() and
lock_rec_convert_impl_to_expl()) will access transactions associated
to other connections. The locks of transactions are protected by
lock_sys->mutex (insertions also by trx->mutex). */

/** Represents an instance of rollback segment along with its state variables.*/
struct trx_undo_ptr_t {
	trx_rseg_t*	rseg;		/*!< rollback segment assigned to the
					transaction, or NULL if not assigned
					yet */
	trx_undo_t*	insert_undo;	/*!< pointer to the insert undo log, or
					NULL if no inserts performed yet */
	trx_undo_t*	update_undo;	/*!< pointer to the update undo log, or
					NULL if no update performed yet */
};

/** An instance of temporary rollback segment. */
struct trx_temp_undo_t {
	/** temporary rollback segment, or NULL if not assigned yet */
	trx_rseg_t*	rseg;
	/** pointer to the undo log, or NULL if nothing logged yet */
	trx_undo_t*	undo;
};

/** Rollback segments assigned to a transaction for undo logging. */
struct trx_rsegs_t {
	/** undo log ptr holding reference to a rollback segment that resides in
	system/undo tablespace used for undo logging of tables that needs
	to be recovered on crash. */
	trx_undo_ptr_t	m_redo;

	/** undo log for temporary tables; discarded immediately after
	transaction commit/rollback */
	trx_temp_undo_t	m_noredo;
};

struct trx_t {
private:
  /**
    Count of references.

    We can't release the locks nor commit the transaction until this reference
    is 0. We can change the state to TRX_STATE_COMMITTED_IN_MEMORY to signify
    that it is no longer "active".
  */

  int32_t n_ref;


public:
	TrxMutex	mutex;		/*!< Mutex protecting the fields
					state and lock (except some fields
					of lock, which are protected by
					lock_sys->mutex) */

	trx_id_t	id;		/*!< transaction id */

	trx_id_t	no;		/*!< transaction serialization number:
					max trx id shortly before the
					transaction is moved to
					COMMITTED_IN_MEMORY state.
					Protected by trx_sys_t::mutex
					when trx->in_rw_trx_list. Initially
					set to TRX_ID_MAX. */

	/** State of the trx from the point of view of concurrency control
	and the valid state transitions.

	Possible states:

	TRX_STATE_NOT_STARTED
	TRX_STATE_ACTIVE
	TRX_STATE_PREPARED
	TRX_STATE_PREPARED_RECOVERED (special case of TRX_STATE_PREPARED)
	TRX_STATE_COMMITTED_IN_MEMORY (alias below COMMITTED)

	Valid state transitions are:

	Regular transactions:
	* NOT_STARTED -> ACTIVE -> COMMITTED -> NOT_STARTED

	Auto-commit non-locking read-only:
	* NOT_STARTED -> ACTIVE -> NOT_STARTED

	XA (2PC):
	* NOT_STARTED -> ACTIVE -> PREPARED -> COMMITTED -> NOT_STARTED

	Recovered XA:
	* NOT_STARTED -> PREPARED -> COMMITTED -> (freed)

	XA (2PC) (shutdown or disconnect before ROLLBACK or COMMIT):
	* NOT_STARTED -> PREPARED -> (freed)

	Disconnected XA can become recovered:
	* ... -> ACTIVE -> PREPARED (connected) -> PREPARED (disconnected)
	Disconnected means from mysql e.g due to the mysql client disconnection.
	Latching and various transaction lists membership rules:

	XA (2PC) transactions are always treated as non-autocommit.

	Transitions to ACTIVE or NOT_STARTED occur when
	!in_rw_trx_list (no trx_sys->mutex needed).

	Autocommit non-locking read-only transactions move between states
	without holding any mutex. They are !in_rw_trx_list.

	All transactions, unless they are determined to be ac-nl-ro,
	explicitly tagged as read-only or read-write, will first be put
	on the read-only transaction list. Only when a !read-only transaction
	in the read-only list tries to acquire an X or IX lock on a table
	do we remove it from the read-only list and put it on the read-write
	list. During this switch we assign it a rollback segment.

	When a transaction is NOT_STARTED, it can be in_mysql_trx_list if
	it is a user transaction. It cannot be in rw_trx_list.

	ACTIVE->PREPARED->COMMITTED is only possible when trx->in_rw_trx_list.
	The transition ACTIVE->PREPARED is protected by trx_sys->mutex.

	ACTIVE->COMMITTED is possible when the transaction is in
	rw_trx_list.

	Transitions to COMMITTED are protected by trx_t::mutex. */
	trx_state_t	state;
	/** whether this is a recovered transaction that should be
	rolled back by trx_rollback_or_clean_recovered().
	Protected by trx_t::mutex for transactions that are in trx_sys. */
	bool		is_recovered;
#ifdef WITH_WSREP
	/** whether wsrep_on(mysql_thd) held at the start of transaction */
	bool		wsrep;
	bool is_wsrep() const { return UNIV_UNLIKELY(wsrep); }
	/** true, if BF thread is performing unique secondary index scanning */
	bool wsrep_UK_scan;
	bool is_wsrep_UK_scan() const { return UNIV_UNLIKELY(wsrep_UK_scan); }
#else /* WITH_WSREP */
	bool is_wsrep() const { return false; }
#endif /* WITH_WSREP */

	ReadView*	read_view;	/*!< consistent read view used in the
					transaction, or NULL if not yet set */

	UT_LIST_NODE_T(trx_t)
			trx_list;	/*!< list of transactions;
					protected by trx_sys->mutex. */
	UT_LIST_NODE_T(trx_t)
			no_list;	/*!< Required during view creation
					to check for the view limit for
					transactions that are committing */

	trx_lock_t	lock;		/*!< Information about the transaction
					locks and state. Protected by
					lock_sys->mutex (insertions also
					by trx_t::mutex). */

	/* These fields are not protected by any mutex. */
	const char*	op_info;	/*!< English text describing the
					current operation, or an empty
					string */
	ulint		isolation_level;/*!< TRX_ISO_REPEATABLE_READ, ... */
	bool		check_foreigns;	/*!< normally TRUE, but if the user
					wants to suppress foreign key checks,
					(in table imports, for example) we
					set this FALSE */
	/*------------------------------*/
	/* MySQL has a transaction coordinator to coordinate two phase
	commit between multiple storage engines and the binary log. When
	an engine participates in a transaction, it's responsible for
	registering itself using the trans_register_ha() API. */
	bool		is_registered;	/* This flag is set to true after the
					transaction has been registered with
					the coordinator using the XA API, and
					is set to false  after commit or
					rollback. */
	/** whether this is holding the prepare mutex */
	bool		active_commit_ordered;
	/*------------------------------*/
	bool		check_unique_secondary;
					/*!< normally TRUE, but if the user
					wants to speed up inserts by
					suppressing unique key checks
					for secondary indexes when we decide
					if we can use the insert buffer for
					them, we set this FALSE */
	bool		flush_log_later;/* In 2PC, we hold the
					prepare_commit mutex across
					both phases. In that case, we
					defer flush of the logs to disk
					until after we release the
					mutex. */
	bool		must_flush_log_later;/*!< this flag is set to TRUE in
					trx_commit() if flush_log_later was
					TRUE, and there were modifications by
					the transaction; in that case we must
					flush the log in
					trx_commit_complete_for_mysql() */
	ulint		duplicates;	/*!< TRX_DUP_IGNORE | TRX_DUP_REPLACE */
	trx_dict_op_t	dict_operation;	/**< @see enum trx_dict_op_t */

	/* Fields protected by the srv_conc_mutex. */
	bool		declared_to_be_inside_innodb;
					/*!< this is TRUE if we have declared
					this transaction in
					srv_conc_enter_innodb to be inside the
					InnoDB engine */
	ib_uint32_t	n_tickets_to_enter_innodb;
					/*!< this can be > 0 only when
					declared_to_... is TRUE; when we come
					to srv_conc_innodb_enter, if the value
					here is > 0, we decrement this by 1 */
	ib_uint32_t	dict_operation_lock_mode;
					/*!< 0, RW_S_LATCH, or RW_X_LATCH:
					the latch mode trx currently holds
					on dict_operation_lock. Protected
					by dict_operation_lock. */

	/** wall-clock time of the latest transition to TRX_STATE_ACTIVE;
	used for diagnostic purposes only */
	time_t		start_time;
	/** microsecond_interval_timer() of transaction start */
	ulonglong	start_time_micro;
	lsn_t		commit_lsn;	/*!< lsn at the time of the commit */
	table_id_t	table_id;	/*!< Table to drop iff dict_operation
					== TRX_DICT_OP_TABLE, or 0. */
	/*------------------------------*/
	THD*		mysql_thd;	/*!< MySQL thread handle corresponding
					to this trx, or NULL */

	const char*	mysql_log_file_name;
					/*!< if MySQL binlog is used, this field
					contains a pointer to the latest file
					name; this is NULL if binlog is not
					used */
	int64_t		mysql_log_offset;
					/*!< if MySQL binlog is used, this
					field contains the end offset of the
					binlog entry */
	/*------------------------------*/
	ib_uint32_t	n_mysql_tables_in_use; /*!< number of Innobase tables
					used in the processing of the current
					SQL statement in MySQL */
	ib_uint32_t	mysql_n_tables_locked;
					/*!< how many tables the current SQL
					statement uses, except those
					in consistent read */
	/*------------------------------*/
#ifdef UNIV_DEBUG
	/** The following two fields are mutually exclusive. */
	/* @{ */

	bool		in_rw_trx_list;	/*!< true if in trx_sys->rw_trx_list */
	/* @} */
#endif /* UNIV_DEBUG */
	UT_LIST_NODE_T(trx_t)
			mysql_trx_list;	/*!< list of transactions created for
					MySQL; protected by trx_sys->mutex */
#ifdef UNIV_DEBUG
	bool		in_mysql_trx_list;
					/*!< true if in
					trx_sys->mysql_trx_list */
#endif /* UNIV_DEBUG */
	/*------------------------------*/
	dberr_t		error_state;	/*!< 0 if no error, otherwise error
					number; NOTE That ONLY the thread
					doing the transaction is allowed to
					set this field: this is NOT protected
					by any mutex */
	const dict_index_t*error_info;	/*!< if the error number indicates a
					duplicate key error, a pointer to
					the problematic index is stored here */
	ulint		error_key_num;	/*!< if the index creation fails to a
					duplicate key error, a mysql key
					number of that index is stored here */
	que_t*		graph;		/*!< query currently run in the session,
					or NULL if none; NOTE that the query
					belongs to the session, and it can
					survive over a transaction commit, if
					it is a stored procedure with a COMMIT
					WORK statement, for instance */
	/*------------------------------*/
	UT_LIST_BASE_NODE_T(trx_named_savept_t)
			trx_savepoints;	/*!< savepoints set with SAVEPOINT ...,
					oldest first */
	/*------------------------------*/
	UndoMutex	undo_mutex;	/*!< mutex protecting the fields in this
					section (down to undo_no_arr), EXCEPT
					last_sql_stat_start, which can be
					accessed only when we know that there
					cannot be any activity in the undo
					logs! */
	undo_no_t	undo_no;	/*!< next undo log record number to
					assign; since the undo log is
					private for a transaction, this
					is a simple ascending sequence
					with no gaps; thus it represents
					the number of modified/inserted
					rows in a transaction */
	ulint		undo_rseg_space;
					/*!< space id where last undo record
					was written */
	trx_savept_t	last_sql_stat_start;
					/*!< undo_no when the last sql statement
					was started: in case of an error, trx
					is rolled back down to this undo
					number; see note at undo_mutex! */
	trx_rsegs_t	rsegs;		/* rollback segments for undo logging */
	undo_no_t	roll_limit;	/*!< least undo number to undo during
					a partial rollback; 0 otherwise */
#ifdef UNIV_DEBUG
	bool		in_rollback;	/*!< true when the transaction is
					executing a partial or full rollback */
#endif /* UNIV_DEBUG */
	ulint		pages_undone;	/*!< number of undo log pages undone
					since the last undo log truncation */
	/*------------------------------*/
	ulint		n_autoinc_rows;	/*!< no. of AUTO-INC rows required for
					an SQL statement. This is useful for
					multi-row INSERTs */
	ib_vector_t*    autoinc_locks;  /* AUTOINC locks held by this
					transaction. Note that these are
					also in the lock list trx_locks. This
					vector needs to be freed explicitly
					when the trx instance is destroyed.
					Protected by lock_sys->mutex. */
	/*------------------------------*/
	bool		read_only;	/*!< true if transaction is flagged
					as a READ-ONLY transaction.
					if auto_commit && !will_lock
					then it will be handled as a
					AC-NL-RO-SELECT (Auto Commit Non-Locking
					Read Only Select). A read only
					transaction will not be assigned an
					UNDO log. */
	bool		auto_commit;	/*!< true if it is an autocommit */
	bool		will_lock;	/*!< set to inform trx_start_low() that
					the transaction may acquire locks */
	/*------------------------------*/
	fts_trx_t*	fts_trx;	/*!< FTS information, or NULL if
					transaction hasn't modified tables
					with FTS indexes (yet). */
	doc_id_t	fts_next_doc_id;/* The document id used for updates */
	/*------------------------------*/
	ib_uint32_t	flush_tables;	/*!< if "covering" the FLUSH TABLES",
					count of tables being flushed. */

	/*------------------------------*/
	bool		ddl;		/*!< true if it is an internal
					transaction for DDL */
	bool		internal;	/*!< true if it is a system/internal
					transaction background task. This
					includes DDL transactions too.  Such
					transactions are always treated as
					read-write. */
	/*------------------------------*/
#ifdef UNIV_DEBUG
	unsigned	start_line;	/*!< Track where it was started from */
	const char*	start_file;	/*!< Filename where it was started */
#endif /* UNIV_DEBUG */

	XID*		xid;		/*!< X/Open XA transaction
					identification to identify a
					transaction branch */
	trx_mod_tables_t mod_tables;	/*!< List of tables that were modified
					by this transaction */
	/*------------------------------*/
	char*		detailed_error;	/*!< detailed error message for last
					error, or empty. */
private:
	/** flush observer used to track flushing of non-redo logged pages
	during bulk create index */
	FlushObserver*	flush_observer;
public:
#ifdef WITH_WSREP
	os_event_t	wsrep_event;	/* event waited for in srv_conc_slot */
#endif /* WITH_WSREP */

	ulint		magic_n;

	/** @return whether any persistent undo log has been generated */
	bool has_logged_persistent() const
	{
		return(rsegs.m_redo.insert_undo || rsegs.m_redo.update_undo);
	}

	/** @return whether any undo log has been generated */
	bool has_logged() const
	{
		return(has_logged_persistent() || rsegs.m_noredo.undo);
	}

	/** @return rollback segment for modifying temporary tables */
	trx_rseg_t* get_temp_rseg()
	{
		if (trx_rseg_t* rseg = rsegs.m_noredo.rseg) {
			ut_ad(id != 0);
			return(rseg);
		}

		return(assign_temp_rseg());
	}

	/** Set the innodb_log_optimize_ddl page flush observer
	@param[in]	space_id	tablespace id
	@param[in,out]	stage		performance_schema accounting */
	void set_flush_observer(ulint space_id, ut_stage_alter_t* stage);

	/** Remove the flush observer */
	void remove_flush_observer();

	/** @return the flush observer */
	FlushObserver* get_flush_observer() const
	{
		return flush_observer;
	}

  /** Transition to committed state, to release implicit locks. */
  inline void commit_state();

  /** Release any explicit locks of a committing transaction. */
  inline void release_locks();


  bool is_referenced()
  {
    return my_atomic_load32_explicit(&n_ref, MY_MEMORY_ORDER_RELAXED) > 0;
  }


  void reference()
  {
#ifdef UNIV_DEBUG
  int32_t old_n_ref=
#endif
    my_atomic_add32_explicit(&n_ref, 1, MY_MEMORY_ORDER_RELAXED);
    ut_ad(old_n_ref >= 0);
  }


  void release_reference()
  {
#ifdef UNIV_DEBUG
  int32_t old_n_ref=
#endif
    my_atomic_add32_explicit(&n_ref, -1, MY_MEMORY_ORDER_RELAXED);
    ut_ad(old_n_ref > 0);
  }

  /** @return whether the table has lock on
  mysql.innodb_table_stats and mysql.innodb_index_stats */
  bool has_stats_table_lock() const;

  /** Free the memory to trx_pools */
  inline void free();


  void assert_freed() const
  {
    ut_ad(state == TRX_STATE_NOT_STARTED);
    ut_ad(!id);
    ut_ad(!has_logged());
    ut_ad(!const_cast<trx_t*>(this)->is_referenced());
    ut_ad(!is_wsrep());
    ut_ad(!trx_get_read_view(this));
    ut_ad(!lock.wait_thr);
    ut_ad(UT_LIST_GET_LEN(lock.trx_locks) == 0);
    ut_ad(lock.table_locks.empty());
    ut_ad(!autoinc_locks || ib_vector_is_empty(autoinc_locks));
    ut_ad(dict_operation == TRX_DICT_OP_NONE);
  }

  /** @return whether this is a non-locking autocommit transaction */
  bool is_autocommit_non_locking() const { return auto_commit && !will_lock; }

private:
  /** Assign a rollback segment for modifying temporary tables.
  @return the assigned rollback segment */
  trx_rseg_t *assign_temp_rseg();
};

/**
Check if transaction is started.
@param[in] trx		Transaction whose state we need to check
@reutrn true if transaction is in state started */
inline bool trx_is_started(const trx_t* trx)
{
	return trx->state != TRX_STATE_NOT_STARTED;
}

/* Transaction isolation levels (trx->isolation_level) */
#define TRX_ISO_READ_UNCOMMITTED	0	/* dirty read: non-locking
						SELECTs are performed so that
						we do not look at a possible
						earlier version of a record;
						thus they are not 'consistent'
						reads under this isolation
						level; otherwise like level
						2 */

#define TRX_ISO_READ_COMMITTED		1	/* somewhat Oracle-like
						isolation, except that in
						range UPDATE and DELETE we
						must block phantom rows
						with next-key locks;
						SELECT ... FOR UPDATE and ...
						LOCK IN SHARE MODE only lock
						the index records, NOT the
						gaps before them, and thus
						allow free inserting;
						each consistent read reads its
						own snapshot */

#define TRX_ISO_REPEATABLE_READ		2	/* this is the default;
						all consistent reads in the
						same trx read the same
						snapshot;
						full next-key locking used
						in locking reads to block
						insertions into gaps */

#define TRX_ISO_SERIALIZABLE		3	/* all plain SELECTs are
						converted to LOCK IN SHARE
						MODE reads */

/* Treatment of duplicate values (trx->duplicates; for example, in inserts).
Multiple flags can be combined with bitwise OR. */
#define TRX_DUP_IGNORE	1U	/* duplicate rows are to be updated */
#define TRX_DUP_REPLACE	2U	/* duplicate rows are to be replaced */


/** Commit node states */
enum commit_node_state {
	COMMIT_NODE_SEND = 1,	/*!< about to send a commit signal to
				the transaction */
	COMMIT_NODE_WAIT	/*!< commit signal sent to the transaction,
				waiting for completion */
};

/** Commit command node in a query graph */
struct commit_node_t{
	que_common_t	common;	/*!< node type: QUE_NODE_COMMIT */
	enum commit_node_state
			state;	/*!< node execution state */
};


/** Test if trx->mutex is owned. */
#define trx_mutex_own(t) mutex_own(&t->mutex)

/** Acquire the trx->mutex. */
#define trx_mutex_enter(t) do {			\
	mutex_enter(&t->mutex);			\
} while (0)

/** Release the trx->mutex. */
#define trx_mutex_exit(t) do {			\
	mutex_exit(&t->mutex);			\
} while (0)

#include "trx0trx.inl"

#endif
