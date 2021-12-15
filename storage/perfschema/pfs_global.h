/* Copyright (c) 2008, 2015, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef PFS_GLOBAL_H
#define PFS_GLOBAL_H

#include "my_compiler.h"

/**
  @file storage/perfschema/pfs_global.h
  Miscellaneous global dependencies (declarations).
*/

/** True when the performance schema is initialized. */
extern bool pfs_initialized;

/** Total memory allocated by the performance schema, in bytes. */
extern size_t pfs_allocated_memory;

#if defined(HAVE_POSIX_MEMALIGN) || defined(HAVE_MEMALIGN) || defined(HAVE_ALIGNED_MALLOC)
#define PFS_ALIGNEMENT CPU_LEVEL1_DCACHE_LINESIZE
#define PFS_ALIGNED MY_ALIGNED(PFS_ALIGNEMENT)
#else
/*
  Known platforms that do not provide aligned memory:
  - MacOSX Darwin (osx10.5)
  For these platforms, compile without the alignment optimization.
*/
#define PFS_ALIGNED
#endif /* HAVE_POSIX_MEMALIGN || HAVE_MEMALIGN || HAVE_ALIGNED_MALLOC */

void *pfs_malloc(size_t size, myf flags);

/** Allocate an array of structures with overflow check. */
void *pfs_malloc_array(size_t n, size_t size, myf flags);

/**
  Helper, to allocate an array of structures.
  @param n number of elements in the array
  @param s size of array element
  @param T type of an element
  @param f flags to use when allocating memory
*/
#define PFS_MALLOC_ARRAY(n, s, T, f) \
  reinterpret_cast<T*>(pfs_malloc_array((n), (s), (f)))

/** Free memory allocated with @sa pfs_malloc. */
void pfs_free(void *ptr);

/** Detect multiplication overflow. */
bool is_overflow(size_t product, size_t n1, size_t n2);

uint pfs_get_socket_address(char *host,
                            uint host_len,
                            uint *port,
                            const struct sockaddr_storage *src_addr,
                            socklen_t src_len);

/**
  Compute a random index value in an interval.
  @param ptr seed address
  @param max_size maximun size of the interval
  @return a random value in [0, max_size-1]
*/
inline uint randomized_index(const void *ptr, uint max_size)
{
  static uint seed1= 0;
  static uint seed2= 0;
  uint result;
  intptr value;

  if (unlikely(max_size == 0))
    return 0;

  /*
    ptr is typically an aligned structure, and can be in an array.
    - The last bits are not random because of alignment,
      so we divide by 8.
    - The high bits are mostly constant, especially with 64 bits architectures,
      but we keep most of them anyway, by doing computation in intptr.
      The high bits are significant depending on where the data is
      stored (the data segment, the stack, the heap, ...).
    - To spread consecutive cells in an array further, we multiply by
      a factor A. This factor should not be too high, which would cause
      an overflow and cause loss of randomness (droping the top high bits).
      The factor is a prime number, to help spread the distribution.
    - To add more noise, and to be more robust if the calling code is
      passing a constant value instead of a random identity,
      we add the previous results, for hysteresys, with a degree 2 polynom,
      X^2 + X + 1.
    - Last, a modulo is applied to be within the [0, max_size - 1] range.
    Note that seed1 and seed2 are static, and are *not* thread safe,
    which is even better.
    Effect with arrays: T array[N]
    - ptr(i) = & array[i] = & array[0] + i * sizeof(T)
    - ptr(i+1) = ptr(i) + sizeof(T).
    What we want here, is to have index(i) and index(i+1) fall into
    very different areas in [0, max_size - 1], to avoid locality.
  */
  value= (reinterpret_cast<intptr> (ptr)) >> 3;
  value*= 1789;
  value+= seed2 + seed1 + 1;

  result= (static_cast<uint> (value)) % max_size;

  seed2= seed1*seed1;
  seed1= result;

  DBUG_ASSERT(result < max_size);
  return result;
}

void pfs_print_error(const char *format, ...);

/**
  Given an array defined as T ARRAY[MAX],
  check that an UNSAFE pointer actually points to an element
  within the array.
*/
#define SANITIZE_ARRAY_BODY(T, ARRAY, MAX, UNSAFE)          \
  intptr offset;                                            \
  if ((&ARRAY[0] <= UNSAFE) &&                              \
      (UNSAFE < &ARRAY[MAX]))                               \
  {                                                         \
    offset= ((intptr) UNSAFE - (intptr) ARRAY) % sizeof(T); \
    if (offset == 0)                                        \
      return UNSAFE;                                        \
  }                                                         \
  return NULL

#endif

