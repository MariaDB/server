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

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/table_description.hpp"

namespace myduck
{

constexpr char DUCKDB_FILE_NAME[]= "duckdb.db";
constexpr char DUCKDB_DEFAULT_TMP_NAME[]= "duckdb_tmp";

class DuckdbManager
{
public:
  DuckdbManager(const DuckdbManager &)= delete;
  DuckdbManager &operator=(const DuckdbManager &)= delete;

  static bool CreateInstance();
  static void Cleanup();
  static inline DuckdbManager &Get();
  static std::shared_ptr<duckdb::Connection> CreateConnection();

private:
  static DuckdbManager *m_instance;

  DuckdbManager();
  ~DuckdbManager();

  bool Initialize();

  duckdb::DuckDB *m_database= nullptr;
  std::mutex m_mutex;
};

} // namespace myduck
