/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2021, MariaDB Corporation.

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
@file trx/trx0roll.cc
Transaction rollback

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0roll.h"

#include <my_service_manager.h>
#include <mysql/service_wsrep.h>

#include "fsp0fsp.h"
#include "lock0lock.h"
#include "mach0data.h"
#include "pars0pars.h"
#include "que0que.h"
#include "read0read.h"
#include "row0mysql.h"
#include "row0undo.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "trx0rec.h"
#include "trx0rseg.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0undo.h"

/** This many pages must be undone before a truncate is tried within
rollback */
static const ulint TRX_ROLL_TRUNC_THRESHOLD = 1;

/** true if trx_rollback_or_clean_all_recovered() thread is active */
bool			trx_rollback_or_clean_is_active;

/** In crash recovery, the current trx to be rolled back; NULL otherwise */
const trx_t*		trx_roll_crash_recv_trx;

/****************************************************************//**
Finishes a transaction rollback. */
static
void
trx_rollback_finish(
/*================*/
	trx_t*		trx);	/*!< in: transaction */

/*******************************************************************//**
Rollback a transaction used in MySQL. */
static
void
trx_rollback_to_savepoint_low(
/*==========================*/
	trx_t*		trx,	/*!< in: transaction handle */
	trx_savept_t*	savept)	/*!< in: pointer to savepoint undo number, if
				partial rollback requested, or NULL for
				complete rollback */
{
	que_thr_t*	thr;
	mem_heap_t*	heap;
	roll_node_t*	roll_node;

	heap = mem_heap_create(512);

	roll_node = roll_node_create(heap);
	ut_ad(!trx->in_rollback);

	if (savept != NULL) {
		roll_node->savept = savept;
		ut_ad(trx->mysql_thd);
		ut_ad(trx->in_mysql_trx_list);
		ut_ad(!trx->is_recovered);
		ut_ad(trx->state == TRX_STATE_ACTIVE);
	} else {
		ut_d(trx_state_t state = trx->state);
		ut_ad(state == TRX_STATE_ACTIVE
		      || state == TRX_STATE_PREPARED
		      || state == TRX_STATE_PREPARED_RECOVERED);
	}

	trx->error_state = DB_SUCCESS;

	if (trx->has_logged()) {

		ut_ad(trx->rsegs.m_redo.rseg != 0
		      || trx->rsegs.m_noredo.rseg != 0);

		thr = pars_complete_graph_for_exec(roll_node, trx, heap, NULL);

		ut_a(thr == que_fork_start_command(
			static_cast<que_fork_t*>(que_node_get_parent(thr))));

		que_run_threads(thr);

		ut_a(roll_node->undo_thr != NULL);
		que_run_threads(roll_node->undo_thr);

		/* Free the memory reserved by the undo graph. */
		que_graph_free(static_cast<que_t*>(
				       roll_node->undo_thr->common.parent));
	}

	if (savept == NULL) {
		trx_rollback_finish(trx);
		MONITOR_INC(MONITOR_TRX_ROLLBACK);
	} else {
		trx->lock.que_state = TRX_QUE_RUNNING;
		MONITOR_INC(MONITOR_TRX_ROLLBACK_SAVEPOINT);
	}

	ut_a(trx->error_state == DB_SUCCESS);
	ut_a(trx->lock.que_state == TRX_QUE_RUNNING);

	mem_heap_free(heap);

	/* There might be work for utility threads.*/
	srv_active_wake_master_thread();

	MONITOR_DEC(MONITOR_TRX_ACTIVE);
}

/*******************************************************************//**
Rollback a transaction to a given savepoint or do a complete rollback.
@return error code or DB_SUCCESS */
dberr_t
trx_rollback_to_savepoint(
/*======================*/
	trx_t*		trx,	/*!< in: transaction handle */
	trx_savept_t*	savept)	/*!< in: pointer to savepoint undo number, if
				partial rollback requested, or NULL for
				complete rollback */
{
	ut_ad(!trx_mutex_own(trx));

	trx_start_if_not_started_xa(trx, true);

	trx_rollback_to_savepoint_low(trx, savept);

	return(trx->error_state);
}

/*******************************************************************//**
Rollback a transaction used in MySQL.
@return error code or DB_SUCCESS */
static
dberr_t
trx_rollback_for_mysql_low(
/*=======================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	trx->op_info = "rollback";

	/* If we are doing the XA recovery of prepared transactions,
	then the transaction object does not have an InnoDB session
	object, and we set a dummy session that we use for all MySQL
	transactions. */

	trx_rollback_to_savepoint_low(trx, NULL);

	trx->op_info = "";

	ut_a(trx->error_state == DB_SUCCESS);

	return(trx->error_state);
}

