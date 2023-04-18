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
@file include/que0que.h
Query graph

Created 5/27/1996 Heikki Tuuri
*******************************************************/

#ifndef que0que_h
#define que0que_h

#include "data0data.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "srv0srv.h"
#include "que0types.h"
#include "row0types.h"
#include "pars0types.h"

/***********************************************************************//**
Creates a query graph fork node.
@return own: fork node */
que_fork_t *que_fork_create(mem_heap_t* heap);
/***********************************************************************//**
Gets the first thr in a fork. */
UNIV_INLINE
que_thr_t*
que_fork_get_first_thr(
/*===================*/
	que_fork_t*	fork);	/*!< in: query fork */
/***********************************************************************//**
Gets the child node of the first thr in a fork. */
UNIV_INLINE
que_node_t*
que_fork_get_child(
/*===============*/
	que_fork_t*	fork);	/*!< in: query fork */
/***********************************************************************//**
Sets the parent of a graph node. */
UNIV_INLINE
void
que_node_set_parent(
/*================*/
	que_node_t*	node,	/*!< in: graph node */
	que_node_t*	parent);/*!< in: parent */
/** Creates a query graph thread node.
@param[in]	parent		parent node, i.e., a fork node
@param[in]	heap		memory heap where created
@param[in]	prebuilt	row prebuilt structure
@return own: query thread node */
que_thr_t*
que_thr_create(
	que_fork_t*	parent,
	mem_heap_t*	heap,
	row_prebuilt_t*	prebuilt);
/**********************************************************************//**
Frees a query graph, but not the heap where it was created. Does not free
explicit cursor declarations, they are freed in que_graph_free. */
void
que_graph_free_recursive(
/*=====================*/
	que_node_t*	node);	/*!< in: query graph node */
/**********************************************************************//**
Frees a query graph. */
void
que_graph_free(
/*===========*/
	que_t*	graph);	/*!< in: query graph; we assume that the memory
			heap where this graph was created is private
			to this graph: if not, then use
			que_graph_free_recursive and free the heap
			afterwards! */

/**********************************************************************//**
Run a query thread. Handles lock waits. */
void
que_run_threads(
/*============*/
	que_thr_t*	thr);	/*!< in: query thread */
/**********************************************************************//**
Starts execution of a command in a query fork. Picks a query thread which
is not in the QUE_THR_RUNNING state and moves it to that state. If none
can be chosen, a situation which may arise in parallelized fetches, NULL
is returned.
@return a query thread of the graph moved to QUE_THR_RUNNING state, or
NULL; the query thread should be executed by que_run_threads by the
caller */
que_thr_t*
que_fork_start_command(
/*===================*/
	que_fork_t*	fork);	/*!< in: a query fork */
/***********************************************************************//**
Gets the trx of a query thread. */
UNIV_INLINE
trx_t*
thr_get_trx(
/*========*/
	que_thr_t*	thr);	/*!< in: query thread */
/***********************************************************************//**
Gets the type of a graph node. */
UNIV_INLINE
ulint
que_node_get_type(
/*==============*/
	const que_node_t*	node);	/*!< in: graph node */
/***********************************************************************//**
Gets pointer to the value data type field of a graph node. */
UNIV_INLINE
dtype_t*
que_node_get_data_type(
/*===================*/
	que_node_t*	node);	/*!< in: graph node */
/***********************************************************************//**
Gets pointer to the value dfield of a graph node. */
UNIV_INLINE
dfield_t*
que_node_get_val(
/*=============*/
	que_node_t*	node);	/*!< in: graph node */
/***********************************************************************//**
Gets the value buffer size of a graph node.
@return val buffer size, not defined if val.data == NULL in node */
UNIV_INLINE
ulint
que_node_get_val_buf_size(
/*======================*/
	que_node_t*	node);	/*!< in: graph node */
/***********************************************************************//**
Sets the value buffer size of a graph node. */
UNIV_INLINE
void
que_node_set_val_buf_size(
/*======================*/
	que_node_t*	node,	/*!< in: graph node */
	ulint		size);	/*!< in: size */
/*********************************************************************//**
Gets the next list node in a list of query graph nodes. */
UNIV_INLINE
que_node_t*
que_node_get_next(
/*==============*/
	que_node_t*	node);	/*!< in: node in a list */
/*********************************************************************//**
Gets the parent node of a query graph node.
@return parent node or NULL */
UNIV_INLINE
que_node_t*
que_node_get_parent(
/*================*/
	que_node_t*	node);	/*!< in: node */
/****************************************************************//**
Get the first containing loop node (e.g. while_node_t or for_node_t) for the
given node, or NULL if the node is not within a loop.
@return containing loop node, or NULL. */
que_node_t*
que_node_get_containing_loop_node(
/*==============================*/
	que_node_t*	node);	/*!< in: node */
