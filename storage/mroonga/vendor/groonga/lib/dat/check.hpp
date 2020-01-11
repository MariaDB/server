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

class GRN_DAT_API Check {
 public:
  Check() : value_(0) {}

  bool operator==(const Check &rhs) const {
    return value_ == rhs.value_;
  }

  // The most significant bit represents whether or not the node ID is used as
  // an offset. Note that the MSB is independent of the other bits.
  bool is_offset() const {
    return (value_ & IS_OFFSET_FLAG) == IS_OFFSET_FLAG;
  }

  UInt32 except_is_offset() const {
    GRN_DAT_DEBUG_THROW_IF(is_phantom());
    return value_ & ~IS_OFFSET_FLAG;
  }

  // A phantom node is a node that has never been used, and such a node is also
  // called an empty element. Phantom nodes form a doubly linked list in each
  // block, and the linked list is represented by next() and prev().
  bool is_phantom() const {
    return (value_ & IS_PHANTOM_FLAG) == IS_PHANTOM_FLAG;
  }

  UInt32 next() const {
    GRN_DAT_DEBUG_THROW_IF(!is_phantom());
    return (value_ >> NEXT_SHIFT) & BLOCK_MASK;
  }
  UInt32 prev() const {
    GRN_DAT_DEBUG_THROW_IF(!is_phantom());
    return (value_ >> PREV_SHIFT) & BLOCK_MASK;
  }

  // A label is attached to each non-phantom node. A label is represented by
  // a byte except for a terminal label '\256'. Note that a phantom node always
  // returns an invalid label with its phantom bit flag so as to reject invalid
  // transitions.
  UInt32 label() const {
    return value_ & (IS_PHANTOM_FLAG | LABEL_MASK);
  }

  // A non-phantom node has the labels of the first child and the next sibling.
  // Note that INVALID_LABEL is stored if the node has no child nodes or has
  // no more siblings.
  UInt32 child() const {
    return (value_ >> CHILD_SHIFT) & LABEL_MASK;
  }
  UInt32 sibling() const {
    return (value_ >> SIBLING_SHIFT) & LABEL_MASK;
  }

  void set_is_offset(bool x) {
    if (x) {
      GRN_DAT_DEBUG_THROW_IF(is_offset());
      value_ |= IS_OFFSET_FLAG;
    } else {
      GRN_DAT_DEBUG_THROW_IF(!is_offset());
      value_ &= ~IS_OFFSET_FLAG;
    }
  }

  void set_except_is_offset(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(is_phantom());
    GRN_DAT_DEBUG_THROW_IF((x & IS_OFFSET_FLAG) == IS_OFFSET_FLAG);
    value_ = (value_ & IS_OFFSET_FLAG) | x;
  }

  // To reject a transition to an incomplete node, set_is_phantom() invalidates
  // its label and links when it becomes non-phantom.
  void set_is_phantom(bool x) {
    if (x) {
      GRN_DAT_DEBUG_THROW_IF(is_phantom());
      value_ |= IS_PHANTOM_FLAG;
    } else {
      GRN_DAT_DEBUG_THROW_IF(!is_phantom());
      value_ = (value_ & IS_OFFSET_FLAG) | (INVALID_LABEL << CHILD_SHIFT) |
          (INVALID_LABEL << SIBLING_SHIFT) | INVALID_LABEL;
    }
  }

  void set_next(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(!is_phantom());
    GRN_DAT_DEBUG_THROW_IF(x > BLOCK_MASK);
    value_ = (value_ & ~(BLOCK_MASK << NEXT_SHIFT)) | (x << NEXT_SHIFT);
  }
  void set_prev(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(!is_phantom());
    GRN_DAT_DEBUG_THROW_IF(x > BLOCK_MASK);
    value_ = (value_ & ~(BLOCK_MASK << PREV_SHIFT)) | (x << PREV_SHIFT);
  }

  void set_label(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(is_phantom());
    GRN_DAT_DEBUG_THROW_IF(x > MAX_LABEL);
    value_ = (value_ & ~LABEL_MASK) | x;
  }

  void set_child(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(is_phantom());
    GRN_DAT_DEBUG_THROW_IF(x > MAX_LABEL);
    value_ = (value_ & ~(LABEL_MASK << CHILD_SHIFT)) | (x << CHILD_SHIFT);
  }
  void set_sibling(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(is_phantom());
    GRN_DAT_DEBUG_THROW_IF(label() > MAX_LABEL);
    GRN_DAT_DEBUG_THROW_IF((sibling() != INVALID_LABEL) && (x == INVALID_LABEL));
    value_ = (value_ & ~(LABEL_MASK << SIBLING_SHIFT)) | (x << SIBLING_SHIFT);
  }

 private:
  UInt32 value_;

  static const UInt32 IS_OFFSET_FLAG = 1U << 31;
  static const UInt32 IS_PHANTOM_FLAG = 1U << 30;
  static const UInt32 NEXT_SHIFT = 9;
  static const UInt32 PREV_SHIFT = 18;
  static const UInt32 CHILD_SHIFT = 9;
  static const UInt32 SIBLING_SHIFT = 18;
};

}  // namespace dat
}  // namespace grn
