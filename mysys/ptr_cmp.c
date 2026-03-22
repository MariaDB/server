/* Copyright (c) 2000, 2015, Oracle and/or its affiliates. All rights reserved.

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

/*
  get_ptr_compare(len) returns a pointer to a optimal byte-compare function
  for a array of stringpointer where all strings have size len.
  The bytes are compare as unsigned chars.
  */

#include "mysys_priv.h"
#include <myisampack.h>
/*
 * On some platforms, memcmp() is faster than the unrolled ptr_compare_N
 * functions, as memcmp() is usually a platform-specific implementation
 * written in assembler. for example one in /usr/lib/libc/libc_hwcap*.so.1.
 * on Solaris, or on Windows inside C runtime library.
 *
 * On Solaris, native implementation is also usually faster than the
 * built-in memcmp supplied by GCC, so it is recommended to build
 * with "-fno-builtin-memcmp"in CFLAGS if building with GCC on Solaris.
 */

/*
  Daniel Blacks tests shows that libc memcmp is generally faster than
  ptr_cmp() at least of x86 and power8 platforms, so we use the libc
  code as default for now
*/

#define USE_NATIVE_MEMCMP 1

#ifdef USE_NATIVE_MEMCMP

#include <string.h>

static int native_compare(void *length_, const void *a_, const void *b_)
{
  size_t *length= length_;
  const unsigned char *const *a= a_;
  const unsigned char *const *b= b_;
  return memcmp(*a, *b, *length);
}

qsort_cmp2 get_ptr_compare (size_t size __attribute__((unused)))
{
  return native_compare;
}

#else /* USE_NATIVE_MEMCMP */

static int ptr_compare(void *compare_length, const void *a, const void *b);
static int ptr_compare_0(void *compare_length, const void *a, const void *b);
static int ptr_compare_1(void *compare_length, const void *a, const void *b);
static int ptr_compare_2(void *compare_length, const void *a, const void *b);
static int ptr_compare_3(void *compare_length, const void *a, const void *b);
static int degenerate_compare_func(void *compare_length, const void *a, const void *b)
{
  DBUG_ASSERT(*((size_t *) compare_length) == 0);
  return 0;
}

qsort_cmp2 get_ptr_compare (size_t size)
{
  if (size == 0)
    return degenerate_compare_func;
  if (size < 4)
    return ptr_compare;
  switch (size & 3) {
    case 0: return ptr_compare_0;
    case 1: return ptr_compare_1;
    case 2: return ptr_compare_2;
    case 3: return ptr_compare_3;
    }
  return 0;					/* Impossible */
}
	/*
	  Compare to keys to see witch is smaller.
	  Loop unrolled to make it quick !!
	*/

#define cmp(N) if (first[N] != last[N]) return (int) first[N] - (int) last[N]

static int ptr_compare(void *compare_length, const void *a, const void *b)
{
  size_t length= *((size_t *) compare_length);
  const uchar *first= *((const uchar *const *) a);
  const uchar *last= *((const uchar *const *) b);

  DBUG_ASSERT(length > 0);
  while (--length)
  {
    if (*first++ != *last++)
      return (int) first[-1] - (int) last[-1];
  }
  return (int) first[0] - (int) last[0];
}


static int ptr_compare_0(void *compare_length, const void *a, const void *b)
{
  size_t length= *((size_t *) compare_length);
  const uchar *first= *((const uchar *const *) a);
  const uchar *last= *((const uchar *const *) b);
 loop:
  cmp(0);
  cmp(1);
  cmp(2);
  cmp(3);
  if ((length-=4))
  {
    first+=4;
    last+=4;
    goto loop;
  }
  return (0);
}


static int ptr_compare_1(void *compare_length, const void *a, const void *b)
{

  size_t length= *((size_t *) compare_length) - 1;
  const uchar *first= *((const uchar *const *) a) + 1;
  const uchar *last= *((const uchar *const *) b) + 1;

  cmp(-1);
 loop:
  cmp(0);
  cmp(1);
  cmp(2);
  cmp(3);
  if ((length-=4))
  {
    first+=4;
    last+=4;
    goto loop;
  }
  return (0);
}

static int ptr_compare_2(void *compare_length, const void *a, const void *b)
{
  size_t length= *((size_t *) compare_length) - 2;
  const uchar *first= *((const uchar *const *) a) + 2;
  const uchar *last= *((const uchar *const *) b) + 2;

  cmp(-2);
  cmp(-1);
 loop:
  cmp(0);
  cmp(1);
  cmp(2);
  cmp(3);
  if ((length-=4))
  {
    first+=4;
    last+=4;
    goto loop;
  }
  return (0);
}

static int ptr_compare_3(void *compare_length, const void *a, const void *b)
{
  size_t length= *((size_t *) compare_length) - 3;
  const uchar *first= *((const uchar *const *) a) + 3;
  const uchar *last= *((const uchar *const *) b) + 3;

  cmp(-3);
  cmp(-2);
  cmp(-1);
 loop:
  cmp(0);
  cmp(1);
  cmp(2);
  cmp(3);
  if ((length-=4))
  {
    first+=4;
    last+=4;
    goto loop;
  }
  return (0);
}

#endif /* USE_NATIVE_MEMCMP */

void my_store_ptr(uchar *buff, size_t pack_length, my_off_t pos)
{
  switch (pack_length) {
#if SIZEOF_OFF_T > 4
  case 8: mi_int8store(buff,pos); break;
  case 7: mi_int7store(buff,pos); break;
  case 6: mi_int6store(buff,pos); break;
  case 5: mi_int5store(buff,pos); break;
#endif
  case 4: mi_int4store(buff,pos); break;
  case 3: mi_int3store(buff,pos); break;
  case 2: mi_int2store(buff,pos); break;
  case 1: buff[0]= (uchar) pos; break;
  default: DBUG_ASSERT(0);
  }
  return;
}

my_off_t my_get_ptr(uchar *ptr, size_t pack_length)
{
  my_off_t pos;
  switch (pack_length) {
#if SIZEOF_OFF_T > 4
  case 8: pos= (my_off_t) mi_uint8korr(ptr); break;
  case 7: pos= (my_off_t) mi_uint7korr(ptr); break;
  case 6: pos= (my_off_t) mi_uint6korr(ptr); break;
  case 5: pos= (my_off_t) mi_uint5korr(ptr); break;
#endif
  case 4: pos= (my_off_t) mi_uint4korr(ptr); break;
  case 3: pos= (my_off_t) mi_uint3korr(ptr); break;
  case 2: pos= (my_off_t) mi_uint2korr(ptr); break;
  case 1: pos= (my_off_t) *(uchar*) ptr; break;
  default: DBUG_ASSERT_NO_ASSUME(0); return 0;
  }
 return pos;
}
