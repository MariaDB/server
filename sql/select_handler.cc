/*
   Copyright (c) 2018, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "sql_cte.h"
#include "sql_explain.h"
#include "select_handler.h"


/**
  The methods of the pushdown_handler class and of the two classes derived
  from it, select_handler and multi_upddel_handler.

  The objects of these classes are used for pushdown of whole statements
  into engines.

  For a select query, the main method is select_handler::execute() that
  initiates execution of the query by a foreign engine, receives the rows of
  the result set, put it in a buffer of a temporary table and send them from
  the buffer directly into output. The method uses the functions of the
  select_handler interface to do this. It also employes plus some helper
  functions to create the needed temporary table and to send rows from the
  temporary table into output.

  For a multi-table UPDATE/DELETE, multi_upddel_handler::execute() hands the
  whole statement over to the engine with a single call and reports the row
  counts the engine returns.
*/


/*
  Walk the tables of a statement starting at first_tbl and return the first
  non-NULL pushdown handler that create() builds for the handlerton of one of
  them, or NULL if no engine offers to take the statement over. create() is
  expected to return NULL for a handlerton that does not provide the requested
  interface.
*/
template <typename Create>
static auto scan_tables_for_pushdown(TABLE_LIST *first_tbl, Create create)
    -> decltype(create((handlerton *) nullptr))
{
  for (TABLE_LIST *tbl= first_tbl; tbl; tbl= tbl->next_global)
  {
    if (!tbl->table)
      continue;
    if (auto handler= create(tbl->table->file->partition_ht()))
      return handler;
  }
  return nullptr;
}


/**
  @brief
    Look for provision of the select_handler interface by a foreign engine.
    Must not be called directly, use find_single_select_handler() or
    find_partial_select_handler() instead.

  @param
    thd             The thread handler
    select_lex      SELECT_LEX object, must be passed in the cases of:
                    - single select pushdown
                    - partial pushdown (part of a UNION/EXCEPT/INTERSECT)
                    Must be NULL in case of entire unit pushdown
    select_lex_unit SELECT_LEX_UNIT object, must be passed in the cases of:
                    - entire unit pushdown
                    - partial pushdown (part of a UNION/EXCEPT/INTERSECT)
                    Must be NULL in case of single select pushdown

  @details
    The function checks that this is an upper level select and if so looks
    through its tables searching for one whose handlerton owns a
    create_select call-back function. If the call of this function returns
    a select_handler interface object then the server will push the select
    query into this engine.
    This function does not check if the select has tables from
    different engines. Such a check must be done inside each engine's
    create_select function.
    Also the engine's create_select function must perform other checks
    to make sure the engine can execute the query.

  @retval the found select_handler if the search is successful
          0  otherwise
*/
static select_handler *find_select_handler_inner(THD *thd,
                                    SELECT_LEX *select_lex,
                                    SELECT_LEX_UNIT *select_lex_unit)
{
  // Pushdown is not supported for non-top-level SELECTs
  if (select_lex->master_unit()->outer_select())
    return 0;

  TABLE_LIST *tbl= nullptr;
  // For SQLCOM_INSERT_SELECT the server takes TABLE_LIST
  // from thd->lex->query_tables and skips its first table
  // b/c it is the target table for the INSERT..SELECT.
  if (thd->lex->sql_command != SQLCOM_INSERT_SELECT)
  {
    tbl= select_lex->join->tables_list;
  }
  else if (thd->lex->query_tables &&
           thd->lex->query_tables->next_global)
  {
    tbl= thd->lex->query_tables->next_global;
  }
  else
    return 0;

  return scan_tables_for_pushdown(tbl, [&](handlerton *ht) -> select_handler *
  {
    return ht->create_select ?
           ht->create_select(thd, select_lex, select_lex_unit) : nullptr;
  });
}


/**
  Wrapper for find_select_handler_inner() for the case of single select
  pushdown. See more comments at the description of
  find_select_handler_inner()

*/
select_handler *find_single_select_handler(THD *thd, SELECT_LEX *select_lex)
{
  return find_select_handler_inner(thd, select_lex, nullptr);
}


