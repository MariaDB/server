/*
   Copyright (c) 2000, 2013, Oracle and/or its affiliates
   Copyright (c) 2009, 2014, SkySQL Ab

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

#include "mysys_priv.h"
#include "mysys_err.h"
#include <m_string.h>

struct my_memory_header
{
  PSI_thread *m_owner;
  size_t m_size;
  PSI_memory_key m_key;
};
typedef struct my_memory_header my_memory_header;
#define HEADER_SIZE 24

#define USER_TO_HEADER(P) ((my_memory_header*)((char *)(P) - HEADER_SIZE))
#define HEADER_TO_USER(P) ((char*)(P) + HEADER_SIZE)

/**
  Inform application that memory usage has changed

  @param size	Size of memory segment allocated or freed
  @param flag   1 if thread specific (allocated by MY_THREAD_SPECIFIC),
                0 if system specific.

  The type os size is long long, to be able to handle negative numbers to
  decrement the memory usage

  @return 0 - ok
          1 - failure, abort the allocation
*/
static void dummy(long long size __attribute__((unused)),
                  my_bool is_thread_specific __attribute__((unused)))
{}

static MALLOC_SIZE_CB update_malloc_size= dummy;

void set_malloc_size_cb(MALLOC_SIZE_CB func)
{
  update_malloc_size= func ? func : dummy;
}
    
    
/**
  Allocate a sized block of memory.

  @param size   The size of the memory block in bytes.
  @param flags  Failure action modifiers (bitmasks).

  @return A pointer to the allocated memory block, or NULL on failure.
*/
void *my_malloc(PSI_memory_key key, size_t size, myf my_flags)
{
  my_memory_header *mh;
  void *point;
  DBUG_ENTER("my_malloc");
  DBUG_PRINT("my",("size: %zu flags: %lu", size, my_flags));
  compile_time_assert(sizeof(my_memory_header) <= HEADER_SIZE);

  if (!(my_flags & (MY_WME | MY_FAE)))
    my_flags|= my_global_flags;

  /* Safety */
  if (!size)
    size=1;
  if (size > SIZE_T_MAX - 1024L*1024L*16L)           /* Wrong call */
    DBUG_RETURN(0);

  /* We have to align size as we store MY_THREAD_SPECIFIC flag in the LSB */
  size= ALIGN_SIZE(size);

  if (DBUG_IF("simulate_out_of_memory"))
    mh= NULL;
  else
    mh= (my_memory_header*) sf_malloc(size + HEADER_SIZE, my_flags);

  if (mh == NULL)
  {
    my_errno=errno;
    if (my_flags & MY_FAE)
      error_handler_hook=fatal_error_handler_hook;
    if (my_flags & (MY_FAE+MY_WME))
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL+ME_ERROR_LOG+ME_FATAL),size);
    if (my_flags & MY_FAE)
      abort();
    point= NULL;
  }
  else
  {
    int flag= MY_TEST(my_flags & MY_THREAD_SPECIFIC);
    mh->m_size= size | flag;
    mh->m_key= PSI_CALL_memory_alloc(key, size, & mh->m_owner);
    update_malloc_size(size + HEADER_SIZE, flag);
    point= HEADER_TO_USER(mh);
    if (my_flags & MY_ZEROFILL)
      bzero(point, size);
    else
      TRASH_ALLOC(point, size);
  }
  DBUG_PRINT("exit",("ptr: %p", point));
  DBUG_RETURN(point);
}


/**
   @brief wrapper around realloc()

   @param  old_point        pointer to currently allocated area
   @param  size            new size requested, must be >0
   @param  my_flags        flags

   @note if size==0 realloc() may return NULL; my_realloc() treats this as an
   error which is not the intention of realloc()
*/
void *my_realloc(PSI_memory_key key, void *old_point, size_t size, myf my_flags)
{
  my_memory_header *old_mh, *mh;
  void *point;
  size_t old_size;
  my_bool old_flags;
  DBUG_ENTER("my_realloc");
  DBUG_PRINT("my",("ptr: %p size: %zu flags: %lu", old_point, size, my_flags));

  DBUG_ASSERT(size > 0);
  if (!old_point && (my_flags & MY_ALLOW_ZERO_PTR))
    DBUG_RETURN(my_malloc(key, size, my_flags));

  old_mh= USER_TO_HEADER(old_point);
  old_size= old_mh->m_size & ~1;
  old_flags= old_mh->m_size & 1;

  DBUG_ASSERT(old_mh->m_key == key || old_mh->m_key == PSI_NOT_INSTRUMENTED);
  DBUG_ASSERT(old_flags == MY_TEST(my_flags & MY_THREAD_SPECIFIC));

  size= ALIGN_SIZE(size);
  mh= sf_realloc(old_mh, size + HEADER_SIZE, my_flags);

  if (mh == NULL)
  {
    if (size < old_size)
      DBUG_RETURN(old_point);
    my_errno=errno;
    if (my_flags & MY_FREE_ON_ERROR)
    {
      /* my_free will take care of size accounting */
      my_free(old_point);
      old_point= 0;
    }
    if (my_flags & (MY_FAE+MY_WME))
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_FATAL), size);
    point= NULL;
  }
  else
  {
    mh->m_size= size | old_flags;
    mh->m_key= PSI_CALL_memory_realloc(key, old_size, size, & mh->m_owner);
    update_malloc_size((longlong)size - (longlong)old_size, old_flags);
    point= HEADER_TO_USER(mh);
  }

  DBUG_PRINT("exit",("ptr: %p", point));
  DBUG_RETURN(point);
}


/**
  Free memory allocated with my_malloc.

  @param ptr Pointer to the memory allocated by my_malloc.
*/
void my_free(void *ptr)
{
  my_memory_header *mh;
  size_t old_size;
  my_bool old_flags;
  DBUG_ENTER("my_free");
  DBUG_PRINT("my",("ptr: %p", ptr));

  if (ptr == NULL)
    DBUG_VOID_RETURN;

  mh= USER_TO_HEADER(ptr);
  old_size= mh->m_size & ~1;
  old_flags= mh->m_size & 1;
  PSI_CALL_memory_free(mh->m_key, old_size, mh->m_owner);

  update_malloc_size(- (longlong) old_size - HEADER_SIZE, old_flags);

#ifndef SAFEMALLOC
  /*
    Trash memory if not safemalloc. We don't have to do this if safemalloc
    is used as safemalloc will also do trashing
  */
  TRASH_FREE(ptr, old_size);
#endif
  sf_free(mh);
  DBUG_VOID_RETURN;
}


void *my_memdup(PSI_memory_key key, const void *from, size_t length, myf my_flags)
{
  void *ptr;
  DBUG_ENTER("my_memdup");

  if ((ptr= my_malloc(key, length,my_flags)) != 0)
    memcpy(ptr, from, length);
  DBUG_RETURN(ptr);
}


char *my_strdup(PSI_memory_key key, const char *from, myf my_flags)
{
  char *ptr;
  size_t length= strlen(from)+1;
  DBUG_ENTER("my_strdup");

  if ((ptr= (char*) my_malloc(key, length, my_flags)))
    memcpy(ptr, from, length);
  DBUG_RETURN(ptr);
}


char *my_strndup(PSI_memory_key key, const char *from, size_t length, myf my_flags)
{
  char *ptr;
  DBUG_ENTER("my_strndup");

  if ((ptr= (char*) my_malloc(key, length+1, my_flags)))
  {
    memcpy(ptr, from, length);
    ptr[length]= 0;
  }
  DBUG_RETURN(ptr);
}

