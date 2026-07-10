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

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/function/replacement_scan.hpp"

#include <string>

struct TABLE;

namespace duckdb
{
class DatabaseInstance;
}

namespace myduck
{

/**
  Describes a non-DuckDB table that participates in a cross-engine query.
  Populated by the select_handler before executing the DuckDB query.
*/
struct ExternalTableInfo
{
  TABLE *table;           /* opened MariaDB TABLE with valid handler */
  std::string table_name; /* unqualified table name */
};

/**
  Thread-local registry of external tables available for the current query.
  Set before Connection::Query(), cleared after the query completes.
  The replacement scan callback reads this to decide which tables to redirect
  to the _mdb_scan table function.
*/
void register_external_table(const std::string &name, TABLE *table);
void register_external_where(const std::string &name,
                             const std::string &where_sql);
std::string find_external_where(const std::string &name);
void clear_external_tables();
TABLE *find_external_table(const std::string &name);

/**
  DuckDB replacement scan callback.
  When DuckDB cannot find a table in its catalog, this callback checks the
  thread-local registry.  If the table is registered, it returns a
  TableFunctionRef pointing to _mdb_scan('table_name').
*/
duckdb::unique_ptr<duckdb::TableRef> mariadb_replacement_scan(
    duckdb::ClientContext &context, duckdb::ReplacementScanInput &input,
    duckdb::optional_ptr<duckdb::ReplacementScanData> data);

/**
  Register the _mdb_scan table function and the replacement scan callback
  with the DuckDB instance.  Called once during DuckdbManager::Initialize().
*/
void register_cross_engine_scan(duckdb::DatabaseInstance &db);

} /* namespace myduck */
