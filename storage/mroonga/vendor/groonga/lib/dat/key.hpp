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

#include "string.hpp"

namespace grn {
namespace dat {

class GRN_DAT_API Key {
 public:
  const UInt8 &operator[](UInt32 i) const {
    GRN_DAT_DEBUG_THROW_IF(i >= length());
    return buf_[i];
  }

  bool is_valid() const {
    return id() != INVALID_KEY_ID;
  }

  String str() const {
    return String(ptr(), length());
  }

  const void *ptr() const {
    return buf_;
  }
  UInt32 length() const {
    return (length_high_ << 4) | (id_and_length_low_ & 0x0F);
  }
  UInt32 id() const {
    return id_and_length_low_ >> 4;
  }

  bool equals_to(const void *ptr, UInt32 length, UInt32 offset = 0) const {
    if (length != this->length()) {
      return false;
    }
    for ( ; offset < length; ++offset) {
      if ((*this)[offset] != static_cast<const UInt8 *>(ptr)[offset]) {
        return false;
      }
    }
    return true;
  }

  // Creates an object of Key from given parameters. Then, the created object
  // is embedded into a specified buffer.
  static const Key &create(UInt32 *buf, UInt32 key_id,
                           const void *key_ptr, UInt32 key_length) {
    GRN_DAT_DEBUG_THROW_IF(buf == NULL);
    GRN_DAT_DEBUG_THROW_IF(key_id > MAX_KEY_ID);
    GRN_DAT_DEBUG_THROW_IF((key_ptr == NULL) && (key_length != 0));
    GRN_DAT_DEBUG_THROW_IF(key_length > MAX_KEY_LENGTH);

    *buf = (key_id << 4) | (key_length & 0x0F);
    UInt8 *ptr = reinterpret_cast<UInt8 *>(buf + 1);
    *ptr++ = key_length >> 4;
    for (UInt32 i = 0; i < key_length; ++i) {
      ptr[i] = static_cast<const UInt8 *>(key_ptr)[i];
    }
    return *reinterpret_cast<const Key *>(buf);
  }

  // Calculates how many UInt32s are required for a string. It is guaranteed
  // that the estimated size is not less than the actual size.
  static UInt32 estimate_size(UInt32 length) {
    return 2 + (length / sizeof(UInt32));
  }

  // Returns a reference to an invalid key.
  static const Key &invalid_key() {
    static const Key invalid_key;
    return invalid_key;
//    static const UInt32 invalid_key_buf[2] = { INVALID_KEY_ID << 4, 0 };
//    return *reinterpret_cast<const Key *>(invalid_key_buf);
  }

 private:
  UInt32 id_and_length_low_;
  UInt8 length_high_;
  UInt8 buf_[3];

  // Disallows instantiation.
  Key() : id_and_length_low_(INVALID_KEY_ID << 4), length_high_(0) {}
  ~Key() {}

  // Disallows copy and assignment.
  Key(const Key &);
  Key &operator=(const Key &);
};

}  // namespace dat
}  // namespace grn
