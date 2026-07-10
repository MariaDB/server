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

#include "duckdb_manager.h"
#include "duckdb_query.h"
#include "delta_appender.h"

#include <my_global.h>
#include <my_bitmap.h>

class THD;
struct TABLE;

namespace myduck
{

enum class BatchState
{
  UNDEFINED= 0,
  NOT_IN_BATCH,
  IN_INSERT_ONLY_BATCH,
  IN_MIX_BATCH
};

class DuckdbThdContext
{
public:
  DuckdbThdContext() : batch_state(BatchState::UNDEFINED)
  {
    m_con= DuckdbManager::CreateConnection();
  }

  ~DuckdbThdContext()
  {
    if (has_transaction())
    {
      std::string error_msg;
      duckdb_trans_rollback(error_msg);
    }
  }

  bool has_transaction() { return m_con && m_con->HasActiveTransaction(); }

  bool duckdb_trans_begin()
  {
    if (!m_con || m_con->HasActiveTransaction())
      return true;
    auto result= duckdb_query(*m_con, "BEGIN");
    return result->HasError();
  }

  bool duckdb_trans_commit(std::string &error_msg)
  {
    if (!m_con)
      return true;

    if (m_con->HasActiveTransaction())
    {
      auto result= duckdb_query(*m_con, "COMMIT");
      if (result->HasError())
      {
        error_msg= result->GetError().c_str();
        return true;
      }
    }
    set_batch_state(BatchState::UNDEFINED);
    return false;
  }

  bool duckdb_trans_rollback(std::string &error_msg)
  {
    if (!m_con)
      return true;

    if (m_con->HasActiveTransaction())
    {
      auto result= duckdb_query(*m_con, "ROLLBACK");
      if (result->HasError())
      {
        error_msg= result->GetError().c_str();
        return true;
      }
    }
    set_batch_state(BatchState::UNDEFINED);
    return false;
  }

  duckdb::Connection &get_connection() { return *m_con; }

  /** Set DuckDB current schema (CREATE SCHEMA IF NOT EXISTS + USE). */
  void config_duckdb_env(const std::string &schema);

  /** Configure DuckDB session variables (timezone, optimizer flags) from THD.
   */
  void config_duckdb_session(THD *thd);

  void delete_appender(const std::string &schema, const std::string &table)
  {
    if (!m_appenders || m_appenders->is_empty())
      return;
    std::string db= schema, tb= table;
    m_appenders->delete_appender(db, tb);
  }

  bool flush_appenders(std::string &error_msg)
  {
    if (m_appenders && !m_appenders->is_empty())
    {
      if (m_appenders->flush_all(false, error_msg))
        return true;
    }
    set_batch_state(BatchState::UNDEFINED);
    return false;
  }

  DeltaAppender *get_appender(TABLE *table);

  int append_row_insert(TABLE *table, const MY_BITMAP *blob_map);

  int append_row_update(TABLE *table, const uchar *old_row);

  int append_row_delete(TABLE *table);

  void set_in_copy_ddl(bool in) { in_copy_ddl= in; }
  bool is_in_copy_ddl() const { return in_copy_ddl; }

  void set_batch_state(BatchState state) { batch_state= state; }
  BatchState get_batch_state() { return batch_state; }

private:
  std::shared_ptr<duckdb::Connection> m_con;
  bool in_copy_ddl= false;
  BatchState batch_state;
  std::unique_ptr<DeltaAppenders> m_appenders;

  /* Cached session variable values — propagated to DuckDB on change */
  ulonglong m_merge_join_threshold= 4611686018427387904ULL;
  ulonglong m_disabled_optimizers= 0;
  std::string m_explain_output_str;
  std::string m_current_schema;
  std::string m_current_timezone;
  std::string m_collation;
};

/** Return true (and push ER_XAER_RMFAIL) when THD is inside an XA
    transaction.  DuckDB does not support XA. */
bool reject_xa_if_active(THD *thd);

} // namespace myduck
