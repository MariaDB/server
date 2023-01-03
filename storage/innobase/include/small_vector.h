/*****************************************************************************

Copyright (c) 2023, MariaDB Corporation.

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
/* A normally small vector, inspired by llvm::SmallVector */
#include "my_global.h"
#include <iterator>
#include <memory>

class small_vector_base
{
protected:
  typedef uint32_t Size_T;
  void *BeginX;
  Size_T Size= 0, Capacity;
  small_vector_base()= delete;
  small_vector_base(void *small, size_t small_size)
    : BeginX(small), Capacity(Size_T(small_size)) {}
  ATTRIBUTE_COLD void grow(void *small, size_t min_size, size_t element_size);
public:
  size_t size() const { return Size; }
  size_t capacity() const { return Capacity; }
  bool empty() const { return !Size; }
  void clear() { Size= 0; }
protected:
  void set_size(size_t N) { Size= Size_T(N); }
};

template <typename T, unsigned N>
class small_vector : public small_vector_base
{
  /** The fixed storage allocation */
  T small[N];

  using small_vector_base::set_size;

  void grow_if_needed()
  {
    if (unlikely(size() >= capacity()))
      grow(small, size() + 1, sizeof small);
  }

public:
  small_vector() : small_vector_base(small, N) {}
  ~small_vector() { if (small != begin()) my_free(begin()); }

  using iterator= T *;
  using const_iterator= const T *;
  using reverse_iterator= std::reverse_iterator<iterator>;
  using reference= T &;

  iterator begin() { return static_cast<iterator>(BeginX); }
  const_iterator begin() const { return static_cast<const_iterator>(BeginX); }
  iterator end() { return begin() + size(); }
  const_iterator end() const { return begin() + size(); }

  reverse_iterator rbegin() { return reverse_iterator(end()); }
  reverse_iterator rend() { return reverse_iterator(begin()); }

  reference operator[](size_t i) { assert(i < size()); return begin()[i]; }

  void erase(const_iterator S, const_iterator E)
  {
    set_size(std::move(const_cast<iterator>(E), end(),
                       const_cast<iterator>(S)) - begin());
  }

  void emplace_back(T &&arg)
  {
    grow_if_needed();
    ::new (end()) T(arg);
    set_size(size() + 1);
  }
};
