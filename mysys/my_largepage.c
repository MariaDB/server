/* Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2019, 2020 IBM.

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
#include <mysys_err.h>

#ifdef __linux__
#include <dirent.h>
#endif
#if defined(__linux__) || defined(MAP_ALIGNED)
#include "my_bit.h"
#endif
#ifdef HAVE_LINUX_MMAN_H
#include <linux/mman.h>
#endif

#ifdef HAVE_SOLARIS_LARGE_PAGES
#if defined(__sun__) && defined(__GNUC__) && defined(__cplusplus) \
    && defined(_XOPEN_SOURCE)
/* memcntl exist within sys/mman.h, but under-defines what is need to use it */
extern int memcntl(caddr_t, size_t, int, caddr_t, int, int);
#endif /* __sun__ ... */
#endif /* HAVE_SOLARIS_LARGE_PAGES */

#if defined(_WIN32)
static size_t my_large_page_size;
#define HAVE_LARGE_PAGES
#elif defined(HAVE_MMAP)
#define HAVE_LARGE_PAGES
#endif

#ifdef HAVE_LARGE_PAGES
static my_bool my_use_large_pages= 0;
#else
#define my_use_large_pages 0
#endif

#if defined(HAVE_GETPAGESIZES) || defined(__linux__)
/* Descending sort */

static int size_t_cmp(const void *a, const void *b)
{
  const size_t ia= *(const size_t *) a;
  const size_t ib= *(const size_t *) b;
  if (ib > ia)
  {
    return 1;
  }
  else if (ib < ia)
  {
    return -1;
  }
  return 0;
}
#endif /* defined(HAVE_GETPAGESIZES) || defined(__linux__) */


#if defined(__linux__) || defined(HAVE_GETPAGESIZES)
#define my_large_page_sizes_length 8
static size_t my_large_page_sizes[my_large_page_sizes_length];
#endif

/**
  Linux-specific function to determine the sizes of large pages
*/
#ifdef __linux__
static inline my_bool my_is_2pow(size_t n) { return !((n) & ((n) - 1)); }

static void my_get_large_page_sizes(size_t sizes[my_large_page_sizes_length])
{
  DIR *dirp;
  struct dirent *r;
  int i= 0;
  DBUG_ENTER("my_get_large_page_sizes");

  dirp= opendir("/sys/kernel/mm/hugepages");
  if (dirp == NULL)
  {
    my_error(EE_DIR, MYF(ME_BELL), "/sys/kernel/mm/hugepages", errno);
  }
  else
  {
    while (i < my_large_page_sizes_length && (r= readdir(dirp)))
    {
      if (strncmp("hugepages-", r->d_name, 10) == 0)
      {
        sizes[i]= strtoull(r->d_name + 10, NULL, 10) * 1024ULL;
        if (!my_is_2pow(sizes[i]))
        {
          my_printf_error(0,
                          "non-power of 2 large page size (%zu) found,"
                          " skipping", MYF(ME_NOTE | ME_ERROR_LOG_ONLY),
                          sizes[i]);
          sizes[i]= 0;
          continue;
        }
        ++i;
      }
    }
    if (closedir(dirp))
    {
      my_error(EE_BADCLOSE, MYF(ME_BELL), "/sys/kernel/mm/hugepages", errno);
    }
    qsort(sizes, i, sizeof(size_t), size_t_cmp);
  }
  DBUG_VOID_RETURN;
}


#elif defined(HAVE_GETPAGESIZES)
static void my_get_large_page_sizes(size_t sizes[my_large_page_sizes_length])
{
  int nelem;

  nelem= getpagesizes(NULL, 0);

  assert(nelem <= my_large_page_sizes_length);
  getpagesizes(sizes, my_large_page_sizes_length);
  qsort(sizes, nelem, sizeof(size_t), size_t_cmp);
  if (nelem < my_large_page_sizes_length)
  {
    sizes[nelem]= 0;
  }
}


#elif defined(_WIN32)
#define my_large_page_sizes_length 0
#define my_get_large_page_sizes(A) do {} while(0)

#else
#define my_large_page_sizes_length 1
static size_t my_large_page_sizes[my_large_page_sizes_length];
static void my_get_large_page_sizes(size_t sizes[])
{
  sizes[0]= my_getpagesize();
}
#endif


/**
  Returns the next large page size smaller or equal to the passed in size.

  The search starts at my_large_page_sizes[*start].

  Assumes my_get_large_page_sizes(my_large_page_sizes) has been called before
  use.

  For first use, have *start=0. There is no need to increment *start.

  @param[in]     sz size to be searched for.
  @param[in,out] start ptr to int representing offset in my_large_page_sizes to
                       start from.
  *start is updated during search and can be used to search again if 0 isn't
  returned.

  @returns the next size found. *start will be incremented to the next potential
           size.
  @retval  a large page size that is valid on this system or 0 if no large page
           size possible.
*/
#if defined(HAVE_MMAP) && !defined(_WIN32)
static size_t my_next_large_page_size(size_t sz, int *start)
{
  DBUG_ENTER("my_next_large_page_size");

  while (*start < my_large_page_sizes_length && my_large_page_sizes[*start] > 0)
  {
    size_t cur= *start;
    (*start)++;
    if (my_large_page_sizes[cur] <= sz)
    {
      DBUG_RETURN(my_large_page_sizes[cur]);
    }
  }
  DBUG_RETURN(0);
}
#endif /* defined(MMAP) || !defined(_WIN32) */


