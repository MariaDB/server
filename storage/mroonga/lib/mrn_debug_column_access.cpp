/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "mrn_debug_column_access.hpp"

namespace mrn {
  DebugColumnAccess::DebugColumnAccess(TABLE *table, MY_BITMAP **bitmap)
    : table_(table),
      bitmap_(bitmap) {
#ifndef DBUG_OFF
    map_ = dbug_tmp_use_all_columns(table_, bitmap_);
#endif
  }

  DebugColumnAccess::~DebugColumnAccess() {
#ifndef DBUG_OFF
    dbug_tmp_restore_column_map(bitmap_, map_);
#endif
  }
}
