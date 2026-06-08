#ifndef MY_BYTEORDER_INCLUDED
#define MY_BYTEORDER_INCLUDED

/* Copyright (c) 2001, 2012, Oracle and/or its affiliates. All rights reserved.

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

#include <string.h> /* memcpy */

/*
  Byte-swap helpers.
*/
#ifdef _MSC_VER
#  include <stdlib.h> /* _byteswap_* */
#  define MY_BSWAP16(x) _byteswap_ushort(x)
#  define MY_BSWAP32(x) _byteswap_ulong(x)
#  define MY_BSWAP64(x) _byteswap_uint64(x)
#elif defined __GNUC__
#  define MY_BSWAP16(x) __builtin_bswap16(x)
#  define MY_BSWAP32(x) __builtin_bswap32(x)
#  define MY_BSWAP64(x) __builtin_bswap64(x)
#else
#  error provide byteswap intrinsics
#endif

/*
  Some inline functions to convert between little-endian and host byte order
  in the spirit of htons/htonl et al.
*/

static inline uint16 my_letoh16(uint16 x)
{
#ifdef WORDS_BIGENDIAN
  return MY_BSWAP16(x);
#else
  return x;
#endif
}

static inline uint16 my_betoh16(uint16 x)
{
#ifdef WORDS_BIGENDIAN
  return x;
#else
  return MY_BSWAP16(x);
#endif
}

static inline uint32 my_letoh32(uint32 x)
{
#ifdef WORDS_BIGENDIAN
  return MY_BSWAP32(x);
#else
  return x;
#endif
}

static inline uint32 my_betoh32(uint32 x)
{
#ifdef WORDS_BIGENDIAN
  return x;
#else
  return MY_BSWAP32(x);
#endif
}

static inline uint64 my_letoh64(uint64 x)
{
#ifdef WORDS_BIGENDIAN
  return MY_BSWAP64(x);
#else
  return x;
#endif
}

static inline uint64 my_betoh64(uint64 x)
{
#ifdef WORDS_BIGENDIAN
  return x;
#else
  return MY_BSWAP64(x);
#endif
}

#define my_htole16(x) my_letoh16(x)
#define my_htobe16(x) my_betoh16(x)
#define my_htole32(x) my_letoh32(x)
#define my_htobe32(x) my_betoh32(x)
#define my_htole64(x) my_letoh64(x)
#define my_htobe64(x) my_betoh64(x)

/*
  Inline functions for reading/storing little endian integers from/to
  potentially unaligned memory.

  memcpy() is used to avoid unaligned access. On most platforms the compiler
  will optimize memcpy to a single instruction.
*/

static inline uint16 uint2korr(const void *p)
{
  uint16 ret;
  memcpy(&ret, p, 2);
  return my_letoh16(ret);
}

static inline uint32 uint4korr(const void *p)
{
  uint32 ret;
  memcpy(&ret, p, 4);
  return my_letoh32(ret);
}

static inline uint64 uint8korr(const void *p)
{
  uint64 ret;
  memcpy(&ret, p, 8);
  return my_letoh64(ret);
}

static inline int16 sint2korr(const void *p)
{
  return (int16) uint2korr(p);
}

static inline int32 sint4korr(const void *p)
{
  return (int32) uint4korr(p);
}

static inline longlong sint8korr(const void *p)
{
  return (longlong) uint8korr(p);
}

static inline void int2store(void *t, ulonglong a)
{
  uint16 v= my_htole16((uint16) a);
  memcpy(t, &v, 2);
}

static inline void int4store(void *t, ulonglong a)
{
  uint32 v= my_htole32((uint32) a);
  memcpy(t, &v, 4);
}

static inline void int8store(void *t, ulonglong a)
{
  uint64 v= my_htole64((uint64) a);
  memcpy(t, &v, 8);
}

/*
  Odd-width and sign-extending functions.  These use only individual
  byte accesses or delegate to the even-width functions above, so they
  are correct on any host endianness without further #ifdefs.
*/


static inline uint32 uint3korr(const void *p)
{
  return (uint32) uint2korr(p) | ((uint32) ((const uchar *) p)[2] << 16);
}

static inline int32 sint3korr(const void *p)
{
  uint32 v = uint3korr(p);
  /*
    Shift left to move sign bit into MSB position, then arithmetic
    right shift to sign-extend back.
  */
  return ((int32)(v << 8)) >> 8;
}

static inline ulonglong uint5korr(const void *p)
{
  return (ulonglong) uint4korr(p) | ((ulonglong) ((const uchar *) p)[4] << 32);
}

static inline ulonglong uint6korr(const void *p)
{
  return (ulonglong) uint4korr(p) |
         ((ulonglong) uint2korr((const uchar *) p + 4) << 32);
}

static inline void int3store(void *t, ulonglong a)
{
  uchar *p= (uchar *) t;
  int2store(t, a);
  p[2]= (uchar) (a >> 16);
}

static inline void int5store(void *t, ulonglong a)
{
  uchar *p= (uchar *) t;
  int4store(t, a);
  p[4]= (uchar) (a >> 32);
}

static inline void int6store(void *t, ulonglong a)
{
  uchar *p= (uchar *) t;
  int4store(t, a);
  int2store(p + 4, a >> 32);
}

/*
  mi_uint*korr: read an N-byte unsigned integer stored in big-endian
  order, as used by MyISAM.
  Below are optimizations for little-endian architectures.
*/

#ifndef WORDS_BIGENDIAN

#define HAVE_mi_uint5korr
#define HAVE_mi_uint6korr
#define HAVE_mi_uint7korr
#define HAVE_mi_uint8korr

static inline ulonglong mi_uint5korr(const void *p)
{
  uint32 lo;
  memcpy(&lo, (const uchar *) p + 1, 4);
  lo= MY_BSWAP32(lo);
  return ((ulonglong) ((const uchar *)p)[0]) << 32 | lo;
}

static inline ulonglong mi_uint6korr(const void *p)
{
  uint32 a;
  uint16 b;
  ulonglong v;
  memcpy(&a, p, 4);
  memcpy(&b, (const uchar *) p + 4, 2);
  v= ((ulonglong) a | ((ulonglong) b << 32)) << 16;
  return MY_BSWAP64(v);
}

static inline ulonglong mi_uint7korr(const void *p)
{
  uint32 a;
  uint16 b;
  ulonglong v;
  ulonglong c= ((const uchar *)p)[6];
  memcpy(&a, p, 4);
  memcpy(&b, (const uchar *) p + 4, 2);
  v= ((ulonglong)a | ((ulonglong)b << 32) | (c << 48)) << 8;
  return MY_BSWAP64(v);
}

static inline ulonglong mi_uint8korr(const void *p)
{
  ulonglong ret;
  memcpy(&ret, p, 8);
  return MY_BSWAP64(ret);
}
#endif /* !WORDS_BIGENDIAN */

/*
  Read a 32-bit integer from network byte order (big-endian)
  from an unaligned memory location.
*/
static inline int32 int4net(const void *p)
{
  int32 ret;
  memcpy(&ret, p, 4);
  return (int32)my_betoh32(ret);
}

/*
  Some macros for reading doubles and floats (clean, do not assume alignment)
  These are defined in big_endian.h and  little_endian.h
*/
#ifdef WORDS_BIGENDIAN
#include "big_endian.h"
#else
#include "little_endian.h"
#endif


#endif /* MY_BYTEORDER_INCLUDED */