/** Rollback a transaction used in MySQL
@param[in, out]	trx	transaction
@return error code or DB_SUCCESS */
dberr_t trx_rollback_for_mysql(trx_t* trx)
{
	/* We are reading trx->state without holding trx_sys->mutex
	here, because the rollback should be invoked for a running
	active MySQL transaction (or recovered prepared transaction)
	that is associated with the current thread. */

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		trx->will_lock = false;
		ut_ad(trx->mysql_thd);
		ut_ad(trx->in_mysql_trx_list);
#ifdef WITH_WSREP
		trx->wsrep = false;
#endif
		return(DB_SUCCESS);

	case TRX_STATE_ACTIVE:
		ut_ad(trx->in_mysql_trx_list);
		ut_ad(trx->mysql_thd);
		ut_ad(!trx->is_recovered);
		ut_ad(!trx->is_autocommit_non_locking() || trx->read_only);
		return(trx_rollback_for_mysql_low(trx));

	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		ut_ad(!trx->is_autocommit_non_locking());
		if (trx->has_logged_persistent()) {
			/* The XA ROLLBACK of a XA PREPARE transaction
			will consist of multiple mini-transactions.

			As the very first step of XA ROLLBACK, we must
			change the undo log state back from
			TRX_UNDO_PREPARED to TRX_UNDO_ACTIVE, in order
			to ensure that recovery will complete the
			rollback.

			Failure to perform this step could cause a
			situation where we would roll back part of
			a XA PREPARE transaction, the server would be
			killed, and finally, the transaction would be
			recovered in XA PREPARE state, with some of
			the actions already having been rolled back. */
			trx_undo_ptr_t*	undo_ptr = &trx->rsegs.m_redo;
			mtr_t		mtr;
			mtr.start();
			mutex_enter(&trx->rsegs.m_redo.rseg->mutex);
			if (undo_ptr->insert_undo != NULL) {
				trx_undo_set_state_at_prepare(
					trx, undo_ptr->insert_undo,
					true, &mtr);
			}
			if (undo_ptr->update_undo != NULL) {
				trx_undo_set_state_at_prepare(
					trx, undo_ptr->update_undo,
					true, &mtr);
			}
			mutex_exit(&trx->rsegs.m_redo.rseg->mutex);
			/* Write the redo log for the XA ROLLBACK
			state change to the global buffer. It is
			not necessary to flush the redo log. If
			a durable log write of a later mini-transaction
			takes place for whatever reason, then this state
			change will be durable as well. */
			mtr.commit();
			ut_ad(mtr.commit_lsn() > 0);
		}
		return(trx_rollback_for_mysql_low(trx));

	case TRX_STATE_COMMITTED_IN_MEMORY:
		ut_ad(!trx->is_autocommit_non_locking());
		break;
	}

	ut_error;
	return(DB_CORRUPTION);
}

/*******************************************************************//**
Rollback the latest SQL statement for MySQL.
@return error code or DB_SUCCESS */
dberr_t
trx_rollback_last_sql_stat_for_mysql(
/*=================================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	dberr_t	err;

	/* We are reading trx->state without holding trx_sys->mutex
	here, because the statement rollback should be invoked for a
	running active MySQL transaction that is associated with the
	current thread. */
	ut_ad(trx->in_mysql_trx_list);

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		return(DB_SUCCESS);

	case TRX_STATE_ACTIVE:
		ut_ad(trx->mysql_thd);
		ut_ad(!trx->is_recovered);
		ut_ad(!trx->is_autocommit_non_locking() || trx->read_only);

		trx->op_info = "rollback of SQL statement";

		err = trx_rollback_to_savepoint(
			trx, &trx->last_sql_stat_start);

		if (trx->fts_trx != NULL) {
			fts_savepoint_rollback_last_stmt(trx);
		}

		/* The following call should not be needed,
		but we play it safe: */
		trx_mark_sql_stat_end(trx);

		trx->op_info = "";

		return(err);

	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		/* The statement rollback is only allowed on an ACTIVE
		transaction, not a PREPARED or COMMITTED one. */
		break;
	}

	ut_error;
	return(DB_CORRUPTION);
}

