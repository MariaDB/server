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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02111-1301  USA */

/*
  Optimized function-like macros for the x86 architecture (_WIN32 included).
*/

#include <string.h>

inline static int16 sint2korr(const void *p)
{
  int16 result= 0;
  memcpy(&result, p, 2);
  return result;
}

inline static int32 sint3korr(const void *p)
{
  int32 result= 0;
  const char *ptr= (const char *) p;
  result= ptr[2];
  result<<= 16;
  memcpy(&result, ptr, 2);
  return result;
}

inline static int32 sint4korr(const void *p)
{
  int32 result= 0;
  memcpy(&result, p, 4);
  return result;
}

inline static uint16 uint2korr(const void *p)
{
  uint16 result= 0;
  memcpy(&result, p, 2);
  return result;
}

inline static uint32 uint3korr(const void *p)
{
  uint32 result= 0;
  memcpy(&result, p, 3);
  return result;
}

inline static uint32 uint4korr(const void *p)
{
  uint32 result= 0;
  memcpy(&result, p, 4);
  return result;
}

inline static ulonglong uint5korr(const void *p)
{
  ulonglong result= 0;
  memcpy(&result, p, 5);
  return result;
}

inline static ulonglong uint6korr(const void *p)
{
  ulonglong result= 0;
  memcpy(&result, p, 6);
  return result;
}

inline static ulonglong uint8korr(const void *p)
{
  ulonglong result= 0;
  memcpy(&result, p, 8);
  return result;
}

inline static longlong sint8korr(const void *p)
{
  longlong result= 0;
  memcpy(&result, p, 8);
  return result;
}

inline static void int2store(void *p, const int16 n) { memcpy(p, &n, 2); }
inline static void int3store(void *p, const int32 n) { memcpy(p, &n, 3); }
inline static void int4store(void *p, const int32 n) { memcpy(p, &n, 4); }
inline static void int5store(void *p, const longlong n) { memcpy(p, &n, 5); }
inline static void int6store(void *p, const longlong n) { memcpy(p, &n, 6); }
inline static void int8store(void *p, const longlong n) { memcpy(p, &n, 8); }


#if defined(__GNUC__)

#define HAVE_mi_uint5korr
#define HAVE_mi_uint6korr
#define HAVE_mi_uint7korr
#define HAVE_mi_uint78orr

/* Read numbers stored in high-bytes-first order */

static inline ulonglong mi_uint5korr(const void *p)
{
  ulonglong v= 0;
  memcpy((uchar *) &v + 3, p, 5);
  asm ("bswapq %0" : "=r" (v) : "0" (v));
  return v;
}

static inline ulonglong mi_uint6korr(const void *p)
{
  ulonglong v= 0;
  memcpy((uchar *) &v + 2, p, 6);
  asm ("bswapq %0" : "=r" (v) : "0" (v));
  return v;
}

static inline ulonglong mi_uint7korr(const void *p)
{
  ulonglong v= 0;
  memcpy((uchar *) &v + 1, p, 7);
  asm ("bswapq %0" : "=r" (v) : "0" (v));
  return v;
}

static inline ulonglong mi_uint8korr(const void *p)
{
  ulonglong v= uint8korr(p);
  asm ("bswapq %0" : "=r" (v) : "0" (v));
  return v;
}

#endif