/**
  Wrapper for find_select_handler_inner() for the case of partial select
  pushdown. Partial pushdown means that a unit (i.e. multiple selects combined
  with UNION/EXCEPT/INTERSECT operators) cannot be pushed down to
  the storage engine as a whole but some particular selects of this unit can.
  For example,
    SELECT a FROM federated.t1  -- can be pushed down to Federated
    UNION
    SELECT b FROM local.t2      -- cannot be pushed down, executed locally

  See more comments at the description of find_select_handler_inner()

*/
select_handler *
find_partial_select_handler(THD *thd, SELECT_LEX *select_lex,
                            SELECT_LEX_UNIT *select_lex_unit)
{
  return find_select_handler_inner(thd, select_lex, select_lex_unit);
}


/**
  @brief
    Look for provision of the multi_upddel_handler interface by a foreign
    engine, i.e. for an engine that can perform the whole multi-table
    UPDATE/DELETE this statement is.

  @details
    Works the same way as find_select_handler_inner() does for a select, see
    the comments there. An engine's create_multi_upddel function must make
    sure the engine can execute the statement, in particular that all its
    tables belong to this engine.

  @retval the found multi_upddel_handler if the search is successful
          0  otherwise
*/

multi_upddel_handler *find_multi_upddel_handler(THD *thd, LEX *lex)
{
  return scan_tables_for_pushdown(lex->first_select_lex()->join->tables_list,
    [&](handlerton *ht) -> multi_upddel_handler *
  {
    return ht->create_multi_upddel ?
           ht->create_multi_upddel(thd, lex) : nullptr;
  });
}

pushdown_handler::pushdown_handler(THD *thd_arg, handlerton *ht_arg,
                                   SELECT_LEX *sel_lex,
                                   select_result *result_arg)
  : select_lex(sel_lex), thd(thd_arg), ht(ht_arg), result(result_arg),
    is_analyze(thd_arg->lex->analyze_stmt)
{}


void pushdown_handler::print_error(int error, myf errflag)
{
  my_error(ER_GET_ERRNO, MYF(0), error, hton_name(ht)->str);
}


bool pushdown_handler::send_eof()
{
  DBUG_ENTER("pushdown_handler::send_eof");
  DBUG_RETURN(result->send_eof());
}


select_handler::select_handler(THD *thd_arg, handlerton *ht_arg,
                               SELECT_LEX *sel_lex)
  : pushdown_handler(thd_arg, ht_arg, sel_lex, sel_lex->join->result),
    lex_unit(nullptr), table(nullptr)
{}

select_handler::select_handler(THD *thd_arg, handlerton *ht_arg,
                               SELECT_LEX_UNIT *sel_unit)
  : pushdown_handler(thd_arg, ht_arg, nullptr, sel_unit->result),
    lex_unit(sel_unit), table(nullptr)
{}

select_handler::select_handler(THD *thd_arg, handlerton *ht_arg,
                               SELECT_LEX *sel_lex, SELECT_LEX_UNIT *sel_unit)
  : pushdown_handler(thd_arg, ht_arg, sel_lex, sel_lex->join->result),
    lex_unit(sel_unit), table(nullptr)
{}

select_handler::~select_handler()
{
  if (table)
    free_tmp_table(thd, table);
}


TABLE *select_handler::create_tmp_table(THD *thd)
{
  List<Item> types;
  TMP_TABLE_PARAM tmp_table_param;
  DBUG_ENTER("select_handler::create_tmp_table");

  SELECT_LEX_UNIT *unit= nullptr;
  uint unit_parts_count= 0;

  if (lex_unit)
  {
    unit= lex_unit;
    SELECT_LEX *sl= unit->first_select();
    while (sl)
    {
      unit_parts_count++;
      sl= sl->next_select();
    }
  }
  else
  {
    unit= select_lex->master_unit();
    unit_parts_count= 1;
  }

  if (unit->join_union_item_types(thd, types, unit_parts_count))
    DBUG_RETURN(NULL);

  tmp_table_param.init();
  tmp_table_param.field_count= tmp_table_param.func_count= types.elements;
  TABLE *table= ::create_tmp_table(thd, &tmp_table_param, types,
                                   (ORDER *) 0, false, 0,
                                   TMP_TABLE_ALL_COLUMNS, 1,
                                   &empty_clex_str, true, false);
  DBUG_RETURN(table);
}


