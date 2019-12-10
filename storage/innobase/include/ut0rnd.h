/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, MariaDB Corporation.

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

/******************************************************************//**
@file include/ut0rnd.h
Random numbers and hashing

Created 1/20/1994 Heikki Tuuri
***********************************************************************/

#ifndef ut0rnd_h
#define ut0rnd_h

#include "ut0byte.h"
#include <my_sys.h>

#ifndef UNIV_INNOCHECKSUM
/** Seed value of ut_rnd_gen() */
extern uint64_t ut_rnd_current;

/** @return a pseudo-random 64-bit number */
inline uint64_t ut_rnd_gen()
{
	/*
	This is a linear congruential pseudo random number generator.
	The formula and the constants
	being used are:
	X[n+1] = (a * X[n] + c) mod m
	where:
	X[0] = my_interval_timer()
	a = 1103515245 (3^5 * 5 * 7 * 129749)
	c = 12345 (3 * 5 * 823)
	m = 18446744073709551616 (1<<64), implicit */

	if (UNIV_UNLIKELY(!ut_rnd_current)) {
		ut_rnd_current = my_interval_timer();
	}

	ut_rnd_current = 1103515245 * ut_rnd_current + 12345;
	return ut_rnd_current;
}

/** @return a random number between 0 and n-1, inclusive */
inline ulint ut_rnd_interval(ulint n)
{
  return n > 1 ? static_cast<ulint>(ut_rnd_gen() % n) : 0;
}

/*******************************************************//**
The following function generates a hash value for a ulint integer
to a hash table of size table_size, which should be a prime or some
random number to work reliably.
@return hash value */
UNIV_INLINE
ulint
ut_hash_ulint(
/*==========*/
	ulint	 key,		/*!< in: value to be hashed */
	ulint	 table_size);	/*!< in: hash table size */
/*************************************************************//**
Folds a 64-bit integer.
@return folded value */
UNIV_INLINE
ulint
ut_fold_ull(
/*========*/
	ib_uint64_t	d)	/*!< in: 64-bit integer */
	MY_ATTRIBUTE((const));
/*************************************************************//**
Folds a character string ending in the null character.
@return folded value */
UNIV_INLINE
ulint
ut_fold_string(
/*===========*/
	const char*	str)	/*!< in: null-terminated string */
	MY_ATTRIBUTE((warn_unused_result));
/***********************************************************//**
Looks for a prime number slightly greater than the given argument.
The prime is chosen so that it is not near any power of 2.
@return prime */
ulint
ut_find_prime(
/*==========*/
	ulint	n)	/*!< in: positive number > 100 */
	MY_ATTRIBUTE((const));

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
	MY_ATTRIBUTE((const));
/*************************************************************//**
Folds a binary string.
@return folded value */
UNIV_INLINE
ulint
ut_fold_binary(
/*===========*/
	const byte*	str,	/*!< in: string of bytes */
	ulint		len)	/*!< in: length */
	MY_ATTRIBUTE((pure));

#include "ut0rnd.ic"

#endif
