#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_select.h"
#include "spd_param.h"
#include "spd_db_include.h"
/* TODO: remove this include */
#include "spd_group_by_handler.h"
#include "spd_select_handler.h"
/* needed by ha_spider.h for SPIDER_PARTITION_HANDLER */
#include "spd_include.h"
#include "ha_spider.h"
/* needed for spider_current_trx */
#include "spd_malloc.h"
/* for spider_check_trx_and_get_conn */
#include "spd_trx.h"
/* for spider_lock_before_query */
#include "spd_conn.h"
/* for spider_db_fetch */
#include "spd_db_conn.h"
/* for spd_table.h */
#include "partition_element.h"
/* for spider_set_result_list_param */
#include "spd_table.h"

extern handlerton *spider_hton_ptr;

spider_select_handler::spider_select_handler(THD *thd, SELECT_LEX *select_lex,
                                             spider_fields *fields)
  : select_handler(thd, spider_hton_ptr, select_lex), fields(fields)
{}

spider_select_handler::~spider_select_handler()
{
  spider_free(spider_current_trx, fields->get_first_table_holder(), MYF(0));
  delete fields;
}

select_handler *spider_create_select_handler(THD *thd, SELECT_LEX *select_lex,
                                             SELECT_LEX_UNIT *)
{
  SPIDER_TABLE_HOLDER *table_holder;
  uint n_tables= 0;
  spider_fields *fields;
  ha_spider *spider;
  TABLE_LIST *from= select_lex->get_table_list();
  int dbton_id = -1;
  if (spider_param_disable_select_handler(thd))
    return NULL;
  for (TABLE_LIST *tl= from; tl; n_tables++, tl= tl->next_local)
  {
    /* One of the join tables is not a spider table */
    if (tl->table->file->partition_ht() != spider_hton_ptr)
      return NULL;
    spider = (ha_spider *) tl->table->file;
    spider->idx_for_direct_join = n_tables;
    /* TODO: only create if all tables have common first backend */
    if (dbton_id == -1)
      dbton_id= spider->share->use_sql_dbton_ids[0];
    else if (dbton_id != (int) spider->share->use_sql_dbton_ids[0])
      return NULL;
  }
  if (!(table_holder= spider_create_table_holder(n_tables)))
    DBUG_RETURN(NULL);
  for (TABLE_LIST *tl= from; tl; tl= tl->next_local)
  {
    spider = (ha_spider *) tl->table->file;
    spider_add_table_holder(spider, table_holder);
  }
  spider= (ha_spider *) from->table->file;
  spider_check_trx_and_get_conn(thd, spider);
  fields= new spider_fields();
  fields->set_table_holder(table_holder, n_tables);
  fields->add_dbton_id(dbton_id);
  return new spider_select_handler(thd, select_lex, fields);
}

int spider_select_handler::init_scan()
{
  /*
    longlong unused;
   */
  Query query= {select_lex->get_item_list(), 0,
    /* TODO: consider handling high_priority, sql_calc_found_rows
      etc. see st_select_lex::print */
    select_lex->options & SELECT_DISTINCT ? true : false,
    select_lex->get_table_list(), select_lex->where,
    /* TODO: do we need to reference join here? Can we get GROUP BY /
      ORDER BY from select_lex directly */
    select_lex->join->group_list, select_lex->join->order,
    select_lex->having, &select_lex->master_unit()->lim};
  ha_spider *spider= fields->get_first_table_holder()->spider;
  int link_idx= 0;
  SPIDER_CONN *conn= spider->conns[link_idx];
  spider_db_handler *dbton_hdl= spider->dbton_handler[conn->dbton_id];
  /*
    spider_prepare_init_scan(query, NULL, fields, spider,
                             spider->wide_handler->trx, unused, thd);
   */
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  spider_set_result_list_param(spider);
  /* TODO: we need to extract result_list init to a separate
    function, see also spider_prepare_init_scan. */
  result_list->keyread = FALSE;
  /* Use original limit and offset for now */
  if (select_lex->limit_params.explicit_limit)
  {
    result_list->limit_num= select_lex->get_limit();
    result_list->internal_offset= select_lex->get_offset();
  }
  else
  {
    result_list->limit_num= 9223372036854775807LL;
    result_list->internal_offset= 0;
  }
  /* print query */
  spider_make_query(query, fields, spider, table);

  /* send query */
  dbton_hdl->set_sql_for_exec(
    SPIDER_SQL_TYPE_SELECT_SQL, link_idx, NULL);
  spider_lock_before_query(conn, &spider->need_mons[link_idx]);
  dbton_hdl->execute_sql(
    SPIDER_SQL_TYPE_SELECT_SQL,
    conn,
    spider->result_list.quick_mode,
    &spider->need_mons[link_idx]);
  /*
    So that in spider_db_store_results the check
       if (conn->connection_id != spider->connection_ids[link_idx])
    will go through
  */
  spider->connection_ids[link_idx] = conn->connection_id;
  spider_unlock_after_query_2(conn, spider, link_idx, table);
  return 0;
}

int spider_select_handler::next_row()
{
  ha_spider *spider= fields->get_first_table_holder()->spider;
  SPIDER_RESULT_LIST *result_list= &spider->result_list;
  if (result_list->current_row_num >= result_list->current->record_num)
    return HA_ERR_END_OF_FILE;
  else
    return spider_db_fetch(table->record[0], spider, table);
}

int spider_select_handler::end_scan()
{
  return 0;
}
