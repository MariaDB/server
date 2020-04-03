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

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#ifdef __linux__
#include <linux/mman.h>
#include <dirent.h>
#endif
#if defined(__linux__) || defined(MAP_ALIGNED)
#include "my_bit.h"
#endif

#ifdef HAVE_SOLARIS_LARGE_PAGES
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#if defined(__sun__) && defined(__GNUC__) && defined(__cplusplus) \
    && defined(_XOPEN_SOURCE)
/* memcntl exist within sys/mman.h, but under-defines what is need to use it */
extern int memcntl(caddr_t, size_t, int, caddr_t, int, int);
#endif /* __sun__ ... */
#endif /* HAVE_SOLARIS_LARGE_PAGES */

#if defined(_WIN32) || (!defined(__linux__) && !defined(MAP_ALIGNED))
static size_t my_large_page_size;
#endif

#ifdef HAVE_LARGE_PAGES
static my_bool my_use_large_pages= 0;
#else
#define my_use_large_pages 0
#endif

#if defined(__linux__) || defined(HAVE_GETPAGESIZES)
#define my_large_page_sizes_length 8
static size_t my_large_page_sizes[my_large_page_sizes_length];
static void my_get_large_page_sizes(size_t sizes[]);
#else
#define my_large_page_sizes_length 0
#define my_get_large_page_sizes(A) do {} while(0)
#endif

static inline my_bool my_is_2pow(size_t n) { return !((n) & ((n) - 1)); }

static uchar* my_large_malloc_int(size_t *size, myf my_flags);
static my_bool my_large_free_int(void *ptr, size_t size);

#ifdef HAVE_LARGE_PAGES

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
#endif /* defined(HAVE_GETPAGESIZES) || defined(__linux__) */


int my_init_large_pages(my_bool super_large_pages)
{
#ifdef _WIN32
  if (!my_obtain_privilege(SE_LOCK_MEMORY_NAME))
  {
    fprintf(stderr, "mysqld: Lock Pages in memory access rights required for "
            "use with large-pages, see https://mariadb.com/kb/en/library/"
            "mariadb-memory-allocation/#huge-pages");
    return 1;
  }
  my_large_page_size= GetLargePageMinimum();
#elif !defined(__linux__) && !defined(MAP_ALIGNED)
  /*
    This is a fudge as we only use this to ensure that mmap allocations are of
    this size.
  */
  my_large_page_size= my_getpagesize();
#endif

  my_use_large_pages= 1;
  my_get_large_page_sizes(my_large_page_sizes);

#ifdef HAVE_SOLARIS_LARGE_PAGES
#define LARGE_PAGESIZE (4*1024*1024)  /* 4MB */
#define SUPER_LARGE_PAGESIZE (256*1024*1024)  /* 256MB */
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
  size_t max_desired_page_size;
  size_t max_page_size= 0;
  if (super_large_pages)
    max_desired_page_size= SUPER_LARGE_PAGESIZE;
  else
    max_desired_page_size= LARGE_PAGESIZE;

  max_page_size= my_next_large_page_size(max_desired_page_size, &nelem);
  if (max_page_size > 0)
  {
    struct memcntl_mha mpss;

    mpss.mha_cmd= MHA_MAPSIZE_BSSBRK;
    mpss.mha_pagesize= max_page_size;
    mpss.mha_flags= 0;
    if (memcntl(NULL, 0, MC_HAT_ADVISE, (caddr_t) &mpss, 0, 0))
    {
      perror("memcntl MC_HAT_ADVISE cmd MHA_MAPSIZE_BSSBRK error (continuing)");
    }
    mpss.mha_cmd= MHA_MAPSIZE_STACK;
    if (memcntl(NULL, 0, MC_HAT_ADVISE, (caddr_t) &mpss, 0, 0))
    {
      perror("memcntl MC_HAT_ADVISE cmd MHA_MAPSIZE_STACK error (continuing)");
    }
  }
#endif /* HAVE_SOLARIS_LARGE_PAGES */
  return 0;
}

/*
  General large pages allocator.
  Tries to allocate memory from large pages pool and falls back to
  my_malloc_lock() in case of failure.
  Every implementation returns a zero filled buffer here.
*/