/*******************************************************************//**
Search for a savepoint using name.
@return savepoint if found else NULL */
static
trx_named_savept_t*
trx_savepoint_find(
/*===============*/
	trx_t*		trx,			/*!< in: transaction */
	const char*	name)			/*!< in: savepoint name */
{
	trx_named_savept_t*	savep;

	for (savep = UT_LIST_GET_FIRST(trx->trx_savepoints);
	     savep != NULL;
	     savep = UT_LIST_GET_NEXT(trx_savepoints, savep)) {

		if (0 == ut_strcmp(savep->name, name)) {
			return(savep);
		}
	}

	return(NULL);
}

/*******************************************************************//**
Frees a single savepoint struct. */
static
void
trx_roll_savepoint_free(
/*=====================*/
	trx_t*			trx,	/*!< in: transaction handle */
	trx_named_savept_t*	savep)	/*!< in: savepoint to free */
{
	UT_LIST_REMOVE(trx->trx_savepoints, savep);

	ut_free(savep->name);
	ut_free(savep);
}

/*******************************************************************//**
Frees savepoint structs starting from savep. */
void
trx_roll_savepoints_free(
/*=====================*/
	trx_t*			trx,	/*!< in: transaction handle */
	trx_named_savept_t*	savep)	/*!< in: free all savepoints starting
					with this savepoint i*/
{
	while (savep != NULL) {
		trx_named_savept_t*	next_savep;

		next_savep = UT_LIST_GET_NEXT(trx_savepoints, savep);

		trx_roll_savepoint_free(trx, savep);

		savep = next_savep;
	}
}

/*******************************************************************//**
Rolls back a transaction back to a named savepoint. Modifications after the
savepoint are undone but InnoDB does NOT release the corresponding locks
which are stored in memory. If a lock is 'implicit', that is, a new inserted
row holds a lock where the lock information is carried by the trx id stored in
the row, these locks are naturally released in the rollback. Savepoints which
were set after this savepoint are deleted.
@return if no savepoint of the name found then DB_NO_SAVEPOINT,
otherwise DB_SUCCESS */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
trx_rollback_to_savepoint_for_mysql_low(
/*====================================*/
	trx_t*			trx,	/*!< in/out: transaction */
	trx_named_savept_t*	savep,	/*!< in/out: savepoint */
	int64_t*		mysql_binlog_cache_pos)
					/*!< out: the MySQL binlog
					cache position corresponding
					to this savepoint; MySQL needs
					this information to remove the
					binlog entries of the queries
					executed after the savepoint */
{
	dberr_t	err;

	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
	ut_ad(trx->in_mysql_trx_list);

	/* Free all savepoints strictly later than savep. */

	trx_roll_savepoints_free(
		trx, UT_LIST_GET_NEXT(trx_savepoints, savep));

	*mysql_binlog_cache_pos = savep->mysql_binlog_cache_pos;

	trx->op_info = "rollback to a savepoint";

	err = trx_rollback_to_savepoint(trx, &savep->savept);

	/* Store the current undo_no of the transaction so that
	we know where to roll back if we have to roll back the
	next SQL statement: */

	trx_mark_sql_stat_end(trx);

	trx->op_info = "";

#ifdef WITH_WSREP
	if (trx->is_wsrep()) {
		trx->lock.was_chosen_as_deadlock_victim = false;
	}
#endif
	return(err);
}

/*******************************************************************//**
Rolls back a transaction back to a named savepoint. Modifications after the
savepoint are undone but InnoDB does NOT release the corresponding locks
which are stored in memory. If a lock is 'implicit', that is, a new inserted
row holds a lock where the lock information is carried by the trx id stored in
the row, these locks are naturally released in the rollback. Savepoints which
were set after this savepoint are deleted.
@return if no savepoint of the name found then DB_NO_SAVEPOINT,
otherwise DB_SUCCESS */
dberr_t
trx_rollback_to_savepoint_for_mysql(
/*================================*/
	trx_t*		trx,			/*!< in: transaction handle */
	const char*	savepoint_name,		/*!< in: savepoint name */
	int64_t*	mysql_binlog_cache_pos)	/*!< out: the MySQL binlog cache
						position corresponding to this
						savepoint; MySQL needs this
						information to remove the
						binlog entries of the queries
						executed after the savepoint */
{
	trx_named_savept_t*	savep;

	/* We are reading trx->state without holding trx_sys->mutex
	here, because the savepoint rollback should be invoked for a
	running active MySQL transaction that is associated with the
	current thread. */
	ut_ad(trx->in_mysql_trx_list);

	savep = trx_savepoint_find(trx, savepoint_name);

	if (savep == NULL) {
		return(DB_NO_SAVEPOINT);
	}

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		ib::error() << "Transaction has a savepoint "
			<< savep->name
			<< " though it is not started";
		return(DB_ERROR);

	case TRX_STATE_ACTIVE:

		return(trx_rollback_to_savepoint_for_mysql_low(
				trx, savep, mysql_binlog_cache_pos));

	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		/* The savepoint rollback is only allowed on an ACTIVE
		transaction, not a PREPARED or COMMITTED one. */
		break;
	}

	ut_error;
	return(DB_CORRUPTION);
}

