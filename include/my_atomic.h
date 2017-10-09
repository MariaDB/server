#ifndef MY_ATOMIC_INCLUDED
#define MY_ATOMIC_INCLUDED

/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

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

/*
  This header defines five atomic operations:

  my_atomic_add#(&var, what)
  my_atomic_add#_explicit(&var, what, memory_order)
    'Fetch and Add'
    add 'what' to *var, and return the old value of *var
    All memory orders are valid.

  my_atomic_fas#(&var, what)
  my_atomic_fas#_explicit(&var, what, memory_order)
    'Fetch And Store'
    store 'what' in *var, and return the old value of *var
    All memory orders are valid.

  my_atomic_cas#(&var, &old, new)
  my_atomic_cas#_weak_explicit(&var, &old, new, succ, fail)
  my_atomic_cas#_strong_explicit(&var, &old, new, succ, fail)
    'Compare And Swap'
    if *var is equal to *old, then store 'new' in *var, and return TRUE
    otherwise store *var in *old, and return FALSE
    succ - the memory synchronization ordering for the read-modify-write
    operation if the comparison succeeds. All memory orders are valid.
    fail - the memory synchronization ordering for the load operation if the
    comparison fails. Cannot be MY_MEMORY_ORDER_RELEASE or
    MY_MEMORY_ORDER_ACQ_REL and cannot specify stronger ordering than succ.

    The weak form is allowed to fail spuriously, that is, act as if *var != *old
    even if they are equal. When a compare-and-exchange is in a loop, the weak
    version will yield better performance on some platforms. When a weak
    compare-and-exchange would require a loop and a strong one would not, the
    strong one is preferable.

  my_atomic_load#(&var)
  my_atomic_load#_explicit(&var, memory_order)
    return *var
    Order must be one of MY_MEMORY_ORDER_RELAXED, MY_MEMORY_ORDER_CONSUME,
    MY_MEMORY_ORDER_ACQUIRE, MY_MEMORY_ORDER_SEQ_CST.

  my_atomic_store#(&var, what)
  my_atomic_store#_explicit(&var, what, memory_order)
    store 'what' in *var
    Order must be one of MY_MEMORY_ORDER_RELAXED, MY_MEMORY_ORDER_RELEASE,
    MY_MEMORY_ORDER_SEQ_CST.

  '#' is substituted by a size suffix - 8, 16, 32, 64, or ptr
  (e.g. my_atomic_add8, my_atomic_fas32, my_atomic_casptr).

  The first version orders memory accesses according to MY_MEMORY_ORDER_SEQ_CST,
  the second version (with _explicit suffix) orders memory accesses according to
  given memory order.

  memory_order specifies how non-atomic memory accesses are to be ordered around
  an atomic operation:

  MY_MEMORY_ORDER_RELAXED - there are no constraints on reordering of memory
                            accesses around the atomic variable.
  MY_MEMORY_ORDER_CONSUME - no reads in the current thread dependent on the
                            value currently loaded can be reordered before this
                            load. This ensures that writes to dependent
                            variables in other threads that release the same
                            atomic variable are visible in the current thread.
                            On most platforms, this affects compiler
                            optimization only.
  MY_MEMORY_ORDER_ACQUIRE - no reads in the current thread can be reordered
                            before this load. This ensures that all writes in
                            other threads that release the same atomic variable
                            are visible in the current thread.
  MY_MEMORY_ORDER_RELEASE - no writes in the current thread can be reordered
                            after this store. This ensures that all writes in
                            the current thread are visible in other threads that
                            acquire the same atomic variable.
  MY_MEMORY_ORDER_ACQ_REL - no reads in the current thread can be reordered
                            before this load as well as no writes in the current
                            thread can be reordered after this store. The
                            operation is read-modify-write operation. It is
                            ensured that all writes in another threads that
                            release the same atomic variable are visible before
                            the modification and the modification is visible in
                            other threads that acquire the same atomic variable.
  MY_MEMORY_ORDER_SEQ_CST - The operation has the same semantics as
                            acquire-release operation, and additionally has
                            sequentially-consistent operation ordering.

  We choose implementation as follows: on Windows using Visual C++ the native
  implementation should be preferrable. When using gcc we prefer the Solaris
  implementation before the gcc because of stability preference, we choose gcc
  builtins if available.
*/

