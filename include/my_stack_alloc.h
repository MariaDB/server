/* Copyright 2019 MariaDB corporation AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#ifndef _my_stack_alloc_h
#define _my_stack_alloc_h

#ifdef _MSC_VER
#include <intrin.h> // For MSVC-specific intrinsics
#else
#include <sys/resource.h>
#endif


/*
  Get the address of the current stack.
  This will fallback to using an estimate for the stack pointer
  in the cases where either the compiler or the architecture is
  not supported.
*/

static inline void *my_get_stack_pointer(void *default_stack)
{
  void *stack_ptr= NULL;

#if defined(__GNUC__) || defined(__clang__) /* GCC and Clang compilers */
#if defined(__i386__) /* Intel x86 (32-bit) */
  __asm__ volatile ("movl %%esp, %0" : "=r" (stack_ptr));
#elif defined(__x86_64__) && defined (__ILP32__) /* Intel x86-64 (64-bit), X32 ABI */
  __asm__ volatile ("movl %%esp, %0" : "=r" (stack_ptr));
#elif defined(__x86_64__) /* Intel x86-64 (64-bit) */
  __asm__ volatile ("movq %%rsp, %0" : "=r" (stack_ptr));
#elif defined(__powerpc__) /* PowerPC (32-bit) */
  __asm__ volatile ("mr %0, 1" : "=r" (stack_ptr)); /* GPR1 is the stack pointer */
#elif defined(__ppc64__) /* PowerPC (64-bit) */
  __asm__ volatile ("mr %0, 1" : "=r" (stack_ptr));
#elif defined(__arm__) /* ARM 32-bit */
  __asm__ volatile ("mov %0, sp" : "=r" (stack_ptr));
#elif defined(__aarch64__) /* ARM 64-bit */
  __asm__ volatile ("mov %0, sp" : "=r" (stack_ptr));
#elif defined(__sparc__) /* SPARC 32-bit */
  __asm__ volatile ("mov %%sp, %0" : "=r" (stack_ptr));
#elif defined(__sparc64__) /* SPARC 64-bit */
  __asm__ volatile ("mov %%sp, %0" : "=r" (stack_ptr));
#elif defined(__s390x__)
  stack_ptr= __builtin_frame_address(0);
#else
  /* Generic fallback for unsupported architectures in GCC/Clang */
  stack_ptr= default_stack ? default_stack : (void*) &stack_ptr;
#endif
#elif defined(_MSC_VER) /* MSVC compiler (Intel only) */
#if defined(_M_IX86) /* Intel x86 (32-bit) */
  __asm { mov stack_ptr, esp }
#elif defined(_M_X64) /* Intel x86-64 (64-bit) */
  /* rsp can’t be accessed directly in MSVC x64 */
  stack_ptr= _AddressOfReturnAddress();
#else
  /* Generic fallback for unsupported architectures in MSVC */
  stack_ptr= default_stack ? default_stack : (void*) &stack_ptr;
#endif
#else
  /* Generic fallback for unsupported compilers */
  stack_ptr= default_stack ? default_stack : (void*) &stack_ptr;
#endif
  return stack_ptr;
}


/*
  Do allocation through alloca if there is enough stack available.
  If not, use my_malloc() instead.

  The idea is that to be able to alloc as much as possible through the
  stack.  To ensure this, we have two different limits, on for big
  blocks and one for small blocks.  This will enable us to continue to
  do allocation for small blocks even when there is less stack space
  available.
  This is for example used by Aria when traversing the b-tree and the code
  needs to allocate one b-tree page and a few keys for each recursion. Even
  if there is not space to allocate the b-tree pages on stack we can still
  continue to allocate the keys.
*/

/*
  Default suggested allocations
*/

/* Allocate big blocks as long as there is this much left */
#define STACK_ALLOC_BIG_BLOCK 1024*64

/* Allocate small blocks as long as there is this much left */
#define STACK_ALLOC_SMALL_BLOCK 1024*32

/* Allocate small blocks as long as the block size is not bigger than */
#define STACK_ALLOC_SMALL_BLOCK_SIZE 4096

/*
  Allocate a block on stack or through malloc.
  The 'must_be_freed' variable will be set to 1 if malloc was called.
  'must_be_freed' must be a variable on the stack!
*/

#ifdef HAVE_ALLOCA
#define alloc_on_stack(stack_end, res, must_be_freed, size)            \
do                                                                     \
{                                                                      \
  size_t alloc_size= (size);                                           \
  void *stack= my_get_stack_pointer(0);                                \
  size_t stack_left= available_stack_size(stack, (stack_end));         \
  if (stack_left > alloc_size + STACK_ALLOC_SMALL_BLOCK &&             \
      (stack_left > alloc_size + STACK_ALLOC_BIG_BLOCK ||              \
       (STACK_ALLOC_SMALL_BLOCK_SIZE >= alloc_size)))                  \
  {                                                                    \
    (must_be_freed)= 0;                                                \
    (res)= alloca(size);                                               \
  }                                                                    \
  else                                                                 \
  {                                                                    \
    (must_be_freed)= 1;                                                \
    (res)= my_malloc(PSI_INSTRUMENT_ME, size, MYF(MY_THREAD_SPECIFIC | MY_WME));          \
  }                                                                    \
} while(0)
#else
#define alloc_on_stack(stack_end, res, must_be_freed, size)            \
  do {                                                                 \
    (must_be_freed)= 1;                                                \
    (res)= my_malloc(PSI_INSTRUMENT_ME, size, MYF(MY_THREAD_SPECIFIC | MY_WME));          \
  } while(0)
#endif /* HAVE_ALLOCA */


/*
  Free memory allocated by stack_alloc
*/

static inline void stack_alloc_free(void *res, my_bool must_be_freed)
{
  if (must_be_freed)
    my_free(res);
}
#endif /* _my_stack_alloc_h */


/* Get start and end of stack */

/*
  This is used in the case when we not know the exact stack start
  and have to estimate stack start with get_stack_pointer()
*/
#define MY_STACK_SAFE_MARGIN 8192

extern void my_get_stack_bounds(void **stack_start, void **stack_end,
                                void *fallback_stack_start,
                                size_t fallback_stack_size);
