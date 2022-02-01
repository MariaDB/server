/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, 2020, MariaDB Corporation.

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
@file include/ut0byte.h
Utilities for byte operations

Created 1/20/1994 Heikki Tuuri
***********************************************************************/

#ifndef ut0byte_h
#define ut0byte_h

#include "univ.i"

/*******************************************************//**
Creates a 64-bit integer out of two 32-bit integers.
@return created integer */
UNIV_INLINE
ib_uint64_t
ut_ull_create(
/*==========*/
	ulint	high,	/*!< in: high-order 32 bits */
	ulint	low)	/*!< in: low-order 32 bits */
	MY_ATTRIBUTE((const));

/********************************************************//**
Rounds a 64-bit integer downward to a multiple of a power of 2.
@return rounded value */
UNIV_INLINE
ib_uint64_t
ut_uint64_align_down(
/*=================*/
	ib_uint64_t	 n,		/*!< in: number to be rounded */
	ulint		 align_no);	/*!< in: align by this number
					which must be a power of 2 */
/********************************************************//**
Rounds ib_uint64_t upward to a multiple of a power of 2.
@return rounded value */
UNIV_INLINE
ib_uint64_t
ut_uint64_align_up(
/*===============*/
	ib_uint64_t	 n,		/*!< in: number to be rounded */
	ulint		 align_no);	/*!< in: align by this number
					which must be a power of 2 */
/** Round down a pointer to the nearest aligned address.
@param ptr        pointer
@param alignment  a power of 2
@return aligned pointer */
static inline void *ut_align_down(void *ptr, size_t alignment)
{
  ut_ad(alignment > 0);
  ut_ad(ut_is_2pow(alignment));
  ut_ad(ptr);
  static_assert(sizeof ptr == sizeof(size_t), "compatibility");

  return reinterpret_cast<void*>(reinterpret_cast<size_t>(ptr) &
                                 ~(alignment - 1));
}

static inline const void *ut_align_down(const void *ptr, size_t alignment)
{
  return ut_align_down(const_cast<void*>(ptr), alignment);
}

/** Compute the offset of a pointer from the nearest aligned address.
@param ptr        pointer
@param alignment  a power of 2
@return distance from aligned pointer */
inline size_t ut_align_offset(const void *ptr, size_t alignment)
{
  ut_ad(alignment > 0);
  ut_ad(ut_is_2pow(alignment));
  ut_ad(ptr);
  return reinterpret_cast<size_t>(ptr) & (alignment - 1);
}

/*****************************************************************//**
Gets the nth bit of a ulint.
@return TRUE if nth bit is 1; 0th bit is defined to be the least significant */
UNIV_INLINE
ibool
ut_bit_get_nth(
/*===========*/
	ulint	a,	/*!< in: ulint */
	ulint	n);	/*!< in: nth bit requested */
/*****************************************************************//**
Sets the nth bit of a ulint.
@return the ulint with the bit set as requested */
UNIV_INLINE
ulint
ut_bit_set_nth(
/*===========*/
	ulint	a,	/*!< in: ulint */
	ulint	n,	/*!< in: nth bit requested */
	ibool	val);	/*!< in: value for the bit to set */

#include "ut0byte.inl"

#endif
