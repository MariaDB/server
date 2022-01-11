/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

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
Calculates the fold value of a page file address: used in inserting or
searching for a lock in the hash table.
@return folded value */
UNIV_INLINE
ulint
lock_rec_fold(
/*==========*/
	ulint	space,	/*!< in: space */
	ulint	page_no)/*!< in: page number */
{
	return(ut_fold_ulint_pair(space, page_no));
}

/*********************************************************************//**
Calculates the hash value of a page file address: used in inserting or
searching for a lock in the hash table.
@return hashed value */
UNIV_INLINE
unsigned
lock_rec_hash(
/*==========*/
	ulint	space,	/*!< in: space */
	ulint	page_no)/*!< in: page number */
{
	return(unsigned(hash_calc_hash(lock_rec_fold(space, page_no),
				       lock_sys.rec_hash)));
}

/*********************************************************************//**
Gets the heap_no of the smallest user record on a page.
@return heap_no of smallest user record, or PAGE_HEAP_NO_SUPREMUM */
UNIV_INLINE
ulint
lock_get_min_heap_no(
/*=================*/
	const buf_block_t*	block)	/*!< in: buffer block */
{
	const page_t*	page	= block->frame;

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

/*************************************************************//**
Get the lock hash table */
UNIV_INLINE
hash_table_t*
lock_hash_get(
/*==========*/
	ulint	mode)	/*!< in: lock mode */
{
	if (mode & LOCK_PREDICATE) {
		return(lock_sys.prdt_hash);
	} else if (mode & LOCK_PRDT_PAGE) {
		return(lock_sys.prdt_page_hash);
	} else {
		return(lock_sys.rec_hash);
	}
}

/*********************************************************************//**
Creates a new record lock and inserts it to the lock queue. Does NOT check
for deadlocks or lock compatibility!
@param[in] c_lock conflicting lock
@param[in] thr thread owning trx
@param[in] type_mode lock mode and wait flag, type is ignored and replaced by
LOCK_REC
@param[in] block buffer block containing the record
@param[in] heap_no heap number of the record
@param[in] index index of record
@param[in,out] trx transaction
@param[in] caller_owns_trx_mutex TRUE if caller owns trx mutex
@param[in] insert_before_waiting if true, inserts new B-tree record lock
just after the last non-waiting lock of the current transaction which is
located before the first waiting for the current transaction lock, otherwise
the lock is inserted at the end of the queue
@return created lock */
UNIV_INLINE
lock_t *lock_rec_create(lock_t *c_lock,
#ifdef WITH_WSREP
                        que_thr_t *thr,
#endif
                        ulint type_mode, const buf_block_t *block,
                        ulint heap_no, dict_index_t *index, trx_t *trx,
                        bool caller_owns_trx_mutex,
                        bool insert_before_waiting)
{
	btr_assert_not_corrupted(block, index);
	return lock_rec_create_low(c_lock,
#ifdef WITH_WSREP
		thr,
#endif
		type_mode,
		block->page.id.space(), block->page.id.page_no(),
		block->frame, heap_no, index, trx,
		caller_owns_trx_mutex, insert_before_waiting);
}
