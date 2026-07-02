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

#include "duckdb_manager.h"

#include <my_global.h>
#include "mysqld.h"
#include "log.h"
#include "duckdb_config.h"

#undef UNKNOWN

#include "cross_engine_scan.h"
#include "duckdb_mysql_compat.h"

namespace myduck
{

DuckdbManager *DuckdbManager::m_instance= nullptr;

DuckdbManager::DuckdbManager() : m_database(nullptr) {}

bool DuckdbManager::Initialize()
{
  if (m_database != nullptr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_database != nullptr)
    return false;

  duckdb::DBConfig config;

  config.options.use_direct_io= global_use_dio;

  /*
    Pin the on-disk storage format version to v1.5.2 for newly created
    databases.  Modern column compression schemes (e.g. DICT_FSST) are gated
    behind the storage version; with the conservative default DuckDB falls
    back to the legacy separate Dictionary/FSST encodings, which makes the
    database file substantially larger.  For a freshly created database
    (no ATTACH storage_version override) StorageManager derives the storage
    version from this option.
  */
  config.options.serialization_compatibility=
      duckdb::SerializationCompatibility::FromString("v1.5.2");

  if (global_max_threads != 0)
    config.options.maximum_threads= global_max_threads;

  /*
    When memory_limit sysvar is 0 (default), DuckDB tries to auto-detect
    available RAM (80%).  Inside some Docker/cgroup environments this
    detection fails and returns 0, making DuckDB unable to allocate
    anything.  Use an explicit 1 GB fallback in that case.
  */
  static constexpr uint64_t DUCKDB_DEFAULT_MEMORY_FALLBACK= 1ULL
                                                            << 30; /* 1 GB */
  if (global_memory_limit != 0)
    config.options.maximum_memory= global_memory_limit;
  else
    config.options.maximum_memory= DUCKDB_DEFAULT_MEMORY_FALLBACK;

  if (global_max_temp_directory_size != 0)
    config.options.maximum_swap_space= global_max_temp_directory_size;

  config.options.checkpoint_wal_size= checkpoint_threshold;

  /* Temp directory: user-specified or default (data directory) */
  {
    char tmp_path[FN_REFLEN];
    if (global_duckdb_temp_directory && global_duckdb_temp_directory[0])
      config.options.temporary_directory= global_duckdb_temp_directory;
    else
    {
      fn_format(tmp_path, DUCKDB_DEFAULT_TMP_NAME, mysql_real_data_home, "",
                MYF(0));
      config.options.temporary_directory= tmp_path;
    }
  }

  /* Store all tables in one file in the data directory */
  char path[FN_REFLEN];
  fn_format(path, DUCKDB_FILE_NAME, mysql_real_data_home, "", MYF(0));

  try
  {
    m_database= new duckdb::DuckDB(path, &config);
  }
  catch (const std::exception &e)
  {
    sql_print_error("DuckDB: failed to open database at '%s': %s", path,
                    e.what());
    m_database= nullptr;
    return true;
  }
  catch (...)
  {
    sql_print_error("DuckDB: failed to open database at '%s': "
                    "unknown exception",
                    path);
    m_database= nullptr;
    return true;
  }

  /* Enable autoloading of statically-linked extensions (core_functions etc.) */
  {
    auto con= std::make_shared<duckdb::Connection>(*m_database);
    con->Query("SET autoload_known_extensions=true");
    con->Query("SET autoinstall_known_extensions=true");

    /*
      Register MariaDB-compatible SQL macros for functions that DuckDB
      lacks but MariaDB pushes down via the original query text.
    */
    con->Query("CREATE OR REPLACE MACRO adddate(d, i) AS d + i");
    /* addtime/subtime registered as C++ UDFs */
    /* curdate/curtime — MariaDB aliases */
    /* datediff(d1, d2) — MariaDB returns days, DuckDB needs 3-arg form */
    con->Query("CREATE OR REPLACE MACRO datediff(d1, d2) AS "
               "(d1::DATE - d2::DATE)");
    con->Query("CREATE OR REPLACE MACRO curdate() AS current_date");
    con->Query("CREATE OR REPLACE MACRO curtime(fsp := 0) AS current_time");
    /* convert_tz(ts, from_tz, to_tz) */
    con->Query("CREATE OR REPLACE MACRO convert_tz(ts, from_tz, to_tz) AS "
               "timezone(to_tz, timezone(from_tz, ts))");
    con->Query("CREATE OR REPLACE MACRO subdate(d, i) AS d - i");
    con->Query("CREATE OR REPLACE MACRO insert(str, pos, len, newstr) AS "
               "CASE WHEN pos < 1 OR pos > length(str) THEN str "
               "ELSE substr(str, 1, pos - 1) || newstr || "
               "substr(str, pos + len) END");
    /* to_base64 / from_base64 — DuckDB uses base64()/from_base64() */
    con->Query("CREATE OR REPLACE MACRO to_base64(x) AS "
               "base64(encode(x))");
    /* substring_index(str, delim, count) */
    con->Query("CREATE OR REPLACE MACRO substring_index(s, d, c) AS "
               "CASE WHEN c > 0 THEN "
               "array_to_string(list_slice(string_split(s, d), 1, c), d) "
               "WHEN c < 0 THEN "
               "array_to_string(list_slice(string_split(s, d), c, NULL), d) "
               "ELSE '' END");
    /* strcmp(s1, s2) — returns 0, -1 or 1 */
    con->Query("CREATE OR REPLACE MACRO strcmp(a, b) AS "
               "CASE WHEN a = b THEN 0 WHEN a < b THEN -1 ELSE 1 END");
    /* MID() registered as C++ UDF in register_mysql_compat_functions() */
    /* oct, bin, hex, locate are now registered as native C++ scalar functions
       in register_mysql_compat_functions() -- no SQL macros needed. */
  }

  /* Register MySQL-compatible function overloads */
  register_mysql_compat_functions(*m_database->instance);

  /* Register cross-engine scan support (_mdb_scan + replacement scan) */
  register_cross_engine_scan(*m_database->instance);

  sql_print_information("DuckDB: DuckdbManager::Initialize succeed, path=%s",
                        path);

  return false;
}

bool DuckdbManager::CreateInstance()
{
  DBUG_ASSERT(m_instance == nullptr);
  m_instance= new DuckdbManager();
  if (m_instance == nullptr)
  {
    sql_print_error("DuckDB: DuckdbManager::CreateInstance failed");
    return true;
  }

  /* Eagerly initialize DuckDB so that errors are caught during plugin init
     rather than later when the first query arrives. */
  if (m_instance->Initialize())
  {
    sql_print_error("DuckDB: DuckdbManager::Initialize failed during "
                    "CreateInstance");
    delete m_instance;
    m_instance= nullptr;
    return true;
  }

  return false;
}

DuckdbManager::~DuckdbManager()
{
  if (m_database != nullptr)
  {
    try
    {
      delete m_database;
    }
    catch (...)
    {
      sql_print_error("DuckDB: exception during DuckDB database destruction");
    }
    m_database= nullptr;
  }
}

void DuckdbManager::Cleanup()
{
  if (m_instance == nullptr)
    return;
  delete m_instance;
  m_instance= nullptr;
}

DuckdbManager &DuckdbManager::Get()
{
  DBUG_ASSERT(m_instance != nullptr);
  return *m_instance;
}

std::shared_ptr<duckdb::Connection> DuckdbManager::CreateConnection()
{
  auto &instance= Get();
  auto connection= std::make_shared<duckdb::Connection>(*instance.m_database);
  return connection;
}

} // namespace myduck
