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

// The most significant bit represents whether or not the node is a linker.
// BASE of a linker represents the position of its associated key and BASE of
// a non-linker represents the offset to its child nodes.
class GRN_DAT_API Base {
 public:
  Base() : value_(0) {}

  bool operator==(const Base &rhs) const {
    return value_ == rhs.value_;
  }

  bool is_linker() const {
    return (value_ & IS_LINKER_FLAG) == IS_LINKER_FLAG;
  }
  UInt32 offset() const {
    GRN_DAT_DEBUG_THROW_IF(is_linker());
    return value_;
  }
  UInt32 key_pos() const {
    GRN_DAT_DEBUG_THROW_IF(!is_linker());
    return value_ & ~IS_LINKER_FLAG;
  }

  void set_offset(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF((x & IS_LINKER_FLAG) != 0);
    GRN_DAT_DEBUG_THROW_IF(x > MAX_OFFSET);
    value_ = x;
  }
  void set_key_pos(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF((x & IS_LINKER_FLAG) != 0);
    GRN_DAT_DEBUG_THROW_IF(x > MAX_OFFSET);
    value_ = IS_LINKER_FLAG | x;
  }

 private:
  UInt32 value_;

  static const UInt32 IS_LINKER_FLAG = 0x80000000U;
};

}  // namespace dat
}  // namespace grn
