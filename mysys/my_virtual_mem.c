/* Copyright (c) 2025, MariaDB

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

#include <my_global.h>
#include <my_sys.h>
#include <mysys_err.h>
#include <my_virtual_mem.h>

/*
  Functionality for handling virtual memory

  - reserve range,
  - commit memory (within reserved range)
  - decommit previously commited memory
  - release range

  Not every OS has a "reserve" functionality, i.e it is not always
  possible to reserve memory larger than swap or RAM for example.

  We try to respect use_large_pages setting, on Windows and Linux
*/
#ifndef _WIN32
char *my_large_mmap(size_t *size, int prot);
#endif

char *my_virtual_mem_reserve(size_t *size)
{
#ifdef _WIN32
  size_t aligned_size= *size;
  if (my_use_large_pages)
    aligned_size= MY_ALIGN(aligned_size, GetLargePageMinimum());
  char* ptr= VirtualAlloc(NULL, aligned_size, MEM_RESERVE, PAGE_READWRITE);
  if (!ptr)
    my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_ERROR_LOG), aligned_size);
  else
    *size= aligned_size;
  return ptr;
#else
  return my_large_mmap(size, PROT_NONE);
#endif
}

#if defined _WIN32 && !defined DBUG_OFF
static my_bool is_memory_committed(char *ptr, size_t size)
{
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
    DBUG_ASSERT(0);

  if (mbi.State & MEM_COMMIT)
    return TRUE;
  else
    return FALSE;
}
#endif

char *my_virtual_mem_commit(char *ptr, size_t size)
{
#ifdef _WIN32
  void *p= NULL;
  DBUG_ASSERT(!is_memory_committed(ptr, size));

  if (my_use_large_pages)
    p= VirtualAlloc(ptr, size, MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);

  if (!p)
  {
    if (!VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE))
    {
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_ERROR_LOG), size);
      return NULL;
    }
  }
#else
  void *p= 0;
  const int flags=
# ifdef __linux__
    MAP_POPULATE |
# endif
    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
# ifdef __linux__
  if (my_use_large_pages)
    return ptr;
# endif
  p= mmap(ptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (p == MAP_FAILED)
  {
    my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_ERROR_LOG), size);
    return NULL;
  }
#endif
  update_malloc_size(size, 0);
  return ptr;
}


void my_virtual_mem_decommit(char *ptr, size_t size)
{
#ifdef _WIN32
  DBUG_ASSERT(is_memory_committed(ptr, size));

  if (!VirtualFree(ptr, size, MEM_DECOMMIT))
  {
    DBUG_ASSERT(0);
    my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size,
             GetLastError());
    return;
  }
#else
# ifdef __linux__
  if (my_use_large_pages)
    return;
# endif
# ifdef __linux__
  /* In InnoDB, buf_pool_t::page_guess() may deference a pointer to this area,
  and according to the documentation of madvise(2) it should read zeroes. */
  madvise(ptr, size, MADV_DONTNEED);
  mprotect(ptr, size, PROT_READ);
# else
  /* On other systems than Linux, the memory area after madvise(MADV_DONTNEED)
  is not documented to be accessible. So, buf_pool_t::page_guess()
  must validate the pointer. */
  madvise(ptr, size, MADV_DONTNEED);
  mprotect(ptr, size, PROT_NONE);
# endif
#endif
  update_malloc_size(-(longlong) size, 0);
}


void my_virtual_mem_release(char *ptr, size_t size)
{
#ifdef _WIN32
  DBUG_ASSERT(!is_memory_committed(ptr, size));
  if (!VirtualFree(ptr, 0, MEM_RELEASE))
  {
    DBUG_ASSERT(0);
    my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size,
             GetLastError());
  }
#else
  if (munmap(ptr, size))
  {
    //DBUG_ASSERT(0);
    my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size, errno);
  }
#endif
}
