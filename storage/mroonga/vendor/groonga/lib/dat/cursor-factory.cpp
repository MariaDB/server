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

#include "cursor-factory.hpp"
#include "id-cursor.hpp"
#include "key-cursor.hpp"
#include "prefix-cursor.hpp"
#include "predictive-cursor.hpp"

#include <new>

namespace grn {
namespace dat {

Cursor *CursorFactory::open(const Trie &trie,
                            const void *min_ptr, UInt32 min_length,
                            const void *max_ptr, UInt32 max_length,
                            UInt32 offset,
                            UInt32 limit,
                            UInt32 flags) {
  const UInt32 cursor_type = flags & CURSOR_TYPE_MASK;
  switch (cursor_type) {
    case ID_RANGE_CURSOR: {
      IdCursor *cursor = new (std::nothrow) IdCursor;
      GRN_DAT_THROW_IF(MEMORY_ERROR, cursor == NULL);
      try {
        cursor->open(trie, String(min_ptr, min_length),
                     String(max_ptr, max_length), offset, limit, flags);
      } catch (...) {
        delete cursor;
        throw;
      }
      return cursor;
    }
    case KEY_RANGE_CURSOR: {
      KeyCursor *cursor = new (std::nothrow) KeyCursor;
      GRN_DAT_THROW_IF(MEMORY_ERROR, cursor == NULL);
      try {
        cursor->open(trie, String(min_ptr, min_length),
                     String(max_ptr, max_length), offset, limit, flags);
      } catch (...) {
        delete cursor;
        throw;
      }
      return cursor;
    }
    case PREFIX_CURSOR: {
      PrefixCursor *cursor = new (std::nothrow) PrefixCursor;
      GRN_DAT_THROW_IF(MEMORY_ERROR, cursor == NULL);
      try {
        cursor->open(trie, String(max_ptr, max_length), min_length,
                     offset, limit, flags);
      } catch (...) {
        delete cursor;
        throw;
      }
      return cursor;
    }
    case PREDICTIVE_CURSOR: {
      PredictiveCursor *cursor = new (std::nothrow) PredictiveCursor;
      GRN_DAT_THROW_IF(MEMORY_ERROR, cursor == NULL);
      try {
        cursor->open(trie, String(min_ptr, min_length),
                     offset, limit, flags);
      } catch (...) {
        delete cursor;
        throw;
      }
      return cursor;
    }
    default: {
      GRN_DAT_THROW(PARAM_ERROR, "unknown cursor type");
    }
  }
}

}  // namespace dat
}  // namespace grn
