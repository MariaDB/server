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

  8- and 16-bit atomics aren't implemented for windows (see generic-msvc.h),
  but can be added, if necessary.
*/

#define intptr         void *
/**
  Currently we don't support 8-bit and 16-bit operations.
  It can be added later if needed.
*/
#undef MY_ATOMIC_HAS_8_16

/*
 * Attempt to do atomic ops without locks
 */
#include "atomic/nolock.h"

#ifndef make_atomic_cas_body
/* nolock.h was not able to generate even a CAS function, fall back */
#error atomic ops for this platform are not implemented
#endif

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

/*
  transparent_union doesn't work in g++
  Bug ?

  Darwin's gcc doesn't want to put pointers in a transparent_union
  when built with -arch ppc64. Complains:
  warning: 'transparent_union' attribute ignored
*/
#if defined(__GNUC__) && !defined(__cplusplus) && \
      ! (defined(__APPLE__) && (defined(_ARCH_PPC64) ||defined (_ARCH_PPC)))
/*
  we want to be able to use my_atomic_xxx functions with
  both signed and unsigned integers. But gcc will issue a warning
  "passing arg N of `my_atomic_XXX' as [un]signed due to prototype"
  if the signedness of the argument doesn't match the prototype, or
  "pointer targets in passing argument N of my_atomic_XXX differ in signedness"
  if int* is used where uint* is expected (or vice versa).
  Let's shut these warnings up
*/
#define make_transparent_unions(S)                              \
        typedef union {                                         \
          int  ## S  i;                                         \
          uint ## S  u;                                         \
        } U_ ## S   __attribute__ ((transparent_union));        \
        typedef union {                                         \
          int  ## S volatile *i;                                \
          uint ## S volatile *u;                                \
        } Uv_ ## S   __attribute__ ((transparent_union));
#define uintptr intptr
make_transparent_unions(8)
make_transparent_unions(16)
make_transparent_unions(32)
make_transparent_unions(64)
make_transparent_unions(ptr)
#undef uintptr
#undef make_transparent_unions
#define a       U_a.i
#define cmp     U_cmp.i
#define v       U_v.i
#define set     U_set.i
#else
#define U_8    int8
#define U_16   int16
#define U_32   int32
#define U_64   int64
#define U_ptr  intptr
#define Uv_8   int8
#define Uv_16  int16
#define Uv_32  int32
#define Uv_64  int64
#define Uv_ptr intptr
#define U_a    volatile *a
#define U_cmp  *cmp
#define U_v    v
#define U_set  set
#endif /* __GCC__ transparent_union magic */

#define make_atomic_cas(S)                                      \
static inline int my_atomic_cas ## S(Uv_ ## S U_a,              \
                            Uv_ ## S U_cmp, U_ ## S U_set)      \
{                                                               \
  int8 ret;                                                     \
  make_atomic_cas_body(S);                                      \
  return ret;                                                   \
}

#define make_atomic_add(S)                                      \
static inline int ## S my_atomic_add ## S(                      \
                        Uv_ ## S U_a, U_ ## S U_v)              \
{                                                               \
  make_atomic_add_body(S);                                      \
  return v;                                                     \
}

#define make_atomic_fas(S)                                      \
static inline int ## S my_atomic_fas ## S(                      \
                         Uv_ ## S U_a, U_ ## S U_v)             \
{                                                               \
  make_atomic_fas_body(S);                                      \
  return v;                                                     \
}

#define make_atomic_load(S)                                     \
static inline int ## S my_atomic_load ## S(Uv_ ## S U_a)        \
{                                                               \
  int ## S ret;                                                 \
  make_atomic_load_body(S);                                     \
  return ret;                                                   \
}

#define make_atomic_store(S)                                    \
static inline void my_atomic_store ## S(                        \
                     Uv_ ## S U_a, U_ ## S U_v)                 \
{                                                               \
  make_atomic_store_body(S);                                    \
}

