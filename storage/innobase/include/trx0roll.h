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
@file include/trx0roll.h
Transaction rollback

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#ifndef trx0roll_h
#define trx0roll_h

#include "trx0trx.h"
#include "mtr0mtr.h"
#include "trx0sys.h"

extern bool		trx_rollback_is_active;
extern const trx_t*	trx_roll_crash_recv_trx;

/** Report progress when rolling back a row of a recovered transaction. */
void trx_roll_report_progress();
/*******************************************************************//**
Rollback or clean up any incomplete transactions which were
encountered in crash recovery.  If the transaction already was
committed, then we clean up a possible insert undo log. If the
transaction was not yet committed, then we roll it back.
@param all true=roll back all recovered active transactions;
false=roll back any incomplete dictionary transaction */
void
trx_rollback_recovered(bool all);
/*******************************************************************//**
Rollback or clean up any incomplete transactions which were
encountered in crash recovery.  If the transaction already was
committed, then we clean up a possible insert undo log. If the
transaction was not yet committed, then we roll it back.
Note: this is done in a background thread. */
void trx_rollback_all_recovered(void*);
/*********************************************************************//**
Creates a rollback command node struct.
@return own: rollback node struct */
roll_node_t*
roll_node_create(
/*=============*/
	mem_heap_t*	heap);	/*!< in: mem heap where created */
/***********************************************************//**
Performs an execution step for a rollback command node in a query graph.
@return query thread to run next, or NULL */
que_thr_t*
trx_rollback_step(
/*==============*/
	que_thr_t*	thr);	/*!< in: query thread */
/*******************************************************************//**
Rollback a transaction used in MySQL.
@return error code or DB_SUCCESS */
dberr_t
trx_rollback_for_mysql(
/*===================*/
	trx_t*	trx)	/*!< in/out: transaction */
	MY_ATTRIBUTE((nonnull));
/*******************************************************************//**
Rollback the latest SQL statement for MySQL.
@return error code or DB_SUCCESS */
dberr_t
trx_rollback_last_sql_stat_for_mysql(
/*=================================*/
	trx_t*	trx)	/*!< in/out: transaction */
	MY_ATTRIBUTE((nonnull));
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
	MY_ATTRIBUTE((nonnull, warn_unused_result));
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
	MY_ATTRIBUTE((nonnull));
/*******************************************************************//**
Releases a named savepoint. Savepoints which
were set after this savepoint are deleted.
@return if no savepoint of the name found then DB_NO_SAVEPOINT,
otherwise DB_SUCCESS */
dberr_t
trx_release_savepoint_for_mysql(
/*============================*/
	trx_t*		trx,			/*!< in: transaction handle */
	const char*	savepoint_name)		/*!< in: savepoint name */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Rollback node states */
enum roll_node_state {
	ROLL_NODE_NONE = 0,		/*!< Unknown state */
	ROLL_NODE_SEND,			/*!< about to send a rollback signal to
					the transaction */
	ROLL_NODE_WAIT			/*!< rollback signal sent to the
					transaction, waiting for completion */
};

/** Rollback command node in a query graph */
struct roll_node_t{
	que_common_t		common;	/*!< node type: QUE_NODE_ROLLBACK */
	enum roll_node_state	state;	/*!< node execution state */
	const trx_savept_t*	savept;	/*!< savepoint to which to
					roll back, in the case of a
					partial rollback */
	que_thr_t*		undo_thr;/*!< undo query graph */
};

/** A savepoint set with SQL's "SAVEPOINT savepoint_id" command */
struct trx_named_savept_t{
	char*		name;		/*!< savepoint name */
	trx_savept_t	savept;		/*!< the undo number corresponding to
					the savepoint */
	int64_t		mysql_binlog_cache_pos;
					/*!< the MySQL binlog cache position
					corresponding to this savepoint, not
					defined if the MySQL binlogging is not
					enabled */
	UT_LIST_NODE_T(trx_named_savept_t)
			trx_savepoints;	/*!< the list of savepoints of a
					transaction */
};

#endif
