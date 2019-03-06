/* Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.

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
#ifdef HAVE_LINUX_LARGE_PAGES
#include <linux/mman.h>
#include <dirent.h>
#endif


static inline my_bool my_is_2pow(size_t n) { return !((n) & ((n) - 1)); }

static uint my_get_large_page_size_int(void);
static uchar* my_large_malloc_int(size_t *size, myf my_flags);
static my_bool my_large_free_int(void *ptr, size_t size);

#ifdef HAVE_LARGE_PAGE_OPTION

/* Gets the size of large pages from the OS */

uint my_get_large_page_size(void)
{
  uint size;
  DBUG_ENTER("my_get_large_page_size");
  
  if (!(size = my_get_large_page_size_int()))
    fprintf(stderr, "Warning: Failed to determine large page size\n");

  DBUG_RETURN(size);
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
  else
    /*
      For ASAN, we need to explicitly unpoison this memory region because the OS
      may reuse that memory for some TLS or stack variable. It will remain
      poisoned if it was explicitly poisioned before release. If this happens,
      we'll have hard to debug false positives like in MDEV-21239.
      For valgrind, we mark it as UNDEFINED rather than NOACCESS because of the
      implict reuse possiblility.
    */
    MEM_UNDEFINED(ptr, size);

  DBUG_VOID_RETURN;
}
#endif /* HAVE_LARGE_PAGE_OPTION */

#ifdef HAVE_LINUX_LARGE_PAGES

/* Linux-specific function to determine the size of large pages */

uint my_get_large_page_size_int(void)
{
  MYSQL_FILE *f;
  uint size = 0;
  char buf[256];
  DBUG_ENTER("my_get_large_page_size_int");

  if (!(f= mysql_file_fopen(key_file_proc_meminfo, "/proc/meminfo",
                            O_RDONLY, MYF(MY_WME))))
    goto finish;

  while (mysql_file_fgets(buf, sizeof(buf), f))
    if (sscanf(buf, "Hugepagesize: %u kB", &size))
      break;

  mysql_file_fclose(f, MYF(MY_WME));
  
finish:
  DBUG_RETURN(size * 1024);
}

/* Descending sort */

static int size_t_cmp(const void *a, const void *b)
{
  const size_t *ia= (const size_t *)a; // casting pointer types
  const size_t *ib= (const size_t *)b;
  if (*ib > *ia)
  {
       return 1;
  }
  else if (*ib < *ia)
  {
       return -1;
  }
  return 0;
}

/*
  Returns the next large page size smaller or equal to the passed in size.

  The search starts at my_large_page_sizes[*start].

  Assumes my_get_large_page_sizes(my_large_page_sizes) has been called before use.

  For first use, have *start=0. There is no need to increment *start.

  @param[in]     sz size to be searched for.
  @param[in,out] start ptr to int representing offset in my_large_page_sizes to start from.
  *start is updated during search and can be used to search again if 0 isn't returned.

  @returns the next size found. *start will be incremented to the next potential size.
  @retval  a large page size that is valid on this system or 0 if no large page size possible.
*/
size_t my_next_large_page_size(size_t sz, int *start)
{
  size_t cur;
  DBUG_ENTER("my_next_large_page_size");

  while (*start < my_large_page_sizes_length
         && my_large_page_sizes[*start] > 0)
  {
    cur= *start;
    (*start)++;
    if (my_large_page_sizes[cur] <= sz)
    {
      DBUG_RETURN(my_large_page_sizes[cur]);
    }
  }
  DBUG_RETURN(0);
}

/* Linux-specific function to determine the sizes of large pages */

void my_get_large_page_sizes(size_t sizes[my_large_page_sizes_length])
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
    while (i < my_large_page_sizes_length &&
          (r= readdir(dirp)))
    {
      if (strncmp("hugepages-", r->d_name, 10) == 0)
      {
        sizes[i]= strtoull(r->d_name + 10, NULL, 10) * 1024ULL;
        if (!my_is_2pow(sizes[i]))
        {
          fprintf(stderr, "Warning: non-power of 2 large page size (%zu) found, skipping\n", sizes[i]);
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

/* Linux-specific large pages allocator  */
    
uchar* my_large_malloc_int(size_t *size, myf my_flags)
{
  uchar* ptr;
  int mapflag;
  DBUG_ENTER("my_large_malloc_int");

  mapflag = MAP_PRIVATE | MAP_ANONYMOUS;

  if (my_use_large_pages && my_large_page_size)
  {
    mapflag|= MAP_HUGETLB;
    /* Align block size to my_large_page_size */
    *size= MY_ALIGN(*size, (size_t) my_large_page_size);
  }
  /* mmap adjusts the size to the hugetlb page size so no adjustment is needed */
  ptr = mmap(NULL, *size, PROT_READ | PROT_WRITE, mapflag, -1, 0);
  if (ptr == (void*) -1) {
    ptr= NULL;
    if (my_flags & MY_WME) {
      fprintf(stderr,
              "Warning: Failed to allocate %zu bytes from %smemory."
              " errno %d\n", *size, my_use_large_pages ? "HugeTLB " : "", errno);
    }
  }
  DBUG_RETURN(ptr);
}

#endif /* HAVE_LINUX_LARGE_PAGES */

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

#if defined(HAVE_MMAP) && !defined(HAVE_LINUX_LARGE_PAGES) && !defined(_WIN32)

/* Solaris for example has only MAP_ANON, FreeBSD has MAP_ANONYMOUS and
MAP_ANON but MAP_ANONYMOUS is marked "for compatibility" */
#if defined(MAP_ANONYMOUS)
#define OS_MAP_ANON     MAP_ANONYMOUS
#elif defined(MAP_ANON)
#define OS_MAP_ANON     MAP_ANON
#else
#error unsupported mmap - no MAP_ANON{YMOUS}
#endif

/* mmap-specific function to determine the size of large pages

This is a fudge as we only use this to ensure that mmap allocations
are of this size.
*/

uint my_get_large_page_size_int(void)
{
  return my_getpagesize();
}

/* mmap(non-Linux) pages allocator  */

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

#endif /* HAVE_MMAP && !HAVE_LINUX_LARGE_PAGES*/

#ifdef _WIN32

/* Windows-specific function to determine the size of large pages */

uint my_get_large_page_size_int(void)
{
  SYSTEM_INFO	system_info;
  DBUG_ENTER("my_get_large_page_size_int");

  GetSystemInfo(&system_info);

  DBUG_RETURN(system_info.dwPageSize);
}

/* Windows-specific large pages allocator */

uchar* my_large_malloc_int(size_t *size, myf my_flags)
{
  DBUG_ENTER("my_large_malloc_int");

  /* Align block size to my_large_page_size */
  *size= MY_ALIGN(*size, (size_t) my_large_page_size);
  ptr= VirtualAlloc(NULL, *size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!ptr)
  {
    if (my_flags & MY_WME)
    {
      fprintf(stderr,
              "Warning: VirtualAlloc(%zu bytes) failed; Windows error %lu\n", *size, GetLastError());
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

