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

namespace grn {
namespace dat {

class Trie;

class GRN_DAT_API CursorFactory {
 public:
  static Cursor *open(const Trie &trie,
                      const void *min_ptr, UInt32 min_length,
                      const void *max_ptr, UInt32 max_length,
                      UInt32 offset = 0,
                      UInt32 limit = MAX_UINT32,
                      UInt32 flags = 0);

 private:
  // Disallows copy and assignment.
  CursorFactory(const CursorFactory &);
  CursorFactory &operator=(const CursorFactory &);
};

}  // namespace dat
}  // namespace grn
