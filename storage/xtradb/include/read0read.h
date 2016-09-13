/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.

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
@file include/read0read.h
Cursor read

Created 2/16/1997 Heikki Tuuri
*******************************************************/

#ifndef read0read_h
#define read0read_h

#include "univ.i"


#include "ut0byte.h"
#include "ut0lst.h"
#include "btr0types.h"
#include "trx0trx.h"
#include "trx0sys.h"
#include "read0types.h"

/*********************************************************************//**
Opens a read view where exactly the transactions serialized before this
point in time are seen in the view.
@return	own: read view struct */
UNIV_INTERN
read_view_t*
read_view_open_now(
/*===============*/
	trx_id_t	cr_trx_id,	/*!< in: trx_id of creating
					transaction, or 0 used in purge */
	read_view_t*&	view);		/*!< in,out: pre-allocated view array or
					NULL if a new one needs to be created */

/*********************************************************************//**
Clones a read view object. This function will allocate space for two read
views contiguously, one identical in size and content as @param view (starting
at returned pointer) and another view immediately following the trx_ids array.
The second view will have space for an extra trx_id_t element.
@return	read view struct */
UNIV_INTERN
read_view_t*
read_view_clone(
/*============*/
	const read_view_t*	view,		/*!< in: view to clone */
	read_view_t*&		prebuilt_clone);/*!< in,out: prebuilt view or
						NULL */
/*********************************************************************//**
Insert the view in the proper order into the trx_sys->view_list. The
read view list is ordered by read_view_t::low_limit_no in descending order. */
UNIV_INTERN
void
read_view_add(
/*==========*/
	read_view_t*	view);		/*!< in: view to add to */
/*********************************************************************//**
Makes a copy of the oldest existing read view, or opens a new. The view
must be closed with ..._close.
@return	own: read view struct */
UNIV_INTERN
read_view_t*
read_view_purge_open(
/*=================*/
	read_view_t*&	clone_view,	/*!< in,out: pre-allocated view that
					will be used to clone the oldest view if
					exists */
	read_view_t*&	view);		/*!< in,out: pre-allocated view array or
					NULL if a new one needs to be created */
/*********************************************************************//**
Remove a read view from the trx_sys->view_list. */
UNIV_INLINE
void
read_view_remove(
/*=============*/
	read_view_t*	view,		/*!< in: read view, can be 0 */
	bool		own_mutex);	/*!< in: true if caller owns the
					trx_sys_t::mutex */
/*********************************************************************//**
Frees memory allocated by a read view. */
UNIV_INTERN
void
read_view_free(
/*===========*/
	read_view_t*&	view);	/*< in,out: read view */
/*********************************************************************//**
Closes a consistent read view for MySQL. This function is called at an SQL
statement end if the trx isolation level is <= TRX_ISO_READ_COMMITTED. */
UNIV_INTERN
void
read_view_close_for_mysql(
/*======================*/
	trx_t*	trx);	/*!< in: trx which has a read view */
/*********************************************************************//**
Checks if a read view sees the specified transaction.
@return	true if sees */
UNIV_INLINE
bool
read_view_sees_trx_id(
/*==================*/
	const read_view_t*	view,	/*!< in: read view */
	trx_id_t		trx_id)	/*!< in: trx id */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Prints a read view to file. */
UNIV_INTERN
void
read_view_print(
/*============*/
	FILE*			file,	/*!< in: file to print to */
	const read_view_t*	view);	/*!< in: read view */
/*********************************************************************//**
Create a consistent cursor view for mysql to be used in cursors. In this
consistent read view modifications done by the creating transaction or future
transactions are not visible. */
UNIV_INTERN
cursor_view_t*
read_cursor_view_create_for_mysql(
/*==============================*/
	trx_t*		cr_trx);/*!< in: trx where cursor view is created */
/*********************************************************************//**
Close a given consistent cursor view for mysql and restore global read view
back to a transaction read view. */
UNIV_INTERN
void
read_cursor_view_close_for_mysql(
/*=============================*/
	trx_t*		trx,		/*!< in: trx */
	cursor_view_t*	curview);	/*!< in: cursor view to be closed */
/*********************************************************************//**
This function sets a given consistent cursor view to a transaction
read view if given consistent cursor view is not NULL. Otherwise, function
restores a global read view to a transaction read view. */
UNIV_INTERN
void
read_cursor_set_for_mysql(
/*======================*/
	trx_t*		trx,	/*!< in: transaction where cursor is set */
	cursor_view_t*	curview);/*!< in: consistent cursor view to be set */

/** Read view lists the trx ids of those transactions for which a consistent
read should not see the modifications to the database. */

struct read_view_t{
	ulint		type;	/*!< VIEW_NORMAL, VIEW_HIGH_GRANULARITY */
	undo_no_t	undo_no;/*!< 0 or if type is
				VIEW_HIGH_GRANULARITY
				transaction undo_no when this high-granularity
				consistent read view was created */
	trx_id_t	low_limit_no;
				/*!< The view does not need to see the undo
				logs for transactions whose transaction number
				is strictly smaller (<) than this value: they
				can be removed in purge if not needed by other
				views */
	trx_id_t	low_limit_id;
				/*!< The read should not see any transaction
				with trx id >= this value. In other words,
				this is the "high water mark". */
	trx_id_t	up_limit_id;
				/*!< The read should see all trx ids which
				are strictly smaller (<) than this value.
				In other words,
				this is the "low water mark". */
	ulint		n_descr;
				/*!< Number of cells in the trx_ids array */
	ulint		max_descr;
				/*!< Maximum number of cells in the trx_ids
				array */
	trx_id_t*	descriptors;
				/*!< Additional trx ids which the read should
				not see: typically, these are the read-write
				active transactions at the time when the read
				is serialized, except the reading transaction
				itself; the trx ids in this array are in a
				ascending order. These trx_ids should be
				between the "low" and "high" water marks,
				that is, up_limit_id and low_limit_id. */
	trx_id_t	creator_trx_id;
				/*!< trx id of creating transaction, or
				0 used in purge */
	UT_LIST_NODE_T(read_view_t) view_list;
				/*!< List of read views in trx_sys */
};

/** Read view types @{ */
#define VIEW_NORMAL		1	/*!< Normal consistent read view
					where transaction does not see changes
					made by active transactions except
					creating transaction. */
#define VIEW_HIGH_GRANULARITY	2	/*!< High-granularity read view where
					transaction does not see changes
					made by active transactions and own
					changes after a point in time when this
					read view was created. */
/* @} */

/** Implement InnoDB framework to support consistent read views in
cursors. This struct holds both heap where consistent read view
is allocated and pointer to a read view. */

struct cursor_view_t{
	mem_heap_t*	heap;
				/*!< Memory heap for the cursor view */
	read_view_t*	read_view;
				/*!< Consistent read view of the cursor*/
	ulint		n_mysql_tables_in_use;
				/*!< number of Innobase tables used in the
				processing of this cursor */
};

#ifndef UNIV_NONINL
#include "read0read.ic"
#endif

#endif