/*******************************************************************//**
Creates a named savepoint. If the transaction is not yet started, starts it.
If there is already a savepoint of the same name, this call erases that old
savepoint and replaces it with a new. Savepoints are deleted in a transaction
commit or rollback.
@return always DB_SUCCESS */
dberr_t
trx_savepoint_for_mysql(
/*====================*/
	trx_t*		trx,			/*!< in: transaction handle */
	const char*	savepoint_name,		/*!< in: savepoint name */
	int64_t		binlog_cache_pos)	/*!< in: MySQL binlog cache
						position corresponding to this
						connection at the time of the
						savepoint */
{
	trx_named_savept_t*	savep;

	trx_start_if_not_started_xa(trx, false);

	savep = trx_savepoint_find(trx, savepoint_name);

	if (savep) {
		/* There is a savepoint with the same name: free that */

		UT_LIST_REMOVE(trx->trx_savepoints, savep);

		ut_free(savep->name);
		ut_free(savep);
	}

	/* Create a new savepoint and add it as the last in the list */

	savep = static_cast<trx_named_savept_t*>(
		ut_malloc_nokey(sizeof(*savep)));

	savep->name = mem_strdup(savepoint_name);

	savep->savept = trx_savept_take(trx);

	savep->mysql_binlog_cache_pos = binlog_cache_pos;

	UT_LIST_ADD_LAST(trx->trx_savepoints, savep);

	return(DB_SUCCESS);
}

/*******************************************************************//**
Releases only the named savepoint. Savepoints which were set after this
savepoint are left as is.
@return if no savepoint of the name found then DB_NO_SAVEPOINT,
otherwise DB_SUCCESS */
dberr_t
trx_release_savepoint_for_mysql(
/*============================*/
	trx_t*		trx,			/*!< in: transaction handle */
	const char*	savepoint_name)		/*!< in: savepoint name */
{
	trx_named_savept_t*	savep;

	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE, true)
	      || trx_state_eq(trx, TRX_STATE_PREPARED, true));
	ut_ad(trx->in_mysql_trx_list);

	savep = trx_savepoint_find(trx, savepoint_name);

	if (savep != NULL) {
		trx_roll_savepoint_free(trx, savep);
	}

	return(savep != NULL ? DB_SUCCESS : DB_NO_SAVEPOINT);
}

/*******************************************************************//**
Returns a transaction savepoint taken at this point in time.
@return savepoint */
trx_savept_t
trx_savept_take(
/*============*/
	trx_t*	trx)	/*!< in: transaction */
{
	trx_savept_t	savept;

	savept.least_undo_no = trx->undo_no;

	return(savept);
}

/*******************************************************************//**
Roll back an active transaction. */
static
void
trx_rollback_active(
/*================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	mem_heap_t*	heap;
	que_fork_t*	fork;
	que_thr_t*	thr;
	roll_node_t*	roll_node;
	dict_table_t*	table;
	ibool		dictionary_locked = FALSE;
	const trx_id_t	trx_id = trx->id;

	ut_ad(trx_id);

	heap = mem_heap_create(512);

	fork = que_fork_create(NULL, NULL, QUE_FORK_RECOVERY, heap);
	fork->trx = trx;

	thr = que_thr_create(fork, heap, NULL);

	roll_node = roll_node_create(heap);

	thr->child = roll_node;
	roll_node->common.parent = thr;

	trx->graph = fork;

	ut_a(thr == que_fork_start_command(fork));

	trx_roll_crash_recv_trx	= trx;

	if (trx_get_dict_operation(trx) != TRX_DICT_OP_NONE) {
		row_mysql_lock_data_dictionary(trx);
		dictionary_locked = TRUE;
	}

	que_run_threads(thr);
	ut_a(roll_node->undo_thr != NULL);

	que_run_threads(roll_node->undo_thr);

	if (trx->error_state != DB_SUCCESS) {
		ut_ad(trx->error_state == DB_INTERRUPTED);
		ut_ad(srv_shutdown_state != SRV_SHUTDOWN_NONE);
		ut_ad(!srv_undo_sources);
		ut_ad(srv_fast_shutdown);
		ut_ad(!dictionary_locked);
		que_graph_free(static_cast<que_t*>(
				       roll_node->undo_thr->common.parent));
		goto func_exit;
	}

	trx_rollback_finish(thr_get_trx(roll_node->undo_thr));

	/* Free the memory reserved by the undo graph */
	que_graph_free(static_cast<que_t*>(
			       roll_node->undo_thr->common.parent));

	ut_a(trx->lock.que_state == TRX_QUE_RUNNING);

	if (trx_get_dict_operation(trx) != TRX_DICT_OP_NONE
	    && trx->table_id != 0) {

		ut_ad(dictionary_locked);

		/* If the transaction was for a dictionary operation,
		we drop the relevant table only if it is not flagged
		as DISCARDED. If it still exists. */

		table = dict_table_open_on_id(
			trx->table_id, TRUE, DICT_TABLE_OP_NORMAL);

		if (table && !dict_table_is_discarded(table)) {
			ib::warn() << "Dropping table '" << table->name
				<< "', with id " << trx->table_id
				<< " in recovery";

			dict_table_close_and_drop(trx, table);

			trx_commit_for_mysql(trx);
		}
	}

	ib::info() << "Rolled back recovered transaction " << trx_id;

func_exit:
	if (dictionary_locked) {
		row_mysql_unlock_data_dictionary(trx);
	}

	mem_heap_free(heap);

	trx_roll_crash_recv_trx	= NULL;
}

