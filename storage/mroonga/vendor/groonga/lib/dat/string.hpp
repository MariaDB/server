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

class GRN_DAT_API String {
 public:
  String()
      : ptr_(NULL),
        length_(0) {}
  String(const void *ptr, UInt32 length)
      : ptr_(static_cast<const UInt8 *>(ptr)),
        length_(length) {}
  template <UInt32 T>
  explicit String(const char (&str)[T])
      : ptr_(reinterpret_cast<const UInt8 *>(str)),
        length_(T - 1) {}
  String(const String &rhs)
      : ptr_(rhs.ptr_),
        length_(rhs.length_) {}

  String &operator=(const String &rhs) {
    set_ptr(rhs.ptr());
    set_length(rhs.length());
    return *this;
  }

  const UInt8 &operator[](UInt32 i) const {
    GRN_DAT_DEBUG_THROW_IF(i >= length_);
    return ptr_[i];
  }

  const void *ptr() const {
    return ptr_;
  }
  UInt32 length() const {
    return length_;
  }

  void set_ptr(const void *x) {
    ptr_ = static_cast<const UInt8 *>(x);
  }
  void set_length(UInt32 x) {
    length_ = x;
  }

  void assign(const void *ptr, UInt32 length) {
    set_ptr(ptr);
    set_length(length);
  }

  String substr(UInt32 offset = 0) const {
    return String(ptr_ + offset, length_ - offset);
  }
  String substr(UInt32 offset, UInt32 length) const {
    return String(ptr_ + offset, length);
  }

  // This function returns an integer as follows:
  // - a negative value if *this < rhs,
  // - zero if *this == rhs,
  // - a positive value if *this > rhs,
  // but if the offset is too large, the result is undefined.
  int compare(const String &rhs, UInt32 offset = 0) const {
    GRN_DAT_DEBUG_THROW_IF(offset > length());
    GRN_DAT_DEBUG_THROW_IF(offset > rhs.length());

    for (UInt32 i = offset; i < length(); ++i) {
      if (i >= rhs.length()) {
        return 1;
      } else if ((*this)[i] != rhs[i]) {
        return (*this)[i] - rhs[i];
      }
    }
    return (length() == rhs.length()) ? 0 : -1;
  }

  bool starts_with(const String &str) const {
    if (length() < str.length()) {
      return false;
    }
    for (UInt32 i = 0; i < str.length(); ++i) {
      if ((*this)[i] != str[i]) {
        return false;
      }
    }
    return true;
  }

  bool ends_with(const String &str) const {
    if (length() < str.length()) {
      return false;
    }
    UInt32 offset = length() - str.length();
    for (UInt32 i = 0; i < str.length(); ++i) {
      if ((*this)[offset + i] != str[i]) {
        return false;
      }
    }
    return true;
  }

  void swap(String *rhs) {
    const UInt8 * const ptr_temp = ptr_;
    ptr_ = rhs->ptr_;
    rhs->ptr_ = ptr_temp;

    const UInt32 length_temp = length_;
    length_ = rhs->length_;
    rhs->length_ = length_temp;
  }

 private:
  const UInt8 *ptr_;
  UInt32 length_;
};

inline bool operator==(const String &lhs, const String &rhs) {
  if (lhs.length() != rhs.length()) {
    return false;
  } else if (lhs.ptr() == rhs.ptr()) {
    return true;
  }
  for (UInt32 i = 0; i < lhs.length(); ++i) {
    if (lhs[i] != rhs[i]) {
      return false;
    }
  }
  return true;
}

inline bool operator!=(const String &lhs, const String &rhs) {
  return !(lhs == rhs);
}

inline bool operator<(const String &lhs, const String &rhs) {
  return lhs.compare(rhs) < 0;
}

inline bool operator>(const String &lhs, const String &rhs) {
  return rhs < lhs;
}

inline bool operator<=(const String &lhs, const String &rhs) {
  return !(lhs > rhs);
}

inline bool operator>=(const String &lhs, const String &rhs) {
  return !(lhs < rhs);
}

}  // namespace dat
}  // namespace grn
