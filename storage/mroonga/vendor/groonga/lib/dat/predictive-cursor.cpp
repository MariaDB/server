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

#include "predictive-cursor.hpp"

#include <algorithm>
#include <cstring>

#include "trie.hpp"

namespace grn {
namespace dat {

PredictiveCursor::PredictiveCursor()
    : trie_(NULL),
      offset_(0),
      limit_(MAX_UINT32),
      flags_(PREDICTIVE_CURSOR),
      buf_(),
      cur_(0),
      end_(0),
      min_length_(0) {}

PredictiveCursor::~PredictiveCursor() {}

void PredictiveCursor::open(const Trie &trie,
                            const String &str,
                            UInt32 offset,
                            UInt32 limit,
                            UInt32 flags) {
  GRN_DAT_THROW_IF(PARAM_ERROR, (str.ptr() == NULL) && (str.length() != 0));

  flags = fix_flags(flags);
  PredictiveCursor new_cursor(trie, offset, limit, flags);
  new_cursor.init(str);
  new_cursor.swap(this);
}

void PredictiveCursor::close() {
  PredictiveCursor new_cursor;
  new_cursor.swap(this);
}

const Key &PredictiveCursor::next() {
  if (cur_ == end_) {
    return Key::invalid_key();
  }

  if ((flags_ & ASCENDING_CURSOR) == ASCENDING_CURSOR) {
    return ascending_next();
  } else {
    return descending_next();
  }
}

PredictiveCursor::PredictiveCursor(const Trie &trie,
                                   UInt32 offset, UInt32 limit, UInt32 flags)
    : trie_(&trie),
      offset_(offset),
      limit_(limit),
      flags_(flags),
      buf_(),
      cur_(0),
      end_(0),
      min_length_(0) {}

UInt32 PredictiveCursor::fix_flags(UInt32 flags) const {
  const UInt32 cursor_type = flags & CURSOR_TYPE_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR, (cursor_type != 0) &&
                                (cursor_type != PREDICTIVE_CURSOR));
  flags |= PREDICTIVE_CURSOR;

  const UInt32 cursor_order = flags & CURSOR_ORDER_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR, (cursor_order != 0) &&
                                (cursor_order != ASCENDING_CURSOR) &&
                                (cursor_order != DESCENDING_CURSOR));
  if (cursor_order == 0) {
    flags |= ASCENDING_CURSOR;
  }

  const UInt32 cursor_options = flags & CURSOR_OPTIONS_MASK;
  GRN_DAT_THROW_IF(PARAM_ERROR, cursor_options & ~(EXCEPT_EXACT_MATCH));

  return flags;
}

void PredictiveCursor::init(const String &str) {
  if (limit_ == 0) {
    return;
  }

  min_length_ = str.length();
  if ((flags_ & EXCEPT_EXACT_MATCH) == EXCEPT_EXACT_MATCH) {
    ++min_length_;
  }
  end_ = (offset_ > (MAX_UINT32 - limit_)) ? MAX_UINT32 : (offset_ + limit_);

  UInt32 node_id = ROOT_NODE_ID;
  for (UInt32 i = 0; i < str.length(); ++i) {
    const Base base = trie_->ith_node(node_id).base();
    if (base.is_linker()) {
      if (offset_ == 0) {
        const Key &key = trie_->get_key(base.key_pos());
        if ((key.length() >= str.length()) &&
            (key.str().substr(0, str.length()).compare(str, i) == 0)) {
          if ((flags_ & ASCENDING_CURSOR) == ASCENDING_CURSOR) {
            node_id |= IS_ROOT_FLAG;
          }
          buf_.push_back(node_id);
        }
      }
      return;
    }

    node_id = base.offset() ^ str[i];
    if (trie_->ith_node(node_id).label() != str[i]) {
      return;
    }
  }

  if ((flags_ & ASCENDING_CURSOR) == ASCENDING_CURSOR) {
    node_id |= IS_ROOT_FLAG;
  }
  buf_.push_back(node_id);
}

void PredictiveCursor::swap(PredictiveCursor *cursor) {
  std::swap(trie_, cursor->trie_);
  std::swap(offset_, cursor->offset_);
  std::swap(limit_, cursor->limit_);
  std::swap(flags_, cursor->flags_);
  buf_.swap(&cursor->buf_);
  std::swap(cur_, cursor->cur_);
  std::swap(end_, cursor->end_);
  std::swap(min_length_, cursor->min_length_);
}

const Key &PredictiveCursor::ascending_next() {
  while (!buf_.empty()) {
    const bool is_root = (buf_.back() & IS_ROOT_FLAG) == IS_ROOT_FLAG;
    const UInt32 node_id = buf_.back() & ~IS_ROOT_FLAG;
    buf_.pop_back();

    const Node node = trie_->ith_node(node_id);
    if (!is_root && (node.sibling() != INVALID_LABEL)) {
      buf_.push_back(node_id ^ node.label() ^ node.sibling());
    }

    if (node.is_linker()) {
      const Key &key = trie_->get_key(node.key_pos());
      if (key.length() >= min_length_) {
        if (cur_++ >= offset_) {
          return key;
        }
      }
    } else if (node.child() != INVALID_LABEL) {
      buf_.push_back(node.offset() ^ node.child());
    }
  }
  return Key::invalid_key();
}

const Key &PredictiveCursor::descending_next() {
  while (!buf_.empty()) {
    const bool post_order = (buf_.back() & POST_ORDER_FLAG) == POST_ORDER_FLAG;
    const UInt32 node_id = buf_.back() & ~POST_ORDER_FLAG;

    const Base base = trie_->ith_node(node_id).base();
    if (post_order) {
      buf_.pop_back();
      if (base.is_linker()) {
        const Key &key = trie_->get_key(base.key_pos());
        if (key.length() >= min_length_) {
          if (cur_++ >= offset_) {
            return key;
          }
        }
      }
    } else {
      buf_.back() |= POST_ORDER_FLAG;
      UInt16 label = trie_->ith_node(node_id).child();
      while (label != INVALID_LABEL) {
        buf_.push_back(base.offset() ^ label);
        label = trie_->ith_node(base.offset() ^ label).sibling();
      }
    }
  }
  return Key::invalid_key();
}

}  // namespace dat
}  // namespace grn