#ifdef MY_ATOMIC_HAS_8_16
make_atomic_cas(8)
make_atomic_cas(16)
#endif
make_atomic_cas(32)
make_atomic_cas(64)
make_atomic_cas(ptr)

#ifdef MY_ATOMIC_HAS_8_16
make_atomic_add(8)
make_atomic_add(16)
#endif
make_atomic_add(32)
make_atomic_add(64)

#ifdef MY_ATOMIC_HAS_8_16
make_atomic_load(8)
make_atomic_load(16)
#endif
make_atomic_load(32)
make_atomic_load(64)
make_atomic_load(ptr)

#ifdef MY_ATOMIC_HAS_8_16
make_atomic_fas(8)
make_atomic_fas(16)
#endif
make_atomic_fas(32)
make_atomic_fas(64)
make_atomic_fas(ptr)

#ifdef MY_ATOMIC_HAS_8_16
make_atomic_store(8)
make_atomic_store(16)
#endif
make_atomic_store(32)
make_atomic_store(64)
make_atomic_store(ptr)

#ifdef _atomic_h_cleanup_
#include _atomic_h_cleanup_
#undef _atomic_h_cleanup_
#endif

#undef U_8
#undef U_16
#undef U_32
#undef U_64
#undef U_ptr
#undef Uv_8
#undef Uv_16
#undef Uv_32
#undef Uv_64
#undef Uv_ptr
#undef a
#undef cmp
#undef v
#undef set
#undef U_a
#undef U_cmp
#undef U_v
#undef U_set
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

/*
  the macro below defines (as an expression) the code that
  will be run in spin-loops. Intel manuals recummend to have PAUSE there.
  It is expected to be defined in include/atomic/ *.h files
*/
#ifndef LF_BACKOFF
#define LF_BACKOFF (1)
#endif

#define MY_ATOMIC_OK       0
#define MY_ATOMIC_NOT_1CPU 1
extern int my_atomic_initialize();

#ifdef __ATOMIC_SEQ_CST
#define MY_MEMORY_ORDER_RELAXED __ATOMIC_RELAXED
#define MY_MEMORY_ORDER_CONSUME __ATOMIC_CONSUME
#define MY_MEMORY_ORDER_ACQUIRE __ATOMIC_ACQUIRE
#define MY_MEMORY_ORDER_RELEASE __ATOMIC_RELEASE
#define MY_MEMORY_ORDER_ACQ_REL __ATOMIC_ACQ_REL
#define MY_MEMORY_ORDER_SEQ_CST __ATOMIC_SEQ_CST

#define my_atomic_store32_explicit(P, D, O) __atomic_store_n((P), (D), (O))
#define my_atomic_store64_explicit(P, D, O) __atomic_store_n((P), (D), (O))
#define my_atomic_storeptr_explicit(P, D, O) __atomic_store_n((P), (D), (O))

#define my_atomic_load32_explicit(P, O) __atomic_load_n((P), (O))
#define my_atomic_load64_explicit(P, O) __atomic_load_n((P), (O))
#define my_atomic_loadptr_explicit(P, O) __atomic_load_n((P), (O))

#define my_atomic_fas32_explicit(P, D, O) __atomic_exchange_n((P), (D), (O))
#define my_atomic_fas64_explicit(P, D, O) __atomic_exchange_n((P), (D), (O))
#define my_atomic_fasptr_explicit(P, D, O) __atomic_exchange_n((P), (D), (O))

#define my_atomic_add32_explicit(P, A, O) __atomic_fetch_add((P), (A), (O))
#define my_atomic_add64_explicit(P, A, O) __atomic_fetch_add((P), (A), (O))

#define my_atomic_cas32_weak_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), true, (S), (F))
#define my_atomic_cas64_weak_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), true, (S), (F))
#define my_atomic_casptr_weak_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), true, (S), (F))

#define my_atomic_cas32_strong_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), false, (S), (F))
#define my_atomic_cas64_strong_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), false, (S), (F))
#define my_atomic_casptr_strong_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), false, (S), (F))
#else
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
