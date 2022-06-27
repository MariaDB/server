/* This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mariadb.h"
#include "sql_array.h"
#include "sql_string.h"
#include "sql_class.h"
#include "sql_show.h"
#include "field.h"
#include "sql_i_s.h"
#include "opt_trace.h"
#include "sql_parse.h"
#include "set_var.h"
#include "my_json_writer.h"
#include "sp_head.h"

#include "rowid_filter.h"

const char I_S_table_name[]= "OPTIMIZER_TRACE";

/**
   Whether a list of tables contains information_schema.OPTIMIZER_TRACE.
   @param  tbl  list of tables

   Can we do better than this here??
   @note this does not catch that a stored routine or view accesses
   the OPTIMIZER_TRACE table. So using a stored routine or view to read
   OPTIMIZER_TRACE will overwrite OPTIMIZER_TRACE as it runs and provide
   uninteresting info.
*/
bool list_has_optimizer_trace_table(const TABLE_LIST *tbl)
{
  for (; tbl; tbl= tbl->next_global)
  {
    if (tbl->schema_table &&
        0 == strcmp(tbl->schema_table->table_name, I_S_table_name))
      return true;
  }
  return false;
}

/*
  Returns if a query has a set command with optimizer_trace being switched on/off.
  True: Don't trace the query(uninteresting)
*/

bool sets_var_optimizer_trace(enum enum_sql_command sql_command,
                              List<set_var_base> *set_vars)
{
  if (sql_command == SQLCOM_SET_OPTION)
  {
    List_iterator_fast<set_var_base> it(*set_vars);
    const set_var_base *var;
    while ((var= it++))
      if (var->is_var_optimizer_trace()) return true;
  }
  return false;
}


namespace Show {


ST_FIELD_INFO optimizer_trace_info[]=
{
  Column("QUERY",                             Longtext(65535), NOT_NULL),
  Column("TRACE",                             Longtext(65535), NOT_NULL),
  Column("MISSING_BYTES_BEYOND_MAX_MEM_SIZE", SLong(20),       NOT_NULL),
  Column("INSUFFICIENT_PRIVILEGES",           STiny(1),        NOT_NULL),
  CEnd()
};

} // namespace Show


/*
  TODO: one-line needs to be implemented seperately
*/
const char *Opt_trace_context::flag_names[]= {"enabled", "default",
                                               NullS};

/*
  Returns if a particular command will be traced or not
*/

inline bool sql_command_can_be_traced(enum enum_sql_command sql_command)
{
  /*
    For first iteration we are only allowing select queries.
    TODO: change to allow other queries.
  */
  return sql_command == SQLCOM_SELECT ||
         sql_command == SQLCOM_UPDATE ||
         sql_command == SQLCOM_DELETE ||
         sql_command == SQLCOM_DELETE_MULTI ||
         sql_command == SQLCOM_UPDATE_MULTI;
}

void opt_trace_print_expanded_query(THD *thd, SELECT_LEX *select_lex,
                                    Json_writer_object *writer)

{
  DBUG_ASSERT(thd->trace_started());

  StringBuffer<1024> str(system_charset_info);
  ulonglong save_option_bits= thd->variables.option_bits;
  thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;
  select_lex->print(thd, &str,
                    enum_query_type(QT_TO_SYSTEM_CHARSET |
                                    QT_SHOW_SELECT_NUMBER |
                                    QT_ITEM_IDENT_SKIP_DB_NAMES |
                                    QT_VIEW_INTERNAL));
  thd->variables.option_bits= save_option_bits;
  /*
    The output is not very pretty lots of back-ticks, the output
    is as the one in explain extended , lets try to improved it here.
  */
  writer->add("expanded_query", str.c_ptr_safe(), str.length());
}