int my_init_large_pages(my_bool super_large_pages)
{
#ifdef _WIN32
  if (!my_obtain_privilege(SE_LOCK_MEMORY_NAME))
  {
    my_printf_error(EE_PERM_LOCK_MEMORY,
                    "Lock Pages in memory access rights required for use with"
                    " large-pages, see https://mariadb.com/kb/en/library/"
                    "mariadb-memory-allocation/#huge-pages", MYF(MY_WME));
  }
  my_large_page_size= GetLargePageMinimum();
#endif

  my_use_large_pages= 1;
  my_get_large_page_sizes(my_large_page_sizes);

#ifndef HAVE_LARGE_PAGES
  my_printf_error(EE_OUTOFMEMORY, "No large page support on this platform",
                  MYF(MY_WME));
#endif

#ifdef HAVE_SOLARIS_LARGE_PAGES
  /*
    tell the kernel that we want to use 4/256MB page for heap storage
    and also for the stack. We use 4 MByte as default and if the
    super-large-page is set we increase it to 256 MByte. 256 MByte
    is for server installations with GBytes of RAM memory where
    the MySQL Server will have page caches and other memory regions
    measured in a number of GBytes.
    We use as big pages as possible which isn't bigger than the above
    desired page sizes.
  */
  int nelem= 0;
  size_t max_desired_page_size= (super_large_pages ? 256 : 4) * 1024 * 1024;
  size_t max_page_size= my_next_large_page_size(max_desired_page_size, &nelem);

  if (max_page_size > 0)
  {
    struct memcntl_mha mpss;

    mpss.mha_cmd= MHA_MAPSIZE_BSSBRK;
    mpss.mha_pagesize= max_page_size;
    mpss.mha_flags= 0;
    if (memcntl(NULL, 0, MC_HAT_ADVISE, (caddr_t) &mpss, 0, 0))
    {
      my_error(EE_MEMCNTL, MYF(ME_WARNING | ME_ERROR_LOG_ONLY), "MC_HAT_ADVISE",
               "MHA_MAPSIZE_BSSBRK");
    }
    mpss.mha_cmd= MHA_MAPSIZE_STACK;
    if (memcntl(NULL, 0, MC_HAT_ADVISE, (caddr_t) &mpss, 0, 0))
    {
      my_error(EE_MEMCNTL, MYF(ME_WARNING | ME_ERROR_LOG_ONLY), "MC_HAT_ADVISE",
               "MHA_MAPSIZE_STACK");
    }
  }
#endif /* HAVE_SOLARIS_LARGE_PAGES */
  return 0;
}


/**
   Large page size helper.
   This rounds down, if needed, the size parameter to the largest
   multiple of an available large page size on the system.
*/
void my_large_page_truncate(size_t *size)
{
  if (my_use_large_pages)
  {
    size_t large_page_size= 0;
#ifdef _WIN32
    large_page_size= my_large_page_size;
#elif defined(HAVE_MMAP)
    int page_i= 0;
    large_page_size= my_next_large_page_size(*size, &page_i);
#endif
    if (large_page_size > 0)
      *size-= *size % large_page_size;
  }
}


#if defined(HAVE_MMAP) && !defined(_WIN32)
/* Solaris for example has only MAP_ANON, FreeBSD has MAP_ANONYMOUS and
MAP_ANON but MAP_ANONYMOUS is marked "for compatibility" */
#if defined(MAP_ANONYMOUS)
#define OS_MAP_ANON     MAP_ANONYMOUS
#elif defined(MAP_ANON)
#define OS_MAP_ANON     MAP_ANON
#else
#error unsupported mmap - no MAP_ANON{YMOUS}
#endif
#endif /* HAVE_MMAP && !_WIN32 */

