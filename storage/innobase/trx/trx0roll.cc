/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2019, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

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
#include "row0mysql.h"
#include "row0undo.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "trx0rec.h"
#include "trx0rseg.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0undo.h"

/** true if trx_rollback_all_recovered() thread is active */
bool			trx_rollback_is_active;

/** In crash recovery, the current trx to be rolled back; NULL otherwise */
const trx_t*		trx_roll_crash_recv_trx;

/** Finish transaction rollback.
@param[in,out]	trx	transaction
@return	whether the rollback was completed normally
@retval	false	if the rollback was aborted by shutdown  */
static bool trx_rollback_finish(trx_t* trx)
{
	trx->mod_tables.clear();
	bool finished = trx->error_state == DB_SUCCESS;
	if (UNIV_LIKELY(finished)) {
		trx_commit(trx);
	} else {
		ut_a(trx->error_state == DB_INTERRUPTED);
		ut_ad(!srv_is_being_started);
		ut_a(!srv_undo_sources);
		ut_ad(srv_fast_shutdown);
		ut_d(trx->in_rollback = false);
		if (trx_undo_t*& undo = trx->rsegs.m_redo.old_insert) {
			UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->old_insert_list,
				       undo);
			ut_free(undo);
			undo = NULL;
		}
		if (trx_undo_t*& undo = trx->rsegs.m_redo.undo) {
			UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->undo_list,
				       undo);
			ut_free(undo);
			undo = NULL;
		}
		if (trx_undo_t*& undo = trx->rsegs.m_noredo.undo) {
			UT_LIST_REMOVE(trx->rsegs.m_noredo.rseg->undo_list,
				       undo);
			ut_free(undo);
			undo = NULL;
		}
		trx_commit_low(trx, NULL);
	}

	trx->lock.que_state = TRX_QUE_RUNNING;

	return finished;
}

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

	if (savept != NULL) {
		roll_node->partial = TRUE;
		roll_node->savept = *savept;
		check_trx_state(trx);
	} else {
		assert_trx_nonlocking_or_in_list(trx);
	}

	trx->error_state = DB_SUCCESS;

	if (trx->has_logged_or_recovered()) {

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
		ut_a(trx->error_state == DB_SUCCESS);
		const undo_no_t limit = savept->least_undo_no;
		for (trx_mod_tables_t::iterator i = trx->mod_tables.begin();
		     i != trx->mod_tables.end(); ) {
			trx_mod_tables_t::iterator j = i++;
			ut_ad(j->second.valid());
			if (j->second.rollback(limit)) {
				trx->mod_tables.erase(j);
			}
		}
		trx->lock.que_state = TRX_QUE_RUNNING;
		MONITOR_INC(MONITOR_TRX_ROLLBACK_SAVEPOINT);
	}

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
#ifdef WITH_WSREP
	if (savept == NULL && wsrep_on(trx->mysql_thd)) {
		wsrep_handle_SR_rollback(NULL, trx->mysql_thd);
	}
#endif /* WITH_WSREP */
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

	return(trx->error_state);
}