void opt_trace_disable_if_no_security_context_access(THD *thd)
{
  if (likely(!(thd->variables.optimizer_trace &
               Opt_trace_context::FLAG_ENABLED)) ||  // (1)
      thd->system_thread)                            // (2)
  {
    /*
      (1) We know that the routine's execution starts with "enabled=off".
      If it stays so until the routine ends, we needn't do security checks on
      the routine.
      If it does not stay so, it means the definer sets it to "on" somewhere
      in the routine's body. Then it is his conscious decision to generate
      traces, thus it is still correct to skip the security check.

      (2) Threads of the Events Scheduler have an unusual security context
      (thd->m_main_security_ctx.priv_user==NULL, see comment in
      Security_context::change_security_context()).
    */
    return;
  }
  Opt_trace_context *const trace= &thd->opt_trace;
  if (!thd->trace_started())
  {
    /*
      @@optimizer_trace has "enabled=on" but trace is not started.
      Either Opt_trace_start ctor was not called for our statement (3), or it
      was called but at that time, the variable had "enabled=off" (4).

      There are no known cases of (3).

      (4) suggests that the user managed to change the variable during
      execution of the statement, and this statement is using
      view/routine (note that we have not been able to provoke this, maybe
      this is impossible). If it happens it is suspicious.

      We disable I_S output. And we cannot do otherwise: we have no place to
      store a possible "missing privilege" information (no Opt_trace_stmt, as
      is_started() is false), so cannot do security checks, so cannot safely
      do tracing, so have to disable I_S output. And even then, we don't know
      when to re-enable I_S output, as we have no place to store the
      information "re-enable tracing at the end of this statement", and we
      don't even have a notion of statement here (statements in the optimizer
      trace world mean an Opt_trace_stmt object, and there is none here). So
      we must disable for the session's life.

      COM_FIELD_LIST opens views, thus used to be a case of (3). To avoid
      disabling I_S output for the session's life when this command is issued
      (like in: "SET OPTIMIZER_TRACE='ENABLED=ON';USE somedb;" in the 'mysql'
      command-line client), we have decided to create a Opt_trace_start for
      this command. The command itself is not traced though
      (SQLCOM_SHOW_FIELDS does not have CF_OPTIMIZER_TRACE).
    */
    return;
  }
  /*
    Note that thd->main_security_ctx.master_access is probably invariant
    accross the life of THD: GRANT/REVOKE don't affect global privileges of an
    existing connection, per the manual.
  */
  if (!(thd->main_security_ctx.check_access(GLOBAL_ACLS & ~GRANT_ACL)) &&
      (0 != strcmp(thd->main_security_ctx.priv_user,
                   thd->security_context()->priv_user) ||
       0 != my_strcasecmp(system_charset_info,
                          thd->main_security_ctx.priv_host,
                          thd->security_context()->priv_host)))
    trace->missing_privilege();
}

void opt_trace_disable_if_no_stored_proc_func_access(THD *thd, sp_head *sp)
{
  if (likely(!(thd->variables.optimizer_trace &
               Opt_trace_context::FLAG_ENABLED)) ||
      thd->system_thread ||
      !thd->trace_started())
    return;

  Opt_trace_context *const trace= &thd->opt_trace;
  bool full_access;
  Security_context *const backup_thd_sctx= thd->security_context();
  thd->set_security_context(&thd->main_security_ctx);
  const bool rc= check_show_routine_access(thd, sp, &full_access) || !full_access;
  thd->set_security_context(backup_thd_sctx);
  if (rc)
    trace->missing_privilege();
}

/**
   If tracing is on, checks additional privileges on a list of tables/views,
   to make sure that the user has the right to do SHOW CREATE TABLE/VIEW and
   "SELECT *". For that:
   - this functions checks table-level SELECT
   - which is sufficient for SHOW CREATE TABLE and "SELECT *", if a base table
   - if a view, if the view has not been identified as such then
   opt_trace_disable_if_no_view_access() will be later called and check SHOW
   VIEW; other we check SHOW VIEW here; SHOW VIEW + SELECT is sufficient for
   SHOW CREATE VIEW.
   If a privilege is missing, notifies the trace system.

   @param thd
   @param tbl list of tables to check
*/

