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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"
#include "log.h"

#undef UNKNOWN

#include "duckdb_context.h"
#include "duckdb_charset_collation.h"
#include "duckdb_config.h"
#include "duckdb_types.h"
#include "duckdb_handler_errors.h"
#include "duckdb_timezone.h"

#include <vector>

namespace myduck
{

static void push_duckdb_warning(THD *thd, std::string &warn_msg)
{
  if (warn_msg.empty())
    return;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                      "DuckDB: %s", warn_msg.c_str());
  warn_msg.clear();
}

static std::string disabled_optimizers_to_string(ulonglong val)
{
  std::string result;
  for (uint i= 0; disabled_optimizers_names[i] != NullS; i++)
  {
    if (val & (1ULL << i))
    {
      if (!result.empty())
        result+= ',';
      result+= disabled_optimizers_names[i];
    }
  }
  return result;
}

void DuckdbThdContext::config_duckdb_env(const std::string &schema)
{
  if (schema.empty() || schema == m_current_schema)
    return;

  std::string sql1= "CREATE SCHEMA IF NOT EXISTS \"" + schema + "\"";
  std::string sql2= "USE \"" + schema + "\"";
  m_current_schema= schema;

  for (auto &sql : {sql1, sql2})
  {
    auto res= duckdb_query(get_connection(), sql);
    if (res && res->HasError())
      sql_print_warning("DuckDB: config_duckdb_env failed: %s (sql=%s)",
                        res->GetError().c_str(), sql.c_str());
  }
}

void DuckdbThdContext::config_duckdb_session(THD *thd)
{
  std::vector<std::string> config_sql;

  /* Timezone */
  std::string warn_msg;
  std::string tz_name= get_timezone_according_thd(thd, warn_msg);
  if (tz_name != m_current_timezone)
  {
    config_sql.push_back("SET TimeZone = '" + tz_name + "'");
    push_duckdb_warning(thd, warn_msg);
    m_current_timezone= tz_name;
  }

  /* Collation: force_no_collation overrides to binary (POSIX) */
  warn_msg.clear();
  std::string collation;
  if (get_thd_force_no_collation(thd))
    collation= COLLATION_BINARY;
  else
    collation=
        get_duckdb_collation(thd->variables.collation_connection, warn_msg);
  if (collation != m_collation)
  {
    config_sql.push_back("SET default_collation = '" + collation + "'");
    /* Only warn when collation is explicitly changed, not on initial setup */
    if (!m_collation.empty())
      push_duckdb_warning(thd, warn_msg);
    m_collation= collation;
  }

  /* merge_join_threshold (session) */
  ulonglong mjt= get_thd_merge_join_threshold(thd);
  if (mjt != m_merge_join_threshold)
  {
    config_sql.push_back("SET merge_join_threshold = " + std::to_string(mjt));
    m_merge_join_threshold= mjt;
  }

  /* disabled_optimizers (session) */
  ulonglong dopt= get_thd_disabled_optimizers(thd);
  if (dopt != m_disabled_optimizers)
  {
    std::string val_str= disabled_optimizers_to_string(dopt);
    config_sql.push_back("SET disabled_optimizers = '" + val_str + "'");
    m_disabled_optimizers= dopt;
  }

  /* explain_output (session, only when EXPLAIN) */
  ulong eo= get_thd_explain_output(thd);
  std::string eo_str= explain_output_names[eo];
  if (eo_str != m_explain_output_str)
  {
    config_sql.push_back("SET explain_output = '" + eo_str + "'");
    m_explain_output_str= eo_str;
  }

  /* Execute all config statements */
  for (auto &sql : config_sql)
  {
    auto res= duckdb_query(get_connection(), sql);
    if (res && res->HasError())
      sql_print_warning("DuckDB: config_duckdb_session failed: %s (sql=%s)",
                        res->GetError().c_str(), sql.c_str());
  }
}

DeltaAppender *DuckdbThdContext::get_appender(TABLE *table)
{
  if (!m_appenders)
    m_appenders= std::make_unique<DeltaAppenders>(m_con);

  DatabaseTableNames dt(table->s->normalized_path.str);
  std::string db= dt.db_name;
  std::string tb= dt.table_name;

  return m_appenders->get_appender(
      db, tb, batch_state == BatchState::IN_INSERT_ONLY_BATCH, table);
}

int DuckdbThdContext::append_row_insert(TABLE *table,
                                        const MY_BITMAP *blob_map)
{
  DeltaAppender *delta= get_appender(table);
  return delta ? delta->append_row_insert(table, 0, blob_map)
               : HA_DUCKDB_APPEND_ERROR;
}

int DuckdbThdContext::append_row_update(TABLE *table, const uchar *old_row)
{
  DeltaAppender *delta= get_appender(table);
  return delta ? delta->append_row_update(table, 0, old_row)
               : HA_DUCKDB_APPEND_ERROR;
}

int DuckdbThdContext::append_row_delete(TABLE *table)
{
  DeltaAppender *delta= get_appender(table);
  return delta ? delta->append_row_delete(table, 0) : HA_DUCKDB_APPEND_ERROR;
}

bool reject_xa_if_active(THD *thd)
{
  if (!thd->transaction->xid_state.is_explicit_XA())
    return false;

  static const char *xa_state_names[]= {"ACTIVE", "IDLE", "PREPARED",
                                         "ROLLBACK ONLY"};
  my_error(ER_XAER_RMFAIL, MYF(0),
           xa_state_names[thd->transaction->xid_state.get_state_code()]);
  return true;
}

} // namespace myduck
