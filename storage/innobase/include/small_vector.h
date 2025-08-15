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
  ATTRIBUTE_COLD void grow_by_1(void *small, size_t element_size) noexcept;
public:
  size_t size() const noexcept { return Size; }
  size_t capacity() const noexcept { return Capacity; }
  bool empty() const noexcept { return !Size; }
  void clear() noexcept { Size= 0; }
protected:
  void set_size(size_t N) noexcept { Size= Size_T(N); }
};

template <typename T, unsigned N>
class small_vector : public small_vector_base
{
  /** The fixed storage allocation */
  T small[N];

  using small_vector_base::set_size;

  void grow_if_needed() noexcept
  {
    if (unlikely(size() >= capacity()))
      grow_by_1(small, sizeof *small);
  }

public:
  small_vector() : small_vector_base(small, N)
  {
    TRASH_ALLOC(small, sizeof small);
  }
  ~small_vector() noexcept
  {
    if (small != begin())
      my_free(begin());
    MEM_MAKE_ADDRESSABLE(small, sizeof small);
  }

  void fake_defined() const noexcept
  {
    ut_ad(empty());
    MEM_MAKE_DEFINED(small, sizeof small);
  }
  void make_undefined() const noexcept { MEM_UNDEFINED(small, sizeof small); }

  bool is_small() const noexcept { return small == BeginX; }

  void deep_clear() noexcept
  {
    if (!is_small())
    {
      my_free(BeginX);
      BeginX= small;
      Capacity= N;
    }
    ut_ad(capacity() == N);
    set_size(0);
  }

  using iterator= T *;
  using const_iterator= const T *;
  using reverse_iterator= std::reverse_iterator<iterator>;
  using reference= T &;
  using const_reference= const T&;

  iterator begin() noexcept { return static_cast<iterator>(BeginX); }
  const_iterator begin() const noexcept
  { return static_cast<const_iterator>(BeginX); }
  iterator end() noexcept { return begin() + size(); }
  const_iterator end() const noexcept { return begin() + size(); }

  reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
  reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

  reference operator[](size_t i) noexcept
  { assert(i < size()); return begin()[i]; }
  const_reference operator[](size_t i) const noexcept
  { return const_cast<small_vector&>(*this)[i]; }

  void erase(const_iterator S, const_iterator E) noexcept
  {
    set_size(std::move(const_cast<iterator>(E), end(),
                       const_cast<iterator>(S)) - begin());
  }

  void emplace_back(T &&arg) noexcept
  {
    grow_if_needed();
    ::new (end()) T(arg);
    set_size(size() + 1);
  }
  void emplace_back(T &arg) noexcept
  {
    grow_if_needed();
    ::new (end()) T(arg);
    set_size(size() + 1);
  }
};