void opt_trace_disable_if_no_tables_access(THD *thd, TABLE_LIST *tbl)
{
  if (likely(!(thd->variables.optimizer_trace &
              Opt_trace_context::FLAG_ENABLED)) ||
      thd->system_thread ||
      !thd->trace_started())
    return;

  Opt_trace_context *const trace= &thd->opt_trace;
  Security_context *const backup_thd_sctx= thd->security_context();
  thd->set_security_context(&thd->main_security_ctx);
  const TABLE_LIST *const first_not_own_table= thd->lex->first_not_own_table();
  for (TABLE_LIST *t= tbl; t != NULL && t != first_not_own_table;
       t= t->next_global)
  {
    /*
      Anonymous derived tables (as in
      "SELECT ... FROM (SELECT ...)") and table functions
      don't have their grant.privilege set.
    */
    if (!t->is_anonymous_derived_table() &&
        !t->table_function)
    {
      const GRANT_INFO backup_grant_info= t->grant;
      Security_context *const backup_table_sctx= t->security_ctx;
      t->security_ctx= NULL;
      /*
        (1) check_table_access() fills t->grant.privilege.
        (2) Because SELECT privileges can be column-based,
        check_table_access() will return 'false' as long as there is SELECT
        privilege on one column. But we want a table-level privilege.
      */

      bool rc =
          check_table_access(thd, SELECT_ACL, t, false, 1, true) ||  // (1)
          ((t->grant.privilege & SELECT_ACL) == NO_ACL);             // (2)
      if (t->is_view())
      {
        /*
          It's a view which has already been opened: we are executing a
          prepared statement. The view has been unfolded in the global list of
          tables. So underlying tables will be automatically checked in the
          present function, but we need an explicit check of SHOW VIEW:
        */
        rc |= check_table_access(thd, SHOW_VIEW_ACL, t, false, 1, true);
      }
      t->security_ctx= backup_table_sctx;
      t->grant= backup_grant_info;
      if (rc)
      {
        trace->missing_privilege();
        break;
      }
    }
  }
  thd->set_security_context(backup_thd_sctx);
  return;
}

void opt_trace_disable_if_no_view_access(THD *thd, TABLE_LIST *view,
                                         TABLE_LIST *underlying_tables)
{

  if (likely(!(thd->variables.optimizer_trace &
               Opt_trace_context::FLAG_ENABLED)) ||
      thd->system_thread ||
      !thd->trace_started())
    return;

  Opt_trace_context *const trace= &thd->opt_trace;
  Security_context *const backup_table_sctx= view->security_ctx;
  Security_context *const backup_thd_sctx= thd->security_context();
  const GRANT_INFO backup_grant_info= view->grant;

  view->security_ctx= NULL;  // no SUID context for view
  // no SUID context for THD
  thd->set_security_context(&thd->main_security_ctx);
  const int rc= check_table_access(thd, SHOW_VIEW_ACL, view, false, 1, true);

  view->security_ctx= backup_table_sctx;
  thd->set_security_context(backup_thd_sctx);
  view->grant= backup_grant_info;

  if (rc)
  {
    trace->missing_privilege();
    return;
  }
  /*
    We needn't check SELECT privilege on this view. Some
    opt_trace_disable_if_no_tables_access() call has or will check it.

    Now we check underlying tables/views of our view:
  */
  opt_trace_disable_if_no_tables_access(thd, underlying_tables);
  return;
}


/**
  @class Opt_trace_stmt

  The trace of one statement.
*/

Opt_trace_stmt::Opt_trace_stmt(Opt_trace_context *ctx_arg)
{
  ctx= ctx_arg;
  current_json= new Json_writer();
  missing_priv= false;
  I_S_disabled= 0;
}

