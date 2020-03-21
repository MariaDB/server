/*
   Copyright (c) 2020, MariaDB Corporation.

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

#pragma once

#include <cassert>
#include <cstring>

#ifdef _MSC_VER
template <std::size_t Alignment, class T>
static inline T my_assume_aligned(T ptr)
{
  assert(reinterpret_cast<size_t>(ptr) % Alignment == 0);
  __assume(reinterpret_cast<size_t>(ptr) % Alignment == 0);
  return ptr;
}
#else
template <std::size_t Alignment, class T>
static inline T my_assume_aligned(T ptr)
{
  assert(reinterpret_cast<size_t>(ptr) % Alignment == 0);
  return static_cast<T>(__builtin_assume_aligned(ptr, Alignment));
}
#endif

template <std::size_t Alignment>
inline void *memcpy_aligned(void *dest, const void *src, size_t n)
{
  static_assert(Alignment && !(Alignment & (Alignment - 1)), "power of 2");

  return std::memcpy(my_assume_aligned<Alignment>(dest),
                     my_assume_aligned<Alignment>(src), n);
}
template <std::size_t Alignment>
inline void *memmove_aligned(void *dest, const void *src, size_t n)
{
  static_assert(Alignment && !(Alignment & (Alignment - 1)), "power of 2");

  return std::memmove(my_assume_aligned<Alignment>(dest),
                      my_assume_aligned<Alignment>(src), n);
}
template <std::size_t Alignment>
inline int memcmp_aligned(const void *s1, const void *s2, size_t n)
{
  static_assert(Alignment && !(Alignment & (Alignment - 1)), "power of 2");

  return std::memcmp(my_assume_aligned<Alignment>(s1),
                     my_assume_aligned<Alignment>(s2), n);
}
template <std::size_t Alignment>
inline void *memset_aligned(void *s, int c, size_t n)
{
  static_assert(Alignment && !(Alignment & (Alignment - 1)), "power of 2");

  return std::memset(my_assume_aligned<Alignment>(s), c, n);
}
