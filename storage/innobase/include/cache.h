/*****************************************************************************

Copyright (c) 2024, MariaDB plc

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

#pragma once
#include <cstddef>

#if defined __x86_64__ || defined __aarch64__ || defined __powerpc64__
struct pmem_control
{
  void (*persist)(const void *, size_t);
public:
  pmem_control();
};
extern const pmem_control pmem;
# define pmem_persist(buf, size) pmem.persist(buf, size)
#else
void pmem_persist(const void *buf, size_t size);
#endif
