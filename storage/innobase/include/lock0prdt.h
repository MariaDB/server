/*****************************************************************************

Copyright (c) 2014, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2021, MariaDB Corporation.

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
@file include/lock0prdt.h
The predicate lock system

Created 9/7/2013 Jimmy Yang
*******************************************************/
#ifndef lock0prdt_h
#define lock0prdt_h

#include "lock0lock.h"

/* Predicate lock data */
typedef struct lock_prdt {
	void*		data;		/* Predicate data */
	uint16		op;		/* Predicate operator */
} lock_prdt_t;

/*********************************************************************//**
Acquire a predicate lock on a block
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_prdt_lock(
/*===========*/
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	lock_prdt_t*	prdt,	/*!< in: Predicate for the lock */
	dict_index_t*	index,	/*!< in: secondary index */
	enum lock_mode	mode,	/*!< in: mode of the lock which
				the read cursor should set on
				records: LOCK_S or LOCK_X; the
				latter is possible in
				SELECT FOR UPDATE */
	unsigned	type_mode,
				/*!< in: LOCK_PREDICATE or LOCK_PRDT_PAGE */
	que_thr_t*	thr);	/*!< in: query thread
				(can be NULL if BTR_NO_LOCKING_FLAG) */

/*********************************************************************//**
Acquire a "Page" lock on a block
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_place_prdt_page_lock(
	const page_id_t	page_id,	/*!< in: page identifier */
	dict_index_t*	index,	/*!< in: secondary index */
	que_thr_t*	thr);	/*!< in: query thread */

/*********************************************************************//**
Initiate a Predicate lock from a MBR */
void
lock_init_prdt_from_mbr(
/*====================*/
	lock_prdt_t*	prdt,	/*!< in/out: predicate to initialized */
	rtr_mbr_t*	mbr,	/*!< in: Minimum Bounding Rectangle */
	ulint		mode,	/*!< in: Search mode */
	mem_heap_t*	heap);	/*!< in: heap for allocating memory */

/*********************************************************************//**
Get predicate lock's minimum bounding box
@return the minimum bounding box*/
lock_prdt_t*
lock_get_prdt_from_lock(
/*====================*/
	const lock_t*	lock);	/*!< in: the lock */

/*********************************************************************//**
Checks if a predicate lock request for a new lock has to wait for
request lock2.
@return true if new lock has to wait for lock2 to be removed */
bool
lock_prdt_has_to_wait(
/*==================*/
	const trx_t*	trx,	/*!< in: trx of new lock */
	unsigned	type_mode,/*!< in: precise mode of the new lock
				to set: LOCK_S or LOCK_X, possibly
				ORed to LOCK_PREDICATE or LOCK_PRDT_PAGE,
				LOCK_INSERT_INTENTION */
	lock_prdt_t*	prdt,	/*!< in: lock predicate to check */
	const lock_t*	lock2);	/*!< in: another record lock; NOTE that
				it is assumed that this has a lock bit
				set on the same record as in the new
				lock we are setting */

/**************************************************************//**
Update predicate lock when page splits */
void
lock_prdt_update_split(
/*===================*/
	buf_block_t*	new_block,	/*!< in/out: the new half page */
	lock_prdt_t*	prdt,		/*!< in: MBR on the old page */
	lock_prdt_t*	new_prdt,	/*!< in: MBR on the new page */
	const page_id_t	page_id);	/*!< in: page number */

/**************************************************************//**
Ajust locks from an ancester page of Rtree on the appropriate level . */
void
lock_prdt_update_parent(
/*====================*/
	buf_block_t*	left_block,	/*!< in/out: page to be split */
	buf_block_t*	right_block,	/*!< in/out: the new half page */
	lock_prdt_t*	left_prdt,	/*!< in: MBR on the old page */
	lock_prdt_t*	right_prdt,	/*!< in: MBR on the new page */
	const page_id_t	page_id);	/*!< in: parent page */

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate insert of
a predicate record.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_prdt_insert_check_and_lock(
/*============================*/
	const rec_t*	rec,	/*!< in: record after which to insert */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	dict_index_t*	index,	/*!< in: index */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	lock_prdt_t*	prdt);	/*!< in: Minimum Bound Rectangle */

/*********************************************************************//**
Append a predicate to the lock */
void
lock_prdt_set_prdt(
/*===============*/
	lock_t*			lock,	/*!< in: lock */
	const lock_prdt_t*	prdt);	/*!< in: Predicate */

#if 0

/*********************************************************************//**
Checks if a predicate lock request for a new lock has to wait for
request lock2.
@return true if new lock has to wait for lock2 to be removed */
UNIV_INLINE
bool
lock_prdt_has_to_wait(
/*==================*/
	const trx_t*	trx,	/*!< in: trx of new lock */
	unsigned	type_mode,/*!< in: precise mode of the new lock
				to set: LOCK_S or LOCK_X, possibly
				ORed to LOCK_PREDICATE or LOCK_PRDT_PAGE,
				LOCK_INSERT_INTENTION */
	lock_prdt_t*	prdt,	/*!< in: lock predicate to check */
	const lock_t*	lock2);	/*!< in: another record lock; NOTE that
				it is assumed that this has a lock bit
				set on the same record as in the new
				lock we are setting */

/*********************************************************************//**
Get predicate lock's minimum bounding box
@return the minimum bounding box*/
UNIV_INLINE
rtr_mbr_t*
prdt_get_mbr_from_prdt(
/*===================*/
	const lock_prdt_t*	prdt);	/*!< in: the lock predicate */


#endif
/*************************************************************//**
Moves the locks of a record to another record and resets the lock bits of
the donating record. */
void
lock_prdt_rec_move(
/*===============*/
	const buf_block_t*	receiver,	/*!< in: buffer block containing
						the receiving record */
	const page_id_t		donator);	/*!< in: target page */

/** Check whether there are R-tree Page lock on a page
@param[in]	trx	trx to test the lock
@param[in]	page_id	page identifier
@return	true if there is none */
bool lock_test_prdt_page_lock(const trx_t *trx, const page_id_t page_id);

#endif
