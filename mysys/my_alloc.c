/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates
   Copyright (c) 2010, 2020, MariaDB

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

/* Routines to handle mallocing of results which will be freed the same time */

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <my_bit.h>
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#undef EXTRA_DEBUG
#define EXTRA_DEBUG

/* data packed in MEM_ROOT -> min_malloc */

/* Don't allocate too small blocks */
#define ROOT_MIN_BLOCK_SIZE 256

/* bits in MEM_ROOT->flags */
#define ROOT_FLAG_THREAD_SPECIFIC 1
#define ROOT_FLAG_MPROTECT        2

#define MALLOC_FLAG(R) MYF((R)->flags & ROOT_FLAG_THREAD_SPECIFIC ? THREAD_SPECIFIC : 0)

#define TRASH_MEM(X) TRASH_FREE(((char*)(X) + ((X)->size-(X)->left)), (X)->left)


/*
  Alloc memory through either my_malloc or mmap()
*/

static void *root_alloc(MEM_ROOT *root, size_t size, size_t *alloced_size,
			myf my_flags)
{
  *alloced_size= size;
#if defined(HAVE_MMAP) && defined(HAVE_MPROTECT) && defined(MAP_ANONYMOUS)
  if (root->flags & ROOT_FLAG_MPROTECT)
  {
    void *res;
    *alloced_size= MY_ALIGN(size, my_system_page_size);
    res= my_mmap(0, *alloced_size, PROT_READ | PROT_WRITE,
                 MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED)
      res= 0;
    return res;
  }
#endif /* HAVE_MMAP */

  return my_malloc(root->psi_key, size,
		   my_flags | MYF(root->flags & ROOT_FLAG_THREAD_SPECIFIC ?
				  MY_THREAD_SPECIFIC : 0));
}

static void root_free(MEM_ROOT *root, void *ptr, size_t size)
{
#if defined(HAVE_MMAP) && defined(HAVE_MPROTECT) && defined(MAP_ANONYMOUS)
  if (root->flags & ROOT_FLAG_MPROTECT)
    my_munmap(ptr, size);
  else
#endif
    my_free(ptr);
}


/*
  Calculate block sizes to use

  Sizes will be updated to next power of 2, minus operating system
  memory management size.

  The idea is to reduce memory fragmentation as most system memory
  allocators are using power of 2 block size internally.
*/

static void calculate_block_sizes(MEM_ROOT *mem_root, size_t block_size,
                                  size_t *pre_alloc_size)
{
  size_t pre_alloc= *pre_alloc_size;

  if (mem_root->flags&= ROOT_FLAG_MPROTECT)
  {
    mem_root->block_size= MY_ALIGN(block_size, my_system_page_size);
    if (pre_alloc)
      pre_alloc= MY_ALIGN(pre_alloc, my_system_page_size);
  }
  else
  {
    DBUG_ASSERT(block_size <= UINT_MAX32);
    mem_root->block_size= (my_round_up_to_next_power((uint32) block_size -
                                                     MALLOC_OVERHEAD)-
                           MALLOC_OVERHEAD);
    if (pre_alloc)
      pre_alloc= (my_round_up_to_next_power((uint32) pre_alloc -
                                            MALLOC_OVERHEAD)-
                  MALLOC_OVERHEAD);
  }
  *pre_alloc_size= pre_alloc;
}


/*
  Initialize memory root

  SYNOPSIS
    init_alloc_root()
      mem_root       - memory root to initialize
      name           - name of memroot (for debugging)
      block_size     - size of chunks (blocks) used for memory allocation.
                       Will be updated to next power of 2, minus
                       internal and system memory management size.  This is
                       will reduce memory fragmentation as most system memory
                       allocators are using power of 2 block size internally.
                       (It is external size of chunk i.e. it should include
                        memory required for internal structures, thus it
                        should be no less than ROOT_MIN_BLOCK_SIZE).
      pre_alloc_size - if non-0, then size of block that should be
                       pre-allocated during memory root initialization.
      my_flags	       MY_THREAD_SPECIFIC flag for my_malloc
                       MY_RROOT_USE_MPROTECT for read only protected memory

  DESCRIPTION
    This function prepares memory root for further use, sets initial size of
    chunk for memory allocation and pre-allocates first block if specified.
    Although error can happen during execution of this function if
    pre_alloc_size is non-0 it won't be reported. Instead it will be
    reported as error in first alloc_root() on this memory root.
*/