Opt_trace_stmt::~Opt_trace_stmt()
{
  delete current_json;
}

size_t Opt_trace_stmt::get_length()
{
  return current_json->output.length();
}

size_t Opt_trace_stmt::get_truncated_bytes()
{
  return current_json->get_truncated_bytes();
}

void Opt_trace_stmt::set_query(const char *query_ptr, size_t length,
                               const CHARSET_INFO *charset)
{
  query.append(query_ptr, length, charset);
}

void Opt_trace_context::missing_privilege()
{
  if (current_trace)
    current_trace->missing_privilege();
}

void Opt_trace_context::set_allowed_mem_size(size_t mem_size)
{
  current_trace->set_allowed_mem_size(mem_size);
}

/*
  TODO: In future when we would be saving multiple trace,
  this function would return
  max_mem_size - memory_occupied_by_the_saved_traces
*/

size_t Opt_trace_context::remaining_mem_size()
{
  return max_mem_size;
}

/*
  Disable tracing for children if the current trace is already present.
  Currently only one trace is stored and there is no mechanism
  to restore traces, so disabling tracing for children is the best option.
*/

bool Opt_trace_context::disable_tracing_if_required()
{
  if (current_trace)
  {
    current_trace->disable_tracing_for_children();
    return true;
  }
  return false;
}

bool Opt_trace_context::enable_tracing_if_required()
{
  if (current_trace)
  {
    current_trace->enable_tracing_for_children();
    return true;
  }
  return false;
}

bool Opt_trace_context::is_enabled()
{
  if (current_trace)
    return current_trace->is_enabled();
  return false;
}

Opt_trace_context::Opt_trace_context() : traces(PSI_INSTRUMENT_MEM)
{
  current_trace= NULL;
  max_mem_size= 0;
}
Opt_trace_context::~Opt_trace_context()
{
  delete_traces();
}

void Opt_trace_context::set_query(const char *query, size_t length, const CHARSET_INFO *charset)
{
  current_trace->set_query(query, length, charset);
}

void Opt_trace_context::start(THD *thd, TABLE_LIST *tbl,
                  enum enum_sql_command sql_command,
                  const char *query,
                  size_t query_length,
                  const CHARSET_INFO *query_charset,
                  ulong max_mem_size_arg)
{
  /*
    This is done currently because we don't want to have multiple
    traces open at the same time, so as soon as a new trace is created
    we forcefully end the previous one, if it has not ended by itself.
    This would mostly happen with stored functions or procedures.

    TODO: handle multiple traces
  */
  DBUG_ASSERT(!current_trace);
  current_trace= new Opt_trace_stmt(this);
  max_mem_size= max_mem_size_arg;
  set_allowed_mem_size(remaining_mem_size());
}

void Opt_trace_context::end()
{
  if (current_trace)
    traces.push(current_trace);

  if (!traces.elements())
    return;
  if (traces.elements() > 1)
  {
    Opt_trace_stmt *prev= traces.at(0);
    delete prev;
    traces.del(0);
  }
  current_trace= NULL;
}


void Opt_trace_start::init(THD *thd,
                           TABLE_LIST *tbl,
                           enum enum_sql_command sql_command,
                           List<set_var_base> *set_vars,
                           const char *query,
                           size_t query_length,
                           const CHARSET_INFO *query_charset)
{
  /*
    if optimizer trace is enabled and the statment we have is traceable,
    then we start the context.
  */
  const ulonglong var= thd->variables.optimizer_trace;
  traceable= FALSE;
  if (unlikely(var & Opt_trace_context::FLAG_ENABLED) &&
      sql_command_can_be_traced(sql_command) &&
      !list_has_optimizer_trace_table(tbl) &&
      !sets_var_optimizer_trace(sql_command, set_vars) &&
      !thd->system_thread &&
      !ctx->disable_tracing_if_required())
  {
    ctx->start(thd, tbl, sql_command, query, query_length, query_charset,
               thd->variables.optimizer_trace_max_mem_size);
    ctx->set_query(query, query_length, query_charset);
    traceable= TRUE;
    opt_trace_disable_if_no_tables_access(thd, tbl);
    Json_writer *w= ctx->get_current_json();
    w->start_object();
    w->add_member("steps").start_array();
  }
}

