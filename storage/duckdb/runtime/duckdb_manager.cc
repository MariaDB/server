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

#define MYSQL_SERVER 1
#include <my_global.h>
#include <unistd.h>
#include "mysqld.h"
#include "sys_vars_shared.h"
#include "log.h"
#include "duckdb_config.h"

#undef UNKNOWN

#include "cross_engine_scan.h"
#include "duckdb_mysql_compat.h"

namespace myduck
{

DuckdbManager *DuckdbManager::m_instance= nullptr;

DuckdbManager::DuckdbManager() : m_database(nullptr) {}

namespace
{

/* Total physical memory of the host, in bytes (0 if it cannot be detected). */
uint64_t duckdb_physical_memory()
{
  long pages= sysconf(_SC_PHYS_PAGES);
  long page_size= sysconf(_SC_PAGESIZE);
  if (pages > 0 && page_size > 0)
    return (uint64_t) pages * (uint64_t) page_size;
  return 0;
}

/* Read a global integer system variable by name (0 if not found/NULL). */
uint64_t read_global_ull_sysvar(const char *name, size_t len)
{
  sys_var *var= intern_find_sys_var(name, len);
  if (!var)
    return 0;
  const LEX_CSTRING base= {nullptr, 0};
  bool is_null= false;
  longlong v= var->val_int(&is_null, current_thd, OPT_GLOBAL, &base);
  return (is_null || v < 0) ? 0 : (uint64_t) v;
}

/*
  Estimate the memory InnoDB may reserve for its buffer pool.
  innodb_buffer_pool_size_max is the address space InnoDB pre-reserves for the
  (dynamically resizable) buffer pool, i.e. the upper bound of its cache
  memory.  When left at its huge default (~8TB of PROT_NONE mapping, larger
  than physical RAM) it is not a meaningful budget figure, so fall back to the
  currently committed innodb_buffer_pool_size in that case.
*/
uint64_t innodb_reserved_memory(uint64_t phys_mem)
{
  uint64_t bp_max=
      read_global_ull_sysvar(STRING_WITH_LEN("innodb_buffer_pool_size_max"));
  if (bp_max == 0 || bp_max >= phys_mem)
    bp_max=
        read_global_ull_sysvar(STRING_WITH_LEN("innodb_buffer_pool_size"));
  return bp_max;
}

} // anonymous namespace

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
    When memory_limit sysvar is 0 (default), size DuckDB's memory as 80% of
    the RAM left after InnoDB's reserved buffer-pool memory:

        memory_limit = 0.8 * (physical_RAM - innodb_buffer_pool_size_max)

    Fall back to an explicit 1 GB if detection is unavailable (e.g. inside
    some Docker/cgroup environments) or the computed value is too small.
  */
  static constexpr uint64_t DUCKDB_DEFAULT_MEMORY_FALLBACK= 1ULL
                                                            << 30; /* 1 GB */
  if (global_memory_limit != 0)
    config.options.maximum_memory= global_memory_limit;
  else
  {
    uint64_t phys_mem= duckdb_physical_memory();
    uint64_t innodb_mem= innodb_reserved_memory(phys_mem);
    uint64_t avail= (phys_mem > innodb_mem)
                        ? (uint64_t) ((phys_mem - innodb_mem) * 0.8)
                        : 0;
    config.options.maximum_memory= (avail > DUCKDB_DEFAULT_MEMORY_FALLBACK)
                                       ? avail
                                       : DUCKDB_DEFAULT_MEMORY_FALLBACK;
    sql_print_information(
        "DuckDB: auto memory_limit=%llu bytes "
        "(physical RAM=%llu, InnoDB reserved=%llu)",
        (unsigned long long) config.options.maximum_memory,
        (unsigned long long) phys_mem, (unsigned long long) innodb_mem);
  }

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
    /* utc_time/utc_timestamp/utc_date — UTC wall-clock; fsp ignored */
    con->Query("CREATE OR REPLACE MACRO utc_timestamp(fsp := 0) AS "
               "timezone('UTC', now())::TIMESTAMP");
    con->Query("CREATE OR REPLACE MACRO utc_time(fsp := 0) AS "
               "timezone('UTC', now())::TIME");
    con->Query("CREATE OR REPLACE MACRO utc_date() AS "
               "timezone('UTC', now())::DATE");
    /* unix_timestamp([ts]) — epoch seconds; no arg = now() */
    con->Query("CREATE OR REPLACE MACRO unix_timestamp(ts := now()) AS "
               "epoch(ts)::BIGINT");
    /* time_to_sec(t) — seconds since midnight */
    con->Query("CREATE OR REPLACE MACRO time_to_sec(t) AS "
               "(date_part('hour', t)*3600 + date_part('minute', t)*60 "
               "+ date_part('second', t))::BIGINT");
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
