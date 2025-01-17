/*****************************************************************************

Copyright (c) 2024 Kristian Nielsen.
Copyright (c) 2013, 2023, MariaDB Corporation.
Copyright (c) 2000, 2020, Oracle and/or its affiliates. All Rights Reserved.

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
@file include/ut0bitop.h
Utilities for fast bitwise operatons.

Created 2024-10-01 Kristian Nielsen <knielsen@knielsen-hq.org>
*******************************************************/

#ifndef INNOBASE_UT0BITOP_H
#define INNOBASE_UT0BITOP_H

/*
The helper function nlz(x) calculates the number of leading zeros
in the binary representation of the number "x", either using a
built-in compiler function or a substitute trick based on the use
of the multiplication operation and a table indexed by the prefix
of the multiplication result:
*/
#ifdef __GNUC__
#define nlz(x) __builtin_clzll(x)
#elif defined(_MSC_VER) && !defined(_M_CEE_PURE) && \
  (defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM64))
#ifndef __INTRIN_H_
#pragma warning(push, 4)
#pragma warning(disable: 4255 4668)
#include <intrin.h>
#pragma warning(pop)
#endif
__forceinline unsigned int nlz (unsigned long long x)
{
#if defined(_M_IX86) || defined(_M_X64)
  unsigned long n;
#ifdef _M_X64
  _BitScanReverse64(&n, x);
  return (unsigned int) n ^ 63;
#else
  unsigned long y = (unsigned long) (x >> 32);
  unsigned int m = 31;
  if (y == 0)
  {
    y = (unsigned long) x;
    m = 63;
  }
  _BitScanReverse(&n, y);
  return (unsigned int) n ^ m;
#endif
#elif defined(_M_ARM64)
  return _CountLeadingZeros64(x);
#endif
}
#else
inline unsigned int nlz (unsigned long long x)
{
  static unsigned char table [48] = {
    32,  6,  5,  0,  4, 12,  0, 20,
    15,  3, 11,  0,  0, 18, 25, 31,
     8, 14,  2,  0, 10,  0,  0,  0,
     0,  0,  0, 21,  0,  0, 19, 26,
     7,  0, 13,  0, 16,  1, 22, 27,
     9,  0, 17, 23, 28, 24, 29, 30
  };
  unsigned int y= (unsigned int) (x >> 32);
  unsigned int n= 0;
  if (y == 0) {
    y= (unsigned int) x;
    n= 32;
  }
  y = y | (y >> 1); // Propagate leftmost 1-bit to the right.
  y = y | (y >> 2);
  y = y | (y >> 4);
  y = y | (y >> 8);
  y = y & ~(y >> 16);
  y = y * 0x3EF5D037;
  return n + table[y >> 26];
}
#endif

#endif /* INNOBASE_UT0BITOP_H */