void init_alloc_root(PSI_memory_key key, MEM_ROOT *mem_root, size_t block_size,
		     size_t pre_alloc_size __attribute__((unused)),
                     myf my_flags)
{
  DBUG_ENTER("init_alloc_root");
  DBUG_PRINT("enter",("root: %p  prealloc: %zu", mem_root, pre_alloc_size));

  mem_root->free= mem_root->used= mem_root->pre_alloc= 0;
  mem_root->min_malloc= 32 + REDZONE_SIZE;
  mem_root->block_size= MY_MAX(block_size, ROOT_MIN_BLOCK_SIZE);
  mem_root->flags= 0;
  if (my_flags & MY_THREAD_SPECIFIC)
    mem_root->flags|= ROOT_FLAG_THREAD_SPECIFIC;
  if (my_flags & MY_ROOT_USE_MPROTECT)
    mem_root->flags|= ROOT_FLAG_MPROTECT;

  calculate_block_sizes(mem_root, block_size, &pre_alloc_size);

  mem_root->error_handler= 0;
  mem_root->block_num= 4;			/* We shift this with >>2 */
  mem_root->first_block_usage= 0;
  mem_root->psi_key= key;

#if !(defined(HAVE_valgrind) && defined(EXTRA_DEBUG))
  if (pre_alloc_size)
  {
    size_t alloced_size;
    if ((mem_root->free= mem_root->pre_alloc=
         (USED_MEM*) root_alloc(mem_root, pre_alloc_size, &alloced_size,
                                MYF(0))))
    {
      mem_root->free->size= alloced_size;
      mem_root->free->left= alloced_size - ALIGN_SIZE(sizeof(USED_MEM));
      mem_root->free->next= 0;
      TRASH_MEM(mem_root->free);
    }
  }
#endif
  DBUG_VOID_RETURN;
}

/*
  SYNOPSIS
    reset_root_defaults()
    mem_root        memory root to change defaults of
    block_size      new value of block size. Must be greater or equal
                    than ALLOC_ROOT_MIN_BLOCK_SIZE (this value is about
                    68 bytes and depends on platform and compilation flags)
    pre_alloc_size  new size of preallocated block. If not zero,
                    must be equal to or greater than block size,
                    otherwise means 'no prealloc'.
  DESCRIPTION
    Function aligns and assigns new value to block size; then it tries to
    reuse one of existing blocks as prealloc block, or malloc new one of
    requested size. If no blocks can be reused, all unused blocks are freed
    before allocation.
*/

void reset_root_defaults(MEM_ROOT *mem_root, size_t block_size,
                         size_t pre_alloc_size __attribute__((unused)))
{
  DBUG_ENTER("reset_root_defaults");
  DBUG_ASSERT(alloc_root_inited(mem_root));

  calculate_block_sizes(mem_root, block_size, &pre_alloc_size);

#if !(defined(HAVE_valgrind) && defined(EXTRA_DEBUG))
  if (pre_alloc_size)
  {
    size_t size= mem_root->block_size, alloced_size;
    if (!mem_root->pre_alloc ||
        mem_root->pre_alloc->size != mem_root->block_size)
    {
      USED_MEM *mem, **prev= &mem_root->free;
      /*
        Free unused blocks, so that consequent calls
        to reset_root_defaults won't eat away memory.
      */
      while (*prev)
      {
        mem= *prev;
        if (mem->size == size)
        {
          /* We found a suitable block, no need to do anything else */
          mem_root->pre_alloc= mem;
          DBUG_VOID_RETURN;
        }
        if (mem->left + ALIGN_SIZE(sizeof(USED_MEM)) == mem->size)
        {
          /* remove block from the list and free it */
          *prev= mem->next;
          root_free(mem_root, mem, mem->size);
        }
        else
          prev= &mem->next;
      }
      /* Allocate new prealloc block and add it to the end of free list */
      if ((mem= (USED_MEM *) root_alloc(mem_root, size, &alloced_size,
                                        MYF(MY_WME))))
      {
        mem->size= alloced_size;
        mem->left= alloced_size - ALIGN_SIZE(sizeof(USED_MEM));
        mem->next= *prev;
        *prev= mem_root->pre_alloc= mem;
        TRASH_MEM(mem);
      }
      else
        mem_root->pre_alloc= 0;
    }
  }
  else
#endif
    mem_root->pre_alloc= 0;

  DBUG_VOID_RETURN;
}


