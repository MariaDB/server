/* Copyright (C) 2008-2019 Kentoku Shiba
   Copyright (C) 2019 MariaDB corp

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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "sql_select.h"
#include "ha_partition.h"
#include "sql_common.h"
#include <errmsg.h>
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_pushdown.h"
#include "spd_include.h"
#include "ha_spider.h"
#include "spd_conn.h"
#include "spd_db_conn.h"
#include "spd_malloc.h"
#include "spd_table.h"
#include "spd_ping_table.h"
#include "spd_group_by_handler.h"

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];

spider_group_by_handler::spider_group_by_handler(
  THD *thd_arg,
  Query *query_arg,
  spider_fields *fields_arg,
  const MY_BITMAP &skips1
) : group_by_handler(thd_arg, spider_hton_ptr),
  query(*query_arg), fields(fields_arg)
{
  DBUG_ENTER("spider_group_by_handler::spider_group_by_handler");
  spider = fields->get_first_table_holder()->spider;
  trx = spider->wide_handler->trx;
  my_bitmap_init(&skips, NULL, skips1.n_bits);
  bitmap_copy(&skips, &skips1);
  DBUG_VOID_RETURN;
}

spider_group_by_handler::~spider_group_by_handler()
{
  DBUG_ENTER("spider_group_by_handler::~spider_group_by_handler");
  spider_free(spider_current_trx, fields->get_first_table_holder(), MYF(0));
  delete fields;
  my_bitmap_free(&skips);
  /*
    The `skips' bitmap may have been copied to the result_list field
    of the same name
  */
  spider->result_list.skips= NULL;
  spider->result_list.n_aux= 0;
  DBUG_VOID_RETURN;
}

static int spider_prepare_init_scan(
  const Query& query, MY_BITMAP *skips, spider_fields *fields, ha_spider *spider,
  SPIDER_TRX *trx, longlong& offset_limit, THD *thd)
{
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  st_select_lex *select_lex;
  longlong select_limit, direct_order_limit;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_prepare_init_scan");

  spider->use_fields = TRUE;
  spider->fields = fields;

  spider->check_pre_call(TRUE);

  spider->pushed_pos = NULL;
  result_list->sorted = (query.group_by || query.order_by);
  spider_set_result_list_param(spider);
  /* TODO: do we need to remove ones that were initialised with the
    same value in ha_spider::init_fields? */
  spider->mrr_with_cnt = FALSE;
  spider->init_index_handler = FALSE;
  spider->use_spatial_index = FALSE;
  result_list->check_direct_order_limit = FALSE;
  /* Disable direct aggregate when GBH is on (MDEV-29502). */
  result_list->direct_aggregate = FALSE;
  spider->select_column_mode = 0;
  spider->search_link_idx = fields->get_ok_link_idx();
  spider->result_link_idx = spider->search_link_idx;

  spider_db_free_one_result_for_start_next(spider);

  spider->do_direct_update = FALSE;
  spider->direct_update_kinds = 0;
  spider_get_select_limit(spider, &select_lex, &select_limit, &offset_limit);
  direct_order_limit = spider_param_direct_order_limit(thd,
    share->direct_order_limit);
  if (
    direct_order_limit &&
    select_lex->limit_params.explicit_limit &&
    !(select_lex->options & OPTION_FOUND_ROWS) &&
    select_limit < direct_order_limit /* - offset_limit */
  ) {
    result_list->internal_limit = select_limit /* + offset_limit */;
    result_list->split_read = select_limit /* + offset_limit */;
    result_list->bgs_split_read = select_limit /* + offset_limit */;

    result_list->split_read_base = 9223372036854775807LL;
    result_list->semi_split_read = 0;
    result_list->semi_split_read_limit = 9223372036854775807LL;
    result_list->first_read = 9223372036854775807LL;
    result_list->second_read = 9223372036854775807LL;
    trx->direct_order_limit_count++;
  }
  result_list->semi_split_read_base = 0;
  result_list->set_split_read = TRUE;
  if (int error_num = spider_set_conn_bg_param(spider))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
  result_list->finish_flg = FALSE;
  result_list->record_num = 0;
  result_list->keyread = FALSE;
  result_list->desc_flg = FALSE;
  result_list->sorted = FALSE;
  result_list->key_info = NULL;
  result_list->key_order = 0;
  result_list->limit_num =
    result_list->internal_limit >= result_list->split_read ?
    result_list->split_read : result_list->internal_limit;
  result_list->skips= skips;
  result_list->n_aux= query.n_aux;

  if (select_lex->limit_params.explicit_limit)
  {
    result_list->internal_offset += offset_limit;
  } else {
    offset_limit = 0;
  }
  DBUG_RETURN(0);
}

