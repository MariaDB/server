/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2011 Brazil

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

#include "id-cursor.hpp"

#include <algorithm>

#include "trie.hpp"

namespace grn {
namespace dat {

IdCursor::IdCursor()
    : trie_(NULL),
      offset_(0),
      limit_(MAX_UINT32),
      flags_(ID_RANGE_CURSOR),
      cur_(INVALID_KEY_ID),
      end_(INVALID_KEY_ID),
      count_(0) {}

IdCursor::~IdCursor() {}

void IdCursor::open(const Trie &trie,
                    const String &min_str,
                    const String &max_str,
                    UInt32 offset,
                    UInt32 limit,
                    UInt32 flags) {
  UInt32 min_id = INVALID_KEY_ID;
  if (min_str.ptr() != NULL) {
    UInt32 key_pos;
    GRN_DAT_THROW_IF(PARAM_ERROR,
                     !trie.search(min_str.ptr(), min_str.length(), &key_pos));
    min_id = trie.get_key(key_pos).id();
  }

  UInt32 max_id = INVALID_KEY_ID;
  if (max_str.ptr() != NULL) {
    UInt32 key_pos;
    GRN_DAT_THROW_IF(PARAM_ERROR,
                     !trie.search(max_str.ptr(), max_str.length(), &key_pos));
    max_id = trie.get_key(key_pos).id();
  }

  open(trie, min_id, max_id, offset, limit, flags);
}

void IdCursor::open(const Trie &trie,
                    UInt32 min_id,
                    UInt32 max_id,
                    UInt32 offset,
                    UInt32 limit,
                    UInt32 flags) {
  flags = fix_flags(flags);

  IdCursor new_cursor(trie, offset, limit, flags);
  new_cursor.init(min_id, max_id);
  new_cursor.swap(this);
}

void IdCursor::close() {
  IdCursor new_cursor;
  new_cursor.swap(this);
}

const Key &IdCursor::next() {
  if (count_ >= limit_) {
    return Key::invalid_key();
  }
  while (cur_ != end_) {
    const Key &key = trie_->ith_key(cur_);
    if ((flags_ & ASCENDING_CURSOR) == ASCENDING_CURSOR) {
      ++cur_;
    } else {
      --cur_;
    }
    if (key.is_valid()) {
      ++count_;
      return key;
    }
  }
  return Key::invalid_key();
}

IdCursor::IdCursor(const Trie &trie,
                   UInt32 offset,
                   UInt32 limit,
                   UInt32 flags)
    : trie_(&trie),
      offset_(offset),
      limit_(limit),
      flags_(flags),
      cur_(INVALID_KEY_ID),
      end_(INVALID_KEY_ID),
      count_(0) {}

UInt32 IdCursor::fix_flags(UInt32 flags) const {
  const UInt32 cursor_type = flags & CURSOR_TYPE_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR, (cursor_type != 0) &&
                                (cursor_type != ID_RANGE_CURSOR));
  flags |= ID_RANGE_CURSOR;

  const UInt32 cursor_order = flags & CURSOR_ORDER_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR, (cursor_order != 0) &&
                                (cursor_order != ASCENDING_CURSOR) &&
                                (cursor_order != DESCENDING_CURSOR));
  if (cursor_order == 0) {
    flags |= ASCENDING_CURSOR;
  }

  const UInt32 cursor_options = flags & CURSOR_OPTIONS_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR,
      cursor_options & ~(EXCEPT_LOWER_BOUND | EXCEPT_UPPER_BOUND));

  return flags;
}

void IdCursor::init(UInt32 min_id, UInt32 max_id) {
  if (min_id == INVALID_KEY_ID) {
    min_id = trie_->min_key_id();
  } else if ((flags_ & EXCEPT_LOWER_BOUND) == EXCEPT_LOWER_BOUND) {
    ++min_id;
  }

  if (max_id == INVALID_KEY_ID) {
    max_id = trie_->max_key_id();
  } else if ((flags_ & EXCEPT_UPPER_BOUND) == EXCEPT_UPPER_BOUND) {
    --max_id;
  }

  if ((max_id < min_id) || ((max_id - min_id) < offset_)) {
    return;
  }

  if ((flags_ & ASCENDING_CURSOR) == ASCENDING_CURSOR) {
    cur_ = min_id;
    end_ = max_id + 1;
    for (UInt32 i = 0; (i < offset_) && (cur_ != end_); ++i) {
      while (cur_ != end_) {
        if (trie_->ith_key(cur_++).is_valid()) {
          break;
        }
      }
    }
  } else {
    cur_ = max_id;
    end_ = min_id - 1;
    for (UInt32 i = 0; (i < offset_) && (cur_ != end_); ++i) {
      while (cur_ != end_) {
        if (trie_->ith_key(cur_--).is_valid()) {
          break;
        }
      }
    }
  }
}

void IdCursor::swap(IdCursor *cursor) {
  std::swap(trie_, cursor->trie_);
  std::swap(offset_, cursor->offset_);
  std::swap(limit_, cursor->limit_);
  std::swap(flags_, cursor->flags_);
  std::swap(cur_, cursor->cur_);
  std::swap(end_, cursor->end_);
  std::swap(count_, cursor->count_);
}

}  // namespace dat
}  // namespace grn
