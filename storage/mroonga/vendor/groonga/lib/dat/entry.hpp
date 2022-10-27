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

// The most significant bit represents whether or not the entry is valid.
// A valid entry stores the position of its associated key and an invalid entry
// stores the index of the next invalid entry.
class GRN_DAT_API Entry {
 public:
  Entry() : value_(0) {}

  bool is_valid() const {
    return (value_ & IS_VALID_FLAG) == IS_VALID_FLAG;
  }
  UInt32 key_pos() const {
    GRN_DAT_DEBUG_THROW_IF(!is_valid());
    return value_ & ~IS_VALID_FLAG;
  }
  UInt32 next() const {
    GRN_DAT_DEBUG_THROW_IF(is_valid());
    return value_;
  }

  void set_key_pos(UInt32 x) {
    value_ = IS_VALID_FLAG | x;
  }
  void set_next(UInt32 x) {
    value_ = x;
  }

 private:
  UInt32 value_;

  static const UInt32 IS_VALID_FLAG = 0x80000000U;
};

}  // namespace dat
}  // namespace grn
