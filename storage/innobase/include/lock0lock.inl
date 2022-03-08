/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/lock0lock.ic
The transaction lock system

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#include "dict0dict.h"
#include "buf0buf.h"
#include "page0page.h"

/*********************************************************************//**
Gets the heap_no of the smallest user record on a page.
@return heap_no of smallest user record, or PAGE_HEAP_NO_SUPREMUM */
UNIV_INLINE
ulint
lock_get_min_heap_no(
/*=================*/
	const buf_block_t*	block)	/*!< in: buffer block */
{
	const page_t*	page	= block->page.frame;

	if (page_is_comp(page)) {
		return(rec_get_heap_no_new(
			       page
			       + rec_get_next_offs(page + PAGE_NEW_INFIMUM,
						   TRUE)));
	} else {
		return(rec_get_heap_no_old(
			       page
			       + rec_get_next_offs(page + PAGE_OLD_INFIMUM,
						   FALSE)));
	}
}

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
	bool			caller_owns_trx_mutex)
					/*!< in: TRUE if caller owns
					trx mutex */
{
	btr_assert_not_corrupted(block, index);
	return lock_rec_create_low(
		c_lock,
		type_mode, block->page.id(), block->page.frame, heap_no,
		index, trx, caller_owns_trx_mutex);
}
