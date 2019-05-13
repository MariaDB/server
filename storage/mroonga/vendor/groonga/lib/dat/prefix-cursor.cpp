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

#include "prefix-cursor.hpp"

#include <algorithm>

#include "trie.hpp"

namespace grn {
namespace dat {

PrefixCursor::PrefixCursor()
    : trie_(NULL),
      offset_(0),
      limit_(MAX_UINT32),
      flags_(PREFIX_CURSOR),
      buf_(),
      cur_(0),
      end_(0) {}

PrefixCursor::~PrefixCursor() {}

void PrefixCursor::open(const Trie &trie,
                        const String &str,
                        UInt32 min_length,
                        UInt32 offset,
                        UInt32 limit,
                        UInt32 flags) {
  GRN_DAT_THROW_IF(PARAM_ERROR, (str.ptr() == NULL) && (str.length() != 0));
  GRN_DAT_THROW_IF(PARAM_ERROR, min_length > str.length());

  flags = fix_flags(flags);
  PrefixCursor new_cursor(trie, offset, limit, flags);
  new_cursor.init(str, min_length);
  new_cursor.swap(this);
}

void PrefixCursor::close() {
  PrefixCursor new_cursor;
  new_cursor.swap(this);
}

const Key &PrefixCursor::next() {
  if (cur_ == end_) {
    return Key::invalid_key();
  }
  if ((flags_ & ASCENDING_CURSOR) == ASCENDING_CURSOR) {
    return trie_->get_key(buf_[cur_++]);
  } else {
    return trie_->get_key(buf_[--cur_]);
  }
}

PrefixCursor::PrefixCursor(const Trie &trie,
                           UInt32 offset, UInt32 limit, UInt32 flags)
    : trie_(&trie),
      offset_(offset),
      limit_(limit),
      flags_(flags),
      buf_(),
      cur_(0),
      end_(0) {}

UInt32 PrefixCursor::fix_flags(UInt32 flags) const {
  const UInt32 cursor_type = flags & CURSOR_TYPE_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR, (cursor_type != 0) &&
                                (cursor_type != PREFIX_CURSOR));
  flags |= PREFIX_CURSOR;

  const UInt32 cursor_order = flags & CURSOR_ORDER_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR, (cursor_order != 0) &&
                                (cursor_order != ASCENDING_CURSOR) &&
                                (cursor_order != DESCENDING_CURSOR));
  if (cursor_order == 0) {
    flags |= ASCENDING_CURSOR;
  }

  const UInt32 cursor_options = flags & CURSOR_OPTIONS_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR, cursor_options & ~EXCEPT_EXACT_MATCH);

  return flags;
}

void PrefixCursor::init(const String &str, UInt32 min_length) {
  if ((limit_ == 0) || (offset_ > (str.length() - min_length))) {
    return;
  }

  UInt32 node_id = ROOT_NODE_ID;
  UInt32 i;
  for (i = 0; i < str.length(); ++i) {
    const Base base = trie_->ith_node(node_id).base();
    if (base.is_linker()) {
      const Key &key = trie_->get_key(base.key_pos());
      if ((key.length() >= min_length) && (key.length() <= str.length()) &&
          (str.substr(0, key.length()).compare(key.str(), i) == 0) &&
          ((key.length() < str.length()) ||
           ((flags_ & EXCEPT_EXACT_MATCH) != EXCEPT_EXACT_MATCH))) {
        buf_.push_back(base.key_pos());
      }
      break;
    }

    if ((i >= min_length) &&
        (trie_->ith_node(node_id).child() == TERMINAL_LABEL)) {
      const Base linker_base =
          trie_->ith_node(base.offset() ^ TERMINAL_LABEL).base();
      if (linker_base.is_linker()) {
        buf_.push_back(linker_base.key_pos());
      }
    }

    node_id = base.offset() ^ str[i];
    if (trie_->ith_node(node_id).label() != str[i]) {
      break;
    }
  }

  if ((i == str.length()) &&
      ((flags_ & EXCEPT_EXACT_MATCH) != EXCEPT_EXACT_MATCH)) {
    const Base base = trie_->ith_node(node_id).base();
    if (base.is_linker()) {
      const Key &key = trie_->get_key(base.key_pos());
      if ((key.length() >= min_length) && (key.length() <= str.length())) {
        buf_.push_back(base.key_pos());
      }
    } else if (trie_->ith_node(node_id).child() == TERMINAL_LABEL) {
      const Base linker_base =
          trie_->ith_node(base.offset() ^ TERMINAL_LABEL).base();
      if (linker_base.is_linker()) {
        buf_.push_back(linker_base.key_pos());
      }
    }
  }

  if (buf_.size() <= offset_) {
    return;
  }

  if ((flags_ & ASCENDING_CURSOR) == ASCENDING_CURSOR) {
    cur_ = offset_;
    end_ = (limit_ < (buf_.size() - cur_)) ? (cur_ + limit_) : buf_.size();
  } else {
    cur_ = buf_.size() - offset_;
    end_ = (limit_ < cur_) ? (cur_ - limit_) : 0;
  }
}

void PrefixCursor::swap(PrefixCursor *cursor) {
  std::swap(trie_, cursor->trie_);
  std::swap(offset_, cursor->offset_);
  std::swap(limit_, cursor->limit_);
  std::swap(flags_, cursor->flags_);
  buf_.swap(&cursor->buf_);
  std::swap(cur_, cursor->cur_);
  std::swap(end_, cursor->end_);
}

}  // namespace dat
}  // namespace grn
