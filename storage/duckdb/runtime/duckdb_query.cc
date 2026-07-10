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

#include "duckdb_query.h"
#include "duckdb/common/exception.hpp"
#include "duckdb_context.h"
#include "duckdb_manager.h"
#include "duckdb_log.h"

extern handlerton *duckdb_hton;

namespace myduck
{

static std::string backticks_to_double_quotes(const std::string &sql)
{
  std::string out(sql);
  for (auto &ch : out)
    if (ch == '`')
      ch= '"';
  return out;
}

duckdb::unique_ptr<duckdb::MaterializedQueryResult>
duckdb_query(duckdb::Connection &connection, const std::string &query)
{
  const std::string q= backticks_to_double_quotes(query);

  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
    sql_print_information("DuckDB query: %s", q.c_str());

  try
  {
    auto res= connection.Query(q);

    if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY_RESULT)
    {
      if (res->HasError())
        sql_print_information("DuckDB error: %s", res->GetError().c_str());
    }
    return res;
  }
  catch (duckdb::Exception &e)
  {
    auto result= duckdb::make_uniq<duckdb::MaterializedQueryResult>(
        duckdb::ErrorData(e.what()));
    return result;
  }
  catch (std::exception &e)
  {
    auto result= duckdb::make_uniq<duckdb::MaterializedQueryResult>(
        duckdb::ErrorData(e.what()));
    return result;
  }
}

static std::string get_thd_schema(THD *thd)
{
  if (thd->db.str && thd->db.length > 0)
    return std::string(thd->db.str, thd->db.length);
  return {};
}

duckdb::unique_ptr<duckdb::MaterializedQueryResult>
duckdb_query(THD *thd, const std::string &query, bool need_config)
{
  auto *ctx=
      static_cast<DuckdbThdContext *>(thd_get_ha_data(thd, duckdb_hton));
  if (!ctx)
  {
    ctx= new DuckdbThdContext();
    thd_set_ha_data(thd, duckdb_hton, ctx);
  }

  if (need_config)
  {
    ctx->config_duckdb_env(get_thd_schema(thd));
    ctx->config_duckdb_session(thd);
  }

  return duckdb_query(ctx->get_connection(), query);
}

duckdb::unique_ptr<duckdb::MaterializedQueryResult>
duckdb_query(const std::string &query)
{
  auto connection= DuckdbManager::CreateConnection();
  return duckdb_query(*connection, query);
}

} // namespace myduck
