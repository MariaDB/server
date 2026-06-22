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
#include "sql_select.h"
#include "log.h"

#undef UNKNOWN

#include "ha_duckdb_pushdown.h"
#include "duckdb_select.h"
#include "duckdb_query.h"
#include "duckdb_context.h"
#include "cross_engine_scan.h"
#include "duckdb_log.h"

extern handlerton *duckdb_hton;

/**
  Check whether a SELECT_LEX can be pushed down to DuckDB.

  Returns true if at least one table is DuckDB.  Non-DuckDB tables are
  collected in @p external_tables so they can be registered with the
  replacement-scan mechanism before executing the DuckDB query.
*/

static bool can_pushdown_to_duckdb(SELECT_LEX *sel_lex,
                                   std::vector<std::string> &external_tables,
                                   bool &has_duckdb_table)
{
  for (TABLE_LIST *tbl= sel_lex->get_table_list(); tbl; tbl= tbl->next_global)
  {
    if (tbl->derived || !tbl->table)
      continue;

    if (tbl->table->file->ht == duckdb_hton)
      has_duckdb_table= true;
    else
      external_tables.emplace_back(tbl->table_name.str);
  }

  return has_duckdb_table;
}

/**
  Check whether a SELECT_LEX_UNIT (UNION/EXCEPT/INTERSECT) can be
  pushed down to DuckDB.  Walks every SELECT_LEX in the unit.
*/

static bool
can_pushdown_unit_to_duckdb(SELECT_LEX_UNIT *unit,
                            std::vector<std::string> &external_tables,
                            bool &has_duckdb_table)
{
  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    for (TABLE_LIST *tbl= sl->get_table_list(); tbl; tbl= tbl->next_global)
    {
      if (tbl->derived || !tbl->table)
        continue;

      if (tbl->table->file->ht == duckdb_hton)
        has_duckdb_table= true;
      else
        external_tables.emplace_back(tbl->table_name.str);
    }
  }

  return has_duckdb_table;
}

/* ----- Factory functions ----- */

select_handler *create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex,
                                             SELECT_LEX_UNIT *sel_unit)
{
  /*
    Only handle plain SELECT and INSERT ... SELECT for now.
  */
  if (thd->lex->sql_command != SQLCOM_SELECT &&
      thd->lex->sql_command != SQLCOM_INSERT_SELECT)
    return nullptr;

  if (!sel_lex)
    return nullptr;

  std::vector<std::string> external_tables;
  bool has_duckdb_table= false;

  if (!can_pushdown_to_duckdb(sel_lex, external_tables, has_duckdb_table))
    return nullptr;

  /* At least one DuckDB table must participate */
  if (!has_duckdb_table)
    return nullptr;

  /* Do not push down queries with side-effects (e.g. user variables) */
  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  auto *handler= new ha_duckdb_select_handler(thd, sel_lex, sel_unit);
  if (!external_tables.empty())
  {
    handler->set_cross_engine(std::move(external_tables));

    if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
      sql_print_information("DuckDB: cross-engine pushdown with %zu "
                            "external table(s)",
                            handler->external_table_count());
  }
  return handler;
}

select_handler *create_duckdb_unit_handler(THD *thd, SELECT_LEX_UNIT *sel_unit)
{
  if (thd->lex->sql_command == SQLCOM_CREATE_VIEW)
    return nullptr;

  if (thd->stmt_arena && thd->stmt_arena->is_stmt_prepare())
    return nullptr;

  if (!sel_unit)
    return nullptr;

  std::vector<std::string> external_tables;
  bool has_duckdb_table= false;

  if (!can_pushdown_unit_to_duckdb(sel_unit, external_tables,
                                   has_duckdb_table))
    return nullptr;

  if (!has_duckdb_table)
    return nullptr;

  auto *handler= new ha_duckdb_select_handler(thd, sel_unit);
  if (!external_tables.empty())
  {
    handler->set_cross_engine(std::move(external_tables));

    if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
      sql_print_information("DuckDB: cross-engine UNION pushdown with %zu "
                            "external table(s)",
                            handler->external_table_count());
  }
  return handler;
}