static int spider_send_query(
  spider_fields *fields, ha_spider *spider, SPIDER_TRX *trx, TABLE *table,
  int& store_error)
{
  int error_num, link_idx;
  spider_db_handler *dbton_hdl;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  SPIDER_LINK_IDX_HOLDER *link_idx_holder;
  DBUG_ENTER("spider_send_query");

  fields->set_pos_to_first_link_idx_chain();
  while ((link_idx_chain = fields->get_next_link_idx_chain()))
  {
    conn = link_idx_chain->conn;
    link_idx_holder = link_idx_chain->link_idx_holder;
    link_idx = link_idx_holder->link_idx;
    dbton_hdl = spider->dbton_handler[conn->dbton_id];
    spider->link_idx_chain = link_idx_chain;
    if (result_list->bgs_phase > 0)
    {
      if ((error_num = spider_check_and_init_casual_read(trx->thd, spider,
        link_idx)))
        DBUG_RETURN(error_num);
      if ((error_num = spider_bg_conn_search(spider, link_idx,
        dbton_hdl->first_link_idx, TRUE, FALSE,
        !fields->is_first_link_ok_chain(link_idx_chain))))
      {
        if (error_num != HA_ERR_END_OF_FILE && spider->need_mons[link_idx])
          error_num = fields->ping_table_mon_from_table(link_idx_chain);
        if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
        {
          store_error = HA_ERR_END_OF_FILE;
          error_num = 0;
        }
        DBUG_RETURN(error_num);
      }
    } else
    {
      if ((error_num = dbton_hdl->set_sql_for_exec(
             SPIDER_SQL_TYPE_SELECT_SQL, link_idx, link_idx_chain)))
        DBUG_RETURN(error_num);
      spider_lock_before_query(conn, &spider->need_mons[link_idx]);
      if ((error_num = spider_db_set_names(spider, conn,
        link_idx)))
      if ((error_num = spider_db_set_names(spider, conn, link_idx)))
      {
        spider_unlock_after_query(conn, 0);
        if (spider->need_mons[link_idx])
          error_num = fields->ping_table_mon_from_table(link_idx_chain);
        if ((error_num = spider->check_error_mode_eof(error_num)) ==
            HA_ERR_END_OF_FILE)
        {
          store_error = HA_ERR_END_OF_FILE;
          error_num = 0;
        }
        DBUG_RETURN(error_num);
      }
      spider_conn_set_timeout_from_share(conn, link_idx, trx->thd, share);
      if (dbton_hdl->execute_sql(
        SPIDER_SQL_TYPE_SELECT_SQL,
        conn,
        spider->result_list.quick_mode,
        &spider->need_mons[link_idx]))
      {
        error_num= spider_unlock_after_query_1(conn);
        if (spider->need_mons[link_idx])
          error_num = fields->ping_table_mon_from_table(link_idx_chain);
        if ((error_num = spider->check_error_mode_eof(error_num)) ==
            HA_ERR_END_OF_FILE)
        {
          store_error = HA_ERR_END_OF_FILE;
          error_num = 0;
        }
        DBUG_RETURN(error_num);
      }
      spider->connection_ids[link_idx] = conn->connection_id;
      if (fields->is_first_link_ok_chain(link_idx_chain))
      {
        if ((error_num = spider_unlock_after_query_2(conn, spider, link_idx, table)))
        {
          if (error_num != HA_ERR_END_OF_FILE && spider->need_mons[link_idx])
            error_num = fields->ping_table_mon_from_table(link_idx_chain);
          if ((error_num = spider->check_error_mode_eof(error_num)) ==
              HA_ERR_END_OF_FILE)
          {
            store_error = HA_ERR_END_OF_FILE;
            error_num = 0;
          }
          DBUG_RETURN(error_num);
        }
        spider->result_link_idx = link_idx;
        spider->result_link_idx_chain = link_idx_chain;
      } else
      {
        spider_db_discard_result(spider, link_idx, conn);
        spider_unlock_after_query(conn, 0);
      }
    }
  }
  DBUG_RETURN(0);
}