void *alloc_root(MEM_ROOT *mem_root, size_t length)
{
  size_t get_size, block_size;
  uchar* point;
  reg1 USED_MEM *next= 0;
  reg2 USED_MEM **prev;
  size_t original_length __attribute__((unused)) = length;
  DBUG_ENTER("alloc_root");
  DBUG_PRINT("enter",("root: %p", mem_root));
  DBUG_ASSERT(alloc_root_inited(mem_root));

  DBUG_EXECUTE_IF("simulate_out_of_memory",
		  {
		    if (mem_root->error_handler)
		      (*mem_root->error_handler)();
		    DBUG_SET("-d,simulate_out_of_memory");
		    DBUG_RETURN((void*) 0); /* purecov: inspected */
		  });

#if defined(HAVE_valgrind) && defined(EXTRA_DEBUG)
  if (!(mem_root->flags & ROOT_FLAG_MPROTECT))
  {
    length+= ALIGN_SIZE(sizeof(USED_MEM));
    if (!(next = (USED_MEM*) my_malloc(mem_root->psi_key, length,
				       MYF(MY_WME | ME_FATAL |
					   (mem_root->flags &
                                            ROOT_FLAG_THREAD_SPECIFIC ?
                                           MY_THREAD_SPECIFIC : 0)))))
    {
      if (mem_root->error_handler)
	(*mem_root->error_handler)();
      DBUG_RETURN((uchar*) 0);			/* purecov: inspected */
    }
    next->next= mem_root->used;
    next->left= 0;
    next->size= length;
    mem_root->used= next;
    DBUG_PRINT("exit",("ptr: %p", (((char*)next)+ALIGN_SIZE(sizeof(USED_MEM)))));
    DBUG_RETURN((((uchar*) next)+ALIGN_SIZE(sizeof(USED_MEM))));
  }
#endif /* defined(HAVE_valgrind) && defined(EXTRA_DEBUG) */

  length= ALIGN_SIZE(length) + REDZONE_SIZE;
  if ((*(prev= &mem_root->free)) != NULL)
  {
    if ((*prev)->left < length &&
	mem_root->first_block_usage++ >= ALLOC_MAX_BLOCK_USAGE_BEFORE_DROP &&
	(*prev)->left < ALLOC_MAX_BLOCK_TO_DROP)
    {
      next= *prev;
      *prev= next->next;			/* Remove block from list */
      next->next= mem_root->used;
      mem_root->used= next;
      mem_root->first_block_usage= 0;
    }
    for (next= *prev ; next && next->left < length ; next= next->next)
      prev= &next->next;
  }
  if (! next)
  {						/* Time to alloc new block */
    size_t alloced_length;

    /* Increase block size over time if there is a lot of mallocs */
    block_size= (MY_ALIGN(mem_root->block_size, ROOT_MIN_BLOCK_SIZE) *
                 (mem_root->block_num >> 2)- MALLOC_OVERHEAD);
    get_size= length + ALIGN_SIZE(sizeof(USED_MEM));
    get_size= MY_MAX(get_size, block_size);

    if (!(next= (USED_MEM*) root_alloc(mem_root, get_size, &alloced_length,
                                       MYF(MY_WME | ME_FATAL))))
    {
      if (mem_root->error_handler)
	(*mem_root->error_handler)();
      DBUG_RETURN((void*) 0);                      /* purecov: inspected */
    }
    mem_root->block_num++;
    next->next= *prev;
    next->size= alloced_length;
    next->left= alloced_length - ALIGN_SIZE(sizeof(USED_MEM));
    *prev=next;
    TRASH_MEM(next);
  }

  point= (uchar*) ((char*) next+ (next->size-next->left));
  /*TODO: next part may be unneded due to mem_root->first_block_usage counter*/
  if ((next->left-= length) < mem_root->min_malloc)
  {						/* Full block */
    *prev= next->next;				/* Remove block from list */
    next->next= mem_root->used;
    mem_root->used= next;
    mem_root->first_block_usage= 0;
  }
  point+= REDZONE_SIZE;
  TRASH_ALLOC(point, original_length);
  DBUG_PRINT("exit",("ptr: %p", point));
  DBUG_RETURN((void*) point);
}


