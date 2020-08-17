/* Copyright (c) 2000, 2001, 2003-2006 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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

/*
  This is a replacement of new/delete operators to be used when compiling
  with gcc 3.0.x to avoid including libstdc++ 
  
  It is also used to make all memory allocations to go through
  my_malloc/my_free wrappers (for debugging/safemalloc and accounting)
*/

#include "mysys_priv.h"
#ifdef HAVE_CXX_NEW
#include <new>
#endif

/*
  We don't yet enable the my new operators by default.
  The reasons (for MariaDB) are:

   - There are several global objects in plugins (wsrep_info, InnoDB,
     tpool) that allocates data with 'new'. These objects are not
     freed properly before exit() is called and safemalloc will report
     these as lost memory.  The proper fix is to ensure that all plugins
     either ensure that all objects frees there data or the global object are
     changed to a pointer that as allocated and freed on demand.
     Doing this will make it easier to find leaks and also speed up plugin
     loads when we don't have to initialize a lot of objects until they
     are really needed.
  - Rocksdb calls malloc_usable_size, that will crash if used with new based
    on my_malloc. One suggested fix would be to not define
    ROCKSDB_MALLOC_USABLE_SIZE if MYSYS_USE_NEW is defined.

    When the above is fixed, we can remove the test for REALLY_USE_MYSYS_NEW
    below.
*/

#if defined(USE_MYSYS_NEW) && defined(REALLY_USE_MYSYS_NEW)

void *operator new (size_t sz)
{
  return (void *) my_malloc(key_memory_new, sz ? sz : 1, MYF(0));
}

void *operator new[] (size_t sz)
{
  return (void *) my_malloc(key_memory_new, sz ? sz : 1, MYF(0));
}

void* operator new(std::size_t sz, const std::nothrow_t&) throw()
{
  return (void *) my_malloc(key_memory_new, sz ? sz : 1, MYF(0));
}

void* operator new[](std::size_t sz, const std::nothrow_t&) throw()
{
  return (void *) my_malloc(key_memory_new, sz ? sz : 1, MYF(0));
}

void operator delete (void *ptr, std::size_t) throw ()
{
  my_free(ptr);
}

void operator delete (void *ptr) throw ()
{
  my_free(ptr);
}

void operator delete[] (void *ptr) throw ()
{
  my_free(ptr);
}

void operator delete[] (void *ptr, std::size_t) throw ()
{
  my_free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) throw()
{
  my_free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) throw()
{
  my_free(ptr);
}

C_MODE_START

int __cxa_pure_virtual()
{
  assert(! "Aborted: pure virtual method called.");
  return 0;
}

C_MODE_END
#else
/* 
  Define a dummy symbol, just to avoid compiler/linker warnings
  about compiling an essentially empty file.
*/
int my_new_cc_symbol;
#endif /* USE_MYSYS_NEW */

