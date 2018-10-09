#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "derived_handler.h"

void derived_handler::set_derived(TABLE_LIST *tbl)
{
  derived= tbl;
  table= tbl->table;
  unit= tbl->derived;
  select= unit->first_select();
  tmp_table_param= select->next_select() ?
                   ((select_unit *)(unit->result))->get_tmp_table_param() :
                   &select->join->tmp_table_param;
}

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
  