/*
 Prepare and send query to data nodes and store the query results.
*/
int spider_group_by_handler::init_scan()
{
  int error_num;
  DBUG_ENTER("spider_group_by_handler::init_scan");
  store_error = 0;
#ifndef DBUG_OFF
  for (Field **field = table->field; *field; field++)
    DBUG_PRINT("info",("spider field_name=%s", SPIDER_field_name_str(*field)));
#endif

  if (trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }

  if ((error_num = spider_prepare_init_scan(
         query, &skips, fields, spider, trx, offset_limit, thd)))
    DBUG_RETURN(error_num);

  if ((error_num = spider_make_query(query, fields, spider, table)))
    DBUG_RETURN(error_num);

  if ((error_num = spider_send_query(fields, spider, trx, table, store_error)))
    DBUG_RETURN(error_num);

  first = TRUE;
  DBUG_RETURN(0);
}

int spider_group_by_handler::next_row()
{
  int error_num, link_idx;
  spider_db_handler *dbton_hdl;
  SPIDER_CONN *conn;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  SPIDER_LINK_IDX_HOLDER *link_idx_holder;
  DBUG_ENTER("spider_group_by_handler::next_row");
  if (trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  if (store_error)
  {
    if (store_error == HA_ERR_END_OF_FILE)
    {
      table->status = STATUS_NOT_FOUND;
    }
    DBUG_RETURN(store_error);
  }
  if (first)
  {
    first = FALSE;
    if (spider->use_pre_call)
    {
      if (spider->store_error_num)
      {
        if (spider->store_error_num == HA_ERR_END_OF_FILE)
          table->status = STATUS_NOT_FOUND;
        DBUG_RETURN(spider->store_error_num);
      }
      if (spider->result_list.bgs_phase > 0)
      {
        fields->set_pos_to_first_link_idx_chain();
        while ((link_idx_chain = fields->get_next_link_idx_chain()))
        {
          conn = link_idx_chain->conn;
          link_idx_holder = link_idx_chain->link_idx_holder;
          link_idx = link_idx_holder->link_idx;
          dbton_hdl = spider->dbton_handler[conn->dbton_id];
          spider->link_idx_chain = link_idx_chain;
          if ((error_num = spider_bg_conn_search(spider, link_idx,
            dbton_hdl->first_link_idx, TRUE, TRUE,
            !fields->is_first_link_ok_chain(link_idx_chain))))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              spider->need_mons[link_idx]
            ) {
              error_num = fields->ping_table_mon_from_table(link_idx_chain);
            }
            if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
            {
              table->status = STATUS_NOT_FOUND;
            }
            DBUG_RETURN(error_num);
          }
        }
      }
      spider->use_pre_call = FALSE;
    }
  } else if (offset_limit)
  {
    --offset_limit;
    DBUG_RETURN(0);
  }
  if ((error_num = spider_db_seek_next(table->record[0], spider,
    spider->search_link_idx, table)))
  {
    if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
    {
      table->status = STATUS_NOT_FOUND;
    }
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_group_by_handler::end_scan()
{
  DBUG_ENTER("spider_group_by_handler::end_scan");
  DBUG_RETURN(0);
}

group_by_handler *spider_create_group_by_handler(
  THD *thd,
  Query *query
) {
  spider_group_by_handler *group_by_handler;
  Item *item;
  TABLE_LIST *from;
  SPIDER_CONN *conn;
  ha_spider *spider;
  SPIDER_SHARE *share;
  int roop_count, lock_mode;
  List_iterator_fast<Item> it(*query->select);
  uchar dbton_bitmap[spider_bitmap_size(SPIDER_DBTON_SIZE)];
  uchar dbton_bitmap_tmp[spider_bitmap_size(SPIDER_DBTON_SIZE)];
  ORDER *order;
  bool keep_going;
  bool find_dbton = FALSE;
  spider_fields *fields = NULL, *fields_arg = NULL;
  SPIDER_TABLE_HOLDER *table_holder;
  uint table_idx, dbton_id, table_count= 0;
  long tgt_link_status;
  MY_BITMAP skips;
  DBUG_ENTER("spider_create_group_by_handler");

  if (spider_param_disable_group_by_handler(thd))
    DBUG_RETURN(NULL);

  switch (thd_sql_command(thd))
  {
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
    case SQLCOM_DELETE:
    case SQLCOM_DELETE_MULTI:
      DBUG_PRINT("info",("spider update and delete does not support this feature"));
      DBUG_RETURN(NULL);
    default:
      break;
  }

  from = query->from;
  do {
    DBUG_PRINT("info",("spider from=%p", from));
    ++table_count;
    if (from->table->part_info)
    {
      DBUG_PRINT("info",("spider partition handler"));
      partition_info *part_info = from->table->part_info;
      uint bits = bitmap_bits_set(&part_info->read_partitions);
      DBUG_PRINT("info",("spider bits=%u", bits));
      if (bits != 1)
      {
        DBUG_PRINT("info",("spider using multiple partitions is not supported by this feature yet"));
        DBUG_RETURN(NULL);
      }
    }
  } while ((from = from->next_local));

  if (!(table_holder= spider_create_table_holder(table_count)))
    DBUG_RETURN(NULL);

  my_bitmap_init(&skips, NULL, query->select->elements);
  table_idx = 0;
  from = query->from;
  if (from->table->part_info)
  {
    partition_info *part_info = from->table->part_info;
    uint part = bitmap_get_first_set(&part_info->read_partitions);
    ha_partition *partition = (ha_partition *) from->table->file;
    handler **handlers = partition->get_child_handlers();
    spider = (ha_spider *) handlers[part];
  } else {
    spider = (ha_spider *) from->table->file;
  }
  share = spider->share;
  spider->idx_for_direct_join = table_idx;
  ++table_idx;
  if (!spider_add_table_holder(spider, table_holder))
  {
    DBUG_PRINT("info",("spider can not add a table"));
    goto skip_free_table_holder;
  }
  memset(dbton_bitmap, 0, spider_bitmap_size(SPIDER_DBTON_SIZE));
  /* Find all backends used by the first table. */
  for (roop_count = 0; roop_count < (int) share->use_dbton_count; ++roop_count)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    if (
      spider_dbton[dbton_id].support_direct_join &&
      spider_dbton[dbton_id].support_direct_join()
    ) {
      spider_set_bit(dbton_bitmap, dbton_id);
    }
  }
  while ((from = from->next_local))
  {
    if (from->table->part_info)
    {
      partition_info *part_info = from->table->part_info;
      uint part = bitmap_get_first_set(&part_info->read_partitions);
      ha_partition *partition = (ha_partition *) from->table->file;
      handler **handlers = partition->get_child_handlers();
      spider = (ha_spider *) handlers[part];
    } else {
      spider = (ha_spider *) from->table->file;
    }
    share = spider->share;
    spider->idx_for_direct_join = table_idx;
    ++table_idx;
    if (!spider_add_table_holder(spider, table_holder))
    {
      DBUG_PRINT("info",("spider can not add a table"));
      goto skip_free_table_holder;
    }
    /* Find all backends used by the current table */
    memset(dbton_bitmap_tmp, 0, spider_bitmap_size(SPIDER_DBTON_SIZE));
    for (roop_count = 0; roop_count < (int) share->use_dbton_count; ++roop_count)
    {
      dbton_id = share->use_sql_dbton_ids[roop_count];
      if (
        spider_dbton[dbton_id].support_direct_join &&
        spider_dbton[dbton_id].support_direct_join()
      ) {
        spider_set_bit(dbton_bitmap_tmp, dbton_id);
      }
    }
    /* Intersect to get common backends used by all tables (so far) */
    for (roop_count = 0;
      roop_count < spider_bitmap_size(SPIDER_DBTON_SIZE); ++roop_count)
    {
      dbton_bitmap[roop_count] &= dbton_bitmap_tmp[roop_count];
    }
  }

  from = query->from;
  do {
    if (from->table->part_info)
    {
      partition_info *part_info = from->table->part_info;
      uint part = bitmap_get_first_set(&part_info->read_partitions);
      ha_partition *partition = (ha_partition *) from->table->file;
      handler **handlers = partition->get_child_handlers();
      spider = (ha_spider *) handlers[part];
    } else {
      spider = (ha_spider *) from->table->file;
    }
    share = spider->share;
    if (spider_param_skip_default_condition(thd,
      share->skip_default_condition))
    {
      /* find skip_default_condition = 1 */
      break;
    }
  } while ((from = from->next_local));

  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; ++roop_count)
  {
    if (spider_bit_is_set(dbton_bitmap, roop_count))
    {
      if (!fields)
      {
        fields_arg = new spider_fields();
        if (!fields_arg)
          goto skip_free_table_holder;
      }
      fields_arg->set_table_holder(table_holder, table_count);
      keep_going = TRUE;
      it.init(*query->select);
      int i= -1, n_aux= query->n_aux;
      while ((item = it++))
      {
        i++;
        n_aux--;
        DBUG_PRINT("info",("spider select item=%p", item));
        if (item->const_item())
        {
          /*
            Do not create the GBH when a derived table or view is
            involved
          */
          if (thd->derived_tables != NULL)
          {
            keep_going= FALSE;
            break;
          }

          /*
            Do not handle the complex case where there's a const item
            in the auxiliary fields. It is too unlikely (if at all) to
            happen to be covered by the GBH.

            TODO: find an example covering this case or determine it
            never happens and remove this consideration.
          */
          if (n_aux >= 0)
          {
            spider_clear_bit(dbton_bitmap, roop_count);
            keep_going= FALSE;
            break;
          }
          bitmap_set_bit(&skips, i);
        }
        if (spider_db_print_item_type(item, NULL, spider, NULL, NULL, 0,
          roop_count, TRUE, fields_arg))
        {
          DBUG_PRINT("info",("spider dbton_id=%d can't create select", roop_count));
          spider_clear_bit(dbton_bitmap, roop_count);
          keep_going = FALSE;
          break;
        }
      }
      if (keep_going)
      {
        if (spider_dbton[roop_count].db_util->append_from_and_tables(
          spider, fields_arg, NULL, query->from, table_idx))
        {
          DBUG_PRINT("info",("spider dbton_id=%d can't create from", roop_count));
          spider_clear_bit(dbton_bitmap, roop_count);
          keep_going = FALSE;
        }
      }
      if (keep_going)
      {
        DBUG_PRINT("info",("spider query->where=%p", query->where));
        if (query->where)
        {
          if (spider_db_print_item_type(query->where, NULL, spider, NULL, NULL, 0,
            roop_count, TRUE, fields_arg))
          {
            DBUG_PRINT("info",("spider dbton_id=%d can't create where", roop_count));
            spider_clear_bit(dbton_bitmap, roop_count);
            keep_going = FALSE;
          }
        }
      }
      if (keep_going)
      {
        DBUG_PRINT("info",("spider query->group_by=%p", query->group_by));
        if (query->group_by)
        {
          for (order = query->group_by; order; order = order->next)
          {
            if (order->item_ptr == NULL ||
                spider_db_print_item_type(order->item_ptr, NULL, spider,
                                          NULL, NULL, 0, roop_count, TRUE,
                                          fields_arg))
            {
              DBUG_PRINT("info",("spider dbton_id=%d can't create group by", roop_count));
              spider_clear_bit(dbton_bitmap, roop_count);
              keep_going = FALSE;
              break;
            }
          }
        }
      }
      if (keep_going)
      {
        DBUG_PRINT("info",("spider query->order_by=%p", query->order_by));
        if (query->order_by)
        {
          for (order = query->order_by; order; order = order->next)
          {
            if (order->item_ptr == NULL ||
                spider_db_print_item_type(order->item_ptr, NULL, spider,
                                          NULL, NULL, 0, roop_count, TRUE,
                                          fields_arg))
            {
              DBUG_PRINT("info",("spider dbton_id=%d can't create order by", roop_count));
              spider_clear_bit(dbton_bitmap, roop_count);
              keep_going = FALSE;
              break;
            }
          }
        }
      }
      if (keep_going)
      {
        DBUG_PRINT("info",("spider query->having=%p", query->having));
        if (query->having)
        {
          if (spider_db_print_item_type(query->having, NULL, spider, NULL, NULL, 0,
            roop_count, TRUE, fields_arg))
          {
            DBUG_PRINT("info",("spider dbton_id=%d can't create having", roop_count));
            spider_clear_bit(dbton_bitmap, roop_count);
            keep_going = FALSE;
          }
        }
      }
      if (keep_going)
      {
        find_dbton = TRUE;
        fields = fields_arg;
        fields_arg = NULL;
      } else {
        delete fields_arg;
      }
    }
  }
  if (!find_dbton)
    goto skip_free_table_holder;

  from = query->from;
  if (from->table->part_info)
  {
    partition_info *part_info = from->table->part_info;
    uint part = bitmap_get_first_set(&part_info->read_partitions);
    ha_partition *partition = (ha_partition *) from->table->file;
    handler **handlers = partition->get_child_handlers();
    spider = (ha_spider *) handlers[part];
  } else {
    spider = (ha_spider *) from->table->file;
  }
  share = spider->share;
  lock_mode = spider_conn_lock_mode(spider);
  if (lock_mode)
  {
    tgt_link_status = SPIDER_LINK_STATUS_RECOVERY;
  } else {
    tgt_link_status = SPIDER_LINK_STATUS_OK;
  }
  DBUG_PRINT("info",("spider s->db=%s", from->table->s->db.str));
  DBUG_PRINT("info",("spider s->table_name=%s", from->table->s->table_name.str));
  if (spider->dml_init())
  {
    DBUG_PRINT("info",("spider can not init for dml"));
    goto skip_free_fields;
  }
  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      tgt_link_status);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      tgt_link_status)
  ) {
    conn = spider->conns[roop_count];
    DBUG_PRINT("info",("spider roop_count=%d", roop_count));
    DBUG_PRINT("info",("spider conn=%p", conn));
    DBUG_ASSERT(conn);
    if (conn->table_lock)
    {
      DBUG_PRINT("info",("spider direct_join does not support with lock tables yet"));
      if (lock_mode)
      {
        goto skip_free_fields;
      }
      continue;
    }
    if (!fields->add_conn(conn,
      share->access_balances[spider->conn_link_idx[roop_count]]))
    {
      DBUG_PRINT("info",("spider can not create conn_holder"));
      goto skip_free_fields;
    }
    if (fields->add_link_idx(conn->conn_holder_for_direct_join, spider, roop_count))
    {
      DBUG_PRINT("info",("spider can not create link_idx_holder"));
      goto skip_free_fields;
    }
  }
  if (!fields->has_conn_holder())
  {
    goto skip_free_fields;
  }

  while ((from = from->next_local))
  {
    fields->clear_conn_holder_checked();

    if (from->table->part_info)
    {
      partition_info *part_info = from->table->part_info;
      uint part = bitmap_get_first_set(&part_info->read_partitions);
      ha_partition *partition = (ha_partition *) from->table->file;
      handler **handlers = partition->get_child_handlers();
      spider = (ha_spider *) handlers[part];
    } else {
      spider = (ha_spider *) from->table->file;
    }
    share = spider->share;
    DBUG_PRINT("info",("spider s->db=%s", from->table->s->db.str));
    DBUG_PRINT("info",("spider s->table_name=%s", from->table->s->table_name.str));
    if (spider->dml_init())
    {
      DBUG_PRINT("info",("spider can not init for dml"));
      goto skip_free_fields;
    }
    for (
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, -1, share->link_count,
        tgt_link_status);
      roop_count < (int) share->link_count;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, roop_count, share->link_count,
        tgt_link_status)
    ) {
      DBUG_PRINT("info",("spider roop_count=%d", roop_count));
      conn = spider->conns[roop_count];
      DBUG_PRINT("info",("spider conn=%p", conn));
      if (!fields->check_conn_same_conn(conn))
      {
        if (lock_mode)
        {
          DBUG_PRINT("info", ("spider connection %p can not be used for this "
                              "query with locking",
                              conn));
          goto skip_free_fields;
        }
        continue;
      }
      if (fields->add_link_idx(conn->conn_holder_for_direct_join, spider, roop_count))
      {
        DBUG_PRINT("info",("spider can not create link_idx_holder"));
        goto skip_free_fields;
      }
    }

    if (fields->remove_conn_if_not_checked())
    {
      if (lock_mode)
      {
        DBUG_PRINT("info",("spider some connections can not be used for this query with locking"));
        goto skip_free_fields;
      }
    }
    /* Do not create if all conn holders have been removed. This
      happens if the current table does not share usable conns with
      the first table. One typical example is when the current table
      is located on a different server from the first table. */
    if (!fields->has_conn_holder())
    {
      goto skip_free_fields;
    }
  }

  fields->check_support_dbton(dbton_bitmap);
  if (!fields->has_conn_holder())
  {
    DBUG_PRINT("info",("spider all chosen connections can't match dbton_id"));
    goto skip_free_fields;
  }

  /* choose a connection */
  if (!lock_mode)
  {
    fields->choose_a_conn();
  }

  if (fields->make_link_idx_chain(tgt_link_status))
  {
    DBUG_PRINT("info",("spider can not create link_idx_chain"));
    goto skip_free_fields;
  }

  /* choose link_id */
  if (fields->check_link_ok_chain())
  {
    DBUG_PRINT("info",("spider do not have link ok status"));
    goto skip_free_fields;
  }

  fields->set_first_link_idx();

  if (!(group_by_handler = new spider_group_by_handler(thd, query, fields, skips)))
  {
    DBUG_PRINT("info",("spider can't create group_by_handler"));
    goto skip_free_fields;
  }
  my_bitmap_free(&skips);
  query->distinct = FALSE;
  query->where = NULL;
  query->group_by = NULL;
  query->having = NULL;
  query->order_by = NULL;
  DBUG_RETURN(group_by_handler);

skip_free_fields:
  delete fields;
skip_free_table_holder:
  spider_free(spider_current_trx, table_holder, MYF(0));
  my_bitmap_free(&skips);
  DBUG_RETURN(NULL);
}
