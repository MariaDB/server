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

/* Allocate small blocks as long as there is this much left */
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
  size_t stack_left= available_stack_size(&alloc_size, (stack_end));   \
  if (stack_left > alloc_size &&                                       \
      (STACK_ALLOC_BIG_BLOCK < stack_left - alloc_size ||              \
       ((STACK_ALLOC_SMALL_BLOCK < stack_left - alloc_size) &&         \
        (STACK_ALLOC_SMALL_BLOCK_SIZE <= alloc_size))))                \
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
