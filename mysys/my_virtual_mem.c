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
#ifdef _AIX
# include <sys/shm.h>
#endif

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
  DWORD flags= my_use_large_pages
    ? MEM_LARGE_PAGES | MEM_RESERVE | MEM_COMMIT
    : MEM_RESERVE;
  char *ptr= VirtualAlloc(NULL, *size, flags, PAGE_READWRITE);
  if (!ptr && (flags & MEM_LARGE_PAGES))
  {
    /* Try without large pages */
    ptr= VirtualAlloc(NULL, *size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!ptr)
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_ERROR_LOG), *size);
  }
  return ptr;
#else
  return my_large_mmap(size, PROT_NONE);
#endif
}

#if defined _WIN32 && !defined DBUG_OFF
static my_bool is_memory_committed(char *ptr, size_t size)
{
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(ptr, &mbi, sizeof mbi) == 0)
    DBUG_ASSERT(0);
  return !!(mbi.State & MEM_COMMIT);
}
#endif

char *my_virtual_mem_commit(char *ptr, size_t size)
{
  DBUG_ASSERT(ptr);
#ifdef _WIN32
  if (my_use_large_pages)
  {
    DBUG_ASSERT(is_memory_committed(ptr, size));
  }
  else
  {
    void *p= VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE);
    DBUG_ASSERT(p == ptr);
    if (!p)
    {
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_ERROR_LOG), size);
      return NULL;
    }
  }
#else
  if (my_use_large_pages)
    /* my_large_mmap() already created a read/write mapping. */;
  else
  {
# ifdef _AIX
    /*
      MAP_FIXED does not not work on IBM AIX in the way does works elsewhere.
      Apparently, it is not possible to mmap(2) a range that is already in use,
      at least not by default.

      mprotect(2) is the fallback, it can't communicate out-of-memory
      conditions, but it looks like overcommitting is not possible on
      AIX anyway.
    */
    if (mprotect(ptr, size, PROT_READ | PROT_WRITE))
    {
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_ERROR_LOG), size);
      return NULL;
    }
# else
    void *p= 0;
    const int flags=
#  ifdef MAP_POPULATE
      MAP_POPULATE |
#  endif
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
    p= mmap(ptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED)
    {
      my_error(EE_OUTOFMEMORY, MYF(ME_BELL + ME_ERROR_LOG), size);
      return NULL;
    }
    DBUG_ASSERT(p == ptr);
#  if defined MADV_FREE_REUSABLE && defined MADV_FREE_REUSE /* Apple macOS */
    madvise(ptr, size, MADV_FREE_REUSE); /* cancel MADV_FREE_REUSABLE */
#  endif
# endif
  }
#endif
  update_malloc_size(size, 0);
  return ptr;
}

void my_virtual_mem_decommit(char *ptr, size_t size)
{
#ifdef _WIN32
  DBUG_ASSERT(is_memory_committed(ptr, size));
# ifndef HAVE_UNACCESSIBLE_AFTER_MEM_DECOMMIT
#  error "VirtualFree(MEM_DECOMMIT) will not allow subsequent reads!"
# endif
  if (!my_use_large_pages)
  {
    if (!VirtualFree(ptr, size, MEM_DECOMMIT))
    {
      my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size,
               GetLastError());
      DBUG_ASSERT(0);
    }
  }
#else
  const int prot=
# ifndef HAVE_UNACCESSIBLE_AFTER_MEM_DECOMMIT
    /*
      In InnoDB, buf_pool_t::page_guess() may deference pointers to
      this, assuming that either the original contents or zeroed
      contents is available.
    */
    PROT_READ
# else
    /* We will explicitly mark the memory unaccessible. */
    PROT_NONE
# endif
    ;
# ifdef _AIX
  disclaim(ptr, size, DISCLAIM_ZEROMEM);
# elif defined __linux__ || defined __osf__
  madvise(ptr, size, MADV_DONTNEED); /* OSF/1, Linux mimicing AIX disclaim() */
# elif defined MADV_FREE_REUSABLE && defined MADV_FREE_REUSE
  /* Mac OS X 10.9; undocumented in Apple macOS */
  madvise(ptr, size, MADV_FREE_REUSABLE); /* macOS mimicing AIX disclaim() */
# elif defined MADV_PURGE /* Illumos */
  madvise(ptr, size, MADV_PURGE); /* Illumos mimicing AIX disclaim() */
# elif defined MADV_FREE
  /* FreeBSD, NetBSD, OpenBSD, Dragonfly BSD, OpenSolaris, Apple macOS */
  madvise(ptr, size, MADV_FREE); /* allow lazy zeroing out */
# elif defined MADV_DONTNEED
#  warning "It is unclear if madvise(MADV_DONTNEED) works as intended"
  madvise(ptr, size, MADV_DONTNEED);
# else
#  warning "Do not know how to decommit memory"
# endif
  if (mprotect(ptr, size, prot))
  {
    my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size, errno);
    DBUG_ASSERT(0);
  }
#endif
  update_malloc_size(-(longlong) size, 0);
}

void my_virtual_mem_release(char *ptr, size_t size)
{
#ifdef _WIN32
  DBUG_ASSERT(my_use_large_pages || !is_memory_committed(ptr, size));
  if (!VirtualFree(ptr, 0, MEM_RELEASE))
  {
    my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size,
             GetLastError());
    DBUG_ASSERT(0);
  }
#else
  if (munmap(ptr, size))
  {
    my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size, errno);
    DBUG_ASSERT(0);
  }
#endif
}