/*
  Allocate many pointers at the same time.

  DESCRIPTION
    ptr1, ptr2, etc all point into big allocated memory area.

  SYNOPSIS
    multi_alloc_root()
      root               Memory root
      ptr1, length1      Multiple arguments terminated by a NULL pointer
      ptr2, length2      ...
      ...
      NULL

  RETURN VALUE
    A pointer to the beginning of the allocated memory block
    in case of success or NULL if out of memory.
*/

void *multi_alloc_root(MEM_ROOT *root, ...)
{
  va_list args;
  char **ptr, *start, *res;
  size_t tot_length, length;
  DBUG_ENTER("multi_alloc_root");
  /*
    We  don't need to do DBUG_PRINT here as it will be done when alloc_root
    is called
  */

  va_start(args, root);
  tot_length= 0;
  while ((ptr= va_arg(args, char **)))
  {
    length= va_arg(args, uint);
    tot_length+= ALIGN_SIZE(length);
  }
  va_end(args);

  if (!(start= (char*) alloc_root(root, tot_length)))
    DBUG_RETURN(0);                            /* purecov: inspected */

  va_start(args, root);
  res= start;
  while ((ptr= va_arg(args, char **)))
  {
    *ptr= res;
    length= va_arg(args, uint);
    res+= ALIGN_SIZE(length);
  }
  va_end(args);
  DBUG_RETURN((void*) start);
}


#if !(defined(HAVE_valgrind) && defined(EXTRA_DEBUG))
/** Mark all data in blocks free for reusage */

static inline void mark_blocks_free(MEM_ROOT* root)
{
  reg1 USED_MEM *next;
  reg2 USED_MEM **last;

  /* iterate through (partially) free blocks, mark them free */
  last= &root->free;
  for (next= root->free; next; next= *(last= &next->next))
  {
    next->left= next->size - ALIGN_SIZE(sizeof(USED_MEM));
    TRASH_MEM(next);
  }

  /* Combine the free and the used list */
  *last= next=root->used;

  /* now go through the used blocks and mark them free */
  for (; next; next= next->next)
  {
    next->left= next->size - ALIGN_SIZE(sizeof(USED_MEM));
    TRASH_MEM(next);
  }

  /* Now everything is set; Indicate that nothing is used anymore */
  root->used= 0;
  root->first_block_usage= 0;
  root->block_num= 4;
}
#endif


/*
  Deallocate everything used by alloc_root or just move
  used blocks to free list if called with MY_USED_TO_FREE

  SYNOPSIS
    free_root()
      root		Memory root
      MyFlags		Flags for what should be freed:

        MY_MARK_BLOCKS_FREED	Don't free blocks, just mark them free
        MY_KEEP_PREALLOC	If this is not set, then free also the
        		        preallocated block

  NOTES
    One can call this function either with root block initialised with
    init_alloc_root() or with a bzero()-ed block.
    It's also safe to call this multiple times with the same mem_root.
*/

