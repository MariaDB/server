/* Copyright (c) 2001, 2012, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  Optimized function-like macros for the x86 architecture (_WIN32 included).
*/

#define sint2korr(A)	(int16) (*((int16 *) (A)))
#define sint3korr(A)	((int32) ((((uchar) (A)[2]) & 128) ? \
				  (((uint32) 255L << 24) | \
				   (((uint32) (uchar) (A)[2]) << 16) |\
				   (((uint32) (uchar) (A)[1]) << 8) | \
				   ((uint32) (uchar) (A)[0])) : \
				  (((uint32) (uchar) (A)[2]) << 16) |\
				  (((uint32) (uchar) (A)[1]) << 8) | \
				  ((uint32) (uchar) (A)[0])))
#define sint4korr(A)	(int32)  (*((int32 *) (A)))
#define uint2korr(A)	(uint16) (*((uint16 *) (A)))
#define uint3korr(A)	(uint32) (((uint32) ((uchar) (A)[0])) |\
				  (((uint32) ((uchar) (A)[1])) << 8) |\
				  (((uint32) ((uchar) (A)[2])) << 16))
#define uint4korr(A)	(uint32) (*((uint32 *) (A)))


static inline ulonglong uint5korr(const void *p)
{
  ulonglong a= *(uint32 *) p;
  ulonglong b= *(4 + (uchar *) p);
  return a | (b << 32);
}
static inline ulonglong uint6korr(const void *p)
{
  ulonglong a= *(uint32 *) p;
  ulonglong b= *(uint16 *) (4 + (char *) p);
  return a | (b << 32);
}

#define uint8korr(A)	(ulonglong) (*((ulonglong *) (A)))
#define sint8korr(A)	(longlong) (*((longlong *) (A)))

#define int2store(T,A)	do { uchar *pT= (uchar*)(T);\
                             *((uint16*)(pT))= (uint16) (A);\
                        } while (0)

#define int3store(T,A)  do { *(T)=  (uchar) ((A));\
                            *(T+1)=(uchar) (((uint) (A) >> 8));\
                            *(T+2)=(uchar) (((A) >> 16));\
                        } while (0)

#define int4store(T,A)	do { uchar *pT= (uchar*)(T);\
                             *((uint32 *) (pT))= (uint32) (A); \
                        } while (0)

#define int5store(T,A)  do { uchar *pT= (uchar*)(T);\
                             *((uint32 *) (pT))= (uint32) (A); \
                             *((pT)+4)=(uchar) (((A) >> 32));\
                        } while (0)

#define int6store(T,A)  do { uchar *pT= (uchar*)(T);\
                             *((uint32 *) (pT))= (uint32) (A); \
                             *((uint16*)(pT+4))= (uint16) (A >> 32);\
                        } while (0)

#define int8store(T,A)	do { uchar *pT= (uchar*)(T);\
                             *((ulonglong *) (pT))= (ulonglong) (A);\
                        } while(0)

#if defined(__GNUC__)

#define HAVE_mi_uint5korr
#define HAVE_mi_uint6korr
#define HAVE_mi_uint7korr
#define HAVE_mi_uint78orr

/* Read numbers stored in high-bytes-first order */

static inline ulonglong mi_uint5korr(const void *p)
{
  ulonglong a= *(uint32 *) p;
  ulonglong b= *(4 + (uchar *) p);
  ulonglong v= (a | (b << 32)) << 24;
  asm ("bswapq %0" : "=r" (v) : "0" (v));
  return v;
}

static inline ulonglong mi_uint6korr(const void *p)
{
  ulonglong a= *(uint32 *) p;
  ulonglong b= *(uint16 *) (4 + (char *) p);
  ulonglong v= (a | (b << 32)) << 16;
  asm ("bswapq %0" : "=r" (v) : "0" (v));
  return v;
}

static inline ulonglong mi_uint7korr(const void *p)
{
  ulonglong a= *(uint32 *) p;
  ulonglong b= *(uint16 *) (4 + (char *) p);
  ulonglong c= *(6 + (uchar *) p);
  ulonglong v= (a | (b << 32) | (c << 48)) << 8;
  asm ("bswapq %0" : "=r" (v) : "0" (v));
  return v;
}

static inline ulonglong mi_uint8korr(const void *p)
{
  ulonglong v= *(ulonglong *) p;
  asm ("bswapq %0" : "=r" (v) : "0" (v));
  return v;
}

#endif
