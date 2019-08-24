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
#include "select_handler.h"


/**
  The methods of the Pushdown_select class.

  The objects of this class are used for pushdown of the select queries
  into engines. The main  method of the class is Pushdown_select::execute()
  that initiates execution of a select query by a foreign engine, receives the
  rows of the result set, put it in a buffer of a temporary table and send
  them from the buffer directly into output.

  The method uses the functions of the select_handle interface to do this.
  It also employes plus some helper functions to create the needed temporary
  table and to send rows from the temporary table into output.
  The constructor of the class gets the select_handler interface as a parameter.
*/


Pushdown_select::Pushdown_select(SELECT_LEX *sel, select_handler *h)
  : select(sel), handler(h)
{
  is_analyze= handler->thd->lex->analyze_stmt;
}


Pushdown_select::~Pushdown_select()
{
  if (handler->table)
    free_tmp_table(handler->thd, handler->table);
  delete handler;
  select->select_h= NULL;
}


bool Pushdown_select::init()
{
  List<Item> types;
  TMP_TABLE_PARAM tmp_table_param;
  THD *thd= handler->thd;
  DBUG_ENTER("Pushdown_select::init");
  if (select->master_unit()->join_union_item_types(thd, types, 1))
    DBUG_RETURN(true);
  tmp_table_param.init();
  tmp_table_param.field_count= types.elements;

  handler->table= create_tmp_table(thd, &tmp_table_param, types,
                                   (ORDER *) 0, false, 0,
                                   TMP_TABLE_ALL_COLUMNS, 1,
                                   &empty_clex_str, true, false);
  if (!handler->table)
    DBUG_RETURN(true);
  if (handler->table->fill_item_list(&result_columns))
    DBUG_RETURN(true);
  DBUG_RETURN(false);
}


bool Pushdown_select::send_result_set_metadata()
{
  THD *thd= handler->thd;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("Pushdown_select::send_result_set_metadata");

#ifdef WITH_WSREP
  if (WSREP(thd) && thd->wsrep_retry_query)
  {
    WSREP_DEBUG("skipping select metadata");
    DBUG_RETURN(false);
  }
  #endif /* WITH_WSREP */
  if (protocol->send_result_set_metadata(&result_columns,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}


bool Pushdown_select::send_data()
{
  THD *thd= handler->thd;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("Pushdown_select::send_data");

  if (thd->killed == ABORT_QUERY)
    DBUG_RETURN(false);

  protocol->prepare_for_resend();
  if (protocol->send_result_set_row(&result_columns))
  {
    protocol->remove_last_row();
    DBUG_RETURN(true);
  }

  thd->inc_sent_row_count(1);

  if (thd->vio_ok())
    DBUG_RETURN(protocol->write());

  DBUG_RETURN(false);
}


bool Pushdown_select::send_eof()
{
  THD *thd= handler->thd;
  DBUG_ENTER("Pushdown_select::send_eof");

  /*
    Don't send EOF if we're in error condition (which implies we've already
    sent or are sending an error)
  */
  if (thd->is_error())
    DBUG_RETURN(true);
  ::my_eof(thd);
  DBUG_RETURN(false);
}


int Pushdown_select::execute()
{
  int err;
  THD *thd= handler->thd;

  DBUG_ENTER("Pushdown_select::execute");

  if ((err= handler->init_scan()))
    goto error;

  if (is_analyze)
  {
    handler->end_scan();
    DBUG_RETURN(0);
  }

  if (send_result_set_metadata())
    DBUG_RETURN(-1);

  while (!(err= handler->next_row()))
  {
    if (thd->check_killed() || send_data())
    {
      handler->end_scan();
      DBUG_RETURN(-1);
    }
  }

  if (err != 0 && err != HA_ERR_END_OF_FILE)
    goto error;

  if ((err= handler->end_scan()))
   goto error_2;

  if (send_eof())
    DBUG_RETURN(-1);

  DBUG_RETURN(0);

error:
  handler->end_scan();
error_2:
  handler->print_error(err, MYF(0));
  DBUG_RETURN(-1);                              // Error not sent to client
}

void select_handler::print_error(int error, myf errflag)
{
  my_error(ER_GET_ERRNO, MYF(0), error, hton_name(ht)->str);
}
