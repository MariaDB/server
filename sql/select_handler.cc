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
#include "select_handler.h"


/**
  The methods of the select_handler class.

  The objects of this class are used for pushdown of the select queries
  into engines. The main  method of the class is select_handler::execute()
  that initiates execution of a select query by a foreign engine, receives the
  rows of the result set, put it in a buffer of a temporary table and send
  them from the buffer directly into output.

  The method uses the functions of the select_handle interface to do this.
  It also employes plus some helper functions to create the needed temporary
  table and to send rows from the temporary table into output.
  The constructor of the class gets the select_handler interface as a parameter.
*/


select_handler::select_handler(THD *thd_arg, handlerton *ht_arg,
                               SELECT_LEX *sel_lex)
  : select_lex(sel_lex), lex_unit(nullptr), table(nullptr),
    thd(thd_arg), ht(ht_arg), result(sel_lex->join->result),
    is_analyze(thd_arg->lex->analyze_stmt)
{}

select_handler::select_handler(THD *thd_arg, handlerton *ht_arg,
                               SELECT_LEX_UNIT *sel_unit)
  : select_lex(nullptr), lex_unit(sel_unit), table(nullptr),
    thd(thd_arg), ht(ht_arg), result(sel_unit->result),
    is_analyze(thd_arg->lex->analyze_stmt)
{}

select_handler::select_handler(THD *thd_arg, handlerton *ht_arg,
                               SELECT_LEX *sel_lex, SELECT_LEX_UNIT *sel_unit)
    : select_lex(sel_lex), lex_unit(sel_unit), table(nullptr), thd(thd_arg),
      ht(ht_arg), result(sel_lex->join->result),
      is_analyze(thd_arg->lex->analyze_stmt)
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


bool select_handler::send_eof()
{
  DBUG_ENTER("select_handler::send_eof");
  DBUG_RETURN(result->send_eof());
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

void select_handler::print_error(int error, myf errflag)
{
  my_error(ER_GET_ERRNO, MYF(0), error, hton_name(ht)->str);
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