uchar* my_large_malloc(size_t *size, myf my_flags)
{
  uchar* ptr;
  DBUG_ENTER("my_large_malloc");
  
  if ((ptr= my_large_malloc_int(size, my_flags)) != NULL)
  {
    MEM_MAKE_DEFINED(ptr, *size);
    DBUG_RETURN(ptr);
  }
  if (my_flags & MY_WME)
    fprintf(stderr, "Warning: Using conventional memory pool\n");
      
  DBUG_RETURN(my_malloc_lock(*size, my_flags));
}

/*
  General large pages deallocator.
  Tries to deallocate memory as if it was from large pages pool and falls back
  to my_free_lock() in case of failure
 */

void my_large_free(void *ptr, size_t size)
{
  DBUG_ENTER("my_large_free");
  
  /*
    my_large_free_int() can only fail if ptr was not allocated with
    my_large_malloc_int(), i.e. my_malloc_lock() was used so we should free it
    with my_free_lock()
  */
  if (!my_large_free_int(ptr, size))
    my_free_lock(ptr);
  /*
    For ASAN, we need to explicitly unpoison this memory region because the OS
    may reuse that memory for some TLS or stack variable. It will remain
    poisoned if it was explicitly poisioned before release. If this happens,
    we'll have hard to debug false positives like in MDEV-21239.
    For valgrind, we mark it as UNDEFINED rather than NOACCESS because of the
    implict reuse possiblility.
  */
  else
    MEM_UNDEFINED(ptr, size);

  DBUG_VOID_RETURN;
}
#endif /* HAVE_LARGE_PAGES */


#ifdef __linux__
/* Linux-specific function to determine the sizes of large pages */

static void my_get_large_page_sizes(size_t sizes[my_large_page_sizes_length])
{
  DIR *dirp;
  struct dirent *r;
  int i= 0;
  DBUG_ENTER("my_get_large_page_sizes");

  dirp= opendir("/sys/kernel/mm/hugepages");
  if (dirp == NULL)
  {
    perror("Warning: failed to open /sys/kernel/mm/hugepages");
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
          fprintf(stderr, "Warning: non-power of 2 large page size (%zu) found,"
                  " skipping\n", sizes[i]);
          sizes[i]= 0;
          continue;
        }
        ++i;
      }
    }
    if (closedir(dirp))
    {
      perror("Warning: failed to close /sys/kernel/mm/hugepages");
    }
    qsort(sizes, i, sizeof(size_t), size_t_cmp);
  }
  DBUG_VOID_RETURN;
}
#endif

/* Multisized (Linux/FreeBSD) large pages allocator  */

#if defined(__linux__) || defined(MAP_ALIGNED)
uchar* my_large_malloc_int(size_t *size, myf my_flags)
{
  uchar* ptr;
  int mapflag;
  int page_i= 0;
  size_t large_page_size= 0;
  size_t aligned_size= *size;
  DBUG_ENTER("my_large_malloc_int");

  while (1)
  {
    mapflag= MAP_PRIVATE | MAP_ANONYMOUS;
    if (my_use_large_pages)
    {
      large_page_size= my_next_large_page_size(*size, &page_i);
      if (large_page_size)
      {
#ifdef __linux__
        mapflag|= MAP_HUGETLB | my_bit_log2_size_t(large_page_size) << MAP_HUGE_SHIFT;
#else
        mapflag|= MAP_ALIGNED_SUPER | MAP_ALIGNED(my_bit_log2_size_t(large_page_size));
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
          fprintf(stderr,
                  "Warning: Failed to allocate %zu bytes from HugeTLB memory"
                  "(page size %zu). errno %d\n", aligned_size, large_page_size,
                  errno);
        }
        else
        {
          fprintf(stderr,
                  "Warning: Failed to allocate %zu bytes from memory."
                  " errno %d\n", aligned_size, errno);
        }
      }
      /* try next smaller memory size */
      if (large_page_size && errno == ENOMEM)
        continue;

      /* other errors are more serious */
      DBUG_RETURN(NULL);
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
      DBUG_RETURN(ptr);
    }
    if (large_page_size == 0)
    {
      break; /* no more options to try */
    }
  }
  DBUG_RETURN(ptr);
}

