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

extern tpool::task_group rollback_all_recovered_group;
extern tpool::waitable_task rollback_all_recovered_task;
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
	undo_no_t		savept;	/*!< savepoint to which to
					roll back; 0=entire transaction */
	que_thr_t*		undo_thr;/*!< undo query graph */
};

#endif