bool select_handler::prepare()
{
  DBUG_ENTER("select_handler::prepare");
  /*
    Some engines (e.g. XPand) initialize "table" on their own.
    So we need to create a temporary table only if "table" is NULL.
  */
  if (!table && !(table= create_tmp_table(thd)))
    DBUG_RETURN(true);
  DBUG_RETURN(table->fill_item_list(&result_columns));
}


bool select_handler::send_result_set_metadata()
{
  DBUG_ENTER("select_handler::send_result_set_metadata");

#ifdef WITH_WSREP
  if (WSREP(thd) && thd->wsrep_retry_query)
  {
    WSREP_DEBUG("skipping select metadata");
    DBUG_RETURN(false);
  }
  #endif /* WITH_WSREP */
  
  DBUG_RETURN(result->send_result_set_metadata(
        result_columns, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF));
}


bool select_handler::send_data()
{
  DBUG_ENTER("select_handler::send_data");
  int res= result->send_data(result_columns);
  // "-1" means "duplicate when executing UNION"
  if (res && res != -1)
    DBUG_RETURN(true);
  DBUG_RETURN(false);
}


int select_handler::execute()
{
  int err;

  DBUG_ENTER("select_handler::execute");

  if ((err= init_scan()))
    goto error;

  if (is_analyze)
  {
    end_scan();
    DBUG_RETURN(0);
  }

  if (send_result_set_metadata())
    DBUG_RETURN(-1);

  while (!(err= next_row()))
  {
    if (thd->check_killed() || send_data())
    {
      end_scan();
      DBUG_RETURN(-1);
    }
  }

  if (err != 0 && err != HA_ERR_END_OF_FILE)
    goto error;

  if ((err= end_scan()))
   goto error_2;

  if (send_eof())
    DBUG_RETURN(-1);

  DBUG_RETURN(0);

error:
  end_scan();
error_2:
  print_error(err, MYF(0));
  DBUG_RETURN(-1);                              // Error not sent to client
}

const char *select_handler::explain_type() const
{
  return pushed_select_text;
}

select_pushdown_type select_handler::get_pushdown_type()
{
  /*
    In the case of single SELECT select_lex is initialized and lex_unit==NULL,
    in the case of whole UNIT select_lex == NULL and lex_unit is initialized,
    in the case of partial pushdown both select_lex and lex_unit
      are initialized
  */
  if(!lex_unit)
    return select_pushdown_type::SINGLE_SELECT;

  return select_lex ? select_pushdown_type::PART_OF_UNIT :
                      select_pushdown_type::WHOLE_UNIT;
}


multi_upddel_handler::multi_upddel_handler(THD *thd_arg, handlerton *ht_arg,
                                           LEX *lex_arg)
  : pushdown_handler(thd_arg, ht_arg, lex_arg->first_select_lex(),
                     lex_arg->first_select_lex()->join->result),
    lex(lex_arg)
{}


/*
  Execute a multi-table UPDATE/DELETE that was pushed down into the engine.

  The engine does the whole job and only reports the row counts. The SQL
  layer still has to do the final part of the statement: invalidate the
  query cache, write the statement to the binary log and send the OK packet
  to the client. All of that is done by multi_update::send_eof() /
  multi_delete::send_eof(), so the counters are passed to the result object
  and send_eof() is called as if the rows had been updated locally.
*/

int multi_upddel_handler::execute()
{
  int err;
  ha_rows found_rows= 0, affected_rows= 0;

  DBUG_ENTER("multi_upddel_handler::execute");

  if ((err= batch_update_delete(&found_rows, &affected_rows)))
  {
    if (!thd->is_error())
      print_error(err, MYF(0));
    DBUG_RETURN(-1);
  }

  result->direct_update_delete_done(found_rows, affected_rows);

  if (send_eof())
    DBUG_RETURN(-1);

  DBUG_RETURN(0);
}


const char *multi_upddel_handler::explain_type() const
{
  enum_sql_command sql_command= lex->sql_command;
  return (sql_command == SQLCOM_DELETE ||
          sql_command == SQLCOM_DELETE_MULTI) ? pushed_delete_text :
                                                pushed_update_text;
}
