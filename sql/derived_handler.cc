/*
   Copyright (c) 2018, 2019 MariaDB

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
#include "derived_handler.h"


/**
  The methods of the Pushdown_derived class.

  The objects of this class are used for pushdown of the derived tables
  into engines. The main  method of the class is Pushdown_derived::execute()
  that initiates execution of the query specifying a derived by a foreign
  engine, receives the rows of the result set and put them in a temporary
  table on the server side.

  The method uses only the functions of the derived_handle interface to do
  this. The constructor of the class gets this interface as a parameter.

  Currently a derived tables pushed into an engine is always materialized.
  It could be changed if the cases when the tables is used as driving table.
*/


Pushdown_derived::Pushdown_derived(TABLE_LIST *tbl, derived_handler *h)
 : derived(tbl), handler(h)
{
  is_analyze= handler->thd->lex->analyze_stmt;
}


Pushdown_derived::~Pushdown_derived()
{
  delete handler;
}


int Pushdown_derived::execute()
{
  int err;
  THD *thd= handler->thd;
  TABLE *table= handler->table;
  TMP_TABLE_PARAM *tmp_table_param= handler->tmp_table_param;

  DBUG_ENTER("Pushdown_query::execute");

  if ((err= handler->init_scan()))
    goto error;

  if (is_analyze)
  {
    handler->end_scan();
    DBUG_RETURN(0);
  }

  while (!(err= handler->next_row()))
  {
    if (unlikely(thd->check_killed()))
    {
      handler->end_scan();
      DBUG_RETURN(-1);
    }

    if ((err= table->file->ha_write_tmp_row(table->record[0])))
    {
      bool is_duplicate;
      if (likely(!table->file->is_fatal_error(err, HA_CHECK_DUP)))
        continue;                           // Distinct elimination

      if (create_internal_tmp_table_from_heap(thd, table,
                                              tmp_table_param->start_recinfo,
                                              &tmp_table_param->recinfo,
                                              err, 1, &is_duplicate))
        DBUG_RETURN(1);
      if (is_duplicate)
        continue;
    }
  }

  if (err != 0 && err != HA_ERR_END_OF_FILE)
    goto error;

  if ((err= handler->end_scan()))
    goto error_2;

  DBUG_RETURN(0);

error:
  handler->end_scan();
error_2:
  handler->print_error(err, MYF(0));
  DBUG_RETURN(-1);                              // Error not sent to client
}


void derived_handler::print_error(int error, myf errflag)
{
  my_error(ER_GET_ERRNO, MYF(0), error, hton_name(ht)->str);
}


void derived_handler::set_derived(TABLE_LIST *tbl)
{
  derived= tbl;
  table= tbl->table;
  unit= tbl->derived;
  select= unit->first_select();
  tmp_table_param= ((select_unit *)(unit->result))->get_tmp_table_param();
}

