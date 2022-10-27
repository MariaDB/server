/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#include "dat.hpp"

#include <new>

namespace grn {
namespace dat {

template <typename T>
class GRN_DAT_API Vector {
 public:
  Vector() : buf_(NULL), size_(0), capacity_(0) {}
  ~Vector() {
    for (UInt32 i = 0; i < size(); ++i) {
      buf_[i].~T();
    }
    delete [] reinterpret_cast<char *>(buf_);
  }

  const T &operator[](UInt32 i) const {
    GRN_DAT_DEBUG_THROW_IF(i >= size());
    return buf_[i];
  }
  T &operator[](UInt32 i) {
    GRN_DAT_DEBUG_THROW_IF(i >= size());
    return buf_[i];
  }

  const T &front() const {
    GRN_DAT_DEBUG_THROW_IF(empty());
    return buf_[0];
  }
  T &front() {
    GRN_DAT_DEBUG_THROW_IF(empty());
    return buf_[0];
  }

  const T &back() const {
    GRN_DAT_DEBUG_THROW_IF(empty());
    return buf_[size() - 1];
  }
  T &back() {
    GRN_DAT_DEBUG_THROW_IF(empty());
    return buf_[size() - 1];
  }

  const T *begin() const {
    return buf_;
  }
  T *begin() {
    return buf_;
  }

  const T *end() const {
    return buf_ + size_;
  }
  T *end() {
    return buf_ + size_;
  }

  void push_back() {
    reserve(size() + 1);
    new (&buf_[size()]) T;
    ++size_;
  }
  void push_back(const T &x) {
    reserve(size() + 1);
    new (&buf_[size()]) T(x);
    ++size_;
  }

  void pop_back() {
    GRN_DAT_DEBUG_THROW_IF(empty());
    back().~T();
    --size_;
  }

  void clear() {
    resize(0);
  }

  void resize(UInt32 new_size) {
    if (new_size > capacity()) {
      reserve(new_size);
    }
    for (UInt32 i = size(); i < new_size; ++i) {
      new (&buf_[i]) T;
    }
    for (UInt32 i = new_size; i < size(); ++i) {
      buf_[i].~T();
    }
    size_ = new_size;
  }
  template <typename U>
  void resize(UInt32 new_size, const U &value) {
    if (new_size > capacity()) {
      reserve(new_size);
    }
    for (UInt32 i = size(); i < new_size; ++i) {
      new (&buf_[i]) T(value);
    }
    for (UInt32 i = new_size; i < size(); ++i) {
      buf_[i].~T();
    }
    size_ = new_size;
  }

  void reserve(UInt32 new_capacity) {
    if (new_capacity <= capacity()) {
      return;
    } else if ((new_capacity / 2) < capacity()) {
      if (capacity() < (MAX_UINT32 / 2)) {
        new_capacity = capacity() * 2;
      } else {
        new_capacity = MAX_UINT32;
      }
    }

    T *new_buf = reinterpret_cast<T *>(
        new (std::nothrow) char[sizeof(new_capacity) * new_capacity]);
    GRN_DAT_THROW_IF(MEMORY_ERROR, new_buf == NULL);

    for (UInt32 i = 0; i < size(); ++i) {
      new (&new_buf[i]) T(buf_[i]);
    }
    for (UInt32 i = 0; i < size(); ++i) {
      buf_[i].~T();
    }

    T *old_buf = buf_;
    buf_ = new_buf;
    delete [] reinterpret_cast<char *>(old_buf);

    capacity_ = new_capacity;
  }

  void swap(Vector *rhs) {
    T * const temp_buf = buf_;
    buf_ = rhs->buf_;
    rhs->buf_ = temp_buf;

    const UInt32 temp_size = size_;
    size_ = rhs->size_;
    rhs->size_ = temp_size;

    const UInt32 temp_capacity = capacity_;
    capacity_ = rhs->capacity_;
    rhs->capacity_ = temp_capacity;
  }

  bool empty() const {
    return size_ == 0;
  }
  UInt32 size() const {
    return size_;
  }
  UInt32 capacity() const {
    return capacity_;
  }

 private:
  T *buf_;
  UInt32 size_;
  UInt32 capacity_;

  // Disallows copy and assignment.
  Vector(const Vector &);
  Vector &operator=(const Vector &);
};

}  // namespace dat
}  // namespace grn
