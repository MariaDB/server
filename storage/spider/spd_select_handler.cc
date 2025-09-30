#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_select.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_pushdown.h"
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
#include "partition_info.h"
#include "ha_partition.h"

extern handlerton *spider_hton_ptr;
/* We only consider the 0th connection */
constexpr int LINK_IDX= 0;

spider_select_handler::spider_select_handler(THD *thd, SELECT_LEX *select_lex,
                                             spider_fields *fields)
  : select_handler(thd, spider_hton_ptr, select_lex), fields(fields),
    store_error(0)
{}

spider_select_handler::~spider_select_handler()
{
  spider_free(spider_current_trx, fields->get_first_table_holder(), MYF(0));
  delete fields;
}

/* Returns whether the spider sh can handle an item for execution */
static bool spider_sh_check_item(Item *item, SPIDER_SHARE *share, THD *thd)
{
  enum Item::Type type= item->type();
  if (type == Item::SUBSELECT_ITEM)
    return false;
  if (!spider_param_use_pushdown_udf(thd, share->use_pushdown_udf) &&
      type == Item::FUNC_ITEM)
  {
    enum Item_func::Functype ftype= ((Item_func *) item)->functype();
    if (ftype == Item_func::UDF_FUNC || ftype == Item_func::FUNC_SP)
      return false;
  }
  return true;
}

/* Check whether the spider sh can handle a SELECT query */
static bool spider_sh_check_query(SELECT_LEX *select_lex, SPIDER_SHARE *share,
                                  THD *thd)
{
  List<Item> items;
  List_iterator_fast<Item> it(*select_lex->get_item_list());
  /*
    spider does not handle SQL_CALC_FOUND_ROWS correctly, see
    MDEV-37722. So we do not make select handler handle it until
    MDEV-37722 is fixed.
  */
  if (select_lex->options & OPTION_FOUND_ROWS)
    return true;
  while (Item *item= it++)
    item->walk(&Item::collect_item_processor, (void *) &items, WALK_SUBQUERY);
  if (select_lex->where)
    select_lex->where->walk(&Item::collect_item_processor, (void *) &items, WALK_SUBQUERY);
  if (select_lex->join->group_list)
    for (ORDER *order= select_lex->join->group_list; order;
         order= order->next)
      if (order->item_ptr)
        order->item_ptr->walk(&Item::collect_item_processor, (void *) &items, WALK_SUBQUERY);
  if (select_lex->join->order)
    for (ORDER *order= select_lex->join->order; order; order= order->next)
      /* TODO: What happens if item_ptr is NULL? When is it NULL? */
      if (order->item_ptr)
        order->item_ptr->walk(&Item::collect_item_processor, (void *) &items, WALK_SUBQUERY);
  if (select_lex->having)
    select_lex->having->walk(&Item::collect_item_processor, (void *) &items, WALK_SUBQUERY);
  it.init(items);
  while (Item *item= it++)
    if (!spider_sh_check_item(item, share, thd))
      return true;
  return false;
}

/*
  Get the spider handler from a table. If the table is partitioned,
  get its first read partition handler.
*/
static ha_spider *spider_sh_get_spider(TABLE* table)
{
  if (table->part_info)
  {
    uint part= bitmap_get_first_set(&table->part_info->read_partitions);
    ha_partition *partition= (ha_partition *) table->file;
    return (ha_spider *) partition->get_child_handlers()[part];
  }
  return (ha_spider *) table->file;
}

/* Initial check whether spider sh can handle tables */
static bool spider_sh_check_tables(TABLE_LIST *from, uint *n_tables)
{
  for (TABLE_LIST *tl= from; tl; (*n_tables)++, tl= tl->next_local)
  {
    TABLE *table= tl->table;
    /* Do not support temporary tables */
    if (!table)
      return true;
    /*
      Do not support partitioned table with more than one (read)
      partition
    */
    if (table->part_info &&
        bitmap_bits_set(&table->part_info->read_partitions) != 1)
      return true;
    /* One of the join tables is not a spider table */
    if (table->file->partition_ht() != spider_hton_ptr)
      return true;
    ha_spider* spider= spider_sh_get_spider(table);
    /* needed for table holder (see spider_add_table_holder()) */
    spider->idx_for_direct_join= *n_tables;
  }
  return false;
}

