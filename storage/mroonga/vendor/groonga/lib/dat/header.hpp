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

class GRN_DAT_API Header {
 public:
  Header()
      : file_size_(0),
        total_key_length_(0),
        next_key_id_(grn::dat::MIN_KEY_ID),
        max_key_id_(0),
        num_keys_(0),
        max_num_keys_(0),
        num_phantoms_(0),
        num_zombies_(0),
        num_blocks_(0),
        max_num_blocks_(0),
        next_key_pos_(0),
        key_buf_size_(0),
        status_flags_(0) {
    for (UInt32 i = 0; i <= MAX_BLOCK_LEVEL; ++i) {
      leaders_[i] = INVALID_LEADER;
    }
    for (UInt32 i = 0; i < (sizeof(reserved_) / sizeof(*reserved_)); ++i) {
      reserved_[i] = 0;
    }
  }

  UInt64 file_size() const {
    return file_size_;
  }
  UInt32 total_key_length() const {
    return total_key_length_;
  }
  UInt32 min_key_id() const {
    return MIN_KEY_ID;
  }
  UInt32 next_key_id() const {
    return next_key_id_;
  }
  UInt32 max_key_id() const {
    return max_key_id_;
  }
  UInt32 num_keys() const {
    return num_keys_;
  }
  UInt32 max_num_keys() const {
    return max_num_keys_;
  }
  UInt32 num_nodes() const {
    return num_blocks() * BLOCK_SIZE;
  }
  UInt32 num_phantoms() const {
    return num_phantoms_;
  }
  UInt32 num_zombies() const {
    return num_zombies_;
  }
  UInt32 max_num_nodes() const {
    return max_num_blocks() * BLOCK_SIZE;
  }
  UInt32 num_blocks() const {
    return num_blocks_;
  }
  UInt32 max_num_blocks() const {
    return max_num_blocks_;
  }
  UInt32 next_key_pos() const {
    return next_key_pos_;
  }
  UInt32 key_buf_size() const {
    return key_buf_size_;
  }
  UInt32 status_flags() const {
    return status_flags_;
  }
  UInt32 ith_leader(UInt32 i) const {
    GRN_DAT_DEBUG_THROW_IF(i > MAX_BLOCK_LEVEL);
    return leaders_[i];
  }

  void set_file_size(UInt64 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_FILE_SIZE);
    file_size_ = x;
  }
  void set_total_key_length(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_TOTAL_KEY_LENGTH);
    total_key_length_ = x;
  }
  void set_next_key_id(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF((x - 1) > MAX_KEY_ID);
    next_key_id_ = x;
  }
  void set_max_key_id(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_KEY_ID);
    max_key_id_ = x;
  }
  void set_num_keys(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_NUM_KEYS);
    num_keys_ = x;
  }
  void set_max_num_keys(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_NUM_KEYS);
    max_num_keys_ = x;
  }
  void set_num_phantoms(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > max_num_nodes());
    num_phantoms_ = x;
  }
  void set_num_zombies(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > max_num_nodes());
    num_zombies_ = x;
  }
  void set_num_blocks(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > max_num_blocks());
    num_blocks_ = x;
  }
  void set_max_num_blocks(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_NUM_BLOCKS);
    max_num_blocks_ = x;
  }
  void set_next_key_pos(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > key_buf_size());
    next_key_pos_ = x;
  }
  void set_key_buf_size(UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(x > MAX_KEY_BUF_SIZE);
    key_buf_size_ = x;
  }
  void set_status_flags(UInt32 x) {
    status_flags_ = x;
  }
  void set_ith_leader(UInt32 i, UInt32 x) {
    GRN_DAT_DEBUG_THROW_IF(i > MAX_BLOCK_LEVEL);
    GRN_DAT_DEBUG_THROW_IF((x != INVALID_LEADER) && (x >= num_blocks()));
    leaders_[i] = x;
  }

 private:
  UInt64 file_size_;
  UInt32 total_key_length_;
  UInt32 next_key_id_;
  UInt32 max_key_id_;
  UInt32 num_keys_;
  UInt32 max_num_keys_;
  UInt32 num_phantoms_;
  UInt32 num_zombies_;
  UInt32 num_blocks_;
  UInt32 max_num_blocks_;
  UInt32 next_key_pos_;
  UInt32 key_buf_size_;
  UInt32 leaders_[MAX_BLOCK_LEVEL + 1];
  UInt32 status_flags_;
  UInt32 reserved_[12];
};

}  // namespace dat
}  // namespace grn
