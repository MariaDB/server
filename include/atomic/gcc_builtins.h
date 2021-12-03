#ifndef ATOMIC_GCC_BUILTINS_INCLUDED
#define ATOMIC_GCC_BUILTINS_INCLUDED

/* Copyright (c) 2017 MariaDB Foundation

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
  __atomic_compare_exchange_n((P), (E), (D), 1, (S), (F))
#define my_atomic_cas64_weak_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), 1, (S), (F))
#define my_atomic_casptr_weak_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), 1, (S), (F))

#define my_atomic_cas32_strong_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), 0, (S), (F))
#define my_atomic_cas64_strong_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), 0, (S), (F))
#define my_atomic_casptr_strong_explicit(P, E, D, S, F) \
  __atomic_compare_exchange_n((P), (E), (D), 0, (S), (F))

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

#endif /* ATOMIC_GCC_BUILTINS_INCLUDED */
