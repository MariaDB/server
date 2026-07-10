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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "m_ctype.h"

#include "duckdb_charset_collation.h"

namespace myduck
{

std::string get_duckdb_collation(const CHARSET_INFO *cs, std::string &warn_msg)
{
  /* DuckDB stores all strings as UTF-8 internally and uses ICU for
     collation support.  The source MariaDB charset does not matter —
     only the collation behavior (case/accent sensitivity) is relevant.
     Map using CHARSET_INFO flags which work for any charset. */

  if (strcmp(cs->cs_name.str, "utf8mb3") &&
      strcmp(cs->cs_name.str, "utf8mb4") && strcmp(cs->cs_name.str, "ascii"))
  {
    return COLLATION_BINARY;
  }

  /* _bin Collation */
  if (cs->state & MY_CS_BINSORT)
    return COLLATION_BINARY;

  /* utf8mb3_tolower_ci is _as_ci actually */
  if (cs->state & MY_CS_LOWER_SORT)
    return COLLATION_NOCASE;

  /* In MariaDB levels_for_order is a bitmask of weight levels:
     bit 0 (primary)   - always set
     bit 1 (secondary) - accent sensitivity
     bit 2 (tertiary)  - case sensitivity
     DuckDB only distinguishes case and accent, so map accordingly. */
  bool accent_sensitive=
      cs->levels_for_order & (1 << MY_CS_LEVEL_BIT_SECONDARY);
  bool case_sensitive= cs->levels_for_order & (1 << MY_CS_LEVEL_BIT_TERTIARY);

  /* _ai_ci Collation (e.g. utf8mb4_0900_ai_ci, latin1_swedish_ci) */
  if (!case_sensitive && !accent_sensitive)
    return COLLATION_NOCASE_NOACCENT;

  /* _as_ci Collation (e.g. utf8mb4_0900_as_ci) */
  if (!case_sensitive)
    return COLLATION_NOCASE;

  /* _as_cs / _ai_cs Collation */
  return COLLATION_BINARY;
}

} // namespace myduck