/**
  General large pages allocator.
  Tries to allocate memory from large pages pool and falls back to
  my_malloc_lock() in case of failure.
  Every implementation returns a zero filled buffer here.
*/
uchar *my_large_malloc(size_t *size, myf my_flags)
{
  uchar *ptr= NULL;

#ifdef _WIN32
  DWORD alloc_type= MEM_COMMIT | MEM_RESERVE;
  size_t orig_size= *size;
  DBUG_ENTER("my_large_malloc");

  if (my_use_large_pages)
  {
    alloc_type|= MEM_LARGE_PAGES;
    /* Align block size to my_large_page_size */
    *size= MY_ALIGN(*size, (size_t) my_large_page_size);
  }
  ptr= VirtualAlloc(NULL, *size, alloc_type, PAGE_READWRITE);
  if (!ptr)
  {
    if (my_flags & MY_WME)
    {
      if (my_use_large_pages)
      {
        my_printf_error(EE_OUTOFMEMORY,
                        "Couldn't allocate %zu bytes (MEM_LARGE_PAGES page "
                        "size %zu); Windows error %lu",
                        MYF(ME_WARNING | ME_ERROR_LOG_ONLY), *size,
                        my_large_page_size, GetLastError());
      }
      else
      {
        my_error(EE_OUTOFMEMORY, MYF(ME_BELL+ME_ERROR_LOG), *size);
      }
    }
    if (my_use_large_pages)
    {
      *size= orig_size;
      ptr= VirtualAlloc(NULL, *size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (!ptr && my_flags & MY_WME)
      {
        my_error(EE_OUTOFMEMORY, MYF(ME_BELL+ME_ERROR_LOG), *size);
      }
    }
  }
#elif defined(HAVE_MMAP)
  int mapflag;
  int page_i= 0;
  size_t large_page_size= 0;
  size_t aligned_size= *size;
  DBUG_ENTER("my_large_malloc");

  while (1)
  {
    mapflag= MAP_PRIVATE | OS_MAP_ANON;
    if (my_use_large_pages)
    {
      large_page_size= my_next_large_page_size(*size, &page_i);
      /* this might be 0, in which case we do a standard mmap */
      if (large_page_size)
      {
#if defined(MAP_HUGETLB) /* linux 2.6.32 */
        mapflag|= MAP_HUGETLB;
#if defined(MAP_HUGE_SHIFT) /* Linux-3.8+ */
        mapflag|= my_bit_log2_size_t(large_page_size) << MAP_HUGE_SHIFT;
#else
# warning "No explicit large page (HUGETLB pages) support in Linux < 3.8"
#endif
#elif defined(MAP_ALIGNED)
        mapflag|= MAP_ALIGNED(my_bit_log2_size_t(large_page_size));
#if defined(MAP_ALIGNED_SUPER)
        mapflag|= MAP_ALIGNED_SUPER;
#endif
#endif
        aligned_size= MY_ALIGN(*size, (size_t) large_page_size);
      }
      else
      {
        aligned_size= *size;
      }
    }
    ptr= mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, mapflag, -1, 0);
    if (ptr == (void*) -1)
    {
      ptr= NULL;
      if (my_flags & MY_WME)
      {
        if (large_page_size)
        {
          my_printf_error(EE_OUTOFMEMORY,
                          "Couldn't allocate %zu bytes (Large/HugeTLB memory "
                          "page size %zu); errno %u; continuing to smaller size",
                          MYF(ME_WARNING | ME_ERROR_LOG_ONLY),
                          aligned_size, large_page_size, errno);
        }
        else
        {
          my_error(EE_OUTOFMEMORY, MYF(ME_BELL+ME_ERROR_LOG), aligned_size);
        }
      }
      /* try next smaller memory size */
      if (large_page_size && errno == ENOMEM)
        continue;

      /* other errors are more serious */
      break;
    }
    else /* success */
    {
      if (large_page_size)
      {
        /*
          we do need to record the adjustment so that munmap gets called with
          the right size. This is only the case for HUGETLB pages.
        */
        *size= aligned_size;
      }
      break;
    }
    if (large_page_size == 0)
    {
      break; /* no more options to try */
    }
  }
#else
  DBUG_RETURN(my_malloc_lock(*size, my_flags));
#endif /* defined(HAVE_MMAP) */

  if (ptr != NULL)
  {
    MEM_MAKE_DEFINED(ptr, *size);
  }

  DBUG_RETURN(ptr);
}


/**
  General large pages deallocator.
  Tries to deallocate memory as if it was from large pages pool and falls back
  to my_free_lock() in case of failure
*/
void my_large_free(void *ptr, size_t size)
{
  DBUG_ENTER("my_large_free");

  /*
    The following implementations can only fail if ptr was not allocated with
    my_large_malloc(), i.e. my_malloc_lock() was used so we should free it
    with my_free_lock()

    For ASAN, we need to explicitly unpoison this memory region because the OS
    may reuse that memory for some TLS or stack variable. It will remain
    poisoned if it was explicitly poisioned before release. If this happens,
    we'll have hard to debug false positives like in MDEV-21239.
    For valgrind, we mark it as UNDEFINED rather than NOACCESS because of the
    implict reuse possiblility.
  */
#if defined(HAVE_MMAP) && !defined(_WIN32)
  if (munmap(ptr, size))
  {
    my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size, errno);
  }
# if !__has_feature(memory_sanitizer)
  else
  {
    MEM_MAKE_ADDRESSABLE(ptr, size);
  }
# endif
#elif defined(_WIN32)
  /*
     When RELEASE memory, the size parameter must be 0.
     Do not use MEM_RELEASE with MEM_DECOMMIT.
  */
  if (ptr && !VirtualFree(ptr, 0, MEM_RELEASE))
  {
    my_error(EE_BADMEMORYRELEASE, MYF(ME_ERROR_LOG_ONLY), ptr, size,
             GetLastError());
  }
# if !__has_feature(memory_sanitizer)
  else
  {
    MEM_MAKE_ADDRESSABLE(ptr, size);
  }
# endif
#else
  my_free_lock(ptr);
#endif

  DBUG_VOID_RETURN;
}
