#ifndef ATOMIC_GCC_BUILTINS_INCLUDED
#define ATOMIC_GCC_BUILTINS_INCLUDED

/* Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.

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

#if defined(HAVE_GCC_C11_ATOMICS)
#define MY_ATOMIC_MODE "gcc-atomics-smp"

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

#define my_atomic_store32(P, D) __atomic_store_n((P), (D), __ATOMIC_SEQ_CST)
#define my_atomic_store64(P, D) __atomic_store_n((P), (D), __ATOMIC_SEQ_CST)
#define my_atomic_storeptr(P, D) __atomic_store_n((P), (D), __ATOMIC_SEQ_CST)

#define my_atomic_load32(P) __atomic_load_n((P), __ATOMIC_SEQ_CST)
#define my_atomic_load64(P) __atomic_load_n((P), __ATOMIC_SEQ_CST)
#define my_atomic_loadptr(P) __atomic_load_n((P), __ATOMIC_SEQ_CST)

#define my_atomic_fas32(P, D) __atomic_exchange_n((P), (D), __ATOMIC_SEQ_CST)
#define my_atomic_fas64(P, D) __atomic_exchange_n((P), (D), __ATOMIC_SEQ_CST)
#define my_atomic_fasptr(P, D) __atomic_exchange_n((P), (D), __ATOMIC_SEQ_CST)

#define my_atomic_add32(P, A) __atomic_fetch_add((P), (A), __ATOMIC_SEQ_CST)
#define my_atomic_add64(P, A) __atomic_fetch_add((P), (A), __ATOMIC_SEQ_CST)

#define my_atomic_cas32(P, E, D) \
  __atomic_compare_exchange_n((P), (E), (D), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define my_atomic_cas64(P, E, D) \
  __atomic_compare_exchange_n((P), (E), (D), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define my_atomic_casptr(P, E, D) \
  __atomic_compare_exchange_n((P), (E), (D), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#else
#define MY_ATOMIC_MODE "gcc-builtins-smp"
#define make_atomic_load_body(S)                    \
  ret= __sync_fetch_and_or(a, 0);
#define make_atomic_store_body(S)                   \
  (void) __sync_lock_test_and_set(a, v);
#define make_atomic_add_body(S)                     \
  v= __sync_fetch_and_add(a, v);
#define make_atomic_fas_body(S)                     \
  v= __sync_lock_test_and_set(a, v);
#define make_atomic_cas_body(S)                     \
  int ## S sav;                                     \
  int ## S cmp_val= *cmp;                           \
  sav= __sync_val_compare_and_swap(a, cmp_val, set);\
  if (!(ret= (sav == cmp_val))) *cmp= sav
#endif

#endif /* ATOMIC_GCC_BUILTINS_INCLUDED */
