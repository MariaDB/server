/*****************************************************************************

Copyright (c) 2007, 2014, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/lock0priv.ic
Lock module internal inline methods.

Created July 16, 2007 Vasil Dimov
*******************************************************/

/* This file contains only methods which are used in
lock/lock0* files, other than lock/lock0lock.cc.
I.e. lock/lock0lock.cc contains more internal inline
methods but they are used only in that file. */

#ifndef LOCK_MODULE_IMPLEMENTATION
#error Do not include lock0priv.ic outside of the lock/ module
#endif

#include "row0row.h"

/*********************************************************************//**
Checks if some transaction has an implicit x-lock on a record in a clustered
index.
@return transaction id of the transaction which has the x-lock, or 0 */
UNIV_INLINE
trx_id_t
lock_clust_rec_some_has_impl(
/*=========================*/
	const rec_t*		rec,	/*!< in: user record */
	const dict_index_t*	index,	/*!< in: clustered index */
	const rec_offs*		offsets)/*!< in: rec_get_offsets(rec, index) */
{
	ut_ad(dict_index_is_clust(index));
	ut_ad(page_rec_is_user_rec(rec));

	return(row_get_rec_trx_id(rec, index, offsets));
}

/*********************************************************************//**
Gets the number of bits in a record lock bitmap.
@return	number of bits */
UNIV_INLINE
ulint
lock_rec_get_n_bits(
/*================*/
	const lock_t*	lock)	/*!< in: record lock */
{
	return(lock->un_member.rec_lock.n_bits);
}

/**********************************************************************//**
Sets the nth bit of a record lock to TRUE. */
inline
void
lock_rec_set_nth_bit(
/*=================*/
	lock_t*	lock,	/*!< in: record lock */
	ulint	i)	/*!< in: index of the bit */
{
	ulint	byte_index;
	ulint	bit_index;

	ut_ad(!lock->is_table());
	ut_ad(i < lock->un_member.rec_lock.n_bits);

	byte_index = i / 8;
	bit_index = i % 8;

#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion" /* GCC 4 and 5 need this here */
#endif
	((byte*) &lock[1])[byte_index] |= static_cast<byte>(1 << bit_index);
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic pop
#endif
#ifdef SUX_LOCK_GENERIC
	ut_ad(lock_sys.is_writer() || lock->trx->mutex_is_owner());
#else
	ut_ad(lock_sys.is_writer() || lock->trx->mutex_is_owner()
	      || (xtest() && !lock->trx->mutex_is_locked()));
#endif
	lock->trx->lock.n_rec_locks++;
}

/*********************************************************************//**
Gets the first or next record lock on a page.
@return	next lock, NULL if none exists */
UNIV_INLINE
lock_t*
lock_rec_get_next_on_page(
/*======================*/
	lock_t*	lock)	/*!< in: a record lock */
{
  return const_cast<lock_t*>(lock_rec_get_next_on_page_const(lock));
}

/*********************************************************************//**
Gets the next explicit lock request on a record.
@return	next lock, NULL if none exists or if heap_no == ULINT_UNDEFINED */
UNIV_INLINE
lock_t*
lock_rec_get_next(
/*==============*/
	ulint	heap_no,/*!< in: heap number of the record */
	lock_t*	lock)	/*!< in: lock */
{
	do {
		lock = lock_rec_get_next_on_page(lock);
	} while (lock && !lock_rec_get_nth_bit(lock, heap_no));

	return(lock);
}

/*********************************************************************//**
Gets the next explicit lock request on a record.
@return	next lock, NULL if none exists or if heap_no == ULINT_UNDEFINED */
UNIV_INLINE
const lock_t*
lock_rec_get_next_const(
/*====================*/
	ulint		heap_no,/*!< in: heap number of the record */
	const lock_t*	lock)	/*!< in: lock */
{
  return lock_rec_get_next(heap_no, const_cast<lock_t*>(lock));
}

/*********************************************************************//**
Gets the nth bit of a record lock.
@return TRUE if bit set also if i == ULINT_UNDEFINED return FALSE*/
UNIV_INLINE
ibool
lock_rec_get_nth_bit(
/*=================*/
	const lock_t*	lock,	/*!< in: record lock */
	ulint		i)	/*!< in: index of the bit */
{
	const byte*     b;

	ut_ad(!lock->is_table());

	if (i >= lock->un_member.rec_lock.n_bits) {

		return(FALSE);
	}

	b = ((const byte*) &lock[1]) + (i / 8);

	return(1 & *b >> (i % 8));
}

/*********************************************************************//**
Gets the first or next record lock on a page.
@return next lock, NULL if none exists */
UNIV_INLINE
const lock_t*
lock_rec_get_next_on_page_const(
/*============================*/
	const lock_t*	lock)	/*!< in: a record lock */
{
  ut_ad(!lock->is_table());

  const page_id_t page_id{lock->un_member.rec_lock.page_id};

  while (!!(lock= static_cast<const lock_t*>(HASH_GET_NEXT(hash, lock))))
    if (lock->un_member.rec_lock.page_id == page_id)
      break;
  return lock;
}

/*********************************************************************//**
Calculates if lock mode 1 is compatible with lock mode 2.
@return nonzero if mode1 compatible with mode2 */
UNIV_INLINE
ulint
lock_mode_compatible(
/*=================*/
	enum lock_mode	mode1,	/*!< in: lock mode */
	enum lock_mode	mode2)	/*!< in: lock mode */
{
	ut_ad((ulint) mode1 < lock_types);
	ut_ad((ulint) mode2 < lock_types);

	return(lock_compatibility_matrix[mode1][mode2]);
}

/*********************************************************************//**
Calculates if lock mode 1 is stronger or equal to lock mode 2.
@return nonzero if mode1 stronger or equal to mode2 */
UNIV_INLINE
ulint
lock_mode_stronger_or_eq(
/*=====================*/
	enum lock_mode	mode1,	/*!< in: lock mode */
	enum lock_mode	mode2)	/*!< in: lock mode */
{
	ut_ad((ulint) mode1 < lock_types);
	ut_ad((ulint) mode2 < lock_types);

	return(lock_strength_matrix[mode1][mode2]);
}

/*********************************************************************//**
Checks if a transaction has the specified table lock, or stronger. This
function should only be called by the thread that owns the transaction.
@return lock or NULL */
UNIV_INLINE
const lock_t*
lock_table_has(
/*===========*/
	const trx_t*		trx,	/*!< in: transaction */
	const dict_table_t*	table,	/*!< in: table */
	lock_mode		in_mode)/*!< in: lock mode */
{
	/* Look for stronger locks the same trx already has on the table */

	for (lock_list::const_iterator it = trx->lock.table_locks.begin(),
             end = trx->lock.table_locks.end(); it != end; ++it) {

		const lock_t*	lock = *it;

		if (lock == NULL) {
			continue;
		}

		ut_ad(trx == lock->trx);
		ut_ad(lock->is_table());
		ut_ad(lock->un_member.tab_lock.table);

		if (table == lock->un_member.tab_lock.table
		    && lock_mode_stronger_or_eq(lock->mode(), in_mode)) {
			ut_ad(!lock->is_waiting());
			return(lock);
		}
	}

	return(NULL);
}