Opt_trace_start::~Opt_trace_start()
{
  if (traceable)
  {
    Json_writer *w= ctx->get_current_json();
    w->end_array();
    w->end_object();
    ctx->end();
    traceable= FALSE;
  }
  else
  {
    ctx->enable_tracing_if_required();
  }
}

void Opt_trace_stmt::fill_info(Opt_trace_info* info)
{
  if (unlikely(info->missing_priv= get_missing_priv()))
  {
    info->trace_ptr= info->query_ptr= "";
    info->trace_length= info->query_length= 0;
    info->query_charset= &my_charset_bin;
    info->missing_bytes= 0;
  }
  else
  {
    info->trace_ptr= current_json->output.get_string()->ptr();
    info->trace_length= get_length();
    info->query_ptr= query.ptr();
    info->query_length= query.length();
    info->query_charset= query.charset();
    info->missing_bytes= get_truncated_bytes();
    info->missing_priv= get_missing_priv();
  }
}

void Opt_trace_stmt::missing_privilege()
{
  missing_priv= true;
}

void Opt_trace_stmt::disable_tracing_for_children()
{
  ++I_S_disabled;
}

void Opt_trace_stmt::enable_tracing_for_children()
{
  if (I_S_disabled)
    --I_S_disabled;
}

void Opt_trace_stmt::set_allowed_mem_size(size_t mem_size)
{
  current_json->set_size_limit(mem_size);
}

/*
  Prefer this when you are iterating over JOIN_TABs
*/

void Json_writer::add_table_name(const JOIN_TAB *tab)
{
  DBUG_ASSERT(tab->join->thd->trace_started());
  if (tab != NULL)
  {
    char table_name_buffer[SAFE_NAME_LEN];
    if (tab->table && tab->table->derived_select_number)
    {
      /* Derived table name generation */
      size_t len= my_snprintf(table_name_buffer, sizeof(table_name_buffer)-1,
                             "<derived%u>",
                             tab->table->derived_select_number);
      add_str(table_name_buffer, len);
    }
    else if (tab->bush_children)
    {
      JOIN_TAB *ctab= tab->bush_children->start;
      size_t len= my_snprintf(table_name_buffer,
                           sizeof(table_name_buffer)-1,
                           "<subquery%d>",
                           ctab->emb_sj_nest->sj_subq_pred->get_identifier());
      add_str(table_name_buffer, len);
    }
    else
    {
      TABLE_LIST *real_table= tab->table->pos_in_table_list;
      add_str(real_table->alias.str, real_table->alias.length);
    }
  }
  else
    DBUG_ASSERT(0);
}

void Json_writer::add_table_name(const TABLE *table)
{
  add_str(table->pos_in_table_list->alias.str);
}


void trace_condition(THD * thd, const char *name, const char *transform_type,
                    Item *item, const char *table_name)
{
  Json_writer_object trace_wrapper(thd);
  Json_writer_object trace_cond(thd, transform_type);
  trace_cond.add("condition", name);
  if (table_name)
    trace_cond.add("attached_to", table_name);
  trace_cond.add("resulting_condition", item);
}


void add_table_scan_values_to_trace(THD *thd, JOIN_TAB *tab)
{
  DBUG_ASSERT(thd->trace_started());
  Json_writer_object table_records(thd);
  table_records.add_table_name(tab);
  Json_writer_object table_rec(thd, "table_scan");
  table_rec.add("rows", tab->found_records)
           .add("cost", tab->read_time);
}


