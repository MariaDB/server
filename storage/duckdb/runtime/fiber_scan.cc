/*
  Copyright (c) 2026, MariaDB Foundation.

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
#include "item.h"
#include "sql_parse.h"
#include "sql_lex.h"

#undef UNKNOWN

#include "fiber_scan.h"
#include "duckdb_log.h"

MYSQL_THD create_background_thd();
void destroy_background_thd(MYSQL_THD thd);

namespace myduck
{

/* ----------------------------------------------------------------
   Item → duckdb::Value conversion
   ---------------------------------------------------------------- */

duckdb::Value item_to_duckdb_value(Item *item, const duckdb::LogicalType &type)
{
  if (item->is_null())
    return duckdb::Value();

  switch (type.id())
  {
  case duckdb::LogicalTypeId::TINYINT:
    return duckdb::Value::TINYINT(static_cast<int8_t>(item->val_int()));
  case duckdb::LogicalTypeId::SMALLINT:
    return duckdb::Value::SMALLINT(static_cast<int16_t>(item->val_int()));
  case duckdb::LogicalTypeId::INTEGER:
    return duckdb::Value::INTEGER(static_cast<int32_t>(item->val_int()));
  case duckdb::LogicalTypeId::BIGINT:
    return duckdb::Value::BIGINT(item->val_int());
  case duckdb::LogicalTypeId::UTINYINT:
    return duckdb::Value::UTINYINT(static_cast<uint8_t>(item->val_uint()));
  case duckdb::LogicalTypeId::USMALLINT:
    return duckdb::Value::USMALLINT(static_cast<uint16_t>(item->val_uint()));
  case duckdb::LogicalTypeId::UINTEGER:
    return duckdb::Value::UINTEGER(static_cast<uint32_t>(item->val_uint()));
  case duckdb::LogicalTypeId::UBIGINT:
    return duckdb::Value::UBIGINT(item->val_uint());
  case duckdb::LogicalTypeId::FLOAT:
    return duckdb::Value::FLOAT(static_cast<float>(item->val_real()));
  case duckdb::LogicalTypeId::DOUBLE:
    return duckdb::Value::DOUBLE(item->val_real());
  case duckdb::LogicalTypeId::BLOB: {
    String buf;
    String *s= item->val_str(&buf);
    if (!s)
      return duckdb::Value();
    return duckdb::Value::BLOB(std::string(s->ptr(), s->length()));
  }
  default: {
    String buf;
    String *s= item->val_str(&buf);
    if (!s)
      return duckdb::Value();
    return duckdb::Value(std::string(s->ptr(), s->length()));
  }
  }
}

/* ----------------------------------------------------------------
   select_to_duckdb implementation
   ---------------------------------------------------------------- */

select_to_duckdb::select_to_duckdb(THD *thd_arg,
                                   struct fiber_context *ctx,
                                   duckdb::DataChunk *buffer,
                                   const duckdb::vector<duckdb::LogicalType> *types)
  : select_result_interceptor(thd_arg),
    ctx_(ctx),
    buffer_(buffer),
    types_(types),
    row_count_(0),
    finished_(false),
    error_(false)
{}

int select_to_duckdb::send_data(List<Item> &items)
{
  List_iterator_fast<Item> it(items);
  Item *item;
  duckdb::idx_t col= 0;

  while ((item= it++))
  {
    if (col < types_->size())
    {
      duckdb::Value val= item_to_duckdb_value(item, (*types_)[col]);
      buffer_->data[col].SetValue(row_count_, val);
    }
    col++;
  }
  row_count_++;

  if (row_count_ >= STANDARD_VECTOR_SIZE)
  {
    buffer_->SetCardinality(row_count_);
    row_count_= 0;
    fiber_context_yield(ctx_);
  }

  return thd->killed ? -1 : 0;
}

bool select_to_duckdb::send_eof()
{
  if (row_count_ > 0)
  {
    buffer_->SetCardinality(row_count_);
    row_count_= 0;
  }
  finished_= true;
  return false;
}

void select_to_duckdb::abort_result_set()
{
  error_= true;
  finished_= true;
}

/* ----------------------------------------------------------------
   FiberScanState
   ---------------------------------------------------------------- */

int FiberScanState::init(TABLE *tbl,
                         const duckdb::vector<duckdb::idx_t> &col_ids,
                         const duckdb::vector<duckdb::LogicalType> &col_types,
                         const std::string &where_sql)
{
  table= tbl;
  column_ids= col_ids;
  types= col_types;
  where_clause= where_sql;

  if (tbl->s->db.str)
    db_name.assign(tbl->s->db.str, tbl->s->db.length);
  if (tbl->s->table_name.str)
    table_name.assign(tbl->s->table_name.str, tbl->s->table_name.length);

  buffer.Initialize(duckdb::Allocator::DefaultAllocator(), types);

  if (fiber_context_init(&ctx, FIBER_STACK_SIZE))
    return 1;

  fiber_thd= create_background_thd();
  if (!fiber_thd)
  {
    fiber_context_destroy(&ctx);
    return 1;
  }

  /* Grant full privileges so the fiber can open any table */
  fiber_thd->security_ctx->master_access= access_t(ALL_KNOWN_ACL);

  /* Disable query cache — fiber queries are internal, not cacheable */
  fiber_thd->query_cache_is_applicable= 0;

  return 0;
}

