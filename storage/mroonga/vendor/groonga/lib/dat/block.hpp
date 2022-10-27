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

class GRN_DAT_API Block {
 public:
  Block() : next_(0), prev_(0), first_phantom_(0), num_phantoms_(0) {}

  // Blocks in the same level are stored in a doubly-linked list which is
  // represented by the following next() and prev().
  UInt32 next() const {
    return next_ / BLOCK_SIZE;
  }
  UInt32 prev() const {
    return prev_ / BLOCK_SIZE;
  }

  // A level indicates how easyily find_offset() can find a good offset in that
  // block. It is easier in lower level blocks.
  UInt32 level() const {
    return next_ & BLOCK_MASK;
  }
  // A block level rises when find_offset() fails to find a good offset
  // MAX_FAILURE_COUNT times in that block.
  UInt32 failure_count() const {
    return prev_ & BLOCK_MASK;
  }

  UInt32 first_phantom() const {
    return first_phantom_;
  }
  UInt32 num_phantoms() const {
    return num_phantoms_;
  }

  void set_next(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_BLOCK_ID);
    next_ = (next_ & BLOCK_MASK) | (x * BLOCK_SIZE);
  }
  void set_prev(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_BLOCK_ID);
    prev_ = (prev_ & BLOCK_MASK) | (x * BLOCK_SIZE);
  }

  void set_level(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_BLOCK_LEVEL);
    GRN_DAT_DEBUG_THROW_IF(x > BLOCK_MASK);
    next_ = (next_ & ~BLOCK_MASK) | x;
  }
  void set_failure_count(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_FAILURE_COUNT);
    GRN_DAT_DEBUG_THROW_IF(x > BLOCK_MASK);
    prev_ = (prev_ & ~BLOCK_MASK) | x;
  }

  void set_first_phantom(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x >= BLOCK_SIZE);
    first_phantom_ = (UInt16)x;
  }
  void set_num_phantoms(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > BLOCK_SIZE);
    num_phantoms_ = (UInt16)x;
  }

 private:
  UInt32 next_;
  UInt32 prev_;
  UInt16 first_phantom_;
  UInt16 num_phantoms_;
};

}  // namespace dat
}  // namespace grn
