/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. 2016 IBM
   All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef _my_aligned_alloc_h
#define _my_aligned_alloc_h


#if defined (_MSC_VER)
#define ALIGNED_ALLOC(S,A) _aligned_malloc((A), (S))
#elif defined (_ISOC11_SOURCE)
#define ALIGNED_ALLOC(S,A)  aligned_alloc((A), (S))
#elif  _POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600
inline void *XXaligned_alloc(size_t size, size_t align)
{
  void *m;
  errno = posix_memalign(&m, align, size);
  return errno ? NULL : m;
}
#define ALIGNED_ALLOC(S,A)  XXaligned_alloc((S), (A))

#else
#warn   "No aligned malloc - cache lines may be shared"
#define ALIGNED_ALLOC(S,A)  malloc(S)
#endif

#ifdef SAFEMALLOC
void *sf_malloc(size_t size, myf my_flags);
void *sf_realloc(void *ptr, size_t size, myf my_flags);
void sf_free(void *ptr);
size_t sf_malloc_usable_size(void *ptr, my_bool *is_thread_specific);
#else
#define sf_malloc(X,Y)    ALIGNED_ALLOC((X), CPU_LEVEL1_DCACHE_LINESIZE)
#define sf_realloc(X,Y,Z) realloc((X), (Y))
#define sf_free(X)      free(X)
#endif

#endif
