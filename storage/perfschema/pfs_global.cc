/* Copyright (c) 2008, 2015, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/**
  @file storage/perfschema/pfs_global.cc
  Miscellaneous global dependencies (implementation).
*/

#include <my_global.h>
#include "pfs_global.h"
#include <my_sys.h>
#include <my_net.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>                             /* memalign() may be here */
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef __WIN__
  #include <winsock2.h>
#else
  #include <arpa/inet.h>
#endif

bool pfs_initialized= false;
size_t pfs_allocated_memory= 0;

/**
  Memory allocation for the performance schema.
  The memory used internally in the performance schema implementation
  is allocated once during startup, and considered static thereafter.
*/
void *pfs_malloc(size_t size, myf flags)
{
  DBUG_ASSERT(! pfs_initialized);
  DBUG_ASSERT(size > 0);

  void *ptr= NULL;

#ifdef PFS_ALIGNEMENT
#ifdef HAVE_POSIX_MEMALIGN
  /* Linux */
  if (unlikely(posix_memalign(& ptr, PFS_ALIGNEMENT, size)))
    return NULL;
#else
#ifdef HAVE_MEMALIGN
  /* Solaris */
  ptr= memalign(PFS_ALIGNEMENT, size);
  if (unlikely(ptr == NULL))
    return NULL;
#else
#ifdef HAVE_ALIGNED_MALLOC
  /* Windows */
  ptr= _aligned_malloc(size, PFS_ALIGNEMENT);
  if (unlikely(ptr == NULL))
    return NULL;
#else
#error "Missing implementation for PFS_ALIGNENT"
#endif /* HAVE_ALIGNED_MALLOC */
#endif /* HAVE_MEMALIGN */
#endif /* HAVE_POSIX_MEMALIGN */
#else /* PFS_ALIGNMENT */
  /* Everything else */
  ptr= malloc(size);
  if (unlikely(ptr == NULL))
    return NULL;
#endif

  pfs_allocated_memory+= size;
  if (flags & MY_ZEROFILL)
    memset(ptr, 0, size);
  return ptr;
}

void pfs_free(void *ptr)
{
  if (ptr == NULL)
    return;

#ifdef HAVE_POSIX_MEMALIGN
  /* Allocated with posix_memalign() */
  free(ptr);
#else
#ifdef HAVE_MEMALIGN
  /* Allocated with memalign() */
  free(ptr);
#else
#ifdef HAVE_ALIGNED_MALLOC
  /* Allocated with _aligned_malloc() */
  _aligned_free(ptr);
#else
  /* Allocated with malloc() */
  free(ptr);
#endif /* HAVE_ALIGNED_MALLOC */
#endif /* HAVE_MEMALIGN */
#endif /* HAVE_POSIX_MEMALIGN */
}

void pfs_print_error(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  /*
    Printing to anything else, like the error log, would generate even more
    recursive calls to the performance schema implementation
    (file io is instrumented), so that could lead to catastrophic results.
    Printing to something safe, and low level: stderr only.
  */
  vfprintf(stderr, format, args);
  va_end(args);
  fflush(stderr);
}

/**
  Array allocation for the performance schema.
  Checks for overflow of n * size before allocating.
  @param n  number of array elements
  @param size  element size
  @param flags malloc flags
  @return pointer to memory on success, else NULL
*/
void *pfs_malloc_array(size_t n, size_t size, myf flags)
{
  DBUG_ASSERT(n > 0);
  DBUG_ASSERT(size > 0);
  size_t array_size= n * size;
  /* Check for overflow before allocating. */
  if (is_overflow(array_size, n, size))
    return NULL;
  return pfs_malloc(array_size, flags);
}

/**
  Detect multiplication overflow.
  @param product  multiplication product
  @param n1  operand
  @param n2  operand
  @return true if multiplication caused an overflow.
*/
bool is_overflow(size_t product, size_t n1, size_t n2)
{
  if (n1 != 0 && (product / n1 != n2))
    return true;
  else
    return false;
}

/** Convert raw ip address into readable format. Do not do a reverse DNS lookup. */

uint pfs_get_socket_address(char *host,
                            uint host_len,
                            uint *port,
                            const struct sockaddr_storage *src_addr,
                            socklen_t src_len)
{
  DBUG_ASSERT(host);
  DBUG_ASSERT(src_addr);
  DBUG_ASSERT(port);

  memset(host, 0, host_len);
  *port= 0;

  switch (src_addr->ss_family)
  {
    case AF_INET:
    {
      if (host_len < INET_ADDRSTRLEN+1)
        return 0;
      struct sockaddr_in *sa4= (struct sockaddr_in *)(src_addr);
    #ifdef __WIN__
      /* Older versions of Windows do not support inet_ntop() */
      getnameinfo((struct sockaddr *)sa4, sizeof(struct sockaddr_in),
                  host, host_len, NULL, 0, NI_NUMERICHOST);
    #else
      inet_ntop(AF_INET, &(sa4->sin_addr), host, INET_ADDRSTRLEN);
    #endif
      *port= ntohs(sa4->sin_port);
    }
    break;

#ifdef HAVE_IPV6
    case AF_INET6:
    {
      if (host_len < INET6_ADDRSTRLEN+1)
        return 0;
      struct sockaddr_in6 *sa6= (struct sockaddr_in6 *)(src_addr);
    #ifdef __WIN__
      /* Older versions of Windows do not support inet_ntop() */
      getnameinfo((struct sockaddr *)sa6, sizeof(struct sockaddr_in6),
                  host, host_len, NULL, 0, NI_NUMERICHOST);
    #else
      inet_ntop(AF_INET6, &(sa6->sin6_addr), host, INET6_ADDRSTRLEN);
    #endif
      *port= ntohs(sa6->sin6_port);
    }
    break;
#endif

    default:
      break;
  }

  /* Return actual IP address string length */
  return ((uint)strlen((const char*)host));
}