/* ----- ha_duckdb_select_handler implementation ----- */

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd_arg,
                                                   SELECT_LEX *sel_lex,
                                                   SELECT_LEX_UNIT *sel_unit)
    : select_handler(thd_arg, duckdb_hton, sel_lex, sel_unit),
      current_row_index(0), query_string(thd_arg->charset())
{
  query_string.length(0);

  /*
    Use the original SQL text from THD instead of SELECT_LEX::print().
    SELECT_LEX::print() converts implicit (comma) joins into explicit
    "JOIN" without ON clauses, which DuckDB's parser rejects.
    The select_handler intercepts the full query in all cases
    (simple SELECT and UNION), so the original text is always usable.
  */
  query_string.append(thd_arg->query(), thd_arg->query_length());
}

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd_arg,
                                                   SELECT_LEX_UNIT *sel_unit)
    : select_handler(thd_arg, duckdb_hton, sel_unit), current_row_index(0),
      query_string(thd_arg->charset())
{
  query_string.length(0);
  query_string.append(thd_arg->query(), thd_arg->query_length());
}

ha_duckdb_select_handler::~ha_duckdb_select_handler()= default;

void ha_duckdb_select_handler::set_cross_engine(
    std::vector<std::string> &&tables)
{
  has_cross_engine= true;
  external_table_names= std::move(tables);
}

size_t ha_duckdb_select_handler::external_table_count() const
{
  return external_table_names.size();
}

/**
  Case-insensitive find that skips single-quoted strings ('...'),
  double-quoted strings ("..."), backtick identifiers (`...`),
  line comments (-- ...) and C-style comments.
  Returns position of the first match at or after @p start,
  or std::string::npos.
*/
static size_t find_in_code(const std::string &sql, const char *pattern,
                           size_t start= 0)
{
  size_t plen= strlen(pattern);
  if (plen == 0 || sql.size() < plen)
    return std::string::npos;

  size_t i= start;
  while (i + plen <= sql.size())
  {
    unsigned char c= sql[i];

    /* Skip single-quoted strings: handle \' and '' escapes */
    if (c == '\'')
    {
      for (i++; i < sql.size(); i++)
      {
        if (sql[i] == '\\') { i++; continue; }
        if (sql[i] == '\'')
        {
          if (i + 1 < sql.size() && sql[i + 1] == '\'') { i++; continue; }
          break;
        }
      }
      i++;
      continue;
    }

    /* Skip double-quoted strings */
    if (c == '"')
    {
      for (i++; i < sql.size(); i++)
      {
        if (sql[i] == '\\') { i++; continue; }
        if (sql[i] == '"')
        {
          if (i + 1 < sql.size() && sql[i + 1] == '"') { i++; continue; }
          break;
        }
      }
      i++;
      continue;
    }

    /* Skip backtick identifiers */
    if (c == '`')
    {
      for (i++; i < sql.size(); i++)
      {
        if (sql[i] == '`')
        {
          if (i + 1 < sql.size() && sql[i + 1] == '`') { i++; continue; }
          break;
        }
      }
      i++;
      continue;
    }

    /* Skip -- line comments */
    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-')
    {
      for (i += 2; i < sql.size() && sql[i] != '\n'; i++) {}
      continue;
    }

    /* Skip C-style comments */
    if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*')
    {
      for (i += 2; i + 1 < sql.size(); i++)
      {
        if (sql[i] == '*' && sql[i + 1] == '/') { i += 2; break; }
      }
      continue;
    }

    /* Case-insensitive match at position i */
    bool match= true;
    for (size_t j= 0; j < plen; j++)
    {
      if (toupper((unsigned char) sql[i + j]) !=
          toupper((unsigned char) pattern[j]))
      {
        match= false;
        break;
      }
    }
    if (match)
      return i;

    i++;
  }

  return std::string::npos;
}

