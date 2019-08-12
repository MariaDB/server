/* Copyright (C) 2010, 2019, MariaDB Corporation.

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

#ifdef HAVE_valgrind
#define IF_VALGRIND(A,B) A
#else
#define IF_VALGRIND(A,B) B
#endif

#if defined(HAVE_VALGRIND) && defined(HAVE_valgrind)
# include <valgrind/memcheck.h>
# define MEM_UNDEFINED(a,len) VALGRIND_MAKE_MEM_UNDEFINED(a,len)
# define MEM_NOACCESS(a,len) VALGRIND_MAKE_MEM_NOACCESS(a,len)
# define MEM_CHECK_ADDRESSABLE(a,len) VALGRIND_CHECK_MEM_IS_ADDRESSABLE(a,len)
# define MEM_CHECK_DEFINED(a,len) VALGRIND_CHECK_MEM_IS_DEFINED(a,len)
# define REDZONE_SIZE 8
#elif defined(__SANITIZE_ADDRESS__)
# include <sanitizer/asan_interface.h>
/* How to do manual poisoning:
https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning */
# define MEM_UNDEFINED(a,len) ASAN_UNPOISON_MEMORY_REGION(a,len)
# define MEM_NOACCESS(a,len) ASAN_POISON_MEMORY_REGION(a,len)
# define MEM_CHECK_ADDRESSABLE(a,len) ((void) 0)
# define MEM_CHECK_DEFINED(a,len) ((void) 0)
# define REDZONE_SIZE 8
#else
# define MEM_UNDEFINED(a,len) ((void) (a), (void) (len))
# define MEM_NOACCESS(a,len) ((void) 0)
# define MEM_CHECK_ADDRESSABLE(a,len) ((void) 0)
# define MEM_CHECK_DEFINED(a,len) ((void) 0)
# define REDZONE_SIZE 0
#endif /* HAVE_VALGRIND */

#ifndef DBUG_OFF
/* NOTE: Do not invoke TRASH_FILL directly! Use TRASH_ALLOC or TRASH_FREE.

The MEM_UNDEFINED() call before memset() is for canceling the effect
of any previous MEM_NOACCESS(). We must invoke MEM_UNDEFINED() after
writing the dummy pattern, unless MEM_NOACCESS() is going to be invoked.
On AddressSanitizer, the MEM_UNDEFINED() in TRASH_ALLOC() has no effect. */
#define TRASH_FILL(A,B,C) do { const size_t trash_tmp= (B); MEM_UNDEFINED(A, trash_tmp); memset(A, C, trash_tmp); } while (0)
#else
#define TRASH_FILL(A,B,C) do { MEM_UNDEFINED((A), (B)); } while (0)
#endif
/** Note that some memory became allocated or uninitialized. */
#define TRASH_ALLOC(A,B) do { TRASH_FILL(A,B,0xA5); MEM_UNDEFINED(A,B); } while(0)
/** Note that some memory became freed. (Prohibit further access to it.) */
#define TRASH_FREE(A,B) do { TRASH_FILL(A,B,0x8F); MEM_NOACCESS(A,B); } while(0)

#endif /* MY_VALGRIND_INCLUDED */
