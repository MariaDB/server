#ifndef SQL_ALLOC_INCLUDED
#define SQL_ALLOC_INCLUDED
/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2017, 2018, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <my_sys.h>                    /* alloc_root, MEM_ROOT, TRASH */

/* MariaDB standard class memory allocator */

class Sql_alloc
{
public:
  static void *operator new(size_t size) throw ()
  {
    return thd_alloc(_current_thd(), size);
  }
  static void *operator new[](size_t size) throw ()
  {
    return thd_alloc(_current_thd(), size);
  }
  static void *operator new[](size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw()
  { return alloc_root(mem_root, size); }
  static void operator delete(void *ptr, size_t size) { TRASH_FREE(ptr, size); }
  static void operator delete(void *, MEM_ROOT *){}
  static void operator delete[](void *, MEM_ROOT *)
  { /* never called */ }
  static void operator delete[](void *ptr, size_t size) { TRASH_FREE(ptr, size); }
};
#endif /* SQL_ALLOC_INCLUDED */
