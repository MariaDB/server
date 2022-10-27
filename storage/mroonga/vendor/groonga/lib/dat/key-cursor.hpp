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

#include "cursor.hpp"
#include "vector.hpp"

namespace grn {
namespace dat {

class Trie;

class GRN_DAT_API KeyCursor : public Cursor {
 public:
  KeyCursor();
  ~KeyCursor();

  void open(const Trie &trie,
            const String &min_str,
            const String &max_str,
            UInt32 offset = 0,
            UInt32 limit = MAX_UINT32,
            UInt32 flags = 0);

  void close();

  const Key &next();

  UInt32 offset() const {
    return offset_;
  }
  UInt32 limit() const {
    return limit_;
  }
  UInt32 flags() const {
    return flags_;
  }

 private:
  const Trie *trie_;
  UInt32 offset_;
  UInt32 limit_;
  UInt32 flags_;

  Vector<UInt32> buf_;
  UInt32 count_;
  UInt32 max_count_;
  bool finished_;
  UInt8 *end_buf_;
  String end_str_;

  KeyCursor(const Trie &trie,
            UInt32 offset, UInt32 limit, UInt32 flags);

  UInt32 fix_flags(UInt32 flags) const;
  void init(const String &min_str, const String &max_str);
  void ascending_init(const String &min_str, const String &max_str);
  void descending_init(const String &min_str, const String &max_str);
  void swap(KeyCursor *cursor);

  const Key &ascending_next();
  const Key &descending_next();

  static const UInt32 POST_ORDER_FLAG = 0x80000000U;

  // Disallows copy and assignment.
  KeyCursor(const KeyCursor &);
  KeyCursor &operator=(const KeyCursor &);
};

}  // namespace dat
}  // namespace grn
