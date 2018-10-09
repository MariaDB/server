#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "select_handler.h"


Pushdown_select::Pushdown_select(SELECT_LEX *sel, select_handler *h)
  : select(sel), handler(h)
{ 
  is_analyze= handler->thd->lex->analyze_stmt;
}

Pushdown_select::~Pushdown_select()
{
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