void free_root(MEM_ROOT *root, myf MyFlags)
{
  reg1 USED_MEM *next,*old;
  DBUG_ENTER("free_root");
  DBUG_PRINT("enter",("root: %p  flags: %lu", root, MyFlags));

#if !(defined(HAVE_valgrind) && defined(EXTRA_DEBUG))
  /*
    There is no point in using mark_blocks_free when using valgrind as
    it will not reclaim any memory
  */
  if (MyFlags & MY_MARK_BLOCKS_FREE)
  {
    mark_blocks_free(root);
    DBUG_VOID_RETURN;
  }
#endif
  if (!(MyFlags & MY_KEEP_PREALLOC))
    root->pre_alloc=0;

  for (next=root->used; next ;)
  {
    old=next; next= next->next ;
    if (old != root->pre_alloc)
      root_free(root, old, old->size);
  }
  for (next=root->free ; next ;)
  {
    old=next; next= next->next;
    if (old != root->pre_alloc)
      root_free(root, old, old->size);
  }
  root->used=root->free=0;
  if (root->pre_alloc)
  {
    root->free=root->pre_alloc;
    root->free->left=root->pre_alloc->size-ALIGN_SIZE(sizeof(USED_MEM));
    TRASH_MEM(root->pre_alloc);
    root->free->next=0;
  }
  root->block_num= 4;
  root->first_block_usage= 0;
  DBUG_VOID_RETURN;
}


/*
  Find block that contains an object and set the pre_alloc to it
*/

void set_prealloc_root(MEM_ROOT *root, char *ptr)
{
  USED_MEM *next;
  for (next=root->used; next ; next=next->next)
  {
    if ((char*) next <= ptr && (char*) next + next->size > ptr)
    {
      root->pre_alloc=next;
      return;
    }
  }
  for (next=root->free ; next ; next=next->next)
  {
    if ((char*) next <= ptr && (char*) next + next->size > ptr)
    {
      root->pre_alloc=next;
      return;
    }
  }
}


/**
   Change protection for all blocks in the mem root
*/

#if defined(HAVE_MMAP) && defined(HAVE_MPROTECT) && defined(MAP_ANONYMOUS)
void protect_root(MEM_ROOT *root, int prot)
{
  reg1 USED_MEM *next,*old;
  DBUG_ENTER("protect_root");
  DBUG_PRINT("enter",("root: %p  prot: %d", root, prot));

  DBUG_ASSERT(root->flags & ROOT_FLAG_MPROTECT);

  for (next= root->used; next ;)
  {
    old= next; next= next->next ;
    mprotect(old, old->size, prot);
  }
  for (next= root->free; next ;)
  {
    old= next; next= next->next ;
    mprotect(old, old->size, prot);
  }
  DBUG_VOID_RETURN;
}
#else
void protect_root(MEM_ROOT *root, int prot)
{
}
#endif /* defined(HAVE_MMAP) && ... */


char *strdup_root(MEM_ROOT *root, const char *str)
{
  return strmake_root(root, str, strlen(str));
}


char *strmake_root(MEM_ROOT *root, const char *str, size_t len)
{
  char *pos;
  if ((pos=alloc_root(root,len+1)))
  {
    if (len)
      memcpy(pos,str,len);
    pos[len]=0;
  }
  return pos;
}


void *memdup_root(MEM_ROOT *root, const void *str, size_t len)
{
  char *pos;
  if ((pos=alloc_root(root,len)) && len)
    memcpy(pos,str,len);
  return pos;
}

LEX_CSTRING safe_lexcstrdup_root(MEM_ROOT *root, const LEX_CSTRING str)
{
  LEX_CSTRING res;
  if (str.length)
    res.str= strmake_root(root, str.str, str.length);
  else
    res.str= (const char *)"";
  res.length= str.length;
  return res;
}
