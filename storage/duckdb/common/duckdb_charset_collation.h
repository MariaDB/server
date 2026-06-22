/*
  Copyright (c) 2026, MariaDB Foundation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#pragma once

#include <string>

#include <my_global.h>
#include "m_ctype.h"

namespace myduck
{

/** Get the corresponding DuckDB collation according to MariaDB CHARSET_INFO.
  DuckDB's collation is not completely consistent with MariaDB.
  We only focus on the two behaviors of NOCASE and NOACCENT.
  @param[in]  cs        Pointer to MariaDB CHARSET_INFO structure
  @param[out] warn_msg  Warning message if there is any warning
  @return  DuckDB collation string
*/
std::string get_duckdb_collation(const CHARSET_INFO *cs,
                                 std::string &warn_msg);

/* Charsets other than utf8mb3 and utf8mb4 use POSIX Collation directly.
   DuckDB treats POSIX same as binary. We cannot use "binary" because it is
   a keyword, so we use POSIX instead. */
static const std::string COLLATION_BINARY= "POSIX";
static const std::string COLLATION_NOCASE= "NOCASE";
static const std::string COLLATION_NOCASE_NOACCENT= "NOCASE.NOACCENT";

} // namespace myduck