/*******************************************************************//**
Rollback or clean up any resurrected incomplete transactions. It assumes
that the caller holds the trx_sys_t::mutex and it will release the
lock if it does a clean up or rollback.
@return TRUE if the transaction was cleaned up or rolled back
and trx_sys->mutex was released. */
static
ibool
trx_rollback_resurrected(
/*=====================*/
	trx_t*	trx,	/*!< in: transaction to rollback or clean */
	ibool*	all)	/*!< in/out: FALSE=roll back dictionary and
			statistics table transactions;
			TRUE=roll back all non-PREPARED transactions */
{
	ut_ad(trx_sys_mutex_own());

	/* The trx->is_recovered flag and trx->state are set
	atomically under the protection of the trx->mutex in
	trx_t::commit_state(). We do not want to accidentally clean up
	a non-recovered transaction here. */

	trx_mutex_enter(trx);
	if (!trx->is_recovered) {
func_exit:
		trx_mutex_exit(trx);
		return(FALSE);
	}

	switch (trx->state) {
	case TRX_STATE_COMMITTED_IN_MEMORY:
		trx_mutex_exit(trx);
		trx_sys_mutex_exit();
		ib::info() << "Cleaning up trx with id " << ib::hex(trx->id);

		trx_cleanup_at_db_startup(trx);
		trx_free_resurrected(trx);
		return(TRUE);
	case TRX_STATE_ACTIVE:
		if (srv_shutdown_state != SRV_SHUTDOWN_NONE
		    && !srv_undo_sources && srv_fast_shutdown) {
fake_prepared:
			trx->state = TRX_STATE_PREPARED;
			*all = FALSE;
			goto func_exit;
		}
		trx_mutex_exit(trx);

		if (*all || trx_get_dict_operation(trx) != TRX_DICT_OP_NONE
		    || trx->has_stats_table_lock()) {
			trx_sys_mutex_exit();
			trx_rollback_active(trx);
			if (trx->error_state != DB_SUCCESS) {
				ut_ad(trx->error_state == DB_INTERRUPTED);
				trx->error_state = DB_SUCCESS;
				ut_ad(!srv_undo_sources);
				ut_ad(srv_fast_shutdown);
				mutex_enter(&trx_sys->mutex);
				trx_mutex_enter(trx);
				goto fake_prepared;
			}
			trx_free_for_background(trx);
			return(TRUE);
		}
		return(FALSE);
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		goto func_exit;
	case TRX_STATE_NOT_STARTED:
		break;
	}

	ut_error;
	goto func_exit;
}

