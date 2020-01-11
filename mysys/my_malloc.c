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

/* If we have our own safemalloc (for debugging) */
#if defined(SAFEMALLOC)
#define MALLOC_SIZE_AND_FLAG(p,b) sf_malloc_usable_size(p,b)
#define MALLOC_PREFIX_SIZE 0
#define MALLOC_STORE_SIZE(a,b,c,d)
#define MALLOC_FIX_POINTER_FOR_FREE(a) a
#else
/*
 *   We use double as prefix size as this guarantees the correct
 *   alignment on all platforms and will optimize things for
 *   memcpy(), memcmp() etc.
 */
#define MALLOC_PREFIX_SIZE (sizeof(double))
#define MALLOC_SIZE(p) (*(size_t*) ((char*)(p) - MALLOC_PREFIX_SIZE))
#define MALLOC_STORE_SIZE(p, type_of_p, size, flag)      \
{\
  *(size_t*) p= (size) | (flag);    \
  (p)= (type_of_p) (((char*) (p)) + MALLOC_PREFIX_SIZE); \
} 
static inline size_t malloc_size_and_flag(void *p, my_bool *is_thread_specific)
{
  size_t size= MALLOC_SIZE(p);
  *is_thread_specific= (size & 1);
  return size & ~ (ulonglong) 1;
}
#define MALLOC_SIZE_AND_FLAG(p,b) malloc_size_and_flag(p, b);
#define MALLOC_FIX_POINTER_FOR_FREE(p) (((char*) (p)) - MALLOC_PREFIX_SIZE)
#endif /* SAFEMALLOC */


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
void *my_malloc(size_t size, myf my_flags)
{
  void* point;
  DBUG_ENTER("my_malloc");
  DBUG_PRINT("my",("size: %lu  my_flags: %lu", (ulong) size, my_flags));
  compile_time_assert(sizeof(size_t) <= sizeof(double));

  if (!(my_flags & (MY_WME | MY_FAE)))
    my_flags|= my_global_flags;

  /* Safety */
  if (!size)
    size=1;

  /* We have to align size to be able to store markers in it */
  size= ALIGN_SIZE(size);
  point= sf_malloc(size + MALLOC_PREFIX_SIZE, my_flags);

  if (point == NULL)
  {
    my_errno=errno;
    if (my_flags & MY_FAE)
      error_handler_hook=fatal_error_handler_hook;
    if (my_flags & (MY_FAE+MY_WME))
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_WAITTANG +
                                   ME_NOREFRESH + ME_FATALERROR),size);
    if (my_flags & MY_FAE)
      abort();
  }
  else
  {
    MALLOC_STORE_SIZE(point, void*, size,
                      MY_TEST(my_flags & MY_THREAD_SPECIFIC));
    update_malloc_size(size + MALLOC_PREFIX_SIZE,
                       MY_TEST(my_flags & MY_THREAD_SPECIFIC));
    TRASH_ALLOC(point, size);
    DBUG_EXECUTE_IF("simulate_out_of_memory",
                    {
                      /* my_free() handles memory accounting */
                      my_free(point);
                      point= NULL;
                    });
    if (my_flags & MY_ZEROFILL)
      bzero(point, size);
  }
  DBUG_PRINT("exit",("ptr: %p", point));
  DBUG_RETURN(point);
}


/**
   @brief wrapper around realloc()

   @param  oldpoint        pointer to currently allocated area
   @param  size            new size requested, must be >0
   @param  my_flags        flags

   @note if size==0 realloc() may return NULL; my_realloc() treats this as an
   error which is not the intention of realloc()
*/
void *my_realloc(void *oldpoint, size_t size, myf my_flags)
{
  void *point;
  size_t old_size;
  my_bool old_flags;
  DBUG_ENTER("my_realloc");
  DBUG_PRINT("my",("ptr: %p  size: %lu  my_flags: %lu", oldpoint,
                   (ulong) size, my_flags));

  DBUG_ASSERT(size > 0);
  if (!oldpoint && (my_flags & MY_ALLOW_ZERO_PTR))
    DBUG_RETURN(my_malloc(size, my_flags));

  size= ALIGN_SIZE(size);
  old_size= MALLOC_SIZE_AND_FLAG(oldpoint, &old_flags);
  /*
    Test that the new and old area are the same, if not MY_THREAD_MOVE is
    given
  */
  DBUG_ASSERT((MY_TEST(my_flags & MY_THREAD_SPECIFIC) == old_flags) ||
              (my_flags & MY_THREAD_MOVE));
  if ((point= sf_realloc(MALLOC_FIX_POINTER_FOR_FREE(oldpoint),
                         size + MALLOC_PREFIX_SIZE, my_flags)) == NULL)
  {
    if (my_flags & MY_FREE_ON_ERROR)
    {
      /* my_free will take care of size accounting */
      my_free(oldpoint);
      oldpoint= 0;
    }
    if (my_flags & MY_HOLD_ON_ERROR)
      DBUG_RETURN(oldpoint);
    my_errno=errno;
    if (my_flags & (MY_FAE+MY_WME))
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_WAITTANG + ME_FATALERROR), size);
  }
  else
  {
    MALLOC_STORE_SIZE(point, void*, size,
                      MY_TEST(my_flags & MY_THREAD_SPECIFIC));
    if (MY_TEST(my_flags & MY_THREAD_SPECIFIC) != old_flags)
    {
      /* memory moved between system and thread specific */
      update_malloc_size(-(longlong) old_size - MALLOC_PREFIX_SIZE, old_flags);
      update_malloc_size((longlong) size + MALLOC_PREFIX_SIZE,
                         MY_TEST(my_flags & MY_THREAD_SPECIFIC));
    }
    else
      update_malloc_size((longlong)size - (longlong)old_size, old_flags);
  }

  DBUG_PRINT("exit",("ptr: %p", point));
  DBUG_RETURN(point);
}


/**
  Free memory allocated with my_malloc.

  @remark Relies on free being able to handle a NULL argument.

  @param ptr Pointer to the memory allocated by my_malloc.
*/
void my_free(void *ptr)
{
  DBUG_ENTER("my_free");
  DBUG_PRINT("my",("ptr: %p", ptr));
  if (ptr)
  {
    size_t old_size;
    my_bool old_flags;
    old_size= MALLOC_SIZE_AND_FLAG(ptr, &old_flags);
    update_malloc_size(- (longlong) old_size - MALLOC_PREFIX_SIZE, old_flags);
    sf_free(MALLOC_FIX_POINTER_FOR_FREE(ptr));
  }
  DBUG_VOID_RETURN;
}


void *my_memdup(const void *from, size_t length, myf my_flags)
{
  void *ptr;
  DBUG_ENTER("my_memdup");

  if ((ptr= my_malloc(length,my_flags)) != 0)
    memcpy(ptr, from, length);
  DBUG_RETURN(ptr);
}


char *my_strdup(const char *from, myf my_flags)
{
  char *ptr;
  size_t length= strlen(from)+1;
  DBUG_ENTER("my_strdup");

  if ((ptr= (char*) my_malloc(length, my_flags)))
    memcpy(ptr, from, length);
  DBUG_RETURN(ptr);
}


char *my_strndup(const char *from, size_t length, myf my_flags)
{
  char *ptr;
  DBUG_ENTER("my_strndup");

  if ((ptr= (char*) my_malloc(length+1, my_flags)))
  {
    memcpy(ptr, from, length);
    ptr[length]= 0;
  }
  DBUG_RETURN(ptr);
}