/**
  Reverse find_in_code: returns position of the last match at or before
  @p before, or std::string::npos.
*/
static size_t rfind_in_code(const std::string &sql, const char *pattern,
                            size_t before= std::string::npos)
{
  size_t last= std::string::npos;
  size_t pos= 0;
  for (;;)
  {
    pos= find_in_code(sql, pattern, pos);
    if (pos == std::string::npos || pos > before)
      break;
    last= pos;
    pos++;
  }
  return last;
}

/**
  Case-insensitive prefix match: does @p sql at position @p pos start
  with @p prefix?
*/
static bool ci_starts_with(const std::string &sql, size_t pos,
                           const char *prefix)
{
  for (size_t i= 0; prefix[i]; i++)
  {
    if (pos + i >= sql.size())
      return false;
    if (toupper((unsigned char) sql[pos + i]) !=
        toupper((unsigned char) prefix[i]))
      return false;
  }
  return true;
}

int ha_duckdb_select_handler::init_scan()
{
  DBUG_ENTER("ha_duckdb_select_handler::init_scan");

  /* Register external tables with the thread-local replacement scan registry
   */
  if (has_cross_engine)
  {
    auto register_tables_from_sel= [this](SELECT_LEX *sl) {
      for (TABLE_LIST *tbl= sl->get_table_list(); tbl; tbl= tbl->next_global)
      {
        if (tbl->derived || !tbl->table)
          continue;
        if (tbl->table->file->ht != duckdb_hton)
        {
          myduck::register_external_table(tbl->table_name.str, tbl->table);

          if (sl->where)
          {
            COND *table_cond=
                make_cond_for_table(thd, sl->where, tbl->table->map,
                                    tbl->table->map, -1, false, false);
            if (table_cond)
            {
              StringBuffer<1024> buf;
              table_cond->print(&buf,
                  QT_ITEM_IDENT_DISABLE_DB_TABLE_NAMES);
              if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
                sql_print_information(
                    "CROSS-ENGINE: WHERE for '%s': %.*s",
                    tbl->table_name.str, (int) buf.length(), buf.ptr());
              myduck::register_external_where(
                  tbl->table_name.str,
                  std::string(buf.ptr(), buf.length()));
            }
          }
        }
      }
    };

    if (select_lex)
    {
      register_tables_from_sel(select_lex);
    }
    else if (lex_unit)
    {
      for (SELECT_LEX *sl= lex_unit->first_select(); sl; sl= sl->next_select())
        register_tables_from_sel(sl);
    }
  }

  std::string sql(query_string.ptr(), query_string.length());

  /*
    Rewrite MariaDB-specific SQL syntax that DuckDB does not understand.
    GROUP BY ... WITH ROLLUP  →  GROUP BY ROLLUP(...)
  */
  {
    size_t search_from= 0;
    size_t rollup_pos;
    while ((rollup_pos= find_in_code(sql, " WITH ROLLUP", search_from))
           != std::string::npos)
    {
      size_t group_pos= rfind_in_code(sql, "GROUP BY ", rollup_pos);
      if (group_pos != std::string::npos && group_pos >= search_from)
      {
        size_t cols_start= group_pos + 9;
        std::string cols= sql.substr(cols_start, rollup_pos - cols_start);
        std::string replacement= "GROUP BY ROLLUP(" + cols + ")";
        sql.replace(group_pos, rollup_pos + 12 - group_pos, replacement);
        search_from= group_pos + replacement.size();
      }
      else
        search_from= rollup_pos + 12;
    }
  }

  /*
    Rewrite CONVERT(expr, TYPE) → CAST(expr AS TYPE)
    MariaDB uses CONVERT(expr, type) syntax, DuckDB uses CAST(expr AS type).
  */
  {
    size_t pos= 0;
    while ((pos= find_in_code(sql, "CONVERT(", pos)) != std::string::npos)
    {
      /* Word boundary: skip if preceded by an identifier character */
      if (pos > 0 &&
          (isalnum((unsigned char) sql[pos - 1]) || sql[pos - 1] == '_'))
      {
        pos++;
        continue;
      }
      /* Find matching closing paren, handling nested parens */
      size_t start= pos + 8; /* after "CONVERT(" */
      int depth= 1;
      size_t i= start;
      size_t comma= std::string::npos;
      for (; i < sql.size() && depth > 0; i++)
      {
        if (sql[i] == '(')
          depth++;
        else if (sql[i] == ')')
          depth--;
        else if (sql[i] == ',' && depth == 1 && comma == std::string::npos)
          comma= i;
      }
      if (comma != std::string::npos && depth == 0)
      {
        std::string expr= sql.substr(start, comma - start);
        std::string type= sql.substr(comma + 1, i - 1 - (comma + 1));
        /* Trim whitespace from type */
        size_t ts= type.find_first_not_of(" \t");
        size_t te= type.find_last_not_of(" \t");
        if (ts != std::string::npos)
          type= type.substr(ts, te - ts + 1);
        std::string replacement= "CAST(" + expr + " AS " + type + ")";
        sql.replace(pos, i - pos, replacement);
      }
      else
        pos++;
    }
  }

  /*
    Rewrite CURRENT_TIME() → current_time, CURRENT_DATE() → current_date,
    CURRENT_TIMESTAMP() → current_timestamp.
    MariaDB outputs these with parens; DuckDB treats them as keywords.
  */
  {
    static const char *time_kws[]= {
        "CURRENT_TIME(", "CURRENT_DATE(", "CURRENT_TIMESTAMP("};
    static const char *time_repls[]= {
        "current_time", "current_date", "current_timestamp"};
    for (int ki= 0; ki < 3; ki++)
    {
      size_t klen= strlen(time_kws[ki]);
      size_t pos= 0;
      while ((pos= find_in_code(sql, time_kws[ki], pos)) != std::string::npos)
      {
        /* Find closing paren */
        size_t end= sql.find(')', pos + klen);
        if (end == std::string::npos) { pos++; continue; }
        sql.replace(pos, end + 1 - pos, time_repls[ki]);
      }
    }
  }

  /*
    Handle STRAIGHT_JOIN: as a SELECT hint — remove; as a join keyword —
    replace with JOIN (preserves ON clause).
    Then rewrite remaining conditionless JOIN → CROSS JOIN.
  */
  {
    size_t pos= 0;
    while ((pos= find_in_code(sql, "STRAIGHT_JOIN", pos)) != std::string::npos)
    {
      /*
        Determine if STRAIGHT_JOIN is a SELECT modifier (hint) or a join
        keyword.  As a hint it follows SELECT/ALL/DISTINCT/DISTINCTROW.
        As a join it follows a table name, alias, or closing paren.
      */
      size_t p= pos;
      while (p > 0 && sql[p - 1] == ' ') p--;
      size_t word_end= p;
      while (p > 0 &&
             (isalnum((unsigned char) sql[p - 1]) || sql[p - 1] == '_'))
        p--;
      size_t wlen= word_end - p;
      bool is_hint=
          wlen > 0 &&
          ((wlen == 6 && ci_starts_with(sql, p, "SELECT")) ||
           (wlen == 3 && ci_starts_with(sql, p, "ALL")) ||
           (wlen == 8 && ci_starts_with(sql, p, "DISTINCT")) ||
           (wlen == 11 && ci_starts_with(sql, p, "DISTINCTROW")));

      if (is_hint)
      {
        size_t elen= 13;
        if (pos + elen < sql.size() && sql[pos + elen] == ' ')
          elen++;
        sql.erase(pos, elen);
      }
      else
        sql.replace(pos, 13, "JOIN");
    }
    /*
      Scan for remaining conditionless JOIN (no ON/USING).
    */
    pos= 0;
    while ((pos= find_in_code(sql, " JOIN ", pos)) != std::string::npos)
    {
      /* Skip qualified JOINs: LEFT/RIGHT/INNER/CROSS/NATURAL/FULL/OUTER */
      {
        size_t p= pos;
        while (p > 0 && sql[p - 1] == ' ') p--;
        size_t word_end= p;
        while (p > 0 && isalpha((unsigned char) sql[p - 1])) p--;
        size_t wlen= word_end - p;
        if (wlen > 0 &&
            ((wlen == 5 && ci_starts_with(sql, p, "CROSS")) ||
             (wlen == 7 && ci_starts_with(sql, p, "NATURAL")) ||
             (wlen == 4 && ci_starts_with(sql, p, "LEFT")) ||
             (wlen == 5 && ci_starts_with(sql, p, "RIGHT")) ||
             (wlen == 5 && ci_starts_with(sql, p, "INNER")) ||
             (wlen == 4 && ci_starts_with(sql, p, "FULL")) ||
             (wlen == 5 && ci_starts_with(sql, p, "OUTER"))))
        {
          pos+= 6;
          continue;
        }
      }

      /* Scan forward to find ON/USING or a clause boundary */
      size_t scan= pos + 6;
      bool has_condition= false;
      while (scan < sql.size())
      {
        /* Check for ON (as keyword, followed by space) */
        if (ci_starts_with(sql, scan, "ON ") ||
            ci_starts_with(sql, scan, "USING ") ||
            ci_starts_with(sql, scan, "USING("))
        {
          has_condition= true;
          break;
        }
        /* Clause boundary — no ON found, this is conditionless */
        if (ci_starts_with(sql, scan, "JOIN ") ||
            ci_starts_with(sql, scan, "WHERE ") ||
            ci_starts_with(sql, scan, "GROUP ") ||
            ci_starts_with(sql, scan, "ORDER ") ||
            ci_starts_with(sql, scan, "LIMIT ") ||
            ci_starts_with(sql, scan, "HAVING ") ||
            sql[scan] == ';')
          break;
        scan++;
      }
      if (!has_condition)
      {
        sql.replace(pos, 6, " CROSS JOIN ");
        pos+= 12;
      }
      else
        pos+= 6;
    }
  }

  /*
    Rewrite REGEXP / NOT REGEXP → regexp_matches().
    MariaDB: expr REGEXP pattern / expr NOT REGEXP pattern
    DuckDB:  regexp_matches(expr, pattern) / NOT regexp_matches(expr, pattern)
  */
  {
    /* Replace NOT REGEXP first (longer), then REGEXP */
    size_t pos= 0;
    while ((pos= find_in_code(sql, " NOT REGEXP ", pos)) != std::string::npos)
      sql.replace(pos, 12, " !~ ");
    pos= 0;
    while ((pos= find_in_code(sql, " REGEXP ", pos)) != std::string::npos)
      sql.replace(pos, 8, " ~ ");
    /* RLIKE is a synonym for REGEXP in MariaDB */
    pos= 0;
    while ((pos= find_in_code(sql, " NOT RLIKE ", pos)) != std::string::npos)
      sql.replace(pos, 11, " !~ ");
    pos= 0;
    while ((pos= find_in_code(sql, " RLIKE ", pos)) != std::string::npos)
      sql.replace(pos, 7, " ~ ");
  }

  /*
    Remove MariaDB-specific SELECT hints that DuckDB doesn't understand.
    HIGH_PRIORITY, SQL_NO_CACHE, SQL_CACHE, STRAIGHT_JOIN (as hint).
  */
  {
    static const char *hints[]= {
        "HIGH_PRIORITY ", "SQL_NO_CACHE ", "SQL_CACHE ",
        "SQL_BUFFER_RESULT ", "SQL_SMALL_RESULT ", "SQL_BIG_RESULT ",
        "SQL_CALC_FOUND_ROWS "};
    for (auto hint : hints)
    {
      size_t hlen= strlen(hint);
      size_t pos;
      while ((pos= find_in_code(sql, hint)) != std::string::npos)
        sql.erase(pos, hlen);
    }
  }

  /*
    Remove MariaDB index hints: FORCE INDEX(...), USE INDEX(...),
    IGNORE INDEX(...).
  */
  {
    static const char *idx_hints[]= {
        "FORCE INDEX(", "USE INDEX(", "IGNORE INDEX("};
    for (auto hint : idx_hints)
    {
      size_t hlen= strlen(hint);
      size_t pos= 0;
      while ((pos= find_in_code(sql, hint, pos)) != std::string::npos)
      {
        /* Find matching closing paren */
        size_t end= sql.find(')', pos + hlen);
        if (end == std::string::npos)
        {
          pos++;
          continue;
        }
        /* Remove including surrounding spaces */
        size_t erase_start= pos;
        size_t erase_end= end + 1;
        while (erase_end < sql.size() && sql[erase_end] == ' ')
          erase_end++;
        sql.erase(erase_start, erase_end - erase_start);
      }
    }
  }

  /*
    Rewrite LIMIT offset,count → LIMIT count OFFSET offset.
    MariaDB: LIMIT 2,5   DuckDB: LIMIT 5 OFFSET 2
  */
  {
    size_t lpos= 0;
    while ((lpos= find_in_code(sql, "LIMIT ", lpos)) != std::string::npos)
    {
      size_t after_limit= lpos + 6;
      /* Skip whitespace */
      while (after_limit < sql.size() && sql[after_limit] == ' ')
        after_limit++;
      /* Read first number */
      size_t num1_start= after_limit;
      while (after_limit < sql.size() && isdigit(sql[after_limit]))
        after_limit++;
      if (after_limit == num1_start)
      {
        lpos= after_limit;
        continue;
      }
      /* Check for comma */
      size_t comma= after_limit;
      while (comma < sql.size() && sql[comma] == ' ')
        comma++;
      if (comma < sql.size() && sql[comma] == ',')
      {
        std::string offset_str= sql.substr(num1_start,
                                           after_limit - num1_start);
        size_t num2_start= comma + 1;
        while (num2_start < sql.size() && sql[num2_start] == ' ')
          num2_start++;
        size_t num2_end= num2_start;
        while (num2_end < sql.size() && isdigit(sql[num2_end]))
          num2_end++;
        std::string count_str= sql.substr(num2_start, num2_end - num2_start);
        std::string replacement= "LIMIT " + count_str + " OFFSET " +
                                 offset_str;
        sql.replace(lpos, num2_end - lpos, replacement);
        lpos+= replacement.size();
      }
      else
        lpos= after_limit;
    }
  }

  query_result= myduck::duckdb_query(thd, sql, true);

  if (!query_result || query_result->HasError())
  {
    if (query_result)
      my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_INTERNAL_ERROR,
               query_result->GetError().c_str(), "DuckDB");
    else
      my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_INTERNAL_ERROR,
               "DuckDB query returned null result", "DuckDB");
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  current_chunk.reset();
  current_row_index= 0;

  DBUG_RETURN(0);
}