#endif /* defined(__linux__) || defined(MAP_ALIGNED) */

#if defined(HAVE_GETPAGESIZES) && !defined(__linux__)
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
#endif

#if defined(HAVE_MMAP) && !defined(_WIN32)

/* mmap and Linux-specific large pages deallocator */

my_bool my_large_free_int(void *ptr, size_t size)
{
  DBUG_ENTER("my_large_free_int");

  if (munmap(ptr, size))
  {
    /* This occurs when the original allocation fell back to conventional memory so ignore the EINVAL error */
    if (errno != EINVAL)
    {
      fprintf(stderr, "Warning: Failed to unmap %zu bytes, errno %d\n", size, errno);
    }
    DBUG_RETURN(0);
  }
  DBUG_RETURN(1);
}
#endif /* HAVE_MMAP */

#if defined(HAVE_MMAP) && !defined(__linux__) && !defined(MAP_ALIGNED) \
    && !defined(_WIN32)

/* Solaris for example has only MAP_ANON, FreeBSD has MAP_ANONYMOUS and
MAP_ANON but MAP_ANONYMOUS is marked "for compatibility" */
#if defined(MAP_ANONYMOUS)
#define OS_MAP_ANON     MAP_ANONYMOUS
#elif defined(MAP_ANON)
#define OS_MAP_ANON     MAP_ANON
#else
#error unsupported mmap - no MAP_ANON{YMOUS}
#endif


/* mmap(non-Linux,non-FreeBSD) pages allocator  */

uchar* my_large_malloc_int(size_t *size, myf my_flags)
{
  uchar* ptr;
  int mapflag;
  DBUG_ENTER("my_large_malloc_int");

  mapflag= MAP_PRIVATE | OS_MAP_ANON;

  if (my_use_large_pages && my_large_page_size)
  {
    /* Align block size to my_large_page_size */
    *size= MY_ALIGN(*size, (size_t) my_large_page_size);
  }
  ptr= mmap(NULL, *size, PROT_READ | PROT_WRITE, mapflag, -1, 0);
  if (ptr == (void*) -1)
  {
    ptr= NULL;
    if (my_flags & MY_WME)
    {
      fprintf(stderr,
              "Warning: Failed to allocate %zu bytes from memory."
              " errno %d\n", *size, errno);
    }
  }
  DBUG_RETURN(ptr);
}
#endif /* defined(HAVE_MMAP) && !defined(__linux__) && !defined(_WIN32) */

#ifdef _WIN32

/* Windows-specific large pages allocator */

uchar* my_large_malloc_int(size_t *size, myf my_flags)
{
  DBUG_ENTER("my_large_malloc_int");
  void* ptr;
  DWORD alloc_type= MEM_COMMIT | MEM_RESERVE;
  size_t orig_size= *size;

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
      fprintf(stderr,
              "Warning: VirtualAlloc(%zu bytes%s) failed; Windows error %lu\n",
              *size,
              my_use_large_pages ? ", MEM_LARGE_PAGES" : "",
              GetLastError());
    }
    *size= orig_size;
    ptr= VirtualAlloc(NULL, *size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ptr && my_flags & MY_WME)
    {
      fprintf(stderr,
              "Warning: VirtualAlloc(%zu bytes) failed; Windows error %lu\n",
              *size, GetLastError());
    }
  }

  DBUG_RETURN(ptr);
}

/* Windows-specific large pages deallocator */

my_bool my_large_free_int(void *ptr, size_t size)
{
  DBUG_ENTER("my_large_free_int");
  /*
     When RELEASE memory, the size parameter must be 0.
     Do not use MEM_RELEASE with MEM_DECOMMIT.
  */
  if (ptr && !VirtualFree(ptr, 0, MEM_RELEASE))
  {
    fprintf(stderr,
            "Error: VirtualFree(%p, %zu) failed; Windows error %lu\n", ptr, size, GetLastError());
    DBUG_RETURN(0);
  }

  DBUG_RETURN(1);
}
#endif /* _WIN32 */