/** Report progress when rolling back a row of a recovered transaction.
@return	whether the rollback should be aborted due to pending shutdown */
bool
trx_roll_must_shutdown()
{
	const trx_t* trx = trx_roll_crash_recv_trx;
	ut_ad(trx);
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));

	if (trx_get_dict_operation(trx) == TRX_DICT_OP_NONE
	    && srv_shutdown_state != SRV_SHUTDOWN_NONE
	    && !srv_undo_sources && srv_fast_shutdown) {
		return true;
	}

	time_t now = time(NULL);
	mutex_enter(&trx_sys->mutex);
	mutex_enter(&recv_sys->mutex);

	if (recv_sys->report(now)) {
		ulint n_trx = 0;
		ulonglong n_rows = 0;
		for (const trx_t* t = UT_LIST_GET_FIRST(trx_sys->rw_trx_list);
		     t != NULL;
		     t = UT_LIST_GET_NEXT(trx_list, t)) {

			ut_ad(!t->read_only);
			ut_ad(t->in_rw_trx_list);
			ut_ad(!t->is_autocommit_non_locking());
			ut_ad(t->state != TRX_STATE_NOT_STARTED);

			if (t->is_recovered
			    && trx_state_eq(t, TRX_STATE_ACTIVE)) {
				n_trx++;
				n_rows += t->undo_no;
			}
		}
		if (n_rows > 0) {
			service_manager_extend_timeout(
				INNODB_EXTEND_TIMEOUT_INTERVAL,
				"To roll back: " ULINTPF " transactions, "
				"%llu rows", n_trx, n_rows);
		}

		ib::info() << "To roll back: " << n_trx << " transactions, "
			   << n_rows << " rows";
	}

	mutex_exit(&recv_sys->mutex);
	mutex_exit(&trx_sys->mutex);
	return false;
}

/*******************************************************************//**
Rollback or clean up any incomplete transactions which were
encountered in crash recovery.  If the transaction already was
committed, then we clean up a possible insert undo log. If the
transaction was not yet committed, then we roll it back. */
void
trx_rollback_or_clean_recovered(
/*============================*/
	ibool	all)	/*!< in: FALSE=roll back dictionary transactions;
			TRUE=roll back all non-PREPARED transactions */
{
	trx_t*	trx;

	ut_a(srv_force_recovery < SRV_FORCE_NO_TRX_UNDO);

	if (trx_sys_get_n_rw_trx() == 0) {

		return;
	}

	if (all) {
		ib::info() << "Starting in background the rollback"
			" of recovered transactions";
	}

	/* Note: For XA recovered transactions, we rely on MySQL to
	do rollback. They will be in TRX_STATE_PREPARED state. If the server
	is shutdown and they are still lingering in trx_sys_t::trx_list
	then the shutdown will hang. */

	/* Loop over the transaction list as long as there are
	recovered transactions to clean up or recover. */

	do {
		trx_sys_mutex_enter();

		for (trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list);
		     trx != NULL;
		     trx = UT_LIST_GET_NEXT(trx_list, trx)) {

			ut_ad(!trx->read_only);
			ut_ad(trx->in_rw_trx_list);
			ut_ad(!trx->is_autocommit_non_locking());
			ut_ad(trx->state != TRX_STATE_NOT_STARTED);

			/* If this function does a cleanup or rollback
			then it will release the trx_sys->mutex, therefore
			we need to reacquire it before retrying the loop. */

			if (trx_rollback_resurrected(trx, &all)) {

				trx_sys_mutex_enter();

				break;
			}
		}

		trx_sys_mutex_exit();

	} while (trx != NULL);

	if (all) {
		ib::info() << "Rollback of non-prepared transactions"
			" completed";
	}
}

/*******************************************************************//**
Rollback or clean up any incomplete transactions which were
encountered in crash recovery.  If the transaction already was
committed, then we clean up a possible insert undo log. If the
transaction was not yet committed, then we roll it back.
Note: this is done in a background thread.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(trx_rollback_or_clean_all_recovered)(
/*================================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	my_thread_init();
	ut_ad(!srv_read_only_mode);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(trx_rollback_clean_thread_key);
#endif /* UNIV_PFS_THREAD */

	trx_rollback_or_clean_recovered(TRUE);

	trx_rollback_or_clean_is_active = false;

	my_thread_end();
	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/** Try to truncate the undo logs.
@param[in,out]	trx	transaction */
static
void
trx_roll_try_truncate(trx_t* trx)
{
	ut_ad(mutex_own(&trx->undo_mutex));

	trx->pages_undone = 0;

	undo_no_t	undo_no		= trx->undo_no;
	trx_undo_t*	insert_undo	= trx->rsegs.m_redo.insert_undo;
	trx_undo_t*	update_undo	= trx->rsegs.m_redo.update_undo;

	if (insert_undo || update_undo) {
		mutex_enter(&trx->rsegs.m_redo.rseg->mutex);
		if (insert_undo) {
			ut_ad(insert_undo->rseg == trx->rsegs.m_redo.rseg);
			trx_undo_truncate_end(insert_undo, undo_no, false);
		}
		if (update_undo) {
			ut_ad(update_undo->rseg == trx->rsegs.m_redo.rseg);
			trx_undo_truncate_end(update_undo, undo_no, false);
		}
		mutex_exit(&trx->rsegs.m_redo.rseg->mutex);
	}

	if (trx_undo_t* undo = trx->rsegs.m_noredo.undo) {
		ut_ad(undo->rseg == trx->rsegs.m_noredo.rseg);
		mutex_enter(&undo->rseg->mutex);
		trx_undo_truncate_end(undo, undo_no, true);
		mutex_exit(&undo->rseg->mutex);
	}
}