/*********************************************************************//**
Catenates a query graph node to a list of them, possible empty list.
@return one-way list of nodes */
UNIV_INLINE
que_node_t*
que_node_list_add_last(
/*===================*/
	que_node_t*	node_list,	/*!< in: node list, or NULL */
	que_node_t*	node);		/*!< in: node */
/*************************************************************************
Get the last node from the list.*/
UNIV_INLINE
que_node_t*
que_node_list_get_last(
/*===================*/
					/* out: node last node from list.*/
	que_node_t*	node_list);	/* in: node list, or NULL */
/*********************************************************************//**
Gets a query graph node list length.
@return length, for NULL list 0 */
UNIV_INLINE
ulint
que_node_list_get_len(
/*==================*/
	que_node_t*	node_list);	/*!< in: node list, or NULL */
/*********************************************************************//**
Evaluate the given SQL
@return error code or DB_SUCCESS */
dberr_t
que_eval_sql(
/*=========*/
	pars_info_t*	info,	/*!< in: info struct, or NULL */
	const char*	sql,	/*!< in: SQL string */
	trx_t*		trx);	/*!< in: trx */

/**********************************************************************//**
Round robin scheduler.
@return a query thread of the graph moved to QUE_THR_RUNNING state, or
NULL; the query thread should be executed by que_run_threads by the
caller */
que_thr_t*
que_fork_scheduler_round_robin(
/*===========================*/
	que_fork_t*	fork,		/*!< in: a query fork */
	que_thr_t*	thr);		/*!< in: current pos */

/** Query thread states */
enum que_thr_state_t {
	/** in selects this means that the thread is at the end of its
	result set (or start, in case of a scroll cursor); in other
	statements, this means the thread has done its task */
	QUE_THR_COMPLETED,
	QUE_THR_RUNNING
};

/** Query thread lock states */
enum que_thr_lock_t {
	QUE_THR_LOCK_NOLOCK,
	QUE_THR_LOCK_ROW,
	QUE_THR_LOCK_TABLE
};

/* Query graph query thread node: the fields are protected by the
trx_t::mutex with the exceptions named below */

struct que_thr_t{
	que_common_t	common;		/*!< type: QUE_NODE_THR */
	que_node_t*	child;		/*!< graph child node */
	que_t*		graph;		/*!< graph where this node belongs */
	que_thr_state_t	state;		/*!< state of the query thread */
	/*------------------------------*/
	/* The following fields are private to the OS thread executing the
	query thread, and are not protected by any mutex: */

	que_node_t*	run_node;	/*!< pointer to the node where the
					subgraph down from this node is
					currently executed */
	que_node_t*	prev_node;	/*!< pointer to the node from which
					the control came */
	ulint		resource;	/*!< resource usage of the query thread
					thus far */
	ulint		lock_state;	/*!< lock state of thread (table or
					row) */
	/*------------------------------*/
	/* The following fields are links for the various lists that
	this type can be on. */
	UT_LIST_NODE_T(que_thr_t)
			thrs;		/*!< list of thread nodes of the fork
					node */
	UT_LIST_NODE_T(que_thr_t)
			queue;		/*!< list of runnable thread nodes in
					the server task queue */
	ulint		fk_cascade_depth; /*!< maximum cascading call depth
					supported for foreign key constraint
					related delete/updates */
	row_prebuilt_t*	prebuilt;	/*!< prebuilt structure processed by
					the query thread */
};

/* Query graph fork node: its fields are protected by the query thread mutex */
struct que_fork_t{
	que_common_t	common;		/*!< type: QUE_NODE_FORK */
	que_t*		graph;		/*!< query graph of this node */
	trx_t*		trx;		/*!< transaction: this is set only in
					the root node */
	ulint		state;		/*!< state of the fork node */
	que_thr_t*	caller;		/*!< pointer to a possible calling query
					thread */
	UT_LIST_BASE_NODE_T(que_thr_t)
			thrs;		/*!< list of query threads */
	/*------------------------------*/
	/* The fields in this section are defined only in the root node */
	sym_tab_t*	sym_tab;	/*!< symbol table of the query,
					generated by the parser, or NULL
					if the graph was created 'by hand' */
	pars_info_t*	info;		/*!< info struct, or NULL */

	sel_node_t*	last_sel_node;	/*!< last executed select node, or NULL
					if none */
	UT_LIST_NODE_T(que_fork_t)
			graphs;		/*!< list of query graphs of a session
					or a stored procedure */
	/*------------------------------*/
	mem_heap_t*	heap;		/*!< memory heap where the fork was
					created */

};

/* Query fork (or graph) states */
#define QUE_FORK_ACTIVE		1
#define QUE_FORK_COMMAND_WAIT	2

/* Flag which is ORed to control structure statement node types */
#define QUE_NODE_CONTROL_STAT	1024

#include "que0que.inl"

#endif