/*
  @brief
    Add the tables inside a partial join to the optimizer trace

  @param join                join handler
  @param idx                 length of the partial QEP in 'join->positions'
  @table_map                 map of all non-const tables of the join

  @note
    This function is used during best_access_path to print the tables
    inside the partial join that were considered doing the cost based
    analysis of the various join orders.
*/

void trace_plan_prefix(JOIN *join, uint idx, table_map join_tables)
{
  THD *const thd= join->thd;
  DBUG_ASSERT(thd->trace_started());

  Json_writer_array plan_prefix(thd, "plan_prefix");
  for (uint i= 0; i < idx; i++)
  {
    TABLE_LIST *const tr= join->positions[i].table->tab_list;
    if (!(tr->map & join_tables))
      plan_prefix.add_table_name(join->positions[i].table);
  }
}


/*
  Print the join order of all the tables for top level select.

  For example:

  select * from ot1
   where ot1.a IN (select it1.a from it1, it2 where it1.b=it2.a);

  So this function would print
    ot1, <subquery2>  ----> For select #1
*/

void print_final_join_order(JOIN *join)
{
  DBUG_ASSERT(join->thd->trace_started());

  Json_writer_object join_order(join->thd);
  Json_writer_array  best_order(join->thd, "best_join_order");
  JOIN_TAB *j;
  uint i;
  for (j= join->join_tab,i=0 ; i < join->top_join_tab_count;
       i++, j++)
    best_order.add_table_name(j);
}


void print_best_access_for_table(THD *thd, POSITION *pos,
                                 enum join_type type)
{
  DBUG_ASSERT(thd->trace_started());

  Json_writer_object obj(thd, "chosen_access_method");
  obj.add("type", type == JT_ALL ? "scan" : join_type_str[type]);
  obj.add("records", pos->records_read);
  obj.add("cost", pos->read_time);
  obj.add("uses_join_buffering", pos->use_join_buffer);
  if (pos->range_rowid_filter_info)
  {
    uint key_no= pos->range_rowid_filter_info->key_no;
    obj.add("rowid_filter_key",
            pos->table->table->key_info[key_no].name);
  }
}


/*
  Introduce enum_query_type flags parameter, maybe also allow
  EXPLAIN also use this function.
*/

void Json_writer::add_str(Item *item)
{
  if (item)
  {
    THD *thd= current_thd;
    StringBuffer<256> str(system_charset_info);

    ulonglong save_option_bits= thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;
    item->print(&str,
                enum_query_type(QT_TO_SYSTEM_CHARSET | QT_SHOW_SELECT_NUMBER
                               | QT_ITEM_IDENT_SKIP_DB_NAMES));
    thd->variables.option_bits= save_option_bits;
    add_str(str.c_ptr_safe());
  }
  else
    add_null();
}

void Opt_trace_context::delete_traces()
{
  if (traces.elements())
  {
    while (traces.elements())
    {
      Opt_trace_stmt *prev= traces.at(0);
      delete prev;
      traces.del(0);
    }
  }
}


int fill_optimizer_trace_info(THD *thd, TABLE_LIST *tables, Item *)
{
  TABLE *table= tables->table;
  Opt_trace_info info;

  /*  get_values of trace, query , missing bytes and missing_priv

      @todo: Need an iterator here to walk over all the traces
  */
  Opt_trace_context* ctx= &thd->opt_trace;

  if (!thd->opt_trace.empty())
  {
    Opt_trace_stmt *stmt= ctx->get_top_trace();
    stmt->fill_info(&info);

    table->field[0]->store(info.query_ptr, static_cast<uint>(info.query_length),
                           info.query_charset);
    table->field[1]->store(info.trace_ptr, static_cast<uint>(info.trace_length),
                           system_charset_info);
    table->field[2]->store(info.missing_bytes, true);
    table->field[3]->store(info.missing_priv, true);
    //  Store in IS
    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}
