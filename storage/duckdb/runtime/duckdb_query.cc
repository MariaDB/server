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
#include "duckdb/main/pending_query_result.hpp"
#include "duckdb_context.h"
#include "duckdb_manager.h"
#include "duckdb_log.h"

#include <cctype>

extern handlerton *duckdb_hton;

namespace myduck
{

SqlRegionType scan_sql_region(const std::string &sql, size_t start,
                              bool backslash_escapes, size_t &end)
{
  end= start;
  if (start >= sql.size())
    return SqlRegionType::NONE;

  char c= sql[start];
  if (c == '/' && start + 1 < sql.size() && sql[start + 1] == '*')
  {
    size_t close= sql.find("*/", start + 2);
    if (close == std::string::npos)
    {
      end= sql.size();
      return SqlRegionType::UNTERMINATED;
    }
    end= close + 2;
    return SqlRegionType::COMMENT;
  }

  if (c == '#' ||
      (c == '-' && start + 1 < sql.size() && sql[start + 1] == '-' &&
       (start + 2 == sql.size() ||
        isspace(static_cast<unsigned char>(sql[start + 2])))))
  {
    size_t newline= sql.find('\n', start + (c == '#' ? 1 : 2));
    end= newline == std::string::npos ? sql.size() : newline + 1;
    return SqlRegionType::COMMENT;
  }

  if (c != '\'' && c != '"' && c != '`')
    return SqlRegionType::NONE;

  for (size_t i= start + 1; i < sql.size(); i++)
  {
    if (sql[i] == '\\' && backslash_escapes && i + 1 < sql.size())
    {
      i++;
      continue;
    }
    if (sql[i] != c)
      continue;
    if (i + 1 < sql.size() && sql[i + 1] == c)
    {
      i++;
      continue;
    }
    end= i + 1;
    return SqlRegionType::QUOTED;
  }

  end= sql.size();
  return SqlRegionType::UNTERMINATED;
}

bool mariadb_query_has_unsafe_quote_escape(THD *thd, const char *query,
                                            size_t length)
{
  if (!thd->backslash_escapes() || length == 0)
    return false;

  const std::string sql(query, length);
  const bool ansi_quotes= thd->variables.sql_mode & MODE_ANSI_QUOTES;
  for (size_t i= 0; i < sql.size();)
  {
    size_t duckdb_end;
    SqlRegionType duckdb_region= scan_sql_region(sql, i, false, duckdb_end);
    if (duckdb_region == SqlRegionType::COMMENT)
    {
      i= duckdb_end;
      continue;
    }

    const bool string_literal=
        sql[i] == '\'' || (sql[i] == '"' && !ansi_quotes);
    if (string_literal)
    {
      size_t mariadb_end;
      SqlRegionType mariadb_region=
          scan_sql_region(sql, i, true, mariadb_end);
      if (mariadb_region != duckdb_region || mariadb_end != duckdb_end)
        return true;
      i= mariadb_end;
      continue;
    }

    if (duckdb_region == SqlRegionType::QUOTED)
      i= duckdb_end;
    else if (duckdb_region == SqlRegionType::UNTERMINATED)
      return true;
    else
      i++;
  }
  return false;
}

/*
  Convert MariaDB's printed SQL (backtick-quoted identifiers) into DuckDB SQL
  (double-quoted identifiers).

  MariaDB delimits identifiers with backticks and doubles an embedded backtick;
  DuckDB delimits with double quotes and doubles an embedded double quote. A
  naive character-by-character swap breaks identifiers that contain a double
  quote (MDEV-40653) and also corrupts backticks that appear inside string
  literals. Walk the string instead: copy string literals and already
  double-quoted identifiers verbatim, and rewrite only backtick-delimited
  identifiers, escaping any embedded double quote.
*/
static std::string backticks_to_double_quotes(const std::string &sql)
{
  std::string out;
  out.reserve(sql.size());
  const size_t n= sql.size();
  size_t i= 0;

  while (i < n)
  {
    char c= sql[i];
    size_t end;
    SqlRegionType region= scan_sql_region(sql, i, false, end);

    if (region == SqlRegionType::COMMENT ||
        region == SqlRegionType::UNTERMINATED)
    {
      out.append(sql, i, end - i);
      i= end;
      continue;
    }

    /* Single-quoted string literal: copy verbatim ('' and \' escapes). */
    if (region == SqlRegionType::QUOTED && c == '\'')
    {
      out.append(sql, i, end - i);
      i= end;
      continue;
    }

    /* Already double-quoted identifier: copy verbatim ("" escape). */
    if (region == SqlRegionType::QUOTED && c == '"')
    {
      out.append(sql, i, end - i);
      i= end;
      continue;
    }

    /* Backtick identifier: rewrite as a double-quoted identifier. */
    if (region == SqlRegionType::QUOTED && c == '`')
    {
      out.push_back('"');
      for (i++; i + 1 < end; i++)
      {
        char d= sql[i];
        if (d == '`' && i + 2 < end && sql[i + 1] == '`')
        {
          out.push_back('`');
          i++;
          continue;
        }
        if (d == '"')
          out.push_back('"'); /* escape " inside a DuckDB identifier */
        out.push_back(d);
      }
      out.push_back('"');
      i= end;
      continue;
    }

    out.push_back(c);
    i++;
  }

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

static duckdb::unique_ptr<duckdb::QueryResult>
duckdb_pending_query(duckdb::Connection &connection, const std::string &query,
                     duckdb::QueryResultOutputType output_type)
{
  const std::string q= backticks_to_double_quotes(query);

  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
    sql_print_information("DuckDB query: %s", q.c_str());

  try
  {
    auto pending= connection.PendingQuery(q, output_type);
    duckdb::unique_ptr<duckdb::QueryResult> res;
    if (pending->HasError())
      res= duckdb::make_uniq<duckdb::MaterializedQueryResult>(
          pending->GetErrorObject());
    else
      res= pending->Execute();

    if ((myduck::duckdb_log_options & LOG_DUCKDB_QUERY_RESULT) &&
        res->HasError())
      sql_print_information("DuckDB error: %s", res->GetError().c_str());
    return res;
  }
  catch (duckdb::Exception &e)
  {
    return duckdb::make_uniq<duckdb::MaterializedQueryResult>(
        duckdb::ErrorData(e.what()));
  }
  catch (std::exception &e)
  {
    return duckdb::make_uniq<duckdb::MaterializedQueryResult>(
        duckdb::ErrorData(e.what()));
  }
}

static duckdb::unique_ptr<duckdb::MaterializedQueryResult>
duckdb_query_single(duckdb::Connection &connection, const std::string &query)
{
  auto res= duckdb_pending_query(
      connection, query, duckdb::QueryResultOutputType::FORCE_MATERIALIZED);
  DBUG_ASSERT(res->type == duckdb::QueryResultType::MATERIALIZED_RESULT);
  return duckdb::unique_ptr_cast<duckdb::QueryResult,
                                 duckdb::MaterializedQueryResult>(
      std::move(res));
}

duckdb::unique_ptr<duckdb::QueryResult>
duckdb_stream_query(duckdb::Connection &connection, const std::string &query)
{
  return duckdb_pending_query(
      connection, query, duckdb::QueryResultOutputType::ALLOW_STREAMING);
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
  if (mariadb_query_has_unsafe_quote_escape(thd, query.data(), query.size()))
    return duckdb::make_uniq<duckdb::MaterializedQueryResult>(
        duckdb::ErrorData("Unsafe MariaDB backslash quote escape in forwarded SQL"));

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

  return duckdb_query_single(ctx->get_connection(), query);
}

duckdb::unique_ptr<duckdb::QueryResult>
duckdb_stream_query(THD *thd, const std::string &query, bool need_config)
{
  if (mariadb_query_has_unsafe_quote_escape(thd, query.data(), query.size()))
    return duckdb::make_uniq<duckdb::MaterializedQueryResult>(
        duckdb::ErrorData("Unsafe MariaDB backslash quote escape in forwarded SQL"));

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

  return duckdb_stream_query(ctx->get_connection(), query);
}

duckdb::unique_ptr<duckdb::MaterializedQueryResult>
duckdb_query(const std::string &query)
{
  auto connection= DuckdbManager::CreateConnection();
  return duckdb_query(*connection, query);
}

} // namespace myduck