/***********************************************************************//**
Pops the topmost undo log record in a single undo log and updates the info
about the topmost record in the undo log memory struct.
@return undo log record, the page s-latched */
static
trx_undo_rec_t*
trx_roll_pop_top_rec(
/*=================*/
	trx_t*		trx,	/*!< in: transaction */
	trx_undo_t*	undo,	/*!< in: undo log */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ut_ad(mutex_own(&trx->undo_mutex));

	page_t*	undo_page = trx_undo_page_get_s_latched(
		page_id_t(undo->space, undo->top_page_no), mtr);

	ulint	offset = undo->top_offset;

	trx_undo_rec_t*	prev_rec = trx_undo_get_prev_rec(
		undo_page + offset, undo->hdr_page_no, undo->hdr_offset,
		true, mtr);

	if (prev_rec == NULL) {

		undo->empty = TRUE;
	} else {
		page_t*	prev_rec_page = page_align(prev_rec);

		if (prev_rec_page != undo_page) {

			trx->pages_undone++;
		}

		undo->top_page_no = page_get_page_no(prev_rec_page);
		undo->top_offset  = prev_rec - prev_rec_page;
		undo->top_undo_no = trx_undo_rec_get_undo_no(prev_rec);
	}

	return(undo_page + offset);
}

/** Get the last undo log record of a transaction (for rollback).
@param[in,out]	trx		transaction
@param[out]	roll_ptr	DB_ROLL_PTR to the undo record
@param[in,out]	heap		memory heap for allocation
@return	undo log record copied to heap
@retval	NULL if none left or the roll_limit (savepoint) was reached */
trx_undo_rec_t*
trx_roll_pop_top_rec_of_trx(trx_t* trx, roll_ptr_t* roll_ptr, mem_heap_t* heap)
{
	mutex_enter(&trx->undo_mutex);

	if (trx->pages_undone >= TRX_ROLL_TRUNC_THRESHOLD) {
		trx_roll_try_truncate(trx);
	}

	trx_undo_t*	undo = NULL;
	trx_undo_t*	insert	= trx->rsegs.m_redo.insert_undo;
	trx_undo_t*	update	= trx->rsegs.m_redo.update_undo;
	trx_undo_t*	temp	= trx->rsegs.m_noredo.undo;
	const undo_no_t	limit	= trx->roll_limit;

	ut_ad(!insert || !update || insert->empty || update->empty
	      || insert->top_undo_no != update->top_undo_no);
	ut_ad(!insert || !temp || insert->empty || temp->empty
	      || insert->top_undo_no != temp->top_undo_no);
	ut_ad(!update || !temp || update->empty || temp->empty
	      || update->top_undo_no != temp->top_undo_no);

	if (insert && !insert->empty && limit <= insert->top_undo_no) {
		undo = insert;
	}

	if (update && !update->empty && update->top_undo_no >= limit) {
		if (!undo) {
			undo = update;
		} else if (undo->top_undo_no < update->top_undo_no) {
			undo = update;
		}
	}

	if (temp && !temp->empty && temp->top_undo_no >= limit) {
		if (!undo) {
			undo = temp;
		} else if (undo->top_undo_no < temp->top_undo_no) {
			undo = temp;
		}
	}

	if (undo == NULL) {
		trx_roll_try_truncate(trx);
		/* Mark any ROLLBACK TO SAVEPOINT completed, so that
		if the transaction object is committed and reused
		later, we will default to a full ROLLBACK. */
		trx->roll_limit = 0;
		ut_d(trx->in_rollback = false);
		mutex_exit(&trx->undo_mutex);
		return(NULL);
	}

	ut_ad(!undo->empty);
	ut_ad(limit <= undo->top_undo_no);

	*roll_ptr = trx_undo_build_roll_ptr(
		false, undo->rseg->id, undo->top_page_no, undo->top_offset);

	mtr_t	mtr;
	mtr.start();

	trx_undo_rec_t*	undo_rec = trx_roll_pop_top_rec(trx, undo, &mtr);
	const undo_no_t	undo_no = trx_undo_rec_get_undo_no(undo_rec);
	switch (trx_undo_rec_get_type(undo_rec)) {
	case TRX_UNDO_RENAME_TABLE:
		ut_ad(undo == insert);
		/* fall through */
	case TRX_UNDO_INSERT_REC:
		ut_ad(undo == insert || undo == temp);
		*roll_ptr |= 1ULL << ROLL_PTR_INSERT_FLAG_POS;
		break;
	default:
		ut_ad(undo == update || undo == temp);
		break;
	}

	ut_ad(trx_roll_check_undo_rec_ordering(
		undo_no, undo->rseg->space, trx));

	trx->undo_no = undo_no;
	trx->undo_rseg_space = undo->rseg->space;
	mutex_exit(&trx->undo_mutex);

	trx_undo_rec_t*	undo_rec_copy = trx_undo_rec_copy(undo_rec, heap);
	mtr.commit();

	return(undo_rec_copy);
}