FiberScanState::~FiberScanState()
{
  if (fiber_started && !finished)
  {
    if (fiber_thd)
      fiber_thd->set_killed_no_mutex(KILL_QUERY);

    /*
      Swap TLS to fiber context before resuming so the fiber can
      finish cleanly (same pattern as mdb_scan_function).
    */
    THD *prev_thd= _current_thd();
    st_my_thread_var *prev_mysys= my_thread_var;

    set_current_thd(fiber_thd);
    if (fiber_thd && fiber_thd->mysys_var)
      set_mysys_var(fiber_thd->mysys_var);

    while (!finished)
    {
      buffer.Reset();
      int rc= fiber_context_continue(&ctx);
      if (rc <= 0)  /* 0 = fiber returned, -1 = error */
        break;
    }

    set_current_thd(prev_thd);
    set_mysys_var(prev_mysys);
  }

  delete result;
  result= nullptr;

  if (fiber_thd)
  {
    THD *saved= _current_thd();
    set_current_thd(nullptr);
    destroy_background_thd(fiber_thd);
    set_current_thd(saved);
    fiber_thd= nullptr;
  }

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   Build synthetic SELECT from column_ids + WHERE
   ---------------------------------------------------------------- */

static std::string escape_backticks(const char *s)
{
  std::string out;
  for (; *s; s++)
  {
    if (*s == '`')
      out+= "``";
    else
      out+= *s;
  }
  return out;
}

static std::string build_synthetic_select(FiberScanState *state)
{
  std::string sql= "SELECT ";

  bool first= true;
  uint nfields= state->table->s->fields;
  for (auto col_idx : state->column_ids)
  {
    if (!first)
      sql+= ", ";
    first= false;

    if (col_idx >= nfields)
    {
      sql+= "NULL";
      continue;
    }
    Field *field= state->table->field[col_idx];
    sql+= '`';
    sql+= escape_backticks(field->field_name.str);
    sql+= '`';
  }

  if (first)
    sql+= "*";

  sql+= " FROM `";
  sql+= escape_backticks(state->db_name.c_str());
  sql+= "`.`";
  sql+= escape_backticks(state->table_name.c_str());
  sql+= '`';

  if (!state->where_clause.empty())
  {
    sql+= " WHERE ";
    sql+= state->where_clause;
  }

  return sql;
}

/* ----------------------------------------------------------------
   Fiber entry point
   ---------------------------------------------------------------- */

void fiber_scan_func(void *arg)
{
  auto *state= static_cast<FiberScanState *>(arg);
  THD *thd= state->fiber_thd;

  /*
    TLS (current_thd + THR_KEY_mysys) is already set by the caller
    (mdb_scan_function) before fiber_context_spawn/continue.
    Do NOT use thd_attach_thd/thd_detach_thd here — they assert
    !current_thd which fails because fibers share the OS thread.
  */

  /* Set thread_stack for check_stack_overrun() — point to fiber's stack */
  char stack_top;
  thd->thread_stack= &stack_top;

  state->result= new select_to_duckdb(thd, &state->ctx,
                                      &state->buffer,
                                      &state->types);

  std::string sql= build_synthetic_select(state);

  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
    sql_print_information("FIBER: synthetic SQL: %s", sql.c_str());

  /* Allocate query buffer on THD mem_root */
  size_t len= sql.size();
  char *buf= static_cast<char *>(thd->alloc(len + 1));
  if (!buf)
  {
    state->error= true;
    state->finished= true;
    return;
  }
  memcpy(buf, sql.c_str(), len + 1);
  thd->set_query_inner(buf, static_cast<uint32>(len),
                       system_charset_info);

  /* Initialize parser */
  lex_start(thd);
  thd->reset_for_next_command();

  Parser_state parser_state;
  if (parser_state.init(thd, buf, len) ||
      parse_sql(thd, &parser_state, NULL) ||
      thd->is_error())
  {
    sql_print_error("FIBER: parse/init error for: %s", sql.c_str());
    if (thd->is_error())
      sql_print_error("FIBER: THD error: %s", thd->get_stmt_da()->message());
    state->error= true;
    state->finished= true;
    thd->end_statement();
    thd->cleanup_after_query();
    return;
  }

  /* Install our result interceptor */
  thd->lex->result= state->result;

  /* Execute — send_data() will yield when DataChunk is full */
  mysql_execute_command(thd);

  /* Prevent end_statement() from deleting our result interceptor */
  thd->lex->result= NULL;

  if (thd->is_error())
  {
    sql_print_error("FIBER: execution error: %s",
                    thd->get_stmt_da()->message());
    state->error= true;
  }

  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
    sql_print_information("FIBER: finished, buffer rows=%llu error=%d",
                          (ulonglong) state->buffer.size(),
                          (int) state->error);

  thd->end_statement();
  thd->cleanup_after_query();

  state->finished= true;
}

} /* namespace myduck */
