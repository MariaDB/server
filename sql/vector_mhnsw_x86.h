/*
   Copyright (c) 2024, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/


#pragma once
#include <cstddef>
#include <cstdint>

/*
  Function pointer table for SIMD-accelerated vector operations.
  Shared between vector_mhnsw.cc and vector_mhnsw_x86.cc.
*/
struct FVector;
struct Vector_ops 
{
  float (*dot_product)(const int16_t *v1, const int16_t *v2, size_t len);
  size_t (*alloc_size)(size_t n);
  FVector * (*align_ptr)(void *ptr);
  void (*fix_tail)(int16_t *dims, size_t vec_len);
};
#if defined(__x86_64__) || defined(_M_X64)
extern "C" Vector_ops vector_ops_x86_available(void);
#endif