#if defined(_MSC_VER)
#include "atomic/generic-msvc.h"
#elif defined(HAVE_SOLARIS_ATOMIC)
#include "atomic/solaris.h"
#elif defined(HAVE_GCC_ATOMIC_BUILTINS) || defined(HAVE_GCC_C11_ATOMICS)
#include "atomic/gcc_builtins.h"
#endif


#ifndef HAVE_GCC_C11_ATOMICS
#ifndef make_atomic_cas_body
/* nolock.h was not able to generate even a CAS function, fall back */
#error atomic ops for this platform are not implemented
#endif

#define intptr         void *

/* define missing functions by using the already generated ones */
#ifndef make_atomic_add_body
#define make_atomic_add_body(S)                                 \
  int ## S tmp=*a;                                              \
  while (!my_atomic_cas ## S(a, &tmp, tmp+v)) ;                 \
  v=tmp;
#endif
#ifndef make_atomic_fas_body
#define make_atomic_fas_body(S)                                 \
  int ## S tmp=*a;                                              \
  while (!my_atomic_cas ## S(a, &tmp, v)) ;                     \
  v=tmp;
#endif
#ifndef make_atomic_load_body
#define make_atomic_load_body(S)                                \
  ret= 0; /* avoid compiler warning */                          \
  (void)(my_atomic_cas ## S(a, &ret, ret));
#endif
#ifndef make_atomic_store_body
#define make_atomic_store_body(S)                               \
  (void)(my_atomic_fas ## S (a, v));
#endif

#define make_atomic_cas(S)                                      \
static inline int my_atomic_cas ## S(int ## S volatile *a,      \
                            int ## S *cmp, int ## S set)        \
{                                                               \
  int8 ret;                                                     \
  make_atomic_cas_body(S);                                      \
  return ret;                                                   \
}

#define make_atomic_add(S)                                      \
static inline int ## S my_atomic_add ## S(                      \
                        int ## S volatile *a, int ## S v)       \
{                                                               \
  make_atomic_add_body(S);                                      \
  return v;                                                     \
}

#define make_atomic_fas(S)                                      \
static inline int ## S my_atomic_fas ## S(                      \
                         int ## S volatile *a, int ## S v)      \
{                                                               \
  make_atomic_fas_body(S);                                      \
  return v;                                                     \
}

#define make_atomic_load(S)                                     \
static inline int ## S my_atomic_load ## S(int ## S volatile *a)\
{                                                               \
  int ## S ret;                                                 \
  make_atomic_load_body(S);                                     \
  return ret;                                                   \
}

#define make_atomic_store(S)                                    \
static inline void my_atomic_store ## S(                        \
                     int ## S volatile *a, int ## S v)          \
{                                                               \
  make_atomic_store_body(S);                                    \
}

make_atomic_cas(32)
make_atomic_cas(64)
make_atomic_cas(ptr)

make_atomic_add(32)
make_atomic_add(64)

make_atomic_load(32)
make_atomic_load(64)
make_atomic_load(ptr)

make_atomic_fas(32)
make_atomic_fas(64)
make_atomic_fas(ptr)

make_atomic_store(32)
make_atomic_store(64)
make_atomic_store(ptr)

#ifdef _atomic_h_cleanup_
#include _atomic_h_cleanup_
#undef _atomic_h_cleanup_
#endif

#undef make_atomic_add
#undef make_atomic_cas
#undef make_atomic_load
#undef make_atomic_store
#undef make_atomic_fas
#undef make_atomic_add_body
#undef make_atomic_cas_body
#undef make_atomic_load_body
#undef make_atomic_store_body
#undef make_atomic_fas_body
#undef intptr
#endif

/*
  the macro below defines (as an expression) the code that
  will be run in spin-loops. Intel manuals recummend to have PAUSE there.
  It is expected to be defined in include/atomic/ *.h files
*/
#ifndef LF_BACKOFF
#define LF_BACKOFF (1)
#endif

#if SIZEOF_LONG == 4
#define my_atomic_addlong(A,B) my_atomic_add32((int32*) (A), (B))
#define my_atomic_loadlong(A) my_atomic_load32((int32*) (A))
#define my_atomic_loadlong_explicit(A,O) my_atomic_load32_explicit((int32*) (A), (O))
#define my_atomic_storelong(A,B) my_atomic_store32((int32*) (A), (B))
#define my_atomic_faslong(A,B) my_atomic_fas32((int32*) (A), (B))
#define my_atomic_caslong(A,B,C) my_atomic_cas32((int32*) (A), (int32*) (B), (C))
#else
#define my_atomic_addlong(A,B) my_atomic_add64((int64*) (A), (B))
#define my_atomic_loadlong(A) my_atomic_load64((int64*) (A))
#define my_atomic_loadlong_explicit(A,O) my_atomic_load64_explicit((int64*) (A), (O))
#define my_atomic_storelong(A,B) my_atomic_store64((int64*) (A), (B))
#define my_atomic_faslong(A,B) my_atomic_fas64((int64*) (A), (B))
#define my_atomic_caslong(A,B,C) my_atomic_cas64((int64*) (A), (int64*) (B), (C))
#endif

#ifndef MY_MEMORY_ORDER_SEQ_CST
#define MY_MEMORY_ORDER_RELAXED
#define MY_MEMORY_ORDER_CONSUME
#define MY_MEMORY_ORDER_ACQUIRE
#define MY_MEMORY_ORDER_RELEASE
#define MY_MEMORY_ORDER_ACQ_REL
#define MY_MEMORY_ORDER_SEQ_CST

#define my_atomic_store32_explicit(P, D, O) my_atomic_store32((P), (D))
#define my_atomic_store64_explicit(P, D, O) my_atomic_store64((P), (D))
#define my_atomic_storeptr_explicit(P, D, O) my_atomic_storeptr((P), (D))

#define my_atomic_load32_explicit(P, O) my_atomic_load32((P))
#define my_atomic_load64_explicit(P, O) my_atomic_load64((P))
#define my_atomic_loadptr_explicit(P, O) my_atomic_loadptr((P))

#define my_atomic_fas32_explicit(P, D, O) my_atomic_fas32((P), (D))
#define my_atomic_fas64_explicit(P, D, O) my_atomic_fas64((P), (D))
#define my_atomic_fasptr_explicit(P, D, O) my_atomic_fasptr((P), (D))

#define my_atomic_add32_explicit(P, A, O) my_atomic_add32((P), (A))
#define my_atomic_add64_explicit(P, A, O) my_atomic_add64((P), (A))
#define my_atomic_addptr_explicit(P, A, O) my_atomic_addptr((P), (A))

#define my_atomic_cas32_weak_explicit(P, E, D, S, F) \
  my_atomic_cas32((P), (E), (D))
#define my_atomic_cas64_weak_explicit(P, E, D, S, F) \
  my_atomic_cas64((P), (E), (D))
#define my_atomic_casptr_weak_explicit(P, E, D, S, F) \
  my_atomic_casptr((P), (E), (D))

#define my_atomic_cas32_strong_explicit(P, E, D, S, F) \
  my_atomic_cas32((P), (E), (D))
#define my_atomic_cas64_strong_explicit(P, E, D, S, F) \
  my_atomic_cas64((P), (E), (D))
#define my_atomic_casptr_strong_explicit(P, E, D, S, F) \
  my_atomic_casptr((P), (E), (D))
#endif

#endif /* MY_ATOMIC_INCLUDED */
