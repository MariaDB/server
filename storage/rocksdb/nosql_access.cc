/*
   Copyright (c) 2019, Facebook, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#define MYSQL_SERVER 1

/* This C++ file's header */
#include "./nosql_access.h"

/* C++ standard header files */
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

/* C standard header files */
#include <ctype.h>

/* MySQL header files */
#include "../../sql/item.h"
#include "../../sql/sql_base.h"
#include "../../sql/sql_class.h"
#include "../../sql/strfunc.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./ha_rocksdb_proto.h"
#include "./rdb_buff.h"
#include "./rdb_datadic.h"

namespace myrocks {

bool rocksdb_handle_single_table_select(THD * /* unused */,
                                        st_select_lex * /* unused */) {
  return false;
}

}  // namespace myrocks
