/*
  Copyright (c) 2026, MariaDB Foundation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#pragma once

namespace duckdb
{
class DatabaseInstance;
}

namespace myduck
{

/**
  Register DuckDB scalar function overloads for MariaDB compatibility.
  Called once during DuckdbManager::Initialize().
*/
void register_mysql_compat_functions(duckdb::DatabaseInstance &db);

} /* namespace myduck */
