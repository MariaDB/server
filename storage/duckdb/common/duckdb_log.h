/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

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

#ifndef DUCKDB_LOG_H
#define DUCKDB_LOG_H

#include <my_global.h>

namespace myduck
{
extern ulonglong duckdb_log_options;

enum enum_duckdb_log_types
{
  DUCKDB_QUERY,
  DUCKDB_QUERY_RESULT
};

extern const char *duckdb_log_types[];
extern TYPELIB log_options_typelib;

#define LOG_DUCKDB_QUERY (1ULL << myduck::enum_duckdb_log_types::DUCKDB_QUERY)
#define LOG_DUCKDB_QUERY_RESULT                                               \
  (1ULL << myduck::enum_duckdb_log_types::DUCKDB_QUERY_RESULT)
} // namespace myduck

#endif // DUCKDB_LOG_H
