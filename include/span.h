/*****************************************************************************

Copyright (c) 2019, 2020 MariaDB Corporation.

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
#include <iterator>

namespace st_
{

template <class ElementType> class span
{
public:
  typedef ElementType element_type;
  typedef ElementType value_type;
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef element_type *pointer;
  typedef const element_type *const_pointer;
  typedef element_type &reference;
  typedef const element_type &const_reference;
  typedef pointer iterator;
  typedef const_pointer const_iterator;
  typedef std::reverse_iterator<iterator> reverse_iterator;
  typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

  span() : data_(NULL), size_(0) {}

  span(pointer ptr, size_type count) : data_(ptr), size_(count) {}

  span(pointer first, pointer last) : data_(first), size_(last - first) {}

  template <size_t N> span(element_type (&arr)[N]) : data_(arr), size_(N) {}

  template <class Container>
  span(Container &cont) : data_(cont.data()), size_(cont.size())
  {
  }

  template <class Container>
  span(const Container &cont) : data_(cont.data()), size_(cont.size())
  {
  }

  span(const span &other) : data_(other.data_), size_(other.size_) {}

  ~span(){};

  span &operator=(const span &other)
  {
    data_= other.data_;
    size_= other.size_;
    return *this;
  }

  template <size_t Count> span<element_type> first() const
  {
    assert(!empty());
    return span(data_, 1);
  }
  template <size_t Count> span<element_type> last() const
  {
    assert(!empty());
    return span(data_ + size() - 1, 1);
  }

  span<element_type> first(size_type count) const
  {
    assert(!empty());
    return span(data_, 1);
  }
  span<element_type> last(size_type count) const
  {
    assert(!empty());
    return span(data_ + size() - 1, 1);
  }
  span<element_type> subspan(size_type offset, size_type count) const
  {
    assert(!empty());
    assert(size() >= offset + count);
    return span(data_ + offset, count);
  }

  size_type size() const { return size_; }
  size_type size_bytes() const { return size_ * sizeof(ElementType); }
  bool empty() const __attribute__((warn_unused_result)) { return size_ == 0; }

  reference operator[](size_type idx) const
  {
    assert(size() > idx);
    return data_[idx];
  }
  reference front() const
  {
    assert(!empty());
    return data_[0];
  }
  reference back() const
  {
    assert(!empty());
    return data_[size() - 1];
  }
  pointer data() const
  {
    assert(!empty());
    return data_;
  }

  iterator begin() const { return data_; }
  iterator end() const { return data_ + size_; }
  reverse_iterator rbegin() const
  {
    return std::reverse_iterator<iterator>(std::advance(end(), -1));
  }
  reverse_iterator rend() const
  {
    return std::reverse_iterator<iterator>(std::advance(begin(), -1));
  }

private:
  pointer data_;
  size_type size_;
};

} // namespace st_
