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

namespace grn {
namespace dat {

// This class is used to detect an out-of-range access in debug mode.
template <typename T>
class GRN_DAT_API Array {
 public:
  Array() : ptr_(NULL), size_(0) {}
  Array(void *ptr, UInt32 size) : ptr_(static_cast<T *>(ptr)), size_(size) {
    GRN_DAT_DEBUG_THROW_IF((ptr == NULL) && (size != 0));
  }
  template <UInt32 U>
  explicit Array(T (&array)[U]) : ptr_(array), size_(U) {}
  ~Array() {}

  const T &operator[](UInt32 i) const {
    GRN_DAT_DEBUG_THROW_IF(i >= size_);
    return ptr_[i];
  }
  T &operator[](UInt32 i) {
    GRN_DAT_DEBUG_THROW_IF(i >= size_);
    return ptr_[i];
  }

  const T *begin() const {
    return ptr();
  }
  T *begin() {
    return ptr();
  }

  const T *end() const {
    return ptr() + size();
  }
  T *end() {
    return ptr() + size();
  }

  void assign(void *ptr, UInt32 size) {
    GRN_DAT_DEBUG_THROW_IF((ptr == NULL) && (size != 0));
    ptr_ = static_cast<T *>(ptr);
    size_ = size;
  }
  template <UInt32 U>
  void assign(T (&array)[U]) {
    assign(array, U);
  }

  void swap(Array *rhs) {
    T * const temp_ptr = ptr_;
    ptr_ = rhs->ptr_;
    rhs->ptr_ = temp_ptr;

    const UInt32 temp_size = size_;
    size_ = rhs->size_;
    rhs->size_ = temp_size;
  }

  T *ptr() const {
    return ptr_;
  }
  UInt32 size() const {
    return size_;
  }

 private:
  T *ptr_;
  UInt32 size_;

  // Disallows copy and assignment.
  Array(const Array &);
  Array &operator=(const Array &);
};

}  // namespace dat
}  // namespace grn