/** Rollback a transaction used in MySQL
@param[in, out]	trx	transaction
@return error code or DB_SUCCESS */
dberr_t trx_rollback_for_mysql(trx_t* trx)
{
	/* We are reading trx->state without holding trx_sys.mutex
	here, because the rollback should be invoked for a running
	active MySQL transaction (or recovered prepared transaction)
	that is associated with the current thread. */

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		trx->will_lock = 0;
		ut_ad(trx->mysql_thd);
		return(DB_SUCCESS);

	case TRX_STATE_ACTIVE:
		ut_ad(trx->mysql_thd);
		assert_trx_nonlocking_or_in_list(trx);
		return(trx_rollback_for_mysql_low(trx));

	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		ut_ad(!trx_is_autocommit_non_locking(trx));
		if (trx->rsegs.m_redo.undo || trx->rsegs.m_redo.old_insert) {
			/* Change the undo log state back from
			TRX_UNDO_PREPARED to TRX_UNDO_ACTIVE
			so that if the system gets killed,
			recovery will perform the rollback. */
			ut_ad(!trx->rsegs.m_redo.undo
			      || trx->rsegs.m_redo.undo->rseg
			      == trx->rsegs.m_redo.rseg);
			ut_ad(!trx->rsegs.m_redo.old_insert
			      || trx->rsegs.m_redo.old_insert->rseg
			      == trx->rsegs.m_redo.rseg);
			mtr_t		mtr;
			mtr.start();
			mutex_enter(&trx->rsegs.m_redo.rseg->mutex);
			if (trx_undo_t* undo = trx->rsegs.m_redo.undo) {
				trx_undo_set_state_at_prepare(trx, undo, true,
							      &mtr);
			}
			if (trx_undo_t* undo = trx->rsegs.m_redo.old_insert) {
				trx_undo_set_state_at_prepare(trx, undo, true,
							      &mtr);
			}
			mutex_exit(&trx->rsegs.m_redo.rseg->mutex);
			/* Persist the XA ROLLBACK, so that crash
			recovery will replay the rollback in case
			the redo log gets applied past this point. */
			mtr.commit();
			ut_ad(mtr.commit_lsn() > 0);
		}
#ifdef ENABLED_DEBUG_SYNC
		if (trx->mysql_thd == NULL) {
			/* We could be executing XA ROLLBACK after
			XA PREPARE and a server restart. */
		} else if (!trx->has_logged_persistent()) {
			/* innobase_close_connection() may roll back a
			transaction that did not generate any
			persistent undo log. The DEBUG_SYNC
			would cause an assertion failure for a
			disconnected thread.

			NOTE: InnoDB will not know about the XID
			if no persistent undo log was generated. */
		} else {
			DEBUG_SYNC_C("trx_xa_rollback");
		}
#endif /* ENABLED_DEBUG_SYNC */
		return(trx_rollback_for_mysql_low(trx));

	case TRX_STATE_COMMITTED_IN_MEMORY:
		check_trx_state(trx);
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

	/* We are reading trx->state without holding trx_sys.mutex
	here, because the statement rollback should be invoked for a
	running active MySQL transaction that is associated with the
	current thread. */
	ut_ad(trx->mysql_thd);

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		return(DB_SUCCESS);

	case TRX_STATE_ACTIVE:
		assert_trx_nonlocking_or_in_list(trx);

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
	ut_ad(trx->mysql_thd);

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
	trx->lock.was_chosen_as_wsrep_victim = FALSE;
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

	/* We are reading trx->state without holding trx_sys.mutex
	here, because the savepoint rollback should be invoked for a
	running active MySQL transaction that is associated with the
	current thread. */
	ut_ad(trx->mysql_thd);

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
	ut_ad(trx->mysql_thd);

	savep = trx_savepoint_find(trx, savepoint_name);

	if (savep != NULL) {
		trx_roll_savepoint_free(trx, savep);
	}

	return(savep != NULL ? DB_SUCCESS : DB_NO_SAVEPOINT);
}

/*******************************************************************//**
Determines if this transaction is rolling back an incomplete transaction
in crash recovery.
@return TRUE if trx is an incomplete transaction that is being rolled
back in crash recovery */
ibool
trx_is_recv(
/*========*/
	const trx_t*	trx)	/*!< in: transaction */
{
	return(trx == trx_roll_crash_recv_trx);
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

	const bool dictionary_locked = trx_get_dict_operation(trx)
		!= TRX_DICT_OP_NONE;

	if (dictionary_locked) {
		row_mysql_lock_data_dictionary(trx);
	}

	que_run_threads(thr);
	ut_a(roll_node->undo_thr != NULL);

	que_run_threads(roll_node->undo_thr);

	que_graph_free(
		static_cast<que_t*>(roll_node->undo_thr->common.parent));

	if (UNIV_UNLIKELY(!trx_rollback_finish(trx))) {
		ut_ad(!dictionary_locked);
		goto func_exit;
	}

	ut_a(trx->lock.que_state == TRX_QUE_RUNNING);

	if (!dictionary_locked || !trx->table_id) {
	} else if (dict_table_t* table = dict_table_open_on_id(
			   trx->table_id, TRUE, DICT_TABLE_OP_NORMAL)) {
		ib::info() << "Dropping table " << table->name
			   << ", with id " << trx->table_id
			   << " in recovery";

		dict_table_close_and_drop(trx, table);

		trx_commit_for_mysql(trx);
	}

	ib::info() << "Rolled back recovered transaction " << trx_id;

func_exit:
	if (dictionary_locked) {
		row_mysql_unlock_data_dictionary(trx);
	}

	mem_heap_free(heap);

	trx_roll_crash_recv_trx	= NULL;
}


struct trx_roll_count_callback_arg
{
  uint32_t n_trx;
  uint64_t n_rows;
  trx_roll_count_callback_arg(): n_trx(0), n_rows(0) {}
};


static my_bool trx_roll_count_callback(rw_trx_hash_element_t *element,
                                       trx_roll_count_callback_arg *arg)
{
  mutex_enter(&element->mutex);
  if (trx_t *trx= element->trx)
  {
    if (trx->is_recovered && trx_state_eq(trx, TRX_STATE_ACTIVE))
    {
      arg->n_trx++;
      arg->n_rows+= trx->undo_no;
    }
  }
  mutex_exit(&element->mutex);
  return 0;
}

