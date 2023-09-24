#ifndef ATOMIC_MSC_INCLUDED
#define ATOMIC_MSC_INCLUDED

/* Copyright (c) 2006, 2014, Oracle and/or its affiliates. All rights reserved.

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

#include <windows.h>

static inline int my_atomic_cas32(int32 volatile *a, int32 *cmp, int32 set)
{
  int32 initial_cmp= *cmp;
  int32 initial_a= InterlockedCompareExchange((volatile LONG*)a,
                                              set, initial_cmp);
  int ret= (initial_a == initial_cmp);
  if (!ret)
    *cmp= initial_a;
  return ret;
}

static inline int my_atomic_cas64(int64 volatile *a, int64 *cmp, int64 set)
{
  int64 initial_cmp= *cmp;
  int64 initial_a= InterlockedCompareExchange64((volatile LONGLONG*)a,
                                                (LONGLONG)set,
                                                (LONGLONG)initial_cmp);
  int ret= (initial_a == initial_cmp);
  if (!ret)
    *cmp= initial_a;
  return ret;
}

static inline int my_atomic_casptr(void * volatile *a, void **cmp, void *set)
{
  void *initial_cmp= *cmp;
  void *initial_a= InterlockedCompareExchangePointer(a, set, initial_cmp);
  int ret= (initial_a == initial_cmp);
  if (!ret)
    *cmp= initial_a;
  return ret;
}

static inline int32 my_atomic_add32(int32 volatile *a, int32 v)
{
  return (int32)InterlockedExchangeAdd((volatile LONG*)a, v);
}

static inline int64 my_atomic_add64(int64 volatile *a, int64 v)
{
  return (int64)InterlockedExchangeAdd64((volatile LONGLONG*)a, (LONGLONG)v);
}


/*
  According to MSDN:

  Simple reads and writes to properly-aligned 32-bit variables are atomic
  operations.
  ...
  Simple reads and writes to properly aligned 64-bit variables are atomic on
  64-bit Windows. Reads and writes to 64-bit values are not guaranteed to be
  atomic on 32-bit Windows.

  https://learn.microsoft.com/en-us/windows/win32/sync/interlocked-variable-access
*/

static inline int32 my_atomic_load32(int32 volatile *a)
{
  int32 value= *a;
  MemoryBarrier();
  return value;
}

static inline int64 my_atomic_load64(int64 volatile *a)
{
#if defined(_M_X64) || defined(_M_ARM64)
  int64 value= *a;
  MemoryBarrier();
  return value;
#else
  return (int64) InterlockedCompareExchange64((volatile LONGLONG *) a, 0, 0);
#endif
}

static inline void* my_atomic_loadptr(void * volatile *a)
{
  void *value= *a;
  MemoryBarrier();
  return value;
}

static inline int32 my_atomic_fas32(int32 volatile *a, int32 v)
{
  return (int32)InterlockedExchange((volatile LONG*)a, v);
}

static inline int64 my_atomic_fas64(int64 volatile *a, int64 v)
{
  return (int64)InterlockedExchange64((volatile LONGLONG*)a, v);
}

static inline void * my_atomic_fasptr(void * volatile *a, void * v)
{
  return InterlockedExchangePointer(a, v);
}

static inline void my_atomic_store32(int32 volatile *a, int32 v)
{
  MemoryBarrier();
  *a= v;
}

static inline void my_atomic_store64(int64 volatile *a, int64 v)
{
#if defined(_M_X64) || defined(_M_ARM64)
  MemoryBarrier();
  *a= v;
#else
  (void) InterlockedExchange64((volatile LONGLONG *) a, v);
#endif
}

static inline void my_atomic_storeptr(void * volatile *a, void *v)
{
  MemoryBarrier();
  *a= v;
}

#endif /* ATOMIC_MSC_INCLUDED */