/* Check whether spider sh can handle table connections. */
static bool spider_sh_check_conns(TABLE_LIST *from, THD *thd,
                                  SPIDER_CONN **conn, int *dbton_id)
{
  *dbton_id= -1;
  *conn= NULL;
  for (TABLE_LIST *tl= from; tl; tl= tl->next_local)
  {
    ha_spider *spider= spider_sh_get_spider(tl->table);
    if (spider_check_trx_and_get_conn(thd, spider))
      return true;
    uint all_link_idx= spider->conn_link_idx[LINK_IDX];
    /*
      Only create if all tables have common backend for the first
      connection.

      TODO: expand this to find connections across tables using a
      common backend which is not necessarily that of the first
      connection of each table.
    */
    if (*dbton_id == -1)
      *dbton_id= spider->share->use_sql_dbton_ids[all_link_idx];
    else if (*dbton_id != (int) spider->share->use_sql_dbton_ids[all_link_idx])
      return true;
    /* Only create if the first connection is ok */
    if (spider->share->link_statuses[all_link_idx] != SPIDER_LINK_STATUS_OK)
      return true;
    /* only create if all tables have common first connection. */
    if (!*conn)
      *conn= spider->conns[LINK_IDX];
    else if (*conn != spider->conns[LINK_IDX])
      return true;
  }
  /*
    This assertion holds because for any j and
    i = spider->conn_link_idx[j]
    spider->share->use_sql_dbton_ids[i] == spider->conns[j]->dbton_id
  */
  assert(*dbton_id == (int) (*conn)->dbton_id);
  return false;
}

/* Setup table attributes for spider sh */
static void spider_sh_setup_tables(TABLE_LIST *from,
                                   SPIDER_TABLE_HOLDER *table_holders,
                                   THD *thd)
{
  for (TABLE_LIST *tl= from; tl; tl= tl->next_local)
  {
    ha_spider *spider= spider_sh_get_spider(tl->table);

    spider_add_table_holder(spider, table_holders);
    /*
      As in dml_init, wide_handler->lock_mode == -2 is a relic from
      MDEV-19002. Needed to add the likes of "lock in share mode" to
      INSERT ... SELECT, as promised by the selupd_lock_mode
      variable
    */
    if (spider->wide_handler->lock_mode == -2)
      spider->wide_handler->lock_mode=
          spider_param_selupd_lock_mode(thd, spider->share->selupd_lock_mode);
    /*
      Sync dbton_hdl->first_link_idx with the chosen connection so
      that translation of table names is correct. NOTE: in spider gbh
      this is done in spider_fields::set_first_link_idx, after a
      connection is randomly chosen by spider_fields::choose_a_conn
    */
    spider->dbton_handler[spider->conns[LINK_IDX]->dbton_id]->first_link_idx=
      LINK_IDX;
  }
}

/* Set up connection attributes for spider sh */
static bool spider_sh_setup_connection(THD *thd, SPIDER_CONN *conn,
                                       ha_spider *spider)
{
  SPIDER_TRX *trx= spider->wide_handler->trx;
  /*
    So that spider executes various "setup" queries according to the
    various spider system variables.

    TODO: in gbh this is part of
    spider_internal_start_trx_for_connection called from dml_init().
    Check whether other statements (e.g. xa start) in those
    subroutines need to be preserved as well.
  */
  if ((spider_check_and_set_sql_log_off(
         thd, conn, &spider->need_mons[LINK_IDX])) ||
      (spider_check_and_set_wait_timeout(
        thd, conn, &spider->need_mons[LINK_IDX])) ||
      (spider_param_sync_sql_mode(thd) &&
       (spider_check_and_set_sql_mode(
         thd, conn, &spider->need_mons[LINK_IDX]))) ||
      (spider_param_sync_autocommit(thd) &&
       (spider_check_and_set_autocommit(
         thd, conn, &spider->need_mons[LINK_IDX]))) ||
      (spider_param_sync_trx_isolation(thd) &&
       spider_check_and_set_trx_isolation(
         conn, &spider->need_mons[LINK_IDX])))
    return true;
  /* Do not create spider sh if in an xa transaction */
  if (trx->trx_xa)
    return true;
  if (!conn->join_trx)
  {
    /* So that spider executes queries that start a transaction. */
    spider_conn_queue_start_transaction(conn);
    /*
      So that spider executes a commit query on the connection, see
      spider_tree_first(trx->join_trx_top) in spider_commit()
    */
    conn->join_trx = 1;
    if (trx->join_trx_top)
      spider_tree_insert(trx->join_trx_top, conn);
    else
    {
      conn->p_small= NULL;
      conn->p_big= NULL;
      conn->c_small= NULL;
      conn->c_big= NULL;
      trx->join_trx_top= conn;
    }
  }
  return false;
}

/* Set up the spider_fields object for spider sh */
static spider_fields* spider_sh_setup_fields(
  SPIDER_TABLE_HOLDER *table_holders, uint n_tables, int dbton_id)
{
  spider_fields *fields= new spider_fields();
  fields->set_table_holder(table_holders, n_tables);
  fields->add_dbton_id(dbton_id);
  return fields;
}

