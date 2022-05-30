/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
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

/**************************************************************//**
@file include/ut0rnd.ic
Random numbers and hashing

Created 5/30/1994 Heikki Tuuri
*******************************************************************/

#define UT_HASH_RANDOM_MASK	1463735687
#define UT_HASH_RANDOM_MASK2	1653893711

#ifndef UNIV_INNOCHECKSUM

/*******************************************************//**
The following function generates a hash value for a ulint integer
to a hash table of size table_size, which should be a prime
or some random number for the hash table to work reliably.
@return hash value */
UNIV_INLINE
ulint
ut_hash_ulint(
/*==========*/
	ulint	 key,		/*!< in: value to be hashed */
	ulint	 table_size)	/*!< in: hash table size */
{
	ut_ad(table_size);
	key = key ^ UT_HASH_RANDOM_MASK2;

	return(key % table_size);
}

/*************************************************************//**
Folds a 64-bit integer.
@return folded value */
UNIV_INLINE
ulint
ut_fold_ull(
/*========*/
	ib_uint64_t	d)	/*!< in: 64-bit integer */
{
	return(ut_fold_ulint_pair((ulint) d & ULINT32_MASK,
				  (ulint) (d >> 32)));
}
#endif /* !UNIV_INNOCHECKSUM */

/*************************************************************//**
Folds a pair of ulints.
@return folded value */
UNIV_INLINE
ulint
ut_fold_ulint_pair(
/*===============*/
	ulint	n1,	/*!< in: ulint */
	ulint	n2)	/*!< in: ulint */
{
	return(((((n1 ^ n2 ^ UT_HASH_RANDOM_MASK2) << 8) + n1)
		^ UT_HASH_RANDOM_MASK) + n2);
}

/*************************************************************//**
Folds a binary string.
@return folded value */
UNIV_INLINE
ulint
ut_fold_binary(
/*===========*/
	const byte*	str,	/*!< in: string of bytes */
	ulint		len)	/*!< in: length */
{
	ulint		fold = 0;
	const byte*	str_end	= str + (len & 0xFFFFFFF8);

	ut_ad(str || !len);

	while (str < str_end) {
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
	}

	switch (len & 0x7) {
	case 7:
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		/* fall through */
	case 6:
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		/* fall through */
	case 5:
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		/* fall through */
	case 4:
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		/* fall through */
	case 3:
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		/* fall through */
	case 2:
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
		/* fall through */
	case 1:
		fold = ut_fold_ulint_pair(fold, (ulint)(*str++));
	}

	return(fold);
}
