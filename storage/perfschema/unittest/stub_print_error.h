/* Copyright (c) 2008, 2023, Oracle and/or its affiliates.

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

#include <my_global.h>
#include <my_sys.h>
#include <pfs_global.h>
#include "aligned.h"
#include "assume_aligned.h"

bool pfs_initialized= false;

void *pfs_malloc(PFS_builtin_memory_class *klass, size_t size, myf flags)
{
  size= MY_ALIGN(size, CPU_LEVEL1_DCACHE_LINESIZE);
  void *ptr= aligned_malloc(size, CPU_LEVEL1_DCACHE_LINESIZE);
  if (ptr && (flags & MY_ZEROFILL))
    memset_aligned<CPU_LEVEL1_DCACHE_LINESIZE>(ptr, 0, size);
  return ptr;
}

void pfs_free(PFS_builtin_memory_class *, size_t, void *ptr)
{
  aligned_free(ptr);
}

void *pfs_malloc_array(PFS_builtin_memory_class *klass, size_t n, size_t size, myf flags)
{
  size_t array_size= n * size;
  /* Check for overflow before allocating. */
  if (is_overflow(array_size, n, size))
    return NULL;
  return pfs_malloc(klass, array_size, flags);
}

void pfs_free_array(PFS_builtin_memory_class *klass, size_t n, size_t size, void *ptr)
{
  if (ptr == NULL)
    return;
  size_t array_size= n * size;
  return pfs_free(klass, array_size, ptr);
}

bool is_overflow(size_t product, size_t n1, size_t n2)
{
  if (n1 != 0 && (product / n1 != n2))
    return true;
  else
    return false;
}

void pfs_print_error(const char *format, ...)
{
  /* Do not pollute the unit test output with annoying messages. */
}