int ha_duckdb_select_handler::next_row()
{
  DBUG_ENTER("ha_duckdb_select_handler::next_row");

  if (!query_result)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  /* Fetch a new chunk when the current one is exhausted */
  if (!current_chunk || current_row_index >= current_chunk->size())
  {
    current_chunk.reset();
    current_chunk= query_result->Fetch();

    if (!current_chunk || current_chunk->size() == 0)
      DBUG_RETURN(HA_ERR_END_OF_FILE);

    current_row_index= 0;
  }

  /*
    Store the fields into table->record[0].
    The select_handler framework provides `table` which is a temporary
    table with one Field per item in the select list.
  */
  size_t col_count= current_chunk->ColumnCount();
  size_t field_count= 0;

  for (Field **f= table->field; *f; f++)
    field_count++;

  size_t ncols= (col_count < field_count) ? col_count : field_count;

  for (size_t col_idx= 0; col_idx < ncols; col_idx++)
  {
    duckdb::Value value= current_chunk->GetValue(col_idx, current_row_index);
    Field *field= table->field[col_idx];
    store_duckdb_field_in_mysql_format(field, value, thd);
  }

  current_row_index++;
  DBUG_RETURN(0);
}

int ha_duckdb_select_handler::end_scan()
{
  DBUG_ENTER("ha_duckdb_select_handler::end_scan");

  if (has_cross_engine)
    myduck::clear_external_tables();

  current_chunk.reset();
  query_result.reset();
  current_row_index= 0;

  free_tmp_table(thd, table);
  table= 0;

  DBUG_RETURN(0);
}