/* Create and return a spider select handler if possible */
select_handler *spider_create_select_handler(THD *thd, SELECT_LEX *select_lex,
                                             SELECT_LEX_UNIT *)
{
  SPIDER_TABLE_HOLDER *table_holders;
  uint n_tables= 0;
  spider_fields *fields;
  ha_spider *first_spider;
  TABLE_LIST *from= select_lex->get_table_list();
  SPIDER_CONN *conn;
  int dbton_id;

  /*
    Conduct checks that a spider sh can be created. TODO: communicate
    in some way the reason for non-creation
  */
  if (spider_param_disable_select_handler(thd))
    return NULL;
  /*
    Do not create if the query has already been optimized. This
    happens for example during 2nd ps execution when spider fails to
    create sh during the 1st execution because there's a subquery in
    the original query.
  */
  if (!select_lex->first_cond_optimization)
    return NULL;
  if (spider_sh_check_tables(from, &n_tables))
    return NULL;
  first_spider= spider_sh_get_spider(from->table);
  if (spider_sh_check_query(select_lex, first_spider->share, thd))
    return NULL;
  if (spider_sh_check_conns(from, thd, &conn, &dbton_id))
    return NULL;

  /* Set up and create the spider sh */
  if (!(table_holders= spider_create_table_holder(n_tables)))
    return NULL;
  spider_sh_setup_tables(from, table_holders, thd);
  if (spider_sh_setup_connection(thd, conn, first_spider))
    goto free_table_holder;
  fields= spider_sh_setup_fields(table_holders, n_tables, dbton_id);
  return new spider_select_handler(thd, select_lex, fields);

free_table_holder:
  spider_free(spider_current_trx, table_holders, MYF(0));
  return NULL;
}

/* Set up result list for spider sh init_scan */
static void spider_sh_setup_result_list(ha_spider *spider,
                                        SELECT_LEX *select_lex)
{
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  /*
    Set result_list attributes which otherwise could be unintialised
    values. These attributes are needed for spider_db_store_results()
  */
  spider_set_result_list_param(spider);
  result_list->keyread = FALSE;
  /*
    Use original limit and offset for now.

    TODO: consider result paging
  */
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
}

/* Execute query with spider sh */
static int spider_sh_execute_query(ha_spider *spider,
                                   int *store_error, TABLE *table)
{
  SPIDER_CONN *conn= spider->conns[LINK_IDX];
  spider_db_handler *dbton_hdl= spider->dbton_handler[conn->dbton_id];
  dbton_hdl->set_sql_for_exec(
    SPIDER_SQL_TYPE_SELECT_SQL, LINK_IDX, NULL);
  spider_lock_before_query(conn, &spider->need_mons[LINK_IDX]);
  if (dbton_hdl->execute_sql_for_sh(
        conn, spider->share->tgt_dbs[conn->link_idx],
        spider->result_list.quick_mode,
        &spider->need_mons[LINK_IDX]))
  {
    int error= spider_unlock_after_query_1(conn);
    if ((error = spider->check_error_mode_eof(error)) ==
        HA_ERR_END_OF_FILE)
    {
      *store_error= HA_ERR_END_OF_FILE;
      error= 0;
    }
    return error;
  }
  /*
    So that in spider_db_store_results the check
       if (conn->connection_id != spider->connection_ids[link_idx])
    will go through
  */
  spider->connection_ids[LINK_IDX] = conn->connection_id;
  spider_unlock_after_query_2(conn, spider, LINK_IDX, table);
  return 0;
}

int spider_select_handler::init_scan()
{
  Query query= {select_lex->get_item_list(), 0,
    /*
      TODO: consider handling high_priority, sql_calc_found_rows
      etc. see st_select_lex::print
    */
    select_lex->options & SELECT_DISTINCT ? true : false,
    select_lex->get_table_list(), select_lex->where,
    select_lex->join->group_list, select_lex->join->order,
    select_lex->having, &select_lex->master_unit()->lim};
  ha_spider *spider= fields->get_first_table_holder()->spider;
  /*
    Reset select_column_mode so that previous insertions would not
    affect.
  */
  spider->select_column_mode= 0;
  spider_sh_setup_result_list(spider, select_lex);

  /* build query string */
  spider_make_query(query, fields, spider, table);

  /* send query */
  return spider_sh_execute_query(spider, &store_error, table);
}

int spider_select_handler::next_row()
{
  /* TODO: may need to execute the query again (result paging) */
  ha_spider *spider= fields->get_first_table_holder()->spider;
  SPIDER_RESULT_LIST *result_list= &spider->result_list;
  if (store_error)
    return store_error;
  if (result_list->current_row_num >= result_list->current->record_num)
    return HA_ERR_END_OF_FILE;
  else
    return spider_db_fetch(table->record[0], spider, table);
}

int spider_select_handler::end_scan()
{
  return 0;
}
