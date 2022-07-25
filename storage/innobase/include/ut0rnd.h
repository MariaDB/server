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
extern int32 ut_rnd_current;

/** @return a pseudo-random 32-bit number */
inline uint32_t ut_rnd_gen()
{
  /* This is a Galois linear-feedback shift register.
  https://en.wikipedia.org/wiki/Linear-feedback_shift_register#Galois_LFSRs
  The generating primitive Galois Field polynomial is the Castagnoli
  polynomial that was made popular by CRC-32C:
  x^32+x^28+x^27+x^26+x^25+x^23+x^22+x^20+
  x^19+x^18+x^14+x^13+x^11+x^10+x^9+x^8+x^6+1 */
  const uint32_t crc32c= 0x1edc6f41;

  uint32_t rnd= my_atomic_load32_explicit(&ut_rnd_current,
                                          MY_MEMORY_ORDER_RELAXED);

  if (UNIV_UNLIKELY(rnd == 0))
  {
    rnd= static_cast<uint32_t>(my_interval_timer());
    if (!rnd) rnd= 1;
  }
  else
  {
    bool lsb= rnd & 1;
    rnd>>= 1;
    if (lsb)
      rnd^= crc32c;
  }

  my_atomic_store32_explicit(&ut_rnd_current, rnd, MY_MEMORY_ORDER_RELAXED);
  return rnd;
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

#include "ut0rnd.inl"

#endif
