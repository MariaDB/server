/* Copyright (c) 2007, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef MY_BIT_INCLUDED
#define MY_BIT_INCLUDED

/*
  Some useful bit functions
*/

C_MODE_START

extern const uchar _my_bits_reverse_table[256];


/*
  my_bit_log2_xxx()

  In the given value, find the highest bit set,
  which is the smallest X that satisfies the condition: (2^X >= value).
  Can be used as a reverse operation for (1<<X), to find X.

  Examples:
  - returns 0 for (1<<0)
  - returns 1 for (1<<1)
  - returns 2 for (1<<2)
  - returns 2 for 3, which has (1<<2) as the highest bit set.

  Note, the behaviour of log2(0) is not defined.
  Let's return 0 for the input 0, for the code simplicity.
  See the 000x branch. It covers both (1<<0) and 0.
*/
static inline CONSTEXPR uint my_bit_log2_hex_digit(uint8 value)
{
  return value & 0x0C ? /*1100*/ (value & 0x08 ? /*1000*/ 3 : /*0100*/ 2) :
                        /*0010*/ (value & 0x02 ? /*0010*/ 1 : /*000x*/ 0);
}
static inline CONSTEXPR uint my_bit_log2_uint8(uint8 value)
{
  return value & 0xF0 ? my_bit_log2_hex_digit((uint8) (value >> 4)) + 4:
                        my_bit_log2_hex_digit(value);
}
static inline CONSTEXPR uint my_bit_log2_uint16(uint16 value)
{
  return value & 0xFF00 ? my_bit_log2_uint8((uint8) (value >> 8)) + 8 :
                          my_bit_log2_uint8((uint8) value);
}
static inline CONSTEXPR uint my_bit_log2_uint32(uint32 value)
{
  return value & 0xFFFF0000UL ?
         my_bit_log2_uint16((uint16) (value >> 16)) + 16 :
         my_bit_log2_uint16((uint16) value);
}
static inline CONSTEXPR uint my_bit_log2_uint64(ulonglong value)
{
  return value & 0xFFFFFFFF00000000ULL ?
         my_bit_log2_uint32((uint32) (value >> 32)) + 32 :
         my_bit_log2_uint32((uint32) value);
}
static inline CONSTEXPR uint my_bit_log2_size_t(size_t value)
{
#ifdef __cplusplus
  static_assert(sizeof(size_t) <= sizeof(ulonglong),
                "size_t <= ulonglong is an assumption that needs to be fixed "
                "for this architecture. Please create an issue on "
                "https://jira.mariadb.org");
#endif
  return my_bit_log2_uint64((ulonglong) value);
}


/*
Count bits in 32bit integer

  Algorithm by Sean Anderson, according to:
  http://graphics.stanford.edu/~seander/bithacks.html
  under "Counting bits set, in parallel"

 (Original code public domain).
*/
static inline uint my_count_bits_uint32(uint32 v)
{
  v = v - ((v >> 1) & 0x55555555);
  v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
  return (((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24;
}


static inline uint my_count_bits(ulonglong x)
{
  return my_count_bits_uint32((uint32)x) + my_count_bits_uint32((uint32)(x >> 32));
}




/*
  Next highest power of two

  SYNOPSIS
    my_round_up_to_next_power()
    v		Value to check

  RETURN
    Next or equal power of 2
    Note: 0 will return 0

  NOTES
    Algorithm by Sean Anderson, according to:
    http://graphics.stanford.edu/~seander/bithacks.html
    (Original code public domain)

    Comments shows how this works with 01100000000000000000000000001011
*/

static inline uint32 my_round_up_to_next_power(uint32 v)
{
  v--;			/* 01100000000000000000000000001010 */
  v|= v >> 1;		/* 01110000000000000000000000001111 */
  v|= v >> 2;		/* 01111100000000000000000000001111 */
  v|= v >> 4;		/* 01111111110000000000000000001111 */
  v|= v >> 8;		/* 01111111111111111100000000001111 */
  v|= v >> 16;		/* 01111111111111111111111111111111 */
  return v+1;		/* 10000000000000000000000000000000 */
}

static inline uint32 my_clear_highest_bit(uint32 v)
{
  uint32 w=v >> 1;
  w|= w >> 1;
  w|= w >> 2;
  w|= w >> 4;
  w|= w >> 8;
  w|= w >> 16;
  return v & w;
}

static inline uint32 my_reverse_bits(uint32 key)
{
  return
    ((uint32)_my_bits_reverse_table[ key      & 255] << 24) |
    ((uint32)_my_bits_reverse_table[(key>> 8) & 255] << 16) |
    ((uint32)_my_bits_reverse_table[(key>>16) & 255] <<  8) |
     (uint32)_my_bits_reverse_table[(key>>24)      ];
}

/*
  a number with the n lowest bits set
  an overflow-safe version of  (1 << n) - 1
*/
static inline uint64 my_set_bits(int n)
{
  return (((1ULL << (n - 1)) - 1) << 1) | 1;
}

/* Create a mask of the significant bits for the last byte (1,3,7,..255) */
static inline uchar last_byte_mask(uint bits)
{
  /* Get the number of used bits-1 (0..7) in the last byte */
  unsigned int const used = (bits - 1U) & 7U;
  /* Return bitmask for the significant bits */
  return (uchar) ((2U << used) - 1);
}

static inline uint my_bits_in_bytes(uint n)
{
  return ((n + 7) / 8);
}

#ifdef _MSC_VER
#include <intrin.h>
#endif

/*
  Find the position of the first(least significant) bit set in
  the argument. Returns 64 if the argument was 0.
*/
static inline uint my_find_first_bit(ulonglong n)
{
  if(!n)
    return 64;
#if defined(__GNUC__)
  return __builtin_ctzll(n);
#elif defined(_MSC_VER)
#if defined(_M_IX86)
  unsigned long bit;
  if( _BitScanForward(&bit, (uint)n))
    return bit;
  _BitScanForward(&bit, (uint)(n>>32));
  return bit + 32;
#else
  unsigned long bit;
  _BitScanForward64(&bit, n);
  return bit;
#endif
#else
  /* Generic case */
  uint  shift= 0;
  static const uchar last_bit[16] = { 32, 0, 1, 0,
                                      2, 0, 1, 0,
                                      3, 0, 1, 0,
                                      2, 0, 1, 0};
  uint bit;
  while ((bit = last_bit[(n >> shift) & 0xF]) == 32)
    shift+= 4;
  return shift+bit;
#endif
}
C_MODE_END

/*
The helper function my_nlz(x) calculates the number of leading zeros
in the binary representation of the number "x", either using a
built-in compiler function or a substitute trick based on the use
of the multiplication operation and a table indexed by the prefix
of the multiplication result:

Moved to mysys from ha_innodb.cc to be able to use in non-InnoDB code.
*/
#ifdef __GNUC__
#define my_nlz(x) __builtin_clzll(x)
#elif defined(_MSC_VER) && !defined(_M_CEE_PURE) && \
  (defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM64))
#ifndef __INTRIN_H_
#pragma warning(push, 4)
#pragma warning(disable: 4255 4668)
#include <intrin.h>
#pragma warning(pop)
#endif
__forceinline unsigned int my_nlz (unsigned long long x)
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
inline unsigned int my_nlz (unsigned long long x)
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

#endif /* MY_BIT_INCLUDED */