/****************************************************************//**
Builds an undo 'query' graph for a transaction. The actual rollback is
performed by executing this query graph like a query subprocedure call.
The reply about the completion of the rollback will be sent by this
graph.
@return own: the query graph */
static
que_t*
trx_roll_graph_build(
/*=================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	mem_heap_t*	heap;
	que_fork_t*	fork;
	que_thr_t*	thr;

	ut_ad(trx_mutex_own(trx));

	heap = mem_heap_create(512);
	fork = que_fork_create(NULL, NULL, QUE_FORK_ROLLBACK, heap);
	fork->trx = trx;

	thr = que_thr_create(fork, heap, NULL);

	thr->child = row_undo_node_create(trx, thr, heap);

	return(fork);
}

/*********************************************************************//**
Starts a rollback operation, creates the UNDO graph that will do the
actual undo operation.
@return query graph thread that will perform the UNDO operations. */
static
que_thr_t*
trx_rollback_start(
/*===============*/
	trx_t*		trx,		/*!< in: transaction */
	undo_no_t	roll_limit)	/*!< in: rollback to undo no (for
					partial undo), 0 if we are rolling back
					the entire transaction */
{
	ut_ad(trx_mutex_own(trx));

	/* Initialize the rollback field in the transaction */

	ut_ad(!trx->roll_limit);
	ut_ad(!trx->in_rollback);

	trx->roll_limit = roll_limit;
	ut_d(trx->in_rollback = true);

	ut_a(trx->roll_limit <= trx->undo_no);

	trx->pages_undone = 0;

	/* Build a 'query' graph which will perform the undo operations */

	que_t*	roll_graph = trx_roll_graph_build(trx);

	trx->graph = roll_graph;

	trx->lock.que_state = TRX_QUE_ROLLING_BACK;

	return(que_fork_start_command(roll_graph));
}

/****************************************************************//**
Finishes a transaction rollback. */
static
void
trx_rollback_finish(
/*================*/
	trx_t*		trx)	/*!< in: transaction */
{
	trx_commit(trx);

	trx->mod_tables.clear();

	trx->lock.que_state = TRX_QUE_RUNNING;
}

/*********************************************************************//**
Creates a rollback command node struct.
@return own: rollback node struct */
roll_node_t*
roll_node_create(
/*=============*/
	mem_heap_t*	heap)	/*!< in: mem heap where created */
{
	roll_node_t*	node;

	node = static_cast<roll_node_t*>(mem_heap_zalloc(heap, sizeof(*node)));

	node->state = ROLL_NODE_SEND;

	node->common.type = QUE_NODE_ROLLBACK;

	return(node);
}

/***********************************************************//**
Performs an execution step for a rollback command node in a query graph.
@return query thread to run next, or NULL */
que_thr_t*
trx_rollback_step(
/*==============*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	roll_node_t*	node;

	node = static_cast<roll_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_ROLLBACK);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = ROLL_NODE_SEND;
	}

	if (node->state == ROLL_NODE_SEND) {
		trx_t*		trx;
		ib_id_t		roll_limit;

		trx = thr_get_trx(thr);

		trx_mutex_enter(trx);

		node->state = ROLL_NODE_WAIT;

		ut_a(node->undo_thr == NULL);

		roll_limit = node->savept ? node->savept->least_undo_no : 0;

		trx_commit_or_rollback_prepare(trx);

		node->undo_thr = trx_rollback_start(trx, roll_limit);

		trx_mutex_exit(trx);

	} else {
		ut_ad(node->state == ROLL_NODE_WAIT);

		thr->run_node = que_node_get_parent(node);
	}

	return(thr);
}
