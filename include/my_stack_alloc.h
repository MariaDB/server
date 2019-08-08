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
  Do allocation trough alloca if there is enough stack available.
  If not, use my_malloc() instead.

  The idea is that to be able to alloc as much as possible trough the
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

struct st_stack_alloc
{
  void **stack_ends_here;                       /* Set on init */
  size_t stack_for_big_blocks;
  size_t stack_for_small_blocks;
  size_t small_block_size;
};
typedef struct st_stack_alloc STACK_ALLOC;


/*
  Initialize STACK_ALLOC structure
*/

static inline void init_stack_alloc(STACK_ALLOC *alloc,
                                    size_t stack_for_big_blocks,
                                    size_t stack_for_small_blocks,
                                    size_t small_block_size)
{
  alloc->stack_ends_here= &my_thread_var->stack_ends_here;
  alloc->stack_for_big_blocks= stack_for_big_blocks;
  alloc->stack_for_small_blocks= stack_for_small_blocks;
  alloc->small_block_size= small_block_size;
}


/*
  Allocate a block on stack or trough malloc.
  The 'must_be_freed' variable will be set to 1 if malloc was called.
  'must_be_freed' must be a variable on the stack!
*/

#ifdef HAVE_ALLOCA
#define alloc_on_stack(alloc, res, must_be_freed, size)                \
{                                                                      \
  size_t stack_left= available_stack_size(&(must_be_freed),            \
                                          *(alloc)->stack_ends_here);   \
  if (stack_left > (size_t) (size) &&                                  \
      ((alloc)->stack_for_big_blocks < stack_left - (size) ||          \
       (((alloc)->stack_for_small_blocks < stack_left - (size)) &&     \
        ((alloc)->small_block_size <= (size_t) (size)))))              \
  {                                                                    \
    (must_be_freed)= 0;                                                \
    (res)= alloca(size);                                               \
  }                                                                    \
  else                                                                 \
  {                                                                    \
    (must_be_freed)= 1;                                                \
    (res)= my_malloc(size, MYF(MY_THREAD_SPECIFIC | MY_WME));          \
  }                                                                    \
}
#else
#define alloc_on_stack(alloc, res, must_be_freed, size)                \
  {                                                                    \
    (must_be_freed)= 1;                                                \
    (res)= my_malloc(size, MYF(MY_THREAD_SPECIFIC | MY_WME));          \
  }
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
