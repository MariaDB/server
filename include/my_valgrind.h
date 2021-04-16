/* Copyright (C) 2010, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef MY_VALGRIND_INCLUDED
#define MY_VALGRIND_INCLUDED

/* clang -> gcc */
#ifndef __has_feature
# define __has_feature(x) 0
#endif
#if __has_feature(address_sanitizer)
# define __SANITIZE_ADDRESS__ 1
#endif

#if __has_feature(memory_sanitizer)
# include <sanitizer/msan_interface.h>
# define HAVE_valgrind
# define HAVE_MEM_CHECK
# define MEM_UNDEFINED(a,len) __msan_allocated_memory(a,len)
# define MEM_MAKE_ADDRESSABLE(a,len) MEM_UNDEFINED(a,len)
# define MEM_MAKE_DEFINED(a,len) __msan_unpoison(a,len)
# define MEM_NOACCESS(a,len) ((void) 0)
# define MEM_CHECK_ADDRESSABLE(a,len) ((void) 0)
# define MEM_CHECK_DEFINED(a,len) __msan_check_mem_is_initialized(a,len)
# define MEM_GET_VBITS(a,b,len) __msan_copy_shadow(b,a,len)
# define MEM_SET_VBITS(a,b,len) __msan_copy_shadow(a,b,len)
# define REDZONE_SIZE 8
#elif defined(HAVE_VALGRIND_MEMCHECK_H) && defined(HAVE_valgrind)
# include <valgrind/memcheck.h>
# define HAVE_MEM_CHECK
# define MEM_UNDEFINED(a,len) VALGRIND_MAKE_MEM_UNDEFINED(a,len)
# define MEM_MAKE_ADDRESSABLE(a,len) MEM_UNDEFINED(a,len)
# define MEM_MAKE_DEFINED(a,len) VALGRIND_MAKE_MEM_DEFINED(a,len)
# define MEM_NOACCESS(a,len) VALGRIND_MAKE_MEM_NOACCESS(a,len)
# define MEM_CHECK_ADDRESSABLE(a,len) VALGRIND_CHECK_MEM_IS_ADDRESSABLE(a,len)
# define MEM_CHECK_DEFINED(a,len) VALGRIND_CHECK_MEM_IS_DEFINED(a,len)
# define MEM_GET_VBITS(a,b,len) VALGRIND_GET_VBITS(a,b,len)
# define MEM_SET_VBITS(a,b,len) VALGRIND_SET_VBITS(a,b,len)
# define REDZONE_SIZE 8
#elif defined(__SANITIZE_ADDRESS__) && (!defined(_MSC_VER) || defined (__clang__))
# include <sanitizer/asan_interface.h>
/* How to do manual poisoning:
https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning */
# define MEM_UNDEFINED(a,len) ((void) 0)
# define MEM_MAKE_ADDRESSABLE(a,len) ASAN_UNPOISON_MEMORY_REGION(a,len)
# define MEM_MAKE_DEFINED(a,len) ((void) 0)
# define MEM_NOACCESS(a,len) ASAN_POISON_MEMORY_REGION(a,len)
# define MEM_CHECK_ADDRESSABLE(a,len) \
  assert(!__asan_region_is_poisoned((void*) a,len))
# define MEM_CHECK_DEFINED(a,len) ((void) 0)
# define MEM_GET_VBITS(a,b,len) ((void) 0)
# define MEM_SET_VBITS(a,b,len) ((void) 0)
# define REDZONE_SIZE 8
#else
# define MEM_UNDEFINED(a,len) ((void) 0)
# define MEM_MAKE_ADDRESSABLE(a,len) ((void) 0)
# define MEM_MAKE_DEFINED(a,len) ((void) 0)
# define MEM_NOACCESS(a,len) ((void) 0)
# define MEM_CHECK_ADDRESSABLE(a,len) ((void) 0)
# define MEM_CHECK_DEFINED(a,len) ((void) 0)
# define MEM_GET_VBITS(a,b,len) ((void) 0)
# define MEM_SET_VBITS(a,b,len) ((void) 0)
# define REDZONE_SIZE 0
#endif /* __has_feature(memory_sanitizer) */

#ifdef HAVE_valgrind
#define IF_VALGRIND(A,B) A
#else
#define IF_VALGRIND(A,B) B
#endif

#ifdef TRASH_FREED_MEMORY
/*
  _TRASH_FILL() has to call MEM_MAKE_ADDRESSABLE() to cancel any effect of
  TRASH_FREE().
  This can happen in the case one does
  TRASH_ALLOC(A,B) ; TRASH_FREE(A,B) ; TRASH_ALLOC(A,B)
  to reuse the same memory in an internal memory allocator like MEM_ROOT.
  _TRASH_FILL() is an internal function and should not be used externally.
*/
#define _TRASH_FILL(A,B,C) do { const size_t trash_tmp= (B); MEM_MAKE_ADDRESSABLE(A, trash_tmp); memset(A, C, trash_tmp); } while (0)
#else
#define _TRASH_FILL(A,B,C) do { MEM_UNDEFINED((A), (B)); } while (0)
#endif
/** Note that some memory became allocated and/or uninitialized. */
#define TRASH_ALLOC(A,B) do { _TRASH_FILL(A,B,0xA5); MEM_MAKE_ADDRESSABLE(A,B); } while(0)
/** Note that some memory became freed. (Prohibit further access to it.) */
#define TRASH_FREE(A,B) do { _TRASH_FILL(A,B,0x8F); MEM_NOACCESS(A,B); } while(0)

#endif /* MY_VALGRIND_INCLUDED */