/** Report progress when rolling back a row of a recovered transaction. */
void trx_roll_report_progress()
{
	ib_time_t time = ut_time();
	mutex_enter(&recv_sys->mutex);
	bool report = recv_sys->report(time);
	mutex_exit(&recv_sys->mutex);

	if (report) {
		trx_roll_count_callback_arg arg;

		/* Get number of recovered active transactions and number of
		rows they modified. Numbers must be accurate, because only this
		thread is allowed to touch recovered transactions. */
		trx_sys.rw_trx_hash.iterate_no_dups(
			reinterpret_cast<my_hash_walk_action>
			(trx_roll_count_callback), &arg);

		if (arg.n_rows > 0) {
			service_manager_extend_timeout(
				INNODB_EXTEND_TIMEOUT_INTERVAL,
				"To roll back: " UINT32PF " transactions, "
				UINT64PF " rows", arg.n_trx, arg.n_rows);
		}

		ib::info() << "To roll back: " << arg.n_trx
			   << " transactions, " << arg.n_rows << " rows";

	}
}


static my_bool trx_rollback_recovered_callback(rw_trx_hash_element_t *element,
                                               std::vector<trx_t*> *trx_list)
{
  mutex_enter(&element->mutex);
  if (trx_t *trx= element->trx)
  {
    mutex_enter(&trx->mutex);
    if (trx->is_recovered && trx_state_eq(trx, TRX_STATE_ACTIVE))
      trx_list->push_back(trx);
    mutex_exit(&trx->mutex);
  }
  mutex_exit(&element->mutex);
  return 0;
}


/**
  Rollback any incomplete transactions which were encountered in crash recovery.

  If the transaction already was committed, then we clean up a possible insert
  undo log. If the transaction was not yet committed, then we roll it back.

  Note: For XA recovered transactions, we rely on MySQL to
  do rollback. They will be in TRX_STATE_PREPARED state. If the server
  is shutdown and they are still lingering in trx_sys_t::trx_list
  then the shutdown will hang.

  @param[in]  all  true=roll back all recovered active transactions;
                   false=roll back any incomplete dictionary transaction
*/

void trx_rollback_recovered(bool all)
{
  std::vector<trx_t*> trx_list;

  ut_a(srv_force_recovery < SRV_FORCE_NO_TRX_UNDO);

  /*
    Collect list of recovered ACTIVE transaction ids first. Once collected, no
    other thread is allowed to modify or remove these transactions from
    rw_trx_hash.
  */
  trx_sys.rw_trx_hash.iterate_no_dups(reinterpret_cast<my_hash_walk_action>
                                      (trx_rollback_recovered_callback),
                                      &trx_list);

  while (!trx_list.empty())
  {
    trx_t *trx= trx_list.back();
    trx_list.pop_back();

#ifdef UNIV_DEBUG
    ut_ad(trx);
    trx_mutex_enter(trx);
    ut_ad(trx->is_recovered && trx_state_eq(trx, TRX_STATE_ACTIVE));
    trx_mutex_exit(trx);
#endif

    if (!srv_is_being_started && !srv_undo_sources && srv_fast_shutdown)
      goto discard;

    if (all || trx_get_dict_operation(trx) != TRX_DICT_OP_NONE)
    {
      trx_rollback_active(trx);
      if (trx->error_state != DB_SUCCESS)
      {
        ut_ad(trx->error_state == DB_INTERRUPTED);
        trx->error_state= DB_SUCCESS;
        ut_ad(!srv_undo_sources);
        ut_ad(srv_fast_shutdown);
discard:
        trx_sys.deregister_rw(trx);
        trx_free_at_shutdown(trx);
      }
      else
        trx_free(trx);
    }
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
DECLARE_THREAD(trx_rollback_all_recovered)(void*)
{
	my_thread_init();
	ut_ad(!srv_read_only_mode);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(trx_rollback_clean_thread_key);
#endif /* UNIV_PFS_THREAD */

	if (trx_sys.rw_trx_hash.size()) {
		ib::info() << "Starting in background the rollback of"
			" recovered transactions";
		trx_rollback_recovered(true);
		ib::info() << "Rollback of non-prepared transactions"
			" completed";
	}

	trx_rollback_is_active = false;

	my_thread_end();
	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
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
	ib_id_t		roll_limit)	/*!< in: rollback to undo no (for
					partial undo), 0 if we are rolling back
					the entire transaction */
{
	ut_ad(trx_mutex_own(trx));

	/* Initialize the rollback field in the transaction */

	ut_ad(!trx->roll_limit);
	ut_ad(!trx->in_rollback);

	trx->roll_limit = roll_limit;
	trx->in_rollback = true;

	ut_a(trx->roll_limit <= trx->undo_no);

	trx->pages_undone = 0;

	/* Build a 'query' graph which will perform the undo operations */

	que_t*	roll_graph = trx_roll_graph_build(trx);

	trx->graph = roll_graph;

	trx->lock.que_state = TRX_QUE_ROLLING_BACK;

	return(que_fork_start_command(roll_graph));
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

		roll_limit = node->partial ? node->savept.least_undo_no : 0;

		trx_commit_or_rollback_prepare(trx);

		node->undo_thr = trx_rollback_start(trx, roll_limit);

		trx_mutex_exit(trx);

	} else {
		ut_ad(node->state == ROLL_NODE_WAIT);

		thr->run_node = que_node_get_parent(node);
	}

	return(thr);
}
