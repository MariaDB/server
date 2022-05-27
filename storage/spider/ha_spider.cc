/* Copyright (C) 2008-2019 Kentoku Shiba
   Copyright (C) 2019-2022 MariaDB corp

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation
#endif

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "key.h"
#include "sql_select.h"
#include "ha_partition.h"
#include "spd_param.h"
#include "spd_err.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "ha_spider.h"
#include "spd_table.h"
#include "spd_sys_table.h"
#include "spd_trx.h"
#include "spd_conn.h"
#include "spd_db_conn.h"
#include "spd_ping_table.h"
#include "spd_malloc.h"

#define SPIDER_CAN_BG_SEARCH (1LL << 37)
#define SPIDER_CAN_BG_INSERT (1LL << 38)
#define SPIDER_CAN_BG_UPDATE (1LL << 39)

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
extern HASH spider_open_tables;
extern pthread_mutex_t spider_lgtm_tblhnd_share_mutex;

/* UTC time zone for timestamp columns */
extern Time_zone *UTC;

ha_spider::ha_spider(
) : handler(spider_hton_ptr, NULL)
{
  DBUG_ENTER("ha_spider::ha_spider");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_alloc_calc_mem_init(mem_calc, 139);
  spider_alloc_calc_mem(spider_current_trx, mem_calc, sizeof(*this));
  share = NULL;
  conns = NULL;
  need_mons = NULL;
  blob_buff = NULL;
  conn_keys = NULL;
  spider_thread_id = 0;
  trx_conn_adjustment = 0;
  search_link_query_id = 0;
  partition_handler = NULL;
  multi_range_keys = NULL;
  mrr_key_buff = NULL;
  append_tblnm_alias = NULL;
  use_index_merge = FALSE;
  is_clone = FALSE;
  pt_clone_source_handler = NULL;
  pt_clone_last_searcher = NULL;
  ft_handler = NULL;
  ft_first = NULL;
  ft_current = NULL;
  ft_count = 0;
  ft_init_without_index_init = FALSE;
  sql_kinds = 0;
  error_mode = 0;
  use_spatial_index = FALSE;
  use_fields = FALSE;
  dml_inited = FALSE;
  use_pre_call = FALSE;
  use_pre_action = FALSE;
  do_direct_update = FALSE;
  prev_index_rnd_init = SPD_NONE;
  direct_aggregate_item_first = NULL;
  result_link_idx = 0;
  result_list.have_sql_kind_backup = FALSE;
  result_list.sqls = NULL;
  result_list.insert_sqls = NULL;
  result_list.update_sqls = NULL;
  result_list.tmp_sqls = NULL;
  result_list.tmp_tables_created = FALSE;
  result_list.bgs_working = FALSE;
  result_list.direct_order_limit = FALSE;
  result_list.direct_limit_offset = FALSE;
  result_list.set_split_read = FALSE;
  result_list.insert_dup_update_pushdown = FALSE;
  result_list.tmp_pos_row_first = NULL;
  result_list.direct_aggregate = FALSE;
  result_list.snap_direct_aggregate = FALSE;
  result_list.direct_distinct = FALSE;
  result_list.casual_read = NULL;
  result_list.use_both_key = FALSE;
  result_list.in_cmp_ref = FALSE;
  DBUG_VOID_RETURN;
}

ha_spider::ha_spider(
  handlerton *hton,
  TABLE_SHARE *table_arg
) : handler(hton, table_arg)
{
  DBUG_ENTER("ha_spider::ha_spider");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_alloc_calc_mem_init(mem_calc, 0);
  spider_alloc_calc_mem(spider_current_trx, mem_calc, sizeof(*this));
  share = NULL;
  conns = NULL;
  need_mons = NULL;
  blob_buff = NULL;
  conn_keys = NULL;
  spider_thread_id = 0;
  trx_conn_adjustment = 0;
  search_link_query_id = 0;
  partition_handler = NULL;
  multi_range_keys = NULL;
  mrr_key_buff = NULL;
  append_tblnm_alias = NULL;
  use_index_merge = FALSE;
  is_clone = FALSE;
  pt_clone_source_handler = NULL;
  pt_clone_last_searcher = NULL;
  ft_handler = NULL;
  ft_first = NULL;
  ft_current = NULL;
  ft_count = 0;
  ft_init_without_index_init = FALSE;
  sql_kinds = 0;
  error_mode = 0;
  use_spatial_index = FALSE;
  use_fields = FALSE;
  dml_inited = FALSE;
  use_pre_call = FALSE;
  use_pre_action = FALSE;
  do_direct_update = FALSE;
  prev_index_rnd_init = SPD_NONE;
  direct_aggregate_item_first = NULL;
  result_link_idx = 0;
  result_list.have_sql_kind_backup = FALSE;
  result_list.sqls = NULL;
  result_list.insert_sqls = NULL;
  result_list.update_sqls = NULL;
  result_list.tmp_sqls = NULL;
  result_list.tmp_tables_created = FALSE;
  result_list.bgs_working = FALSE;
  result_list.direct_order_limit = FALSE;
  result_list.direct_limit_offset = FALSE;
  result_list.set_split_read = FALSE;
  result_list.insert_dup_update_pushdown = FALSE;
  result_list.tmp_pos_row_first = NULL;
  result_list.direct_aggregate = FALSE;
  result_list.snap_direct_aggregate = FALSE;
  result_list.direct_distinct = FALSE;
  result_list.casual_read = NULL;
  result_list.use_both_key = FALSE;
  result_list.in_cmp_ref = FALSE;
  ref_length = sizeof(SPIDER_POSITION);
  DBUG_VOID_RETURN;
}

ha_spider::~ha_spider()
{
  DBUG_ENTER("ha_spider::~ha_spider");
  DBUG_PRINT("info",("spider this=%p", this));
  partition_handler = NULL;
  if (wide_handler_owner)
  {
    spider_free(spider_current_trx, wide_handler, MYF(0));
  }
  wide_handler = NULL;
  spider_free_mem_calc(spider_current_trx, mem_calc_id, sizeof(*this));
  DBUG_VOID_RETURN;
}

handler *ha_spider::clone(
  const char *name,
  MEM_ROOT *mem_root
) {
  ha_spider *spider;
  DBUG_ENTER("ha_spider::clone");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    !(spider = (ha_spider *)
      get_new_handler(table->s, mem_root, spider_hton_ptr)) ||
    !(spider->ref = (uchar*) alloc_root(mem_root, ALIGN_SIZE(ref_length) * 2))
  )
    DBUG_RETURN(NULL);
  spider->is_clone = TRUE;
  spider->pt_clone_source_handler = this;
  if (spider->ha_open(table, name, table->db_stat,
    HA_OPEN_IGNORE_IF_LOCKED))
    DBUG_RETURN(NULL);
  spider->sync_from_clone_source_base(this);
  use_index_merge = TRUE;

  DBUG_RETURN((handler *) spider);
}

static const char *ha_spider_exts[] = {
  NullS
};

const char **ha_spider::bas_ext() const
{
  return ha_spider_exts;
}

int ha_spider::open(
  const char* name,
  int mode,
  uint test_if_locked
) {
  THD *thd = ha_thd();
  int error_num, roop_count;
  int init_sql_alloc_size;
  ha_spider *spider, *owner;
  bool wide_handler_alloc = FALSE;
  SPIDER_WIDE_SHARE *wide_share;
  uint part_num;
  bool partition_handler_alloc = FALSE;
  ha_spider **wide_handler_handlers = NULL;
  ha_partition *clone_source;
  DBUG_ENTER("ha_spider::open");
  DBUG_PRINT("info",("spider this=%p", this));

  dup_key_idx = (uint) -1;
  conn_kinds = SPIDER_CONN_KIND_MYSQL;
  table->file->get_no_parts("", &part_num);
  if (part_num)
  {
    wide_handler_handlers =
      (ha_spider **) ((ha_partition *) table->file)->get_child_handlers();
    spider = wide_handler_handlers[0];
    owner = wide_handler_handlers[part_num - 1];
    clone_source = ((ha_partition *) table->file)->get_clone_source();
    if (clone_source)
    {
      is_clone = TRUE;
    }
  } else {
    spider = this;
    owner = this;
    clone_source = NULL;
  }
  if (!spider->wide_handler)
  {
    uchar *searched_bitmap;
    uchar *ft_discard_bitmap;
    uchar *position_bitmap;
    uchar *idx_read_bitmap;
    uchar *idx_write_bitmap;
    uchar *rnd_read_bitmap;
    uchar *rnd_write_bitmap;
    if (!(wide_handler = (SPIDER_WIDE_HANDLER *)
      spider_bulk_malloc(spider_current_trx, 16, MYF(MY_WME | MY_ZEROFILL),
        &wide_handler, sizeof(SPIDER_WIDE_HANDLER),
        &searched_bitmap,
          (uint) sizeof(uchar) * no_bytes_in_map(table->read_set),
        &ft_discard_bitmap,
          (uint) sizeof(uchar) * no_bytes_in_map(table->read_set),
        &position_bitmap,
          (uint) sizeof(uchar) * no_bytes_in_map(table->read_set),
        &idx_read_bitmap,
          (uint) sizeof(uchar) * no_bytes_in_map(table->read_set),
        &idx_write_bitmap,
          (uint) sizeof(uchar) * no_bytes_in_map(table->read_set),
        &rnd_read_bitmap,
          (uint) sizeof(uchar) * no_bytes_in_map(table->read_set),
        &rnd_write_bitmap,
          (uint) sizeof(uchar) * no_bytes_in_map(table->read_set),
        &partition_handler,
          (uint) sizeof(SPIDER_PARTITION_HANDLER),
        NullS)
        )
    ) {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_wide_handler_alloc;
    }
    spider->wide_handler = wide_handler;
    owner->wide_handler = wide_handler;
    wide_handler->searched_bitmap = searched_bitmap;
    wide_handler->ft_discard_bitmap = ft_discard_bitmap;
    wide_handler->position_bitmap = position_bitmap;
    wide_handler->idx_read_bitmap = idx_read_bitmap;
    wide_handler->idx_write_bitmap = idx_write_bitmap;
    wide_handler->rnd_read_bitmap = rnd_read_bitmap;
    wide_handler->rnd_write_bitmap = rnd_write_bitmap;
    wide_handler->partition_handler = partition_handler;
    wide_handler->owner = owner;
    if (table_share->tmp_table == NO_TMP_TABLE)
      wide_handler->top_share = table->s;
    owner->wide_handler_owner = TRUE;
    memset(wide_handler->ft_discard_bitmap, 0xFF,
      no_bytes_in_map(table->read_set));
    memset(wide_handler->searched_bitmap, 0,
      no_bytes_in_map(table->read_set));
    wide_handler_alloc = TRUE;

  if (!share && !spider_get_share(name, table, thd, this, &error_num))
    goto error_get_share;

  wide_share = share->wide_share;

    DBUG_PRINT("info",("spider create partition_handler"));
    DBUG_PRINT("info",("spider table=%p", table));
    partition_handler->table = table;
    partition_handler->no_parts = part_num;
    partition_handler->owner = owner;
    partition_handler->parallel_search_query_id = 0;
    spider->partition_handler = partition_handler;
    owner->partition_handler = partition_handler;
    partition_handler->handlers = wide_handler_handlers;
    partition_handler_alloc = TRUE;
  } else {
    wide_handler = spider->wide_handler;
    partition_handler = wide_handler->partition_handler;

    if (!share && !spider_get_share(name, table, thd, this, &error_num))
      goto error_get_share;

    wide_share= share->wide_share;
  }
  if (wide_handler_alloc)
  {
    thr_lock_data_init(&wide_share->lock, &wide_handler->lock, NULL);
  }

  init_sql_alloc_size =
    spider_param_init_sql_alloc_size(thd, share->init_sql_alloc_size);

  result_list.table = table;
  result_list.first = NULL;
  result_list.last = NULL;
  result_list.current = NULL;
  result_list.record_num = 0;
  if (
    !(result_list.sqls = new spider_string[share->link_count]) ||
    !(result_list.insert_sqls = new spider_string[share->link_count]) ||
    !(result_list.update_sqls = new spider_string[share->link_count]) ||
    !(result_list.tmp_sqls = new spider_string[share->link_count])
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_init_result_list;
  }
  for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
  {
    result_list.sqls[roop_count].init_calc_mem(80);
    result_list.insert_sqls[roop_count].init_calc_mem(81);
    result_list.update_sqls[roop_count].init_calc_mem(82);
    result_list.tmp_sqls[roop_count].init_calc_mem(83);
    uint all_link_idx = conn_link_idx[roop_count];
    uint dbton_id = share->sql_dbton_ids[all_link_idx];
    if (share->dbton_share[dbton_id]->need_change_db_table_name())
    {
      if (
        result_list.sqls[roop_count].real_alloc(init_sql_alloc_size) ||
        result_list.insert_sqls[roop_count].real_alloc(init_sql_alloc_size) ||
        result_list.update_sqls[roop_count].real_alloc(init_sql_alloc_size) ||
        result_list.tmp_sqls[roop_count].real_alloc(init_sql_alloc_size)
      ) {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error_init_result_list;
      }
    }
    result_list.sqls[roop_count].set_charset(share->access_charset);
    result_list.insert_sqls[roop_count].set_charset(share->access_charset);
    result_list.update_sqls[roop_count].set_charset(share->access_charset);
    result_list.tmp_sqls[roop_count].set_charset(share->access_charset);
  }

  DBUG_PRINT("info",("spider blob_fields=%d", table_share->blob_fields));
  if (table_share->blob_fields)
  {
    if (!(blob_buff = new spider_string[table_share->fields]))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_blob_buff;
    }
    for (roop_count = 0; roop_count < (int) table_share->fields; roop_count++)
    {
      blob_buff[roop_count].init_calc_mem(84);
      blob_buff[roop_count].set_charset(table->field[roop_count]->charset());
    }
  }

  if (is_clone)
  {
    if (part_num)
    {
      for (roop_count = 0; roop_count < (int) part_num; roop_count++)
      {
        if (partition_handler->handlers[roop_count]->share == share)
        {
          pt_clone_source_handler =
            partition_handler->handlers[roop_count];
          break;
        }
      }
    }

    wide_handler->external_lock_type =
      pt_clone_source_handler->wide_handler->external_lock_type;

    if (wide_handler_alloc)
    {
      wide_handler->lock_mode =
        pt_clone_source_handler->wide_handler->lock_mode;
      if (!partition_handler->clone_bitmap_init)
      {
        pt_clone_source_handler->set_select_column_mode();
        partition_handler->clone_bitmap_init = TRUE;
      }
      set_clone_searched_bitmap();
      wide_handler->position_bitmap_init = FALSE;
      wide_handler->sql_command =
        pt_clone_source_handler->wide_handler->sql_command;
    }
  } else {
    if (share->semi_table_lock)
    {
      wide_handler->semi_table_lock = TRUE;
    }
  }

  if (reset())
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_reset;
  }

  DBUG_RETURN(0);

error_reset:
  delete [] blob_buff;
  blob_buff = NULL;
error_init_blob_buff:
error_init_result_list:
  if (partition_handler_alloc)
  {
    wide_share = share->wide_share;
    spider->partition_handler = NULL;
    owner->partition_handler = NULL;
  }
  partition_handler = NULL;
  spider_free_share(share);
  share = NULL;
  if (conn_keys)
  {
    spider_free(spider_current_trx, conn_keys, MYF(0));
    conn_keys = NULL;
  }
error_get_share:
  if (wide_handler_alloc)
  {
    spider_free(spider_current_trx, wide_handler, MYF(0));
    if (wide_handler_handlers)
    {
      wide_handler_handlers[0]->wide_handler = NULL;
    }
    spider->wide_handler = NULL;
    owner->wide_handler = NULL;
    owner->wide_handler_owner = FALSE;
  }
  wide_handler = NULL;
error_wide_handler_alloc:
  DBUG_RETURN(error_num);
}

int ha_spider::close()
{
  int error_num = 0, roop_count, error_num2;
  THD *thd = ha_thd();
  backup_error_status();
  DBUG_ENTER("ha_spider::close");
  DBUG_PRINT("info",("spider this=%p", this));

  if (multi_range_keys)
  {
    DBUG_PRINT("info",("spider free multi_range_keys=%p", multi_range_keys));
    spider_free(spider_current_trx, multi_range_keys, MYF(0));
    multi_range_keys = NULL;
  }
  if (mrr_key_buff)
  {
    delete [] mrr_key_buff;
    mrr_key_buff = NULL;
  }
  while (direct_aggregate_item_first)
  {
    direct_aggregate_item_current = direct_aggregate_item_first->next;
    if (direct_aggregate_item_first->item)
    {
      delete direct_aggregate_item_first->item;
    }
    spider_free(spider_current_trx, direct_aggregate_item_first, MYF(0));
    direct_aggregate_item_first = direct_aggregate_item_current;
  }
  if (is_clone)
  {
    for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
    {
      if ((error_num2 = close_opened_handler(roop_count, FALSE)))
      {
        if (check_error_mode(error_num2))
          error_num = error_num2;
      }
    }
  }
  for (roop_count = share->use_dbton_count - 1; roop_count >= 0; roop_count--)
  {
    uint dbton_id = share->use_dbton_ids[roop_count];
    if (dbton_handler[dbton_id])
    {
      delete dbton_handler[dbton_id];
      dbton_handler[dbton_id] = NULL;
    }
  }

  if (!thd || !thd_get_ha_data(thd, spider_hton_ptr))
  {
    for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
      conns[roop_count] = NULL;
  }

  if (ft_first)
  {
    st_spider_ft_info *tmp_ft_info;
    do {
      tmp_ft_info = ft_first->next;
      spider_free(spider_current_trx, ft_first, MYF(0));
      ft_first = tmp_ft_info;
    } while (ft_first);
  }

  spider_db_free_result(this, TRUE);
  if (conn_keys)
  {
    spider_free(spider_current_trx, conn_keys, MYF(0));
    conn_keys = NULL;
  }
  partition_handler = NULL;
  if (wide_handler_owner)
  {
    spider_free(spider_current_trx, wide_handler, MYF(0));
    wide_handler_owner = FALSE;
  }
  wide_handler = NULL;
  if (blob_buff)
  {
    delete [] blob_buff;
    blob_buff = NULL;
  }
  if (result_list.sqls)
  {
    delete [] result_list.sqls;
    result_list.sqls = NULL;
  }
  if (result_list.insert_sqls)
  {
    delete [] result_list.insert_sqls;
    result_list.insert_sqls = NULL;
  }
  if (result_list.update_sqls)
  {
    delete [] result_list.update_sqls;
    result_list.update_sqls = NULL;
  }
  if (result_list.tmp_sqls)
  {
    delete [] result_list.tmp_sqls;
    result_list.tmp_sqls = NULL;
  }

  spider_free_share(share);
  is_clone = FALSE;
  pt_clone_source_handler = NULL;
  share = NULL;
  conns = NULL;

  DBUG_RETURN(error_num);
}

int ha_spider::check_access_kind_for_connection(
  THD *thd,
  bool write_request
) {
  int error_num, roop_count;
  DBUG_ENTER("ha_spider::check_access_kind_for_connection");
  DBUG_PRINT("info",("spider this=%p", this));
  conn_kinds = 0;
  switch (wide_handler->sql_command)
  {
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
    case SQLCOM_DELETE:
    case SQLCOM_DELETE_MULTI:
    default:
      conn_kinds |= SPIDER_CONN_KIND_MYSQL;
      for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
      {
        conn_kind[roop_count] = SPIDER_CONN_KIND_MYSQL;
      }
      break;
  }
  if ((error_num = spider_check_trx_and_get_conn(thd, this, TRUE)))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_PRINT("info",("spider wide_handler->semi_trx_isolation_chk = %s",
    wide_handler->semi_trx_isolation_chk ? "TRUE" : "FALSE"));
  if (wide_handler->semi_trx_isolation_chk)
  {
    SPIDER_SET_CONNS_PARAM(semi_trx_isolation_chk, TRUE, conns,
      share->link_statuses, conn_link_idx, (int) share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
  }
  DBUG_PRINT("info",("spider wide_handler->semi_trx_chk = %s",
    wide_handler->semi_trx_chk ? "TRUE" : "FALSE"));
  if (wide_handler->semi_trx_chk)
  {
    SPIDER_SET_CONNS_PARAM(semi_trx_chk, TRUE, conns, share->link_statuses,
      conn_link_idx, (int) share->link_count, SPIDER_LINK_STATUS_RECOVERY);
  } else {
    SPIDER_SET_CONNS_PARAM(semi_trx_chk, FALSE, conns, share->link_statuses,
      conn_link_idx, (int) share->link_count, SPIDER_LINK_STATUS_RECOVERY);
  }
  DBUG_RETURN(0);
}

void ha_spider::check_access_kind(
  THD *thd
) {
  DBUG_ENTER("ha_spider::check_access_kind");
  DBUG_PRINT("info",("spider this=%p", this));
  wide_handler->sql_command = thd_sql_command(thd);
  DBUG_PRINT("info",("spider sql_command=%u", wide_handler->sql_command));
  DBUG_PRINT("info",("spider thd->query_id=%lld", thd->query_id));
    wide_handler->update_request = FALSE;
  DBUG_VOID_RETURN;
}


THR_LOCK_DATA **ha_spider::store_lock(
  THD *thd,
  THR_LOCK_DATA **to,
  enum thr_lock_type lock_type
) {
  DBUG_ENTER("ha_spider::store_lock");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    wide_handler->stage == SPD_HND_STAGE_STORE_LOCK &&
    wide_handler->stage_executor != this)
  {
    DBUG_RETURN(to);
  }
  wide_handler->stage = SPD_HND_STAGE_STORE_LOCK;
  wide_handler->stage_executor = this;
  wide_handler->lock_table_type = 0;
  if (lock_type == TL_IGNORE)
  {
    *to++ = &wide_handler->lock;
    DBUG_RETURN(to);
  }
  check_access_kind(thd);
  DBUG_PRINT("info",("spider sql_command=%u", wide_handler->sql_command));
  DBUG_PRINT("info",("spider lock_type=%d", lock_type));
  DBUG_PRINT("info",("spider thd->query_id=%lld", thd->query_id));

  wide_handler->lock_type = lock_type;
  if (
    wide_handler->sql_command != SQLCOM_DROP_TABLE &&
    wide_handler->sql_command != SQLCOM_ALTER_TABLE
  ) {
    wide_handler->semi_trx_chk = FALSE;
  }
  switch (wide_handler->sql_command)
  {
    case SQLCOM_SELECT:
    case SQLCOM_HA_READ:
      if (lock_type == TL_READ_WITH_SHARED_LOCKS)
        wide_handler->lock_mode = 1;
      else if (lock_type <= TL_READ_NO_INSERT)
      {
        wide_handler->lock_mode = 0;
        wide_handler->semi_trx_isolation_chk = TRUE;
      } else
        wide_handler->lock_mode = -1;
      wide_handler->semi_trx_chk = TRUE;
      break;
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
    case SQLCOM_CREATE_TABLE:
    case SQLCOM_INSERT:
    case SQLCOM_INSERT_SELECT:
    case SQLCOM_DELETE:
    case SQLCOM_LOAD:
    case SQLCOM_REPLACE:
    case SQLCOM_REPLACE_SELECT:
    case SQLCOM_DELETE_MULTI:
      if (lock_type >= TL_READ && lock_type <= TL_READ_NO_INSERT)
      {
        wide_handler->lock_mode = -2;
        wide_handler->semi_trx_isolation_chk = TRUE;
      } else
        wide_handler->lock_mode = -1;
      wide_handler->semi_trx_chk = TRUE;
      break;
    default:
        wide_handler->lock_mode = -1;
  }
  switch (lock_type)
  {
    case TL_READ_HIGH_PRIORITY:
      wide_handler->high_priority = TRUE;
      break;
    case TL_WRITE_DELAYED:
      wide_handler->insert_delayed = TRUE;
      break;
    case TL_WRITE_LOW_PRIORITY:
      wide_handler->low_priority = TRUE;
      break;
    default:
      break;
  }

  if (wide_handler->lock_type != TL_IGNORE &&
    wide_handler->lock.type == TL_UNLOCK)
  {
    if (
      wide_handler->sql_command == SQLCOM_DROP_TABLE ||
      wide_handler->sql_command == SQLCOM_ALTER_TABLE ||
      wide_handler->sql_command == SQLCOM_SHOW_CREATE ||
      wide_handler->sql_command == SQLCOM_SHOW_FIELDS
    ) {
      if (
        lock_type == TL_READ_NO_INSERT &&
        !thd->in_lock_tables
      )
        lock_type = TL_READ;
      if (
        lock_type >= TL_WRITE_CONCURRENT_INSERT && lock_type <= TL_WRITE &&
        !thd->in_lock_tables && !thd_tablespace_op(thd)
      )
        lock_type = TL_WRITE_ALLOW_WRITE;
    } else if (
      wide_handler->sql_command == SQLCOM_LOCK_TABLES ||
      (spider_param_lock_exchange(thd) == 1 && wide_handler->semi_table_lock))
    {
      DBUG_PRINT("info",("spider lock exchange route"));
      DBUG_PRINT("info",("spider lock_type=%u", wide_handler->lock_type));
      if (
        (
          wide_handler->lock_type == TL_READ ||
          wide_handler->lock_type == TL_READ_NO_INSERT ||
          wide_handler->lock_type == TL_WRITE_LOW_PRIORITY ||
          wide_handler->lock_type == TL_WRITE
        ) &&
        !spider_param_local_lock_table(thd)
      ) {
        wide_handler->lock_table_type = 1;
        if (partition_handler && partition_handler->handlers)
        {
          uint roop_count;
          for (roop_count = 0; roop_count < partition_handler->no_parts;
            ++roop_count)
          {
            if (unlikely((store_error_num =
              partition_handler->handlers[roop_count]->
                append_lock_tables_list())))
            {
              break;
            }
          }
        } else {
          store_error_num = append_lock_tables_list();
        }
      }
    } else {
      DBUG_PRINT("info",("spider default lock route"));
      DBUG_PRINT("info",("spider lock_type=%u", wide_handler->lock_type));
      if (
        wide_handler->lock_type == TL_READ ||
        wide_handler->lock_type == TL_READ_NO_INSERT ||
        wide_handler->lock_type == TL_WRITE_LOW_PRIORITY ||
        wide_handler->lock_type == TL_WRITE
      ) {
        if (
          !spider_param_local_lock_table(thd) &&
          spider_param_semi_table_lock(thd, wide_handler->semi_table_lock)
        ) {
          wide_handler->lock_table_type = 2;
          if (partition_handler && partition_handler->handlers)
          {
            uint roop_count;
            for (roop_count = 0;
              roop_count < partition_handler->no_parts;
              ++roop_count)
            {
              if (unlikely((store_error_num =
                partition_handler->handlers[roop_count]->
                  append_lock_tables_list())))
              {
                break;
              }
            }
          } else {
            store_error_num = append_lock_tables_list();
          }
        }
      }
      if (
        lock_type == TL_READ_NO_INSERT &&
        !thd->in_lock_tables
      )
        lock_type = TL_READ;
      if (
        lock_type >= TL_WRITE_CONCURRENT_INSERT && lock_type <= TL_WRITE &&
        lock_type != TL_WRITE_DELAYED &&
        !thd->in_lock_tables && !thd_tablespace_op(thd)
      )
        lock_type = TL_WRITE_ALLOW_WRITE;
    }
    wide_handler->lock.type = lock_type;
  }
  *to++ = &wide_handler->lock;
  DBUG_RETURN(to);
}

int ha_spider::external_lock(
  THD *thd,
  int lock_type
) {
  int error_num = 0;
  SPIDER_TRX *trx;
  backup_error_status();

  DBUG_ENTER("ha_spider::external_lock");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider lock_type=%x", lock_type));
  DBUG_PRINT("info", ("spider sql_command=%d", thd_sql_command(thd)));

  if (wide_handler->stage == SPD_HND_STAGE_EXTERNAL_LOCK)
  {
    /* Only the stage executor deals with table locks. */
    if (wide_handler->stage_executor != this)
    {
      DBUG_RETURN(0);
    }
  }
  else
  {
    /* Update the stage executor when the stage changes */
    wide_handler->stage= SPD_HND_STAGE_EXTERNAL_LOCK;
    wide_handler->stage_executor= this;
  }

  info_auto_called = FALSE;
  wide_handler->external_lock_type= lock_type;
  wide_handler->sql_command = thd_sql_command(thd);

  /* We treat BEGIN as if UNLOCK TABLE. */
  if (wide_handler->sql_command == SQLCOM_BEGIN)
  {
    wide_handler->sql_command = SQLCOM_UNLOCK_TABLES;
  }
  if (lock_type == F_UNLCK &&
      wide_handler->sql_command != SQLCOM_UNLOCK_TABLES)
  {
    DBUG_RETURN(0);
  }

  trx = spider_get_trx(thd, TRUE, &error_num);
  if (error_num)
  {
    DBUG_RETURN(error_num);
  }
  wide_handler->trx = trx;

  /* Question: Why the following if block is necessary? Why here? */
  if (store_error_num)
  {
    DBUG_RETURN(store_error_num);
  }

  DBUG_ASSERT(wide_handler->sql_command != SQLCOM_RENAME_TABLE &&
              wide_handler->sql_command != SQLCOM_DROP_DB);

  if (wide_handler->sql_command == SQLCOM_DROP_TABLE ||
      wide_handler->sql_command == SQLCOM_ALTER_TABLE)
  {
    if (trx->locked_connections)
    {
      my_message(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM,
                 ER_SPIDER_ALTER_BEFORE_UNLOCK_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM);
    }
    DBUG_RETURN(0);
  }

  if (lock_type != F_UNLCK)
  {
    if (unlikely((error_num= spider_internal_start_trx(this))))
    {
      DBUG_RETURN(error_num);
    }
    if (wide_handler->sql_command != SQLCOM_SELECT &&
        wide_handler->sql_command != SQLCOM_HA_READ)
    {
      trx->updated_in_this_trx= TRUE;
      DBUG_PRINT("info", ("spider trx->updated_in_this_trx=TRUE"));
    }
  }

  if (wide_handler->lock_table_type > 0 ||
    wide_handler->sql_command == SQLCOM_UNLOCK_TABLES)
  {
    if (wide_handler->sql_command == SQLCOM_UNLOCK_TABLES)
    {
      /* lock tables does not call reset() */
      /* unlock tables does not call store_lock() */
      wide_handler->lock_table_type = 0;
    }

    /* lock/unlock tables */
    if (partition_handler && partition_handler->handlers)
    {
      for (uint roop_count= 0; roop_count < partition_handler->no_parts;
           ++roop_count)
      {
        if (unlikely((error_num =
          partition_handler->handlers[roop_count]->lock_tables())))
        {
          DBUG_RETURN(error_num);
        }
      }
    }
    else if (unlikely((error_num= lock_tables())))
    {
      DBUG_RETURN(error_num);
    }
  }

  DBUG_RETURN(0);
}

int ha_spider::start_stmt(
  THD *thd,
  thr_lock_type lock_type
) {
  DBUG_ENTER("ha_spider::start_stmt");
  if (
    wide_handler->stage == SPD_HND_STAGE_START_STMT &&
    wide_handler->stage_executor != this)
  {
    DBUG_RETURN(0);
  }
  wide_handler->stage = SPD_HND_STAGE_START_STMT;
  wide_handler->stage_executor = this;
  DBUG_RETURN(0);
}

int ha_spider::reset()
{
  int error_num = 0, error_num2, roop_count;
  THD *thd = ha_thd();
  SPIDER_TRX *tmp_trx, *trx_bak;
  SPIDER_CONDITION *tmp_cond;
/*
  char first_byte, first_byte_bak;
*/
  backup_error_status();
  DBUG_ENTER("ha_spider::reset");
  DBUG_PRINT("info",("spider this=%p", this));
  direct_aggregate_item_current = direct_aggregate_item_first;
  while (direct_aggregate_item_current)
  {
    if (direct_aggregate_item_current->item)
    {
      delete direct_aggregate_item_current->item;
      direct_aggregate_item_current->item = NULL;
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY_AND_THDPTR
      if (direct_aggregate_item_current->init_mem_root)
      {
        free_root(&direct_aggregate_item_current->mem_root, MYF(0));
        direct_aggregate_item_current->init_mem_root = FALSE;
      }
#endif
    }
    direct_aggregate_item_current = direct_aggregate_item_current->next;
  }
  result_list.direct_aggregate = FALSE;
  result_list.snap_direct_aggregate = FALSE;
  result_list.direct_distinct = FALSE;
  store_error_num = 0;
  if (
    wide_handler &&
    wide_handler->sql_command != SQLCOM_END
  ) {
    wide_handler->sql_command = SQLCOM_END;
    wide_handler->between_flg = FALSE;
    wide_handler->idx_bitmap_is_set = FALSE;
    wide_handler->rnd_bitmap_is_set = FALSE;
    wide_handler->quick_mode = FALSE;
    wide_handler->keyread = FALSE;
    wide_handler->ignore_dup_key = FALSE;
    wide_handler->write_can_replace = FALSE;
    wide_handler->insert_with_update = FALSE;
    wide_handler->low_priority = FALSE;
    wide_handler->high_priority = FALSE;
    wide_handler->insert_delayed = FALSE;
    wide_handler->lock_table_type = 0;
    wide_handler->semi_trx_isolation_chk = FALSE;
    wide_handler->semi_trx_chk = FALSE;
    if (!is_clone)
    {
      memset(wide_handler->ft_discard_bitmap, 0xFF,
        no_bytes_in_map(table->read_set));
      memset(wide_handler->searched_bitmap, 0,
        no_bytes_in_map(table->read_set));
    }
    while (wide_handler->condition)
    {
      tmp_cond = wide_handler->condition->next;
      spider_free(spider_current_trx, wide_handler->condition, MYF(0));
      wide_handler->condition = tmp_cond;
    }
    wide_handler->cond_check = FALSE;
    wide_handler->direct_update_fields = NULL;
#ifdef INFO_KIND_FORCE_LIMIT_BEGIN
    wide_handler->info_limit = 9223372036854775807LL;
#endif
    wide_handler->stage = SPD_HND_STAGE_NONE;
    wide_handler->stage_executor = NULL;
  }
  if (!(tmp_trx = spider_get_trx(thd, TRUE, &error_num2)))
  {
    DBUG_PRINT("info",("spider get trx error"));
    if (check_error_mode(error_num2))
      error_num = error_num2;
  }
  if (share)
  {
    trx_bak = wide_handler->trx;
    wide_handler->trx = tmp_trx;
    if ((error_num2 = spider_db_free_result(this, FALSE)))
      error_num = error_num2;
    wide_handler->trx = trx_bak;
    memset(need_mons, 0, sizeof(int) * share->link_count);
    memset(result_list.casual_read, 0, sizeof(int) * share->link_count);
    rm_bulk_tmp_table();
    for (roop_count = share->link_count - 1; roop_count >= 0; roop_count--)
    {
      result_list.update_sqls[roop_count].length(0);

      if ((error_num2 = close_opened_handler(roop_count, TRUE)))
      {
        if (check_error_mode(error_num2))
          error_num = error_num2;
      }

      conn_kind[roop_count] = SPIDER_CONN_KIND_MYSQL;
    }
    result_list.bulk_update_mode = 0;
    result_list.bulk_update_size = 0;
    result_list.bulk_update_start = SPD_BU_NOT_START;
    for (roop_count = 0; roop_count < (int) share->use_dbton_count;
      roop_count++)
    {
      uint dbton_id = share->use_dbton_ids[roop_count];
      if ((error_num2 = dbton_handler[dbton_id]->reset()))
      {
        if (check_error_mode(error_num2))
          error_num = error_num2;
      }
    }
  }
  dml_inited = FALSE;
  use_pre_call = FALSE;
  use_pre_action = FALSE;
  pre_bitmap_checked = FALSE;
  bulk_insert = FALSE;
  partition_handler->clone_bitmap_init = FALSE;
  result_list.tmp_table_join = FALSE;
  result_list.use_union = FALSE;
  result_list.use_both_key = FALSE;
  pt_clone_last_searcher = NULL;
  conn_kinds = SPIDER_CONN_KIND_MYSQL;
  use_index_merge = FALSE;
  init_rnd_handler = FALSE;
  if (multi_range_keys)
  {
    DBUG_PRINT("info",("spider free multi_range_keys=%p", multi_range_keys));
    spider_free(spider_current_trx, multi_range_keys, MYF(0));
    multi_range_keys = NULL;
  }
  multi_range_num = 0;
  ft_handler = NULL;
  ft_current = NULL;
  ft_count = 0;
  ft_init_without_index_init = FALSE;
  sql_kinds = 0;
  do_direct_update = FALSE;
  prev_index_rnd_init = SPD_NONE;
  result_list.have_sql_kind_backup = FALSE;
  result_list.direct_order_limit = FALSE;
  result_list.direct_limit_offset = FALSE;
  result_list.set_split_read = FALSE;
  result_list.insert_dup_update_pushdown = FALSE;
  use_spatial_index = FALSE;
  use_fields = FALSE;
  error_mode = 0;
  DBUG_RETURN(error_num);
}

int ha_spider::extra(
  enum ha_extra_function operation
) {
  int error_num;
  DBUG_ENTER("ha_spider::extra");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider operation=%d", (int) operation));
  if (
    wide_handler->stage == SPD_HND_STAGE_EXTRA &&
    wide_handler->stage_executor != this)
  {
    DBUG_RETURN(0);
  }
  wide_handler->stage = SPD_HND_STAGE_EXTRA;
  wide_handler->stage_executor = this;
  switch (operation)
  {
    case HA_EXTRA_QUICK:
      wide_handler->quick_mode = TRUE;
      break;
    case HA_EXTRA_KEYREAD:
      if (!is_clone)
      {
        wide_handler->keyread = TRUE;
        if (wide_handler->update_request)
        {
          if (check_partitioned())
            wide_handler->keyread = FALSE;
        }
      }
      break;
    case HA_EXTRA_NO_KEYREAD:
      wide_handler->keyread = FALSE;
      break;
    case HA_EXTRA_IGNORE_DUP_KEY:
      wide_handler->ignore_dup_key = TRUE;
      break;
    case HA_EXTRA_NO_IGNORE_DUP_KEY:
      wide_handler->ignore_dup_key = FALSE;
      break;
    case HA_EXTRA_WRITE_CAN_REPLACE:
      wide_handler->write_can_replace = TRUE;
      break;
    case HA_EXTRA_WRITE_CANNOT_REPLACE:
      wide_handler->write_can_replace = FALSE;
      break;
    case HA_EXTRA_INSERT_WITH_UPDATE:
      wide_handler->insert_with_update = TRUE;
      break;
    case HA_EXTRA_ATTACH_CHILDREN:
      DBUG_PRINT("info",("spider HA_EXTRA_ATTACH_CHILDREN"));
      if (!(wide_handler->trx = spider_get_trx(ha_thd(), TRUE, &error_num)))
        DBUG_RETURN(error_num);
      break;
    case HA_EXTRA_ADD_CHILDREN_LIST:
      DBUG_PRINT("info",("spider HA_EXTRA_ADD_CHILDREN_LIST"));
      if (!(wide_handler->trx = spider_get_trx(ha_thd(), TRUE, &error_num)))
        DBUG_RETURN(error_num);
      break;
#if defined(HA_EXTRA_HAS_STARTING_ORDERED_INDEX_SCAN) || defined(HA_EXTRA_HAS_HA_EXTRA_USE_CMP_REF)
#ifdef HA_EXTRA_HAS_STARTING_ORDERED_INDEX_SCAN
    case HA_EXTRA_STARTING_ORDERED_INDEX_SCAN:
#endif
#ifdef HA_EXTRA_HAS_HA_EXTRA_USE_CMP_REF
    case HA_EXTRA_USE_CMP_REF:
#endif
      DBUG_PRINT("info",("spider HA_EXTRA_STARTING_ORDERED_INDEX_SCAN"));
      if (table_share->primary_key != MAX_KEY)
      {
        DBUG_PRINT("info",("spider need primary key columns"));
        KEY *key_info = &table->key_info[table->s->primary_key];
        KEY_PART_INFO *key_part;
        uint part_num;
        for (
          key_part = key_info->key_part, part_num = 0;
          part_num < spider_user_defined_key_parts(key_info);
          key_part++, part_num++
        ) {
          spider_set_bit(wide_handler->searched_bitmap,
            key_part->field->field_index);
        }
      } else {
        DBUG_PRINT("info",("spider need all columns"));
        Field **field;
        for (
          field = table->field;
          *field;
          field++
        ) {
          spider_set_bit(wide_handler->searched_bitmap, (*field)->field_index);
        }
      }
      break;
#endif
    default:
      break;
  }
  DBUG_RETURN(0);
}

int ha_spider::index_init(
  uint idx,
  bool sorted
) {
  int error_num;
  DBUG_ENTER("ha_spider::index_init");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider idx=%u", idx));
  if (!dml_inited)
  {
    if (unlikely((error_num = dml_init())))
    {
      DBUG_RETURN(error_num);
    }
  }
  pushed_pos = NULL;
  active_index = idx;
  result_list.sorted = sorted;
  spider_set_result_list_param(this);
  mrr_with_cnt = FALSE;
  init_index_handler = FALSE;
  use_spatial_index = FALSE;

  if (pre_bitmap_checked)
    pre_bitmap_checked = FALSE;
  else {
    if (wide_handler->external_lock_type == F_WRLCK)
    {
      pk_update = FALSE;
/*
      check_and_start_bulk_update(SPD_BU_START_BY_INDEX_OR_RND_INIT);
*/
      if (
        wide_handler->update_request &&
        share->have_recovery_link &&
        (pk_update = spider_check_pk_update(table))
      ) {
        bitmap_set_all(table->read_set);
        if (is_clone)
          memset(wide_handler->searched_bitmap, 0xFF,
            no_bytes_in_map(table->read_set));
      }
    }

    if (!is_clone)
      set_select_column_mode();
  }

  if ((error_num = reset_sql_sql(
    SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
    DBUG_RETURN(error_num);
  result_list.check_direct_order_limit = FALSE;
  prev_index_rnd_init = SPD_INDEX;
  DBUG_RETURN(0);
}


int ha_spider::index_end()
{
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::index_end");
  DBUG_PRINT("info",("spider this=%p", this));
  active_index = MAX_KEY;
/*
#ifdef INFO_KIND_FORCE_LIMIT_BEGIN
  info_limit = 9223372036854775807LL;
#endif
  if (
    (error_num = drop_tmp_tables()) ||
    (error_num = check_and_end_bulk_update(
      SPD_BU_START_BY_INDEX_OR_RND_INIT)) ||
    (error_num = spider_trx_check_link_idx_failed(this))
  )
    DBUG_RETURN(check_error_mode(error_num));
*/
  if ((error_num = drop_tmp_tables()))
    DBUG_RETURN(check_error_mode(error_num));
  result_list.use_union = FALSE;
  DBUG_RETURN(0);
}


int ha_spider::index_read_map_internal(
  uchar *buf,
  const uchar *key,
  key_part_map keypart_map,
  enum ha_rkey_function find_flag
) {
  int error_num, roop_count;
  key_range start_key;
  SPIDER_CONN *conn;
  backup_error_status();
  DBUG_ENTER("ha_spider::index_read_map_internal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  do_direct_update = FALSE;
  if (
    find_flag >= HA_READ_MBR_CONTAIN &&
    find_flag <= HA_READ_MBR_EQUAL
  )
    use_spatial_index = TRUE;

  if ((error_num = index_handler_init()))
    DBUG_RETURN(check_error_mode_eof(error_num));
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  spider_db_free_one_result_for_start_next(this);
  spider_set_result_list_param(this);
  check_direct_order_limit();
  start_key.key = key;
  start_key.keypart_map = keypart_map;
  start_key.flag = find_flag;
  if ((error_num = reset_sql_sql(
    SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
    DBUG_RETURN(error_num);
  if ((error_num = spider_set_conn_bg_param(this)))
    DBUG_RETURN(error_num);
  check_select_column(FALSE);
  DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
  result_list.finish_flg = FALSE;
  result_list.record_num = 0;
  if (wide_handler->keyread)
    result_list.keyread = TRUE;
  else
    result_list.keyread = FALSE;
  if (
    (error_num = spider_db_append_select(this)) ||
    (error_num = spider_db_append_select_columns(this))
  )
    DBUG_RETURN(error_num);
  if (
    share->key_hint &&
    (error_num = append_hint_after_table_sql_part(
      SPIDER_SQL_TYPE_SELECT_SQL))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
  result_list.desc_flg = FALSE;
  result_list.sorted = TRUE;
  result_list.key_info = &table->key_info[active_index];
  check_distinct_key_query();
  result_list.limit_num =
    result_list.internal_limit >= result_list.split_read ?
    result_list.split_read : result_list.internal_limit;
  DBUG_PRINT("info",("spider result_list.internal_limit=%lld",
    result_list.internal_limit));
  DBUG_PRINT("info",("spider result_list.split_read=%lld",
    result_list.split_read));
  DBUG_PRINT("info",("spider result_list.limit_num=%lld",
    result_list.limit_num));
  if (
    (error_num = spider_db_append_key_where(
      &start_key, NULL, this))
  )
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider result_list.internal_limit=%lld",
    result_list.internal_limit));
  DBUG_PRINT("info",("spider result_list.split_read=%lld",
    result_list.split_read));
  DBUG_PRINT("info",("spider result_list.limit_num=%lld",
    result_list.limit_num));
  if (sql_kinds & SPIDER_SQL_KIND_SQL)
  {
    if (result_list.direct_order_limit)
    {
      if ((error_num =
        append_key_order_for_direct_order_limit_with_alias_sql_part(
          NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    } else {
      if ((error_num = append_key_order_with_alias_sql_part(
        NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    if ((error_num = append_limit_sql_part(
      result_list.internal_offset,
      result_list.limit_num,
      SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
    if (
      (error_num = append_select_lock_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
  {
    if ((error_num = append_limit_sql_part(
      result_list.internal_offset,
      result_list.limit_num,
      SPIDER_SQL_TYPE_HANDLER)))
    {
      DBUG_RETURN(error_num);
    }
  }

  int roop_start, roop_end, lock_mode, link_ok;
  lock_mode = spider_conn_lock_mode(this);
  if (lock_mode)
  {
    /* "for update" or "lock in share mode" */
    link_ok = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_OK);
    roop_start = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_end = share->link_count;
  } else {
    link_ok = search_link_idx;
    roop_start = search_link_idx;
    roop_end = search_link_idx + 1;
  }
  for (roop_count = roop_start; roop_count < roop_end;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    if (result_list.bgs_phase > 0)
    {
      if ((error_num = spider_check_and_init_casual_read(
        wide_handler->trx->thd, this,
        roop_count)))
        DBUG_RETURN(error_num);
      if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
        TRUE, FALSE, (roop_count != link_ok))))
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          share->monitoring_kind[roop_count] &&
          need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              wide_handler->trx,
              wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(check_error_mode_eof(error_num));
      }
    } else {
      ulong sql_type;
        conn = conns[roop_count];
        if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
        {
          sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
        } else {
          sql_type = SPIDER_SQL_TYPE_HANDLER;
        }
      spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      if ((error_num = dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
      {
        if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(error_num);
      }
      if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
        conn->need_mon = &need_mons[roop_count];
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        if ((error_num = spider_db_set_names(this, conn,
          roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        spider_conn_set_timeout_from_share(conn, roop_count,
          wide_handler->trx->thd, share);
        if (dbton_hdl->execute_sql(
          sql_type,
          conn,
          result_list.quick_mode,
          &need_mons[roop_count])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          error_num = spider_db_errorno(conn);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        connection_ids[roop_count] = conn->connection_id;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        if (roop_count == link_ok)
        {
          if ((error_num = spider_db_store_result(this, roop_count, table)))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          result_link_idx = link_ok;
        } else {
          spider_db_discard_result(this, roop_count, conn);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
    }
  }
  if (buf && (error_num = spider_db_fetch(buf, this, table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(0);
}

int ha_spider::pre_index_read_map(
  const uchar *key,
  key_part_map keypart_map,
  enum ha_rkey_function find_flag,
  bool use_parallel
) {
  DBUG_ENTER("ha_spider::pre_index_read_map");
  DBUG_PRINT("info",("spider this=%p", this));
  check_pre_call(use_parallel);
  if (use_pre_call)
  {
    store_error_num =
      index_read_map_internal(NULL, key, keypart_map, find_flag);
    DBUG_RETURN(store_error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::index_read_map(
  uchar *buf,
  const uchar *key,
  key_part_map keypart_map,
  enum ha_rkey_function find_flag
) {
  int error_num;
  DBUG_ENTER("ha_spider::index_read_map");
  DBUG_PRINT("info",("spider this=%p", this));
  if (use_pre_call)
  {
    if (store_error_num)
    {
      if (store_error_num == HA_ERR_END_OF_FILE)
        table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(store_error_num);
    }
    if ((error_num = spider_bg_all_conn_pre_next(this, search_link_idx)))
      DBUG_RETURN(error_num);
    use_pre_call = FALSE;
    if (
      result_list.sorted &&
      result_list.desc_flg
    ) {
      DBUG_RETURN(index_prev(buf));
    }
    DBUG_RETURN(index_next(buf));
  }
  DBUG_RETURN(index_read_map_internal(buf, key, keypart_map, find_flag));
}

int ha_spider::index_read_last_map_internal(
  uchar *buf,
  const uchar *key,
  key_part_map keypart_map
) {
  int error_num;
  key_range start_key;
  SPIDER_CONN *conn;
  backup_error_status();
  DBUG_ENTER("ha_spider::index_read_last_map_internal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  do_direct_update = FALSE;
  if ((error_num = index_handler_init()))
    DBUG_RETURN(check_error_mode_eof(error_num));
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
/*
  spider_db_free_one_result_for_start_next(this);
*/
  if (
      result_list.current
    &&
    (error_num = spider_db_free_result(this, FALSE))
  )
    DBUG_RETURN(error_num);

  check_direct_order_limit();
  start_key.key = key;
  start_key.keypart_map = keypart_map;
  start_key.flag = HA_READ_KEY_EXACT;
  if ((error_num = reset_sql_sql(
    SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
    DBUG_RETURN(error_num);
  if ((error_num = spider_set_conn_bg_param(this)))
    DBUG_RETURN(error_num);
  check_select_column(FALSE);
  DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
  result_list.finish_flg = FALSE;
  result_list.record_num = 0;
  if (wide_handler->keyread)
    result_list.keyread = TRUE;
  else
    result_list.keyread = FALSE;
  if (
    (error_num = spider_db_append_select(this)) ||
    (error_num = spider_db_append_select_columns(this))
  )
    DBUG_RETURN(error_num);
  if (
    share->key_hint &&
    (error_num = append_hint_after_table_sql_part(
      SPIDER_SQL_TYPE_SELECT_SQL))
  )
    DBUG_RETURN(error_num);
  set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
  result_list.desc_flg = TRUE;
  result_list.sorted = TRUE;
  result_list.key_info = &table->key_info[active_index];
  check_distinct_key_query();
  result_list.limit_num =
    result_list.internal_limit >= result_list.split_read ?
    result_list.split_read : result_list.internal_limit;
  if (
    (error_num = spider_db_append_key_where(
      &start_key, NULL, this))
  )
    DBUG_RETURN(error_num);
  if (sql_kinds & SPIDER_SQL_KIND_SQL)
  {
    if (result_list.direct_order_limit)
    {
      if ((error_num =
        append_key_order_for_direct_order_limit_with_alias_sql_part(
          NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    } else {
      if ((error_num = append_key_order_with_alias_sql_part(
        NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    if ((error_num = append_limit_sql_part(
      result_list.internal_offset,
      result_list.limit_num,
      SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
    if (
      (error_num = append_select_lock_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
  {
    if ((error_num = append_limit_sql_part(
      result_list.internal_offset,
      result_list.limit_num,
      SPIDER_SQL_TYPE_HANDLER)))
    {
      DBUG_RETURN(error_num);
    }
  }

  int roop_start, roop_end, roop_count, tmp_lock_mode, link_ok;
  tmp_lock_mode = spider_conn_lock_mode(this);
  if (tmp_lock_mode)
  {
    /* "for update" or "lock in share mode" */
    link_ok = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_OK);
    roop_start = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_end = share->link_count;
  } else {
    link_ok = search_link_idx;
    roop_start = search_link_idx;
    roop_end = search_link_idx + 1;
  }
  for (roop_count = roop_start; roop_count < roop_end;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    if (result_list.bgs_phase > 0)
    {
      if ((error_num = spider_check_and_init_casual_read(
        wide_handler->trx->thd, this,
        roop_count)))
        DBUG_RETURN(error_num);
      if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
        TRUE, FALSE, (roop_count != link_ok))))
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          share->monitoring_kind[roop_count] &&
          need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              wide_handler->trx,
              wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(check_error_mode_eof(error_num));
      }
    } else {
      ulong sql_type;
        conn = conns[roop_count];
        if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
        {
          sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
        } else {
          sql_type = SPIDER_SQL_TYPE_HANDLER;
        }
      spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      if ((error_num = dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
      {
        if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(error_num);
      }
      if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
        conn->need_mon = &need_mons[roop_count];
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        if ((error_num = spider_db_set_names(this, conn,
          roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        spider_conn_set_timeout_from_share(conn, roop_count,
          wide_handler->trx->thd, share);
        if (dbton_hdl->execute_sql(
          sql_type,
          conn,
          result_list.quick_mode,
          &need_mons[roop_count])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          error_num = spider_db_errorno(conn);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        connection_ids[roop_count] = conn->connection_id;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        if (roop_count == link_ok)
        {
          if ((error_num = spider_db_store_result(this, roop_count, table)))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          result_link_idx = link_ok;
        } else {
          spider_db_discard_result(this, roop_count, conn);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
    }
  }
  if (buf && (error_num = spider_db_fetch(buf, this, table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(0);
}

int ha_spider::pre_index_read_last_map(
  const uchar *key,
  key_part_map keypart_map,
  bool use_parallel
) {
  DBUG_ENTER("ha_spider::pre_index_read_last_map");
  DBUG_PRINT("info",("spider this=%p", this));
  check_pre_call(use_parallel);
  if (use_pre_call)
  {
    store_error_num =
      index_read_last_map_internal(NULL, key, keypart_map);
    DBUG_RETURN(store_error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::index_read_last_map(
  uchar *buf,
  const uchar *key,
  key_part_map keypart_map
) {
  int error_num;
  DBUG_ENTER("ha_spider::index_read_last_map");
  DBUG_PRINT("info",("spider this=%p", this));
  if (use_pre_call)
  {
    if (store_error_num)
    {
      if (store_error_num == HA_ERR_END_OF_FILE)
        table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(store_error_num);
    }
    if ((error_num = spider_bg_all_conn_pre_next(this, search_link_idx)))
      DBUG_RETURN(error_num);
    use_pre_call = FALSE;
    DBUG_RETURN(index_prev(buf));
  }
  DBUG_RETURN(index_read_last_map_internal(buf, key, keypart_map));
}

int ha_spider::index_next(
  uchar *buf
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::index_next");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  if (
    result_list.sorted &&
    result_list.desc_flg
  ) {
    if ((error_num = spider_db_seek_prev(buf, this, table)))
      DBUG_RETURN(check_error_mode_eof(error_num));
    DBUG_RETURN(0);
  }
  if ((error_num = spider_db_seek_next(buf, this, search_link_idx, table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(0);
}

int ha_spider::index_prev(
  uchar *buf
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::index_prev");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  if (
    result_list.sorted &&
    result_list.desc_flg
  ) {
    if ((error_num = spider_db_seek_next(buf, this, search_link_idx, table)))
      DBUG_RETURN(check_error_mode_eof(error_num));
    DBUG_RETURN(0);
  }
  if ((error_num = spider_db_seek_prev(buf, this, table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(0);
}

int ha_spider::index_first_internal(
  uchar *buf
) {
  int error_num;
  SPIDER_CONN *conn;
  backup_error_status();
  DBUG_ENTER("ha_spider::index_first_internal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  do_direct_update = FALSE;
  if ((error_num = index_handler_init()))
    DBUG_RETURN(check_error_mode_eof(error_num));
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  if (
    sql_is_empty(SPIDER_SQL_TYPE_HANDLER) ||
    sql_is_empty(SPIDER_SQL_TYPE_SELECT_SQL)
  ) {
/*
    spider_db_free_one_result_for_start_next(this);
*/
    if ((error_num = spider_db_free_result(this, FALSE)))
      DBUG_RETURN(error_num);
    if ((error_num = reset_sql_sql(
      SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
      DBUG_RETURN(error_num);

    check_direct_order_limit();
    if ((error_num = spider_set_conn_bg_param(this)))
      DBUG_RETURN(error_num);
    check_select_column(FALSE);
    DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
    result_list.finish_flg = FALSE;
    result_list.record_num = 0;
    if (wide_handler->keyread)
      result_list.keyread = TRUE;
    else
      result_list.keyread = FALSE;
    if (
      (error_num = spider_db_append_select(this)) ||
      (error_num = spider_db_append_select_columns(this))
    )
      DBUG_RETURN(error_num);
    if (
      share->key_hint &&
      (error_num = append_hint_after_table_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL))
    )
      DBUG_RETURN(error_num);
    set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
    result_list.desc_flg = FALSE;
    result_list.sorted = TRUE;
    result_list.key_info = &table->key_info[active_index];
    result_list.key_order = 0;
    check_distinct_key_query();
    result_list.limit_num =
      result_list.internal_limit >= result_list.split_read ?
      result_list.split_read : result_list.internal_limit;
    if (
      (error_num = spider_db_append_key_where(
        NULL, NULL, this))
    )
      DBUG_RETURN(error_num);
    if (sql_kinds & SPIDER_SQL_KIND_SQL)
    {
      if (result_list.direct_order_limit)
      {
        if ((error_num =
          append_key_order_for_direct_order_limit_with_alias_sql_part(
            NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
          DBUG_RETURN(error_num);
      } else {
        if ((error_num = append_key_order_with_alias_sql_part(
          NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
          DBUG_RETURN(error_num);
      }
      if ((error_num = append_limit_sql_part(
        result_list.internal_offset,
        result_list.limit_num,
        SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
      if (
        (error_num = append_select_lock_sql_part(
          SPIDER_SQL_TYPE_SELECT_SQL))
      ) {
        DBUG_RETURN(error_num);
      }
    }
    if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      if ((error_num = append_limit_sql_part(
        result_list.internal_offset,
        result_list.limit_num,
        SPIDER_SQL_TYPE_HANDLER)))
      {
        DBUG_RETURN(error_num);
      }
    }

    int roop_start, roop_end, roop_count, tmp_lock_mode, link_ok;
    tmp_lock_mode = spider_conn_lock_mode(this);
    if (tmp_lock_mode)
    {
      /* "for update" or "lock in share mode" */
      link_ok = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_OK);
      roop_start = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_end = share->link_count;
    } else {
      link_ok = search_link_idx;
      roop_start = search_link_idx;
      roop_end = search_link_idx + 1;
    }
    for (roop_count = roop_start; roop_count < roop_end;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (result_list.bgs_phase > 0)
      {
        if ((error_num = spider_check_and_init_casual_read(
          wide_handler->trx->thd, this,
          roop_count)))
          DBUG_RETURN(error_num);
        if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
          TRUE, FALSE, (roop_count != link_ok))))
        {
          if (
            error_num != HA_ERR_END_OF_FILE &&
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
      } else {
        ulong sql_type;
          conn = conns[roop_count];
          if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
          {
            sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
          } else {
            sql_type = SPIDER_SQL_TYPE_HANDLER;
          }
        spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if ((error_num =
          dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
        {
          if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
          DBUG_RETURN(error_num);
        }
        if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
          conn->need_mon = &need_mons[roop_count];
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = TRUE;
          conn->mta_conn_mutex_unlock_later = TRUE;
          if ((error_num = spider_db_set_names(this, conn,
            roop_count)))
          {
            DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = FALSE;
            conn->mta_conn_mutex_unlock_later = FALSE;
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
            if (
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          spider_conn_set_timeout_from_share(conn, roop_count,
            wide_handler->trx->thd, share);
          if (dbton_hdl->execute_sql(
            sql_type,
            conn,
            result_list.quick_mode,
            &need_mons[roop_count])
          ) {
            DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = FALSE;
            conn->mta_conn_mutex_unlock_later = FALSE;
            error_num = spider_db_errorno(conn);
            if (
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          connection_ids[roop_count] = conn->connection_id;
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          if (roop_count == link_ok)
          {
            if ((error_num = spider_db_store_result(this, roop_count, table)))
            {
              if (
                error_num != HA_ERR_END_OF_FILE &&
                share->monitoring_kind[roop_count] &&
                need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    roop_count,
                    (uint32) share->monitoring_sid[roop_count],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[roop_count],
                    NULL,
                    0,
                    share->monitoring_kind[roop_count],
                    share->monitoring_limit[roop_count],
                    share->monitoring_flag[roop_count],
                    TRUE
                  );
              }
              DBUG_RETURN(check_error_mode_eof(error_num));
            }
            result_link_idx = link_ok;
          } else {
            spider_db_discard_result(this, roop_count, conn);
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
      }
    }
  }

  if (buf)
  {
    if (
      result_list.sorted &&
      result_list.desc_flg
    ) {
      if ((error_num = spider_db_seek_last(buf, this, search_link_idx, table)))
        DBUG_RETURN(check_error_mode_eof(error_num));
      DBUG_RETURN(0);
    }
    if ((error_num = spider_db_seek_first(buf, this, table)))
      DBUG_RETURN(check_error_mode_eof(error_num));
  }
  DBUG_RETURN(0);
}

int ha_spider::pre_index_first(
  bool use_parallel
) {
  DBUG_ENTER("ha_spider::pre_index_first");
  DBUG_PRINT("info",("spider this=%p", this));
  check_pre_call(use_parallel);
  if (use_pre_call)
  {
    store_error_num =
      index_first_internal(NULL);
    DBUG_RETURN(store_error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::index_first(
  uchar *buf
) {
  int error_num;
  DBUG_ENTER("ha_spider::index_first");
  DBUG_PRINT("info",("spider this=%p", this));
  if (use_pre_call)
  {
    if (store_error_num)
    {
      if (store_error_num == HA_ERR_END_OF_FILE)
        table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(store_error_num);
    }
    if ((error_num = spider_bg_all_conn_pre_next(this, search_link_idx)))
      DBUG_RETURN(error_num);
    use_pre_call = FALSE;
    DBUG_RETURN(index_next(buf));
  }
  DBUG_RETURN(index_first_internal(buf));
}

int ha_spider::index_last_internal(
  uchar *buf
) {
  int error_num;
  SPIDER_CONN *conn;
  backup_error_status();
  DBUG_ENTER("ha_spider::index_last_internal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  do_direct_update = FALSE;
  if ((error_num = index_handler_init()))
    DBUG_RETURN(check_error_mode_eof(error_num));
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  if (
    sql_is_empty(SPIDER_SQL_TYPE_HANDLER) ||
    sql_is_empty(SPIDER_SQL_TYPE_SELECT_SQL)
  ) {
/*
    spider_db_free_one_result_for_start_next(this);
*/
    if ((error_num = spider_db_free_result(this, FALSE)))
      DBUG_RETURN(error_num);
    if ((error_num = reset_sql_sql(
      SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
      DBUG_RETURN(error_num);

    check_direct_order_limit();
    if ((error_num = spider_set_conn_bg_param(this)))
      DBUG_RETURN(error_num);
    check_select_column(FALSE);
    DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
    result_list.finish_flg = FALSE;
    result_list.record_num = 0;
    if (wide_handler->keyread)
      result_list.keyread = TRUE;
    else
      result_list.keyread = FALSE;
    if (
      (error_num = spider_db_append_select(this)) ||
      (error_num = spider_db_append_select_columns(this))
    )
      DBUG_RETURN(error_num);
    if (
      share->key_hint &&
      (error_num = append_hint_after_table_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL))
    )
      DBUG_RETURN(error_num);
    set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
    result_list.desc_flg = TRUE;
    result_list.sorted = TRUE;
    result_list.key_info = &table->key_info[active_index];
    result_list.key_order = 0;
    check_distinct_key_query();
    result_list.limit_num =
      result_list.internal_limit >= result_list.split_read ?
      result_list.split_read : result_list.internal_limit;
    if (
      (error_num = spider_db_append_key_where(
        NULL, NULL, this))
    )
      DBUG_RETURN(error_num);
    if (sql_kinds & SPIDER_SQL_KIND_SQL)
    {
      if (result_list.direct_order_limit)
      {
        if ((error_num =
          append_key_order_for_direct_order_limit_with_alias_sql_part(
            NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
          DBUG_RETURN(error_num);
      } else {
        if ((error_num = append_key_order_with_alias_sql_part(
          NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
          DBUG_RETURN(error_num);
      }
      if ((error_num = append_limit_sql_part(
        result_list.internal_offset,
        result_list.limit_num,
        SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
      if (
        (error_num = append_select_lock_sql_part(
          SPIDER_SQL_TYPE_SELECT_SQL))
      ) {
        DBUG_RETURN(error_num);
      }
    }
    if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      if ((error_num = append_limit_sql_part(
        result_list.internal_offset,
        result_list.limit_num,
        SPIDER_SQL_TYPE_HANDLER)))
      {
        DBUG_RETURN(error_num);
      }
    }

    int roop_start, roop_end, roop_count, tmp_lock_mode, link_ok;
    tmp_lock_mode = spider_conn_lock_mode(this);
    if (tmp_lock_mode)
    {
      /* "for update" or "lock in share mode" */
      link_ok = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_OK);
      roop_start = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_end = share->link_count;
    } else {
      link_ok = search_link_idx;
      roop_start = search_link_idx;
      roop_end = search_link_idx + 1;
    }
    for (roop_count = roop_start; roop_count < roop_end;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (result_list.bgs_phase > 0)
      {
        if ((error_num = spider_check_and_init_casual_read(
          wide_handler->trx->thd, this,
          roop_count)))
          DBUG_RETURN(error_num);
        if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
          TRUE, FALSE, (roop_count != link_ok))))
        {
          if (
            error_num != HA_ERR_END_OF_FILE &&
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
      } else {
        ulong sql_type;
          conn = conns[roop_count];
          if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
          {
            sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
          } else {
            sql_type = SPIDER_SQL_TYPE_HANDLER;
          }
        spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if ((error_num =
          dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
        {
          if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
          DBUG_RETURN(error_num);
        }
        if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
          conn->need_mon = &need_mons[roop_count];
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = TRUE;
          conn->mta_conn_mutex_unlock_later = TRUE;
          if ((error_num = spider_db_set_names(this, conn,
            roop_count)))
          {
            DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = FALSE;
            conn->mta_conn_mutex_unlock_later = FALSE;
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
            if (
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          spider_conn_set_timeout_from_share(conn, roop_count,
            wide_handler->trx->thd, share);
          if (dbton_hdl->execute_sql(
            sql_type,
            conn,
            result_list.quick_mode,
            &need_mons[roop_count])
          ) {
            DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = FALSE;
            conn->mta_conn_mutex_unlock_later = FALSE;
            error_num = spider_db_errorno(conn);
            if (
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          connection_ids[roop_count] = conn->connection_id;
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          if (roop_count == link_ok)
          {
            if ((error_num = spider_db_store_result(this, roop_count, table)))
            {
              if (
                error_num != HA_ERR_END_OF_FILE &&
                share->monitoring_kind[roop_count] &&
                need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    roop_count,
                    (uint32) share->monitoring_sid[roop_count],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[roop_count],
                    NULL,
                    0,
                    share->monitoring_kind[roop_count],
                    share->monitoring_limit[roop_count],
                    share->monitoring_flag[roop_count],
                    TRUE
                  );
              }
              DBUG_RETURN(check_error_mode_eof(error_num));
            }
            result_link_idx = link_ok;
          } else {
            spider_db_discard_result(this, roop_count, conn);
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
      }
    }
  }

  if (buf)
  {
    if (
      result_list.sorted &&
      result_list.desc_flg
    ) {
      if ((error_num = spider_db_seek_first(buf, this, table)))
        DBUG_RETURN(check_error_mode_eof(error_num));
      DBUG_RETURN(0);
    }
    if ((error_num = spider_db_seek_last(buf, this, search_link_idx, table)))
      DBUG_RETURN(check_error_mode_eof(error_num));
  }
  DBUG_RETURN(0);
}

int ha_spider::pre_index_last(
  bool use_parallel
) {
  DBUG_ENTER("ha_spider::pre_index_last");
  DBUG_PRINT("info",("spider this=%p", this));
  check_pre_call(use_parallel);
  if (use_pre_call)
  {
    store_error_num =
      index_last_internal(NULL);
    DBUG_RETURN(store_error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::index_last(
  uchar *buf
) {
  int error_num;
  DBUG_ENTER("ha_spider::index_last");
  DBUG_PRINT("info",("spider this=%p", this));
  if (use_pre_call)
  {
    if (store_error_num)
    {
      if (store_error_num == HA_ERR_END_OF_FILE)
        table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(store_error_num);
    }
    if ((error_num = spider_bg_all_conn_pre_next(this, search_link_idx)))
      DBUG_RETURN(error_num);
    use_pre_call = FALSE;
    DBUG_RETURN(index_prev(buf));
  }
  DBUG_RETURN(index_last_internal(buf));
}

int ha_spider::index_next_same(
  uchar *buf,
  const uchar *key,
  uint keylen
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::index_next_same");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  if (
    result_list.sorted &&
    result_list.desc_flg
  ) {
    if ((error_num = spider_db_seek_prev(buf, this, table)))
      DBUG_RETURN(check_error_mode_eof(error_num));
    DBUG_RETURN(0);
  }
  if ((error_num = spider_db_seek_next(buf, this, search_link_idx, table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(0);
}

int ha_spider::read_range_first_internal(
  uchar *buf,
  const key_range *start_key,
  const key_range *end_key,
  bool eq_range,
  bool sorted
) {
  int error_num;
  SPIDER_CONN *conn;
  backup_error_status();
  DBUG_ENTER("ha_spider::read_range_first_internal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  do_direct_update = FALSE;
  if (
    start_key &&
    start_key->flag >= HA_READ_MBR_CONTAIN &&
    start_key->flag <= HA_READ_MBR_EQUAL
  )
    use_spatial_index = TRUE;

  if (end_key)
  {
    key_compare_result_on_equal =
      ((end_key->flag == HA_READ_BEFORE_KEY) ? 1 :
      (end_key->flag == HA_READ_AFTER_KEY) ? -1 : 0);
  }
  range_key_part = table->key_info[active_index].key_part;

  if ((error_num = index_handler_init()))
    DBUG_RETURN(check_error_mode_eof(error_num));
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  spider_db_free_one_result_for_start_next(this);
  check_direct_order_limit();
  if ((error_num = reset_sql_sql(
    SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
    DBUG_RETURN(error_num);
  if ((error_num = spider_set_conn_bg_param(this)))
    DBUG_RETURN(error_num);
  check_select_column(FALSE);
  DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
  result_list.finish_flg = FALSE;
  result_list.record_num = 0;
  if (wide_handler->keyread)
    result_list.keyread = TRUE;
  else
    result_list.keyread = FALSE;
  if (
    (error_num = spider_db_append_select(this)) ||
    (error_num = spider_db_append_select_columns(this))
  )
    DBUG_RETURN(error_num);
  if (
    share->key_hint &&
    (error_num = append_hint_after_table_sql_part(
      SPIDER_SQL_TYPE_SELECT_SQL))
  )
    DBUG_RETURN(error_num);
  set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
  result_list.desc_flg = FALSE;
  result_list.sorted = sorted;
  result_list.key_info = &table->key_info[active_index];
  check_distinct_key_query();
  result_list.limit_num =
    result_list.internal_limit >= result_list.split_read ?
    result_list.split_read : result_list.internal_limit;
  DBUG_PRINT("info",("spider limit_num=%lld", result_list.limit_num));
  if (
    (error_num = spider_db_append_key_where(
      start_key, eq_range ? NULL : end_key, this))
  )
    DBUG_RETURN(error_num);
  if (sql_kinds & SPIDER_SQL_KIND_SQL)
  {
    if (result_list.direct_order_limit)
    {
      if ((error_num =
        append_key_order_for_direct_order_limit_with_alias_sql_part(
          NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    } else {
      if ((error_num = append_key_order_with_alias_sql_part(
        NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    if ((error_num = append_limit_sql_part(
      result_list.internal_offset,
      result_list.limit_num,
      SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
    if (
      (error_num = append_select_lock_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
  {
    if ((error_num = append_limit_sql_part(
      result_list.internal_offset,
      result_list.limit_num,
      SPIDER_SQL_TYPE_HANDLER)))
    {
      DBUG_RETURN(error_num);
    }
  }

  int roop_start, roop_end, roop_count, tmp_lock_mode, link_ok;
  tmp_lock_mode = spider_conn_lock_mode(this);
  if (tmp_lock_mode)
  {
    /* "for update" or "lock in share mode" */
    link_ok = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_OK);
    roop_start = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_end = share->link_count;
  } else {
    link_ok = search_link_idx;
    roop_start = search_link_idx;
    roop_end = search_link_idx + 1;
  }
  for (roop_count = roop_start; roop_count < roop_end;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    if (result_list.bgs_phase > 0)
    {
      if ((error_num = spider_check_and_init_casual_read(
        wide_handler->trx->thd, this,
        roop_count)))
        DBUG_RETURN(error_num);
      if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
        TRUE, FALSE, (roop_count != link_ok))))
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          share->monitoring_kind[roop_count] &&
          need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              wide_handler->trx,
              wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(check_error_mode_eof(error_num));
      }
    } else {
      ulong sql_type;
        conn = conns[roop_count];
        if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
        {
          sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
        } else {
          sql_type = SPIDER_SQL_TYPE_HANDLER;
        }
      spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      if ((error_num = dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
      {
        if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(error_num);
      }
      if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
        conn->need_mon = &need_mons[roop_count];
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        if ((error_num = spider_db_set_names(this, conn,
          roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        spider_conn_set_timeout_from_share(conn, roop_count,
          wide_handler->trx->thd, share);
        if (dbton_hdl->execute_sql(
          sql_type,
          conn,
          result_list.quick_mode,
          &need_mons[roop_count])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          error_num = spider_db_errorno(conn);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        connection_ids[roop_count] = conn->connection_id;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        if (roop_count == link_ok)
        {
          if ((error_num = spider_db_store_result(this, roop_count, table)))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          result_link_idx = link_ok;
        } else {
          spider_db_discard_result(this, roop_count, conn);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
    }
  }
  if (buf && (error_num = spider_db_fetch(buf, this, table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(0);
}

int ha_spider::pre_read_range_first(
  const key_range *start_key,
  const key_range *end_key,
  bool eq_range,
  bool sorted,
  bool use_parallel
) {
  DBUG_ENTER("ha_spider::pre_read_range_first");
  DBUG_PRINT("info",("spider this=%p", this));
  check_pre_call(use_parallel);
  if (use_pre_call)
  {
    store_error_num =
      read_range_first_internal(NULL, start_key, end_key, eq_range, sorted);
    DBUG_RETURN(store_error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::read_range_first(
  const key_range *start_key,
  const key_range *end_key,
  bool eq_range,
  bool sorted
) {
  int error_num;
  DBUG_ENTER("ha_spider::read_range_first");
  DBUG_PRINT("info",("spider this=%p", this));
  if (use_pre_call)
  {
    if (store_error_num)
    {
      if (store_error_num == HA_ERR_END_OF_FILE)
        table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(store_error_num);
    }
    if ((error_num = spider_bg_all_conn_pre_next(this, search_link_idx)))
      DBUG_RETURN(error_num);
    use_pre_call = FALSE;
    if ((error_num = read_range_next()))
      DBUG_RETURN(error_num);
    DBUG_RETURN(check_ha_range_eof());
  }
  if ((error_num = read_range_first_internal(table->record[0], start_key,
    end_key, eq_range, sorted)))
    DBUG_RETURN(error_num);
  DBUG_RETURN(check_ha_range_eof());
}

int ha_spider::read_range_next()
{
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::read_range_next");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  if (
    result_list.sorted &&
    result_list.desc_flg
  ) {
    if ((error_num = spider_db_seek_prev(table->record[0], this, table)))
      DBUG_RETURN(check_error_mode_eof(error_num));
    DBUG_RETURN(0);
  }
  if ((error_num = spider_db_seek_next(table->record[0], this, search_link_idx,
    table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(check_ha_range_eof());
}

void ha_spider::reset_no_where_cond()
{
  uint roop_count;
  DBUG_ENTER("ha_spider::reset_no_where_cond");
    for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
    {
      dbton_handler[share->use_sql_dbton_ids[roop_count]]->no_where_cond =
        FALSE;
    }
  DBUG_VOID_RETURN;
}

bool ha_spider::check_no_where_cond()
{
  uint roop_count;
  DBUG_ENTER("ha_spider::check_no_where_cond");
    for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
    {
      if (dbton_handler[share->use_sql_dbton_ids[roop_count]]->no_where_cond)
      {
        DBUG_RETURN(TRUE);
      }
    }
  DBUG_RETURN(FALSE);
}

ha_rows ha_spider::multi_range_read_info_const(
  uint keyno,
  RANGE_SEQ_IF *seq,
  void *seq_init_param,
  uint n_ranges,
  uint *bufsz,
  uint *flags,
  Cost_estimate *cost
)
{
  DBUG_ENTER("ha_spider::multi_range_read_info_const");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!pre_bitmap_checked)
  {
    if (wide_handler->external_lock_type == F_WRLCK)
    {
      pk_update = FALSE;
      if (
        wide_handler->update_request &&
        share->have_recovery_link &&
        (pk_update = spider_check_pk_update(table))
      ) {
        bitmap_set_all(table->read_set);
        if (is_clone)
          memset(wide_handler->searched_bitmap, 0xFF,
            no_bytes_in_map(table->read_set));
      }
    }

    if (!is_clone)
      set_select_column_mode();

    pre_bitmap_checked = TRUE;
  }
/*
  multi_range_num = n_ranges;
  mrr_have_range = FALSE;
*/
  ha_rows rows =
    handler::multi_range_read_info_const(
      keyno,
      seq,
      seq_init_param,
      n_ranges,
      bufsz,
      flags,
      cost
    );
  *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
  DBUG_PRINT("info",("spider rows=%llu", rows));
  DBUG_RETURN(rows);
}

ha_rows ha_spider::multi_range_read_info(
  uint keyno,
  uint n_ranges,
  uint keys,
  uint key_parts,
  uint *bufsz,
  uint *flags,
  Cost_estimate *cost
)
{
  DBUG_ENTER("ha_spider::multi_range_read_info");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!pre_bitmap_checked)
  {
    if (wide_handler->external_lock_type == F_WRLCK)
    {
      pk_update = FALSE;
      if (
        wide_handler->update_request &&
        share->have_recovery_link &&
        (pk_update = spider_check_pk_update(table))
      ) {
        bitmap_set_all(table->read_set);
        if (is_clone)
          memset(wide_handler->searched_bitmap, 0xFF,
            no_bytes_in_map(table->read_set));
      }
    }

    if (!is_clone)
      set_select_column_mode();

    pre_bitmap_checked = TRUE;
  }
/*
  multi_range_num = n_ranges;
  mrr_have_range = FALSE;
*/
  ha_rows rows =
    handler::multi_range_read_info(
      keyno,
      n_ranges,
      keys,
      key_parts,
      bufsz,
      flags,
      cost
    );
  *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
  DBUG_PRINT("info",("spider rows=%llu", rows));
  DBUG_RETURN(rows);
}

int ha_spider::multi_range_read_init(
  RANGE_SEQ_IF *seq,
  void *seq_init_param,
  uint n_ranges,
  uint mode,
  HANDLER_BUFFER *buf
) {
  bka_mode = spider_param_bka_mode(wide_handler->trx->thd, share->bka_mode);
  backup_error_status();
  DBUG_ENTER("ha_spider::multi_range_read_init");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider n_ranges=%u", n_ranges));
  multi_range_num = n_ranges;
  mrr_have_range = FALSE;
  reset_no_where_cond();
  DBUG_RETURN(
    handler::multi_range_read_init(
      seq,
      seq_init_param,
      n_ranges,
      mode,
      buf
    )
  );
}

int ha_spider::multi_range_read_next_first(
  range_id_t *range_info
)
{
  int error_num, roop_count;
  SPIDER_CONN *conn;
  int range_res;
  backup_error_status();
  DBUG_ENTER("ha_spider::multi_range_read_next_first");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  do_direct_update = FALSE;
  if ((error_num = index_handler_init()))
    DBUG_RETURN(check_error_mode_eof(error_num));
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }

  spider_db_free_one_result_for_start_next(this);
  check_direct_order_limit();
  if ((error_num = spider_set_conn_bg_param(this)))
    DBUG_RETURN(error_num);
  check_select_column(FALSE);
  DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
  result_list.finish_flg = FALSE;
  result_list.record_num = 0;
  if ((error_num = reset_sql_sql(
    SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
    DBUG_RETURN(error_num);
  result_list.desc_flg = FALSE;
  result_list.sorted = mrr_is_output_sorted;
  result_list.key_info = &table->key_info[active_index];
  if (
    multi_range_num == 1 ||
    result_list.multi_split_read <= 1 ||
    (sql_kinds & SPIDER_SQL_KIND_HANDLER)
  ) {
    if (wide_handler->keyread)
      result_list.keyread = TRUE;
    else
      result_list.keyread = FALSE;
    mrr_with_cnt = FALSE;
    if (
      (error_num = spider_db_append_select(this)) ||
      (error_num = spider_db_append_select_columns(this))
    )
      DBUG_RETURN(error_num);
    if (
      share->key_hint &&
      (error_num = append_hint_after_table_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL))
    )
      DBUG_RETURN(error_num);
    set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
    error_num = HA_ERR_END_OF_FILE;
    while (!(range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range)))
    {
      DBUG_PRINT("info",("spider range_res1=%d", range_res));
      result_list.limit_num =
        result_list.internal_limit - result_list.record_num >=
        result_list.split_read ?
        result_list.split_read :
        result_list.internal_limit - result_list.record_num;
      DBUG_PRINT("info",("spider limit_num=%lld", result_list.limit_num));
      if (
        (error_num = spider_db_append_key_where(
          &mrr_cur_range.start_key,
          SPIDER_TEST(mrr_cur_range.range_flag & EQ_RANGE) ?
          NULL : &mrr_cur_range.end_key, this))
      )
        DBUG_RETURN(error_num);
      if (sql_kinds & SPIDER_SQL_KIND_SQL)
      {
        if (result_list.direct_order_limit)
        {
          if ((error_num =
            append_key_order_for_direct_order_limit_with_alias_sql_part(
              NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        } else {
          if ((error_num = append_key_order_with_alias_sql_part(
            NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        }
        if ((error_num = append_limit_sql_part(
          result_list.internal_offset + result_list.record_num,
          result_list.limit_num,
          SPIDER_SQL_TYPE_SELECT_SQL)))
        {
          DBUG_RETURN(error_num);
        }
        if (
          (error_num = append_select_lock_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL))
        ) {
          DBUG_RETURN(error_num);
        }
      }
      if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
      {
        if ((error_num = append_limit_sql_part(
          result_list.internal_offset + result_list.record_num,
          result_list.limit_num,
          SPIDER_SQL_TYPE_HANDLER)))
        {
          DBUG_RETURN(error_num);
        }
      }

      int roop_start, roop_end, tmp_lock_mode, link_ok;
      tmp_lock_mode = spider_conn_lock_mode(this);
      if (tmp_lock_mode)
      {
        /* "for update" or "lock in share mode" */
        link_ok = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_OK);
        roop_start = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_end = share->link_count;
      } else {
        link_ok = search_link_idx;
        roop_start = search_link_idx;
        roop_end = search_link_idx + 1;
      }
      for (roop_count = roop_start; roop_count < roop_end;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, roop_count, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        if (result_list.bgs_phase > 0)
        {
          if ((error_num = spider_check_and_init_casual_read(
            wide_handler->trx->thd, this,
            roop_count)))
            DBUG_RETURN(error_num);
          error_num = spider_bg_conn_search(this, roop_count, roop_start,
            TRUE, FALSE, (roop_count != link_ok));
          if (
            error_num &&
            error_num != HA_ERR_END_OF_FILE &&
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
        } else {
          ulong sql_type;
            conn = conns[roop_count];
            if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
            {
              sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
            } else {
              sql_type = SPIDER_SQL_TYPE_HANDLER;
            }
          spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
          if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          if ((error_num =
            dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
          {
            if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
            {
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
            DBUG_RETURN(error_num);
          }
          if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
            conn->need_mon = &need_mons[roop_count];
            DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = TRUE;
            conn->mta_conn_mutex_unlock_later = TRUE;
            if ((error_num = spider_db_set_names(this, conn,
              roop_count)))
            {
              DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
              conn->mta_conn_mutex_lock_already = FALSE;
              conn->mta_conn_mutex_unlock_later = FALSE;
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
              if (
                share->monitoring_kind[roop_count] &&
                need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    roop_count,
                    (uint32) share->monitoring_sid[roop_count],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[roop_count],
                    NULL,
                    0,
                    share->monitoring_kind[roop_count],
                    share->monitoring_limit[roop_count],
                    share->monitoring_flag[roop_count],
                    TRUE
                  );
              }
            }
            if (!error_num)
            {
              spider_conn_set_timeout_from_share(conn, roop_count,
                wide_handler->trx->thd, share);
              if (dbton_hdl->execute_sql(
                sql_type,
                conn,
                result_list.quick_mode,
                &need_mons[roop_count])
              ) {
                DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                conn->mta_conn_mutex_lock_already = FALSE;
                conn->mta_conn_mutex_unlock_later = FALSE;
                error_num = spider_db_errorno(conn);
                if (
                  share->monitoring_kind[roop_count] &&
                  need_mons[roop_count]
                ) {
                  error_num = spider_ping_table_mon_from_table(
                      wide_handler->trx,
                      wide_handler->trx->thd,
                      share,
                      roop_count,
                      (uint32) share->monitoring_sid[roop_count],
                      share->table_name,
                      share->table_name_length,
                      conn_link_idx[roop_count],
                      NULL,
                      0,
                      share->monitoring_kind[roop_count],
                      share->monitoring_limit[roop_count],
                      share->monitoring_flag[roop_count],
                      TRUE
                    );
                }
              }
            }
            if (!error_num)
            {
              connection_ids[roop_count] = conn->connection_id;
              DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
              conn->mta_conn_mutex_lock_already = FALSE;
              conn->mta_conn_mutex_unlock_later = FALSE;
              if (roop_count == link_ok)
              {
                error_num = spider_db_store_result(this, roop_count, table);
                if (
                  error_num &&
                  error_num != HA_ERR_END_OF_FILE &&
                  share->monitoring_kind[roop_count] &&
                  need_mons[roop_count]
                ) {
                  error_num = spider_ping_table_mon_from_table(
                      wide_handler->trx,
                      wide_handler->trx->thd,
                      share,
                      roop_count,
                      (uint32) share->monitoring_sid[roop_count],
                      share->table_name,
                      share->table_name_length,
                      conn_link_idx[roop_count],
                      NULL,
                      0,
                      share->monitoring_kind[roop_count],
                      share->monitoring_limit[roop_count],
                      share->monitoring_flag[roop_count],
                      TRUE
                    );
                }
                result_link_idx = link_ok;
              } else {
                spider_db_discard_result(this, roop_count, conn);
                SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                pthread_mutex_unlock(&conn->mta_conn_mutex);
              }
            }
        }
        if (error_num)
          break;
      }
      if (error_num)
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          check_error_mode(error_num)
        )
          DBUG_RETURN(error_num);
        DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
        result_list.finish_flg = FALSE;
        result_list.record_num = 0;
        if (result_list.current)
        {
          DBUG_PRINT("info",
            ("spider result_list.current->finish_flg = FALSE"));
          result_list.current->finish_flg = FALSE;
          spider_db_free_one_result(&result_list,
            (SPIDER_RESULT *) result_list.current);
          if (result_list.current == result_list.first)
            result_list.current = NULL;
          else
            result_list.current = result_list.current->prev;
        }
      } else {
        if (!range_info)
          DBUG_RETURN(0);
        if (!(error_num = spider_db_fetch(table->record[0], this, table)))
        {
          *range_info = (char *) mrr_cur_range.ptr;
          DBUG_RETURN(check_ha_range_eof());
        }
        if (
          error_num != HA_ERR_END_OF_FILE &&
          check_error_mode(error_num)
        )
          DBUG_RETURN(error_num);
        DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
        result_list.finish_flg = FALSE;
        result_list.record_num = 0;
        if (result_list.current)
        {
          DBUG_PRINT("info",
            ("spider result_list.current->finish_flg = FALSE"));
          result_list.current->finish_flg = FALSE;
          spider_db_free_one_result(&result_list,
            (SPIDER_RESULT *) result_list.current);
          if (result_list.current == result_list.first)
            result_list.current = NULL;
          else
            result_list.current = result_list.current->prev;
        }
      }
      if (check_no_where_cond())
      {
        DBUG_RETURN(check_error_mode_eof(0));
      }
      set_where_to_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
      set_where_to_pos_sql(SPIDER_SQL_TYPE_HANDLER);
    }
    DBUG_PRINT("info",("spider range_res2=%d", range_res));
    if (error_num)
      DBUG_RETURN(check_error_mode_eof(error_num));
  } else {
    bool tmp_high_priority = wide_handler->high_priority;
    bool have_multi_range;
    const uchar *first_mrr_start_key;
    const uchar *first_mrr_end_key;
    uint first_mrr_start_key_length;
    uint first_mrr_end_key_length;
    have_second_range = FALSE;
    if (wide_handler->keyread)
      result_list.keyread = TRUE;
    else
      result_list.keyread = FALSE;
    mrr_with_cnt = TRUE;
    multi_range_cnt = 0;
    multi_range_hit_point = 0;
    if (multi_range_keys)
    {
      DBUG_PRINT("info",("spider free multi_range_keys=%p", multi_range_keys));
      spider_free(spider_current_trx, multi_range_keys, MYF(0));
    }
    if (!(multi_range_keys = (range_id_t *)
      spider_malloc(spider_current_trx, 1, sizeof(range_id_t) *
        (multi_range_num < result_list.multi_split_read ?
          multi_range_num : result_list.multi_split_read), MYF(MY_WME)))
    )
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    DBUG_PRINT("info",("spider alloc multi_range_keys=%p", multi_range_keys));
    if (!mrr_key_buff)
    {
      if (!(mrr_key_buff = new spider_string[2]))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      for (roop_count = 0; roop_count < 2; roop_count++)
        mrr_key_buff[roop_count].init_calc_mem(235);
    }
    error_num = 0;
    if ((range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range)))
    {
      DBUG_PRINT("info",("spider range_res3=%d", range_res));
      DBUG_PRINT("info",("spider result_list.finish_flg = TRUE"));
      result_list.finish_flg = TRUE;
      if (result_list.current)
      {
        DBUG_PRINT("info",("spider result_list.current->finish_flg = TRUE"));
        result_list.current->finish_flg = TRUE;
      }
      table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    DBUG_PRINT("info",("spider range_res4=%d", range_res));
    mrr_key_buff[0].length(0);
    first_mrr_start_key = mrr_cur_range.start_key.key;
    first_mrr_start_key_length = mrr_cur_range.start_key.length;
    if (first_mrr_start_key_length)
    {
      if (mrr_key_buff[0].reserve(first_mrr_start_key_length))
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      mrr_key_buff[0].q_append((const char *) first_mrr_start_key,
        first_mrr_start_key_length);
      mrr_cur_range.start_key.key = (const uchar *) mrr_key_buff[0].ptr();
    }
    mrr_key_buff[1].length(0);
    first_mrr_end_key = mrr_cur_range.end_key.key;
    first_mrr_end_key_length = mrr_cur_range.end_key.length;
    if (first_mrr_end_key_length)
    {
      if (mrr_key_buff[1].reserve(first_mrr_end_key_length))
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      mrr_key_buff[1].q_append((const char *) first_mrr_end_key,
        first_mrr_end_key_length);
      mrr_cur_range.end_key.key = (const uchar *) mrr_key_buff[1].ptr();
    }
    result_list.tmp_table_join = FALSE;
    memset(result_list.tmp_table_join_first, 0, share->link_bitmap_size);
    do
    {
      if ((range_res = mrr_funcs.next(mrr_iter, &mrr_second_range)))
      {
        have_second_range = FALSE;
        have_multi_range = FALSE;
      } else {
        have_second_range = TRUE;
        have_multi_range = TRUE;
      }
      DBUG_PRINT("info",("spider range_res5=%d", range_res));
      result_list.tmp_reuse_sql = FALSE;
      if (bka_mode &&
        have_multi_range &&
        SPIDER_TEST(mrr_cur_range.range_flag & EQ_RANGE)
      ) {
        if (
          result_list.tmp_table_join &&
          result_list.tmp_table_join_key_part_map ==
            mrr_cur_range.start_key.keypart_map
        ) {
          /* reuse tmp_sql */
          result_list.tmp_reuse_sql = TRUE;
        } else {
          /* create tmp_sql */
          result_list.tmp_table_join = TRUE;
          result_list.tmp_table_join_key_part_map =
            mrr_cur_range.start_key.keypart_map;
          if ((error_num = reset_sql_sql(
            SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_TMP_SQL)))
            DBUG_RETURN(error_num);
          if ((sql_kinds & SPIDER_SQL_KIND_SQL))
          {
            for (roop_count = 0; roop_count < (int) share->link_count;
              roop_count++)
            {
              result_list.sql_kind_backup[roop_count] = sql_kind[roop_count];
              sql_kind[roop_count] = SPIDER_SQL_KIND_SQL;
            }
            result_list.sql_kinds_backup = sql_kinds;
            sql_kinds = SPIDER_SQL_KIND_SQL;
            result_list.have_sql_kind_backup = TRUE;
          }
        }
        memset(result_list.tmp_table_join_first, 0xFF,
          share->link_bitmap_size);
      } else {
        result_list.tmp_table_join = FALSE;
        if (result_list.have_sql_kind_backup)
        {
          for (roop_count = 0; roop_count < (int) share->link_count;
            roop_count++)
          {
            sql_kind[roop_count] =
              result_list.sql_kind_backup[roop_count];
          }
          sql_kinds = result_list.sql_kinds_backup;
          result_list.have_sql_kind_backup = FALSE;
        }
      }
      result_list.tmp_table_join_break_after_get_next = FALSE;

      if (result_list.tmp_table_join)
      {
        result_list.limit_num =
          result_list.internal_limit >= result_list.split_read ?
          result_list.split_read : result_list.internal_limit;
        if (bka_mode == 2)
        {
          if (!result_list.tmp_reuse_sql)
          {
            if ((error_num = append_union_table_and_sql_for_bka(
              &mrr_cur_range.start_key
            ))) {
              DBUG_RETURN(error_num);
            }
          } else {
            if ((error_num = reuse_union_table_and_sql_for_bka()))
            {
              DBUG_RETURN(error_num);
            }
          }
        } else {
          if (!result_list.tmp_reuse_sql)
          {
            if ((error_num = append_tmp_table_and_sql_for_bka(
              &mrr_cur_range.start_key
            ))) {
              DBUG_RETURN(error_num);
            }
          } else {
            if ((error_num = reuse_tmp_table_and_sql_for_bka()))
            {
              DBUG_RETURN(error_num);
            }
          }
        }

        do
        {
          if (
            !SPIDER_TEST(mrr_cur_range.range_flag & EQ_RANGE) ||
            result_list.tmp_table_join_key_part_map !=
              mrr_cur_range.start_key.keypart_map
          ) {
            result_list.tmp_table_join_break_after_get_next = TRUE;
            break;
          }

          multi_range_keys[multi_range_cnt] = mrr_cur_range.ptr;
          if (bka_mode == 2)
          {
            if ((error_num = spider_db_append_select(this)))
              DBUG_RETURN(error_num);
            if (multi_range_cnt == 0)
            {
              if ((error_num = append_multi_range_cnt_with_name_sql_part(
                SPIDER_SQL_TYPE_SELECT_SQL, multi_range_cnt)))
                DBUG_RETURN(error_num);
              if ((error_num = append_key_column_values_with_name_sql_part(
                &mrr_cur_range.start_key,
                SPIDER_SQL_TYPE_SELECT_SQL)))
                DBUG_RETURN(error_num);
            } else {
              if ((error_num = append_multi_range_cnt_sql_part(
                SPIDER_SQL_TYPE_SELECT_SQL, multi_range_cnt, TRUE)))
                DBUG_RETURN(error_num);
              if ((error_num = append_key_column_values_sql_part(
                &mrr_cur_range.start_key,
                SPIDER_SQL_TYPE_SELECT_SQL)))
                DBUG_RETURN(error_num);
            }
            if ((error_num = append_union_table_connector_sql_part(
              SPIDER_SQL_TYPE_SELECT_SQL)))
              DBUG_RETURN(error_num);
          } else {
            if ((error_num = append_multi_range_cnt_sql_part(
              SPIDER_SQL_TYPE_TMP_SQL, multi_range_cnt, TRUE)))
              DBUG_RETURN(error_num);
            if ((error_num = append_key_column_values_sql_part(
              &mrr_cur_range.start_key,
              SPIDER_SQL_TYPE_TMP_SQL)))
              DBUG_RETURN(error_num);
            if ((error_num =
              append_values_connector_sql_part(SPIDER_SQL_TYPE_TMP_SQL)))
              DBUG_RETURN(error_num);
          }

          multi_range_cnt++;
          if (multi_range_cnt >= (uint) result_list.multi_split_read)
            break;
          if (multi_range_cnt == 1)
          {
            if (have_multi_range)
            {
              memcpy(&mrr_cur_range, &mrr_second_range,
                sizeof(KEY_MULTI_RANGE));
              have_second_range = FALSE;
              range_res = 0;
            } else {
              range_res = 1;
            }
          } else {
            range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
            DBUG_PRINT("info",("spider range_res6=%d", range_res));
          }
        }
        while (!range_res);
        if (bka_mode == 2)
        {
          if ((error_num = append_union_table_terminator_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        } else {
          if ((error_num =
            append_values_terminator_sql_part(SPIDER_SQL_TYPE_TMP_SQL)))
            DBUG_RETURN(error_num);
        }
        result_list.use_union = FALSE;

        if ((error_num = append_limit_sql_part(
          result_list.internal_offset,
          result_list.limit_num,
          SPIDER_SQL_TYPE_SELECT_SQL)))
        {
          DBUG_RETURN(error_num);
        }
        if (
          (error_num = append_select_lock_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL))
        ) {
          DBUG_RETURN(error_num);
        }
      } else {
        result_list.limit_num = result_list.internal_limit;
        result_list.split_read = result_list.internal_limit;
        if (
          (error_num = init_union_table_name_pos_sql()) ||
          (error_num = append_union_all_start_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL))
        )
          DBUG_RETURN(error_num);

        do
        {
          DBUG_PRINT("info",("spider range_res7=%d", range_res));
          multi_range_keys[multi_range_cnt] = mrr_cur_range.ptr;
          if ((error_num = spider_db_append_select(this)))
            DBUG_RETURN(error_num);
          if ((error_num = append_multi_range_cnt_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL, multi_range_cnt, TRUE)))
            DBUG_RETURN(error_num);
          if (
            (error_num = spider_db_append_select_columns(this)) ||
            (error_num = set_union_table_name_pos_sql())
          )
            DBUG_RETURN(error_num);
          wide_handler->high_priority = FALSE;
          if (
            share->key_hint &&
            (error_num = append_hint_after_table_sql_part(
              SPIDER_SQL_TYPE_SELECT_SQL))
          )
            DBUG_RETURN(error_num);
          set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
          DBUG_PRINT("info",("spider internal_offset=%lld",
            result_list.internal_offset));
          DBUG_PRINT("info",("spider limit_num=%lld", result_list.limit_num));
          if (
            (error_num = spider_db_append_key_where(
              &mrr_cur_range.start_key,
              SPIDER_TEST(mrr_cur_range.range_flag & EQ_RANGE) ?
              NULL : &mrr_cur_range.end_key, this))
          )
            DBUG_RETURN(error_num);
          if (result_list.direct_order_limit)
          {
            if ((error_num =
              append_key_order_for_direct_order_limit_with_alias_sql_part(
                NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
              DBUG_RETURN(error_num);
          } else {
            if ((error_num = append_key_order_with_alias_sql_part(
              NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
              DBUG_RETURN(error_num);
          }
          if ((error_num = append_limit_sql_part(
            0,
            result_list.internal_offset + result_list.limit_num,
            SPIDER_SQL_TYPE_SELECT_SQL)))
          {
            DBUG_RETURN(error_num);
          }
          if (
            (error_num = append_select_lock_sql_part(
              SPIDER_SQL_TYPE_SELECT_SQL))
          )
            DBUG_RETURN(error_num);
          if ((error_num = append_union_all_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
          multi_range_cnt++;
          if (multi_range_cnt >= (uint) result_list.multi_split_read)
            break;
          if (multi_range_cnt == 1)
          {
            if (have_multi_range)
            {
              memcpy(&mrr_cur_range, &mrr_second_range,
                sizeof(KEY_MULTI_RANGE));
              have_second_range = FALSE;
              range_res = 0;
            } else {
              range_res = 1;
            }
          } else {
            range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
            DBUG_PRINT("info",("spider range_res8=%d", range_res));
          }
          if (check_no_where_cond())
          {
            range_res = 1;
            break;
          }
        }
        while (!range_res);
        wide_handler->high_priority = tmp_high_priority;
        if ((error_num = append_union_all_end_sql_part(
          SPIDER_SQL_TYPE_SELECT_SQL)))
          DBUG_RETURN(error_num);
        result_list.use_union = TRUE;

        bool direct_aggregate_backup = result_list.direct_aggregate;
        result_list.direct_aggregate = FALSE;
        if (result_list.direct_order_limit)
        {
          if ((error_num =
            append_key_order_for_direct_order_limit_with_alias_sql_part(
              NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        } else {
          if ((error_num = append_key_order_with_alias_sql_part(
            NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        }
        result_list.direct_aggregate = direct_aggregate_backup;
        if ((error_num = append_limit_sql_part(
          result_list.internal_offset,
          result_list.limit_num,
          SPIDER_SQL_TYPE_SELECT_SQL)))
        {
          DBUG_RETURN(error_num);
        }
      }

      int roop_start, roop_end, roop_count, tmp_lock_mode, link_ok;
      tmp_lock_mode = spider_conn_lock_mode(this);
      if (tmp_lock_mode)
      {
        /* "for update" or "lock in share mode" */
        link_ok = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_OK);
        roop_start = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_end = share->link_count;
      } else {
        link_ok = search_link_idx;
        roop_start = search_link_idx;
        roop_end = search_link_idx + 1;
      }

      for (roop_count = roop_start; roop_count < roop_end;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, roop_count, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        if (result_list.bgs_phase > 0)
        {
          if ((error_num = spider_check_and_init_casual_read(
            wide_handler->trx->thd, this,
            roop_count)))
            DBUG_RETURN(error_num);
          if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
            TRUE, FALSE, (roop_count != link_ok))))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            break;
          }
        } else {
          ulong sql_type;
            conn = conns[roop_count];
            if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
            {
              sql_type = SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_TMP_SQL;
            } else {
              sql_type = SPIDER_SQL_TYPE_HANDLER;
            }
          spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
          if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          if ((error_num =
            dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
          {
            if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
            {
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
            DBUG_RETURN(error_num);
          }
          if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          sql_type &= ~SPIDER_SQL_TYPE_TMP_SQL;
          DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
            conn->need_mon = &need_mons[roop_count];
            DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = TRUE;
            conn->mta_conn_mutex_unlock_later = TRUE;
            if ((error_num = spider_db_set_names(this, conn,
              roop_count)))
            {
              DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
              conn->mta_conn_mutex_lock_already = FALSE;
              conn->mta_conn_mutex_unlock_later = FALSE;
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
              if (
                share->monitoring_kind[roop_count] &&
                need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    roop_count,
                    (uint32) share->monitoring_sid[roop_count],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[roop_count],
                    NULL,
                    0,
                    share->monitoring_kind[roop_count],
                    share->monitoring_limit[roop_count],
                    share->monitoring_flag[roop_count],
                    TRUE
                  );
              }
              break;
            }
            if (
              result_list.tmp_table_join && bka_mode != 2 &&
              spider_bit_is_set(result_list.tmp_table_join_first, roop_count)
            ) {
              spider_clear_bit(result_list.tmp_table_join_first, roop_count);
              spider_set_bit(result_list.tmp_table_created, roop_count);
              result_list.tmp_tables_created = TRUE;
              spider_conn_set_timeout_from_share(conn, roop_count,
                wide_handler->trx->thd, share);
              if (dbton_hdl->execute_sql(
                SPIDER_SQL_TYPE_TMP_SQL,
                conn,
                -1,
                &need_mons[roop_count])
              ) {
                DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                conn->mta_conn_mutex_lock_already = FALSE;
                conn->mta_conn_mutex_unlock_later = FALSE;
                error_num = spider_db_errorno(conn);
                if (
                  share->monitoring_kind[roop_count] &&
                  need_mons[roop_count]
                ) {
                  error_num = spider_ping_table_mon_from_table(
                      wide_handler->trx,
                      wide_handler->trx->thd,
                      share,
                      roop_count,
                      (uint32) share->monitoring_sid[roop_count],
                      share->table_name,
                      share->table_name_length,
                      conn_link_idx[roop_count],
                      NULL,
                      0,
                      share->monitoring_kind[roop_count],
                      share->monitoring_limit[roop_count],
                      share->monitoring_flag[roop_count],
                      TRUE
                    );
                }
                break;
              }
              spider_db_discard_multiple_result(this, roop_count, conn);
            }
            spider_conn_set_timeout_from_share(conn, roop_count,
              wide_handler->trx->thd, share);
            if (dbton_hdl->execute_sql(
              sql_type,
              conn,
              result_list.quick_mode,
              &need_mons[roop_count])
            ) {
              DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
              conn->mta_conn_mutex_lock_already = FALSE;
              conn->mta_conn_mutex_unlock_later = FALSE;
              error_num = spider_db_errorno(conn);
              if (
                share->monitoring_kind[roop_count] &&
                need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    roop_count,
                    (uint32) share->monitoring_sid[roop_count],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[roop_count],
                    NULL,
                    0,
                    share->monitoring_kind[roop_count],
                    share->monitoring_limit[roop_count],
                    share->monitoring_flag[roop_count],
                    TRUE
                  );
              }
              break;
            }
            connection_ids[roop_count] = conn->connection_id;
            DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = FALSE;
            conn->mta_conn_mutex_unlock_later = FALSE;
            if (roop_count == link_ok)
            {
              if ((error_num = spider_db_store_result(this, roop_count, table)))
              {
                if (
                  error_num != HA_ERR_END_OF_FILE &&
                  share->monitoring_kind[roop_count] &&
                  need_mons[roop_count]
                ) {
                  error_num = spider_ping_table_mon_from_table(
                      wide_handler->trx,
                      wide_handler->trx->thd,
                      share,
                      roop_count,
                      (uint32) share->monitoring_sid[roop_count],
                      share->table_name,
                      share->table_name_length,
                      conn_link_idx[roop_count],
                      NULL,
                      0,
                      share->monitoring_kind[roop_count],
                      share->monitoring_limit[roop_count],
                      share->monitoring_flag[roop_count],
                      TRUE
                    );
                }
                break;
              }
              result_link_idx = link_ok;
            } else {
              spider_db_discard_result(this, roop_count, conn);
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
        }
      }
      if (error_num)
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          !check_error_mode(error_num)
        )
          error_num = HA_ERR_END_OF_FILE;
        if (error_num == HA_ERR_END_OF_FILE)
        {
          if (multi_range_cnt >= (uint) result_list.multi_split_read)
          {
            range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
            DBUG_PRINT("info",("spider range_res9=%d", range_res));
          }
          if (
            range_res
          ) {
            table->status = STATUS_NOT_FOUND;
            DBUG_RETURN(error_num);
          }

          DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
          result_list.finish_flg = FALSE;
          result_list.record_num = 0;
          if (result_list.current)
          {
            DBUG_PRINT("info",
              ("spider result_list.current->finish_flg = FALSE"));
            result_list.current->finish_flg = FALSE;
            spider_db_free_one_result(&result_list,
              (SPIDER_RESULT *) result_list.current);
            if (result_list.current == result_list.first)
              result_list.current = NULL;
            else
              result_list.current = result_list.current->prev;
          }
          error_num = 0;
        } else
          DBUG_RETURN(error_num);
      } else {
        if (!range_info)
          DBUG_RETURN(0);
        if (!(error_num = spider_db_fetch(table->record[0], this, table)))
        {
          *range_info = multi_range_keys[multi_range_hit_point];
          DBUG_RETURN(0);
        }
        if (
          error_num != HA_ERR_END_OF_FILE &&
          !check_error_mode(error_num)
        )
          error_num = HA_ERR_END_OF_FILE;
        if (error_num == HA_ERR_END_OF_FILE)
        {
          if (multi_range_cnt >= (uint) result_list.multi_split_read)
          {
            range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
            DBUG_PRINT("info",("spider range_res10=%d", range_res));
          }
          if (
            range_res
          ) {
            table->status = STATUS_NOT_FOUND;
            DBUG_RETURN(error_num);
          }

          DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
          result_list.finish_flg = FALSE;
          result_list.record_num = 0;
          if (result_list.current)
          {
            DBUG_PRINT("info",
              ("spider result_list.current->finish_flg = FALSE"));
            result_list.current->finish_flg = FALSE;
            spider_db_free_one_result(&result_list,
              (SPIDER_RESULT *) result_list.current);
            if (result_list.current == result_list.first)
              result_list.current = NULL;
            else
              result_list.current = result_list.current->prev;
          }
          error_num = 0;
        } else
          DBUG_RETURN(error_num);
      }
      if (check_no_where_cond())
      {
        DBUG_RETURN(check_error_mode_eof(0));
      }
      multi_range_cnt = 0;
      if ((error_num = reset_sql_sql(
        SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
        DBUG_RETURN(error_num);
    } while (!error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::pre_multi_range_read_next(
  bool use_parallel
) {
  DBUG_ENTER("ha_spider::pre_multi_range_read_next");
  DBUG_PRINT("info",("spider this=%p", this));
  check_pre_call(use_parallel);
  if (use_pre_call)
  {
    store_error_num =
      multi_range_read_next_first(NULL);
    DBUG_RETURN(store_error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::multi_range_read_next(
  range_id_t *range_info
)
{
  int error_num;
  DBUG_ENTER("ha_spider::multi_range_read_next");
  DBUG_PRINT("info",("spider this=%p", this));
  if (use_pre_call)
  {
    if (store_error_num)
    {
      if (store_error_num == HA_ERR_END_OF_FILE)
        table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(store_error_num);
    }
    if ((error_num = spider_bg_all_conn_pre_next(this, search_link_idx)))
      DBUG_RETURN(error_num);
    use_pre_call = FALSE;
    mrr_have_range = TRUE;
    DBUG_RETURN(multi_range_read_next_next(range_info));
  }
  if (!mrr_have_range)
  {
    error_num = multi_range_read_next_first(range_info);
    mrr_have_range = TRUE;
  } else
    error_num = multi_range_read_next_next(range_info);
  DBUG_RETURN(error_num);
}

int ha_spider::multi_range_read_next_next(
  range_id_t *range_info
)
{
  int error_num, roop_count;
  SPIDER_CONN *conn;
  int range_res;
  backup_error_status();
  DBUG_ENTER("ha_spider::multi_range_read_next_next");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  if (
    multi_range_num == 1 ||
    result_list.multi_split_read <= 1 ||
    (sql_kinds & SPIDER_SQL_KIND_HANDLER)
  ) {
    if (!(error_num = spider_db_seek_next(table->record[0], this,
      search_link_idx, table)))
    {
      *range_info = (char *) mrr_cur_range.ptr;
      DBUG_RETURN(0);
    }

    range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
    DBUG_PRINT("info",("spider range_res1=%d", range_res));
    if (
      error_num != HA_ERR_END_OF_FILE &&
      !check_error_mode(error_num)
    )
      error_num = HA_ERR_END_OF_FILE;
    if (
      error_num != HA_ERR_END_OF_FILE ||
      range_res
    )
      DBUG_RETURN(error_num);
    spider_db_free_one_result_for_start_next(this);
    spider_first_split_read_param(this);
    if ((error_num = spider_set_conn_bg_param(this)))
      DBUG_RETURN(error_num);
    DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
    result_list.finish_flg = FALSE;
    if (result_list.current)
    {
      DBUG_PRINT("info",("spider result_list.current->finish_flg = FALSE"));
      result_list.current->finish_flg = FALSE;
    }
    result_list.record_num = 0;
    do
    {
      DBUG_PRINT("info",("spider range_res2=%d", range_res));
      if (check_no_where_cond())
      {
        DBUG_RETURN(check_error_mode_eof(0));
      }
      set_where_to_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
      set_where_to_pos_sql(SPIDER_SQL_TYPE_HANDLER);
      result_list.limit_num =
        result_list.internal_limit - result_list.record_num >=
        result_list.split_read ?
        result_list.split_read :
        result_list.internal_limit - result_list.record_num;
      if (
        (error_num = spider_db_append_key_where(
          &mrr_cur_range.start_key,
          SPIDER_TEST(mrr_cur_range.range_flag & EQ_RANGE) ?
          NULL : &mrr_cur_range.end_key, this))
      )
        DBUG_RETURN(error_num);
      if (sql_kinds & SPIDER_SQL_KIND_SQL)
      {
        if (result_list.direct_order_limit)
        {
          if ((error_num =
            append_key_order_for_direct_order_limit_with_alias_sql_part(
              NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        } else {
          if ((error_num = append_key_order_with_alias_sql_part(
            NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        }
        if ((error_num = append_limit_sql_part(
          result_list.internal_offset + result_list.record_num,
          result_list.limit_num,
          SPIDER_SQL_TYPE_SELECT_SQL)))
        {
          DBUG_RETURN(error_num);
        }
        if (
          (error_num = append_select_lock_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL))
        ) {
          DBUG_RETURN(error_num);
        }
      }
      if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
      {
        if ((error_num = append_limit_sql_part(
          result_list.internal_offset + result_list.record_num,
          result_list.limit_num,
          SPIDER_SQL_TYPE_HANDLER)))
        {
          DBUG_RETURN(error_num);
        }
      }

      int roop_start, roop_end, tmp_lock_mode, link_ok;
      tmp_lock_mode = spider_conn_lock_mode(this);
      if (tmp_lock_mode)
      {
        /* "for update" or "lock in share mode" */
        link_ok = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_OK);
        roop_start = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_end = share->link_count;
      } else {
        link_ok = search_link_idx;
        roop_start = search_link_idx;
        roop_end = search_link_idx + 1;
      }
      for (roop_count = roop_start; roop_count < roop_end;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, roop_count, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        if (result_list.bgs_phase > 0)
        {
          if ((error_num = spider_check_and_init_casual_read(
            wide_handler->trx->thd, this,
            roop_count)))
            DBUG_RETURN(error_num);
          error_num = spider_bg_conn_search(this, roop_count, roop_start,
            TRUE, FALSE, (roop_count != link_ok));
          if (
            error_num &&
            error_num != HA_ERR_END_OF_FILE &&
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
        } else {
          ulong sql_type;
            conn = conns[roop_count];
            if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
            {
              sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
            } else {
              sql_type = SPIDER_SQL_TYPE_HANDLER;
            }
          spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
          if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          if ((error_num =
            dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
          {
            if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
            {
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
            DBUG_RETURN(error_num);
          }
          if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
            conn->need_mon = &need_mons[roop_count];
            DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = TRUE;
            conn->mta_conn_mutex_unlock_later = TRUE;
            if ((error_num = spider_db_set_names(this, conn,
              roop_count)))
            {
              DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
              conn->mta_conn_mutex_lock_already = FALSE;
              conn->mta_conn_mutex_unlock_later = FALSE;
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
              if (
                share->monitoring_kind[roop_count] &&
                need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    roop_count,
                    (uint32) share->monitoring_sid[roop_count],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[roop_count],
                    NULL,
                    0,
                    share->monitoring_kind[roop_count],
                    share->monitoring_limit[roop_count],
                    share->monitoring_flag[roop_count],
                    TRUE
                  );
              }
            }
            if (!error_num)
            {
              spider_conn_set_timeout_from_share(conn, roop_count,
                wide_handler->trx->thd, share);
              if (dbton_hdl->execute_sql(
                sql_type,
                conn,
                result_list.quick_mode,
                &need_mons[roop_count])
              ) {
                DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                conn->mta_conn_mutex_lock_already = FALSE;
                conn->mta_conn_mutex_unlock_later = FALSE;
                error_num = spider_db_errorno(conn);
                if (
                  share->monitoring_kind[roop_count] &&
                  need_mons[roop_count]
                ) {
                  error_num = spider_ping_table_mon_from_table(
                      wide_handler->trx,
                      wide_handler->trx->thd,
                      share,
                      roop_count,
                      (uint32) share->monitoring_sid[roop_count],
                      share->table_name,
                      share->table_name_length,
                      conn_link_idx[roop_count],
                      NULL,
                      0,
                      share->monitoring_kind[roop_count],
                      share->monitoring_limit[roop_count],
                      share->monitoring_flag[roop_count],
                      TRUE
                    );
                }
              }
            }
            if (!error_num)
            {
              connection_ids[roop_count] = conn->connection_id;
              DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
              conn->mta_conn_mutex_lock_already = FALSE;
              conn->mta_conn_mutex_unlock_later = FALSE;
              if (roop_count == link_ok)
              {
                error_num = spider_db_store_result(this, roop_count, table);
                if (
                  error_num &&
                  error_num != HA_ERR_END_OF_FILE &&
                  share->monitoring_kind[roop_count] &&
                  need_mons[roop_count]
                ) {
                  error_num = spider_ping_table_mon_from_table(
                      wide_handler->trx,
                      wide_handler->trx->thd,
                      share,
                      roop_count,
                      (uint32) share->monitoring_sid[roop_count],
                      share->table_name,
                      share->table_name_length,
                      conn_link_idx[roop_count],
                      NULL,
                      0,
                      share->monitoring_kind[roop_count],
                      share->monitoring_limit[roop_count],
                      share->monitoring_flag[roop_count],
                      TRUE
                    );
                }
                result_link_idx = link_ok;
              } else {
                spider_db_discard_result(this, roop_count, conn);
                SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                pthread_mutex_unlock(&conn->mta_conn_mutex);
              }
            }
        }
        if (error_num)
          break;
      }
      if (error_num)
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          !check_error_mode(error_num)
        )
          error_num = HA_ERR_END_OF_FILE;
        if (error_num == HA_ERR_END_OF_FILE)
        {
          DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
          result_list.finish_flg = FALSE;
          result_list.record_num = 0;
          if (result_list.current)
          {
            DBUG_PRINT("info",
              ("spider result_list.current->finish_flg = FALSE"));
            result_list.current->finish_flg = FALSE;
            spider_db_free_one_result(&result_list,
              (SPIDER_RESULT *) result_list.current);
            result_list.current = result_list.current->prev;
          }
        } else
          DBUG_RETURN(error_num);
      } else {
        if (!(error_num = spider_db_fetch(table->record[0], this, table)))
        {
          *range_info = (char *) mrr_cur_range.ptr;
          DBUG_RETURN(check_ha_range_eof());
        }
        if (
          error_num != HA_ERR_END_OF_FILE &&
          !check_error_mode(error_num)
        )
          error_num = HA_ERR_END_OF_FILE;
        if (error_num == HA_ERR_END_OF_FILE)
        {
          DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
          result_list.finish_flg = FALSE;
          result_list.record_num = 0;
          if (result_list.current)
          {
            DBUG_PRINT("info",
              ("spider result_list.current->finish_flg = FALSE"));
            result_list.current->finish_flg = FALSE;
            spider_db_free_one_result(&result_list,
              (SPIDER_RESULT *) result_list.current);
            result_list.current = result_list.current->prev;
          }
        } else
          DBUG_RETURN(error_num);
      }
      range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
      DBUG_PRINT("info",("spider range_res3=%d", range_res));
    }
    while (!range_res);
    if (error_num)
      DBUG_RETURN(check_error_mode_eof(error_num));
  } else {
    if (!(error_num = spider_db_seek_next(table->record[0], this,
      search_link_idx, table)))
    {
      *range_info = multi_range_keys[multi_range_hit_point];
      DBUG_RETURN(0);
    }

    const uchar *first_mrr_start_key;
    const uchar *first_mrr_end_key;
    uint first_mrr_start_key_length;
    uint first_mrr_end_key_length;
    if (!result_list.tmp_table_join_break_after_get_next)
    {
      range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
      DBUG_PRINT("info",("spider range_res4=%d", range_res));
      if (!range_res)
      {
        mrr_key_buff[0].length(0);
        first_mrr_start_key = mrr_cur_range.start_key.key;
        first_mrr_start_key_length = mrr_cur_range.start_key.length;
        if (first_mrr_start_key_length)
        {
          if (mrr_key_buff[0].reserve(first_mrr_start_key_length))
            DBUG_RETURN(HA_ERR_END_OF_FILE);
          mrr_key_buff[0].q_append((const char *) first_mrr_start_key,
            first_mrr_start_key_length);
          mrr_cur_range.start_key.key = (const uchar *) mrr_key_buff[0].ptr();
        }
        mrr_key_buff[1].length(0);
        first_mrr_end_key = mrr_cur_range.end_key.key;
        first_mrr_end_key_length = mrr_cur_range.end_key.length;
        if (first_mrr_end_key_length)
        {
          if (mrr_key_buff[1].reserve(first_mrr_end_key_length))
            DBUG_RETURN(HA_ERR_END_OF_FILE);
          mrr_key_buff[1].q_append((const char *) first_mrr_end_key,
            first_mrr_end_key_length);
          mrr_cur_range.end_key.key = (const uchar *) mrr_key_buff[1].ptr();
        }
      }
    } else {
      result_list.tmp_table_join_break_after_get_next = FALSE;
      range_res = 0;
    }

    if (
      error_num != HA_ERR_END_OF_FILE &&
      !check_error_mode(error_num)
    )
      error_num = HA_ERR_END_OF_FILE;
    if (
      error_num != HA_ERR_END_OF_FILE ||
      range_res
    )
      DBUG_RETURN(error_num);
    if (check_no_where_cond())
    {
      DBUG_RETURN(check_error_mode_eof(0));
    }
    spider_db_free_one_result_for_start_next(this);
    spider_first_split_read_param(this);
    if ((error_num = spider_set_conn_bg_param(this)))
      DBUG_RETURN(error_num);
    DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
    result_list.finish_flg = FALSE;
    if (result_list.current)
    {
      DBUG_PRINT("info",("spider result_list.current->finish_flg = FALSE"));
      result_list.current->finish_flg = FALSE;
    }
    result_list.record_num = 0;

    if ((error_num = reset_sql_sql(
      SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
      DBUG_RETURN(error_num);

    bool tmp_high_priority = wide_handler->high_priority;
    bool have_multi_range;
    multi_range_cnt = 0;
    error_num = 0;
    do
    {
      if (
        !have_second_range &&
        (range_res = mrr_funcs.next(mrr_iter, &mrr_second_range))
      )
      {
        have_second_range = FALSE;
        have_multi_range = FALSE;
      } else {
        have_second_range = TRUE;
        have_multi_range = TRUE;
      }
      DBUG_PRINT("info",("spider range_res5=%d", range_res));
      result_list.tmp_reuse_sql = FALSE;
      if (bka_mode &&
        have_multi_range &&
        SPIDER_TEST(mrr_cur_range.range_flag & EQ_RANGE)
      ) {
        if (
          result_list.tmp_table_join &&
          result_list.tmp_table_join_key_part_map ==
            mrr_cur_range.start_key.keypart_map
        ) {
          /* reuse tmp_sql */
          result_list.tmp_reuse_sql = TRUE;
        } else {
          /* create tmp_sql */
          result_list.tmp_table_join = TRUE;
          result_list.tmp_table_join_key_part_map =
            mrr_cur_range.start_key.keypart_map;
          if ((error_num = reset_sql_sql(
            SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_TMP_SQL)))
            DBUG_RETURN(error_num);
          if ((sql_kinds & SPIDER_SQL_KIND_SQL))
          {
            for (roop_count = 0; roop_count < (int) share->link_count;
              roop_count++)
            {
              result_list.sql_kind_backup[roop_count] = sql_kind[roop_count];
              sql_kind[roop_count] = SPIDER_SQL_KIND_SQL;
            }
            result_list.sql_kinds_backup = sql_kinds;
            sql_kinds = SPIDER_SQL_KIND_SQL;
            result_list.have_sql_kind_backup = TRUE;
          }
        }
        memset(result_list.tmp_table_join_first, 0xFF,
          share->link_bitmap_size);
      } else {
        result_list.tmp_table_join = FALSE;
        if (result_list.have_sql_kind_backup)
        {
          for (roop_count = 0; roop_count < (int) share->link_count;
            roop_count++)
          {
            sql_kind[roop_count] =
              result_list.sql_kind_backup[roop_count];
          }
          sql_kinds = result_list.sql_kinds_backup;
          result_list.have_sql_kind_backup = FALSE;
        }
      }

      if (result_list.tmp_table_join)
      {
        result_list.limit_num =
          result_list.internal_limit - result_list.record_num >=
          result_list.split_read ?
          result_list.split_read :
          result_list.internal_limit - result_list.record_num;
        if (bka_mode == 2)
        {
          if (!result_list.tmp_reuse_sql)
          {
            if ((error_num = append_union_table_and_sql_for_bka(
              &mrr_cur_range.start_key
            ))) {
              DBUG_RETURN(error_num);
            }
          } else {
            if ((error_num = reuse_union_table_and_sql_for_bka()))
            {
              DBUG_RETURN(error_num);
            }
          }
        } else {
          if (!result_list.tmp_reuse_sql)
          {
            if ((error_num = append_tmp_table_and_sql_for_bka(
              &mrr_cur_range.start_key
            ))) {
              DBUG_RETURN(error_num);
            }
          } else {
            if ((error_num = reuse_tmp_table_and_sql_for_bka()))
            {
              DBUG_RETURN(error_num);
            }
          }
        }

        do
        {
          if (
            !SPIDER_TEST(mrr_cur_range.range_flag & EQ_RANGE) ||
            result_list.tmp_table_join_key_part_map !=
              mrr_cur_range.start_key.keypart_map
          ) {
            result_list.tmp_table_join_break_after_get_next = TRUE;
            break;
          }

          multi_range_keys[multi_range_cnt] = mrr_cur_range.ptr;
          if (bka_mode == 2)
          {
            if ((error_num = spider_db_append_select(this)))
              DBUG_RETURN(error_num);
            if (multi_range_cnt == 0)
            {
              if ((error_num = append_multi_range_cnt_with_name_sql_part(
                SPIDER_SQL_TYPE_SELECT_SQL, multi_range_cnt)))
                DBUG_RETURN(error_num);
              if ((error_num = append_key_column_values_with_name_sql_part(
                &mrr_cur_range.start_key,
                SPIDER_SQL_TYPE_SELECT_SQL)))
                DBUG_RETURN(error_num);
            } else {
              if ((error_num = append_multi_range_cnt_sql_part(
                SPIDER_SQL_TYPE_SELECT_SQL, multi_range_cnt, TRUE)))
                DBUG_RETURN(error_num);
              if ((error_num = append_key_column_values_sql_part(
                &mrr_cur_range.start_key,
                SPIDER_SQL_TYPE_SELECT_SQL)))
                DBUG_RETURN(error_num);
            }
            if ((error_num = append_union_table_connector_sql_part(
              SPIDER_SQL_TYPE_SELECT_SQL)))
              DBUG_RETURN(error_num);
          } else {
            if ((error_num = append_multi_range_cnt_sql_part(
              SPIDER_SQL_TYPE_TMP_SQL, multi_range_cnt, TRUE)))
              DBUG_RETURN(error_num);
            if ((error_num = append_key_column_values_sql_part(
              &mrr_cur_range.start_key,
              SPIDER_SQL_TYPE_TMP_SQL)))
              DBUG_RETURN(error_num);

            if ((error_num =
              append_values_connector_sql_part(SPIDER_SQL_TYPE_TMP_SQL)))
              DBUG_RETURN(error_num);
          }
          multi_range_cnt++;
          if (multi_range_cnt >= (uint) result_list.multi_split_read)
            break;
          if (multi_range_cnt == 1)
          {
            if (have_multi_range)
            {
              memcpy(&mrr_cur_range, &mrr_second_range,
                sizeof(KEY_MULTI_RANGE));
              have_second_range = FALSE;
              range_res = 0;
            } else {
              range_res = 1;
            }
          } else {
            range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
            DBUG_PRINT("info",("spider range_res6=%d", range_res));
          }
        }
        while (!range_res);
        if (bka_mode == 2)
        {
          if ((error_num = append_union_table_terminator_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        } else {
          if ((error_num =
            append_values_terminator_sql_part(SPIDER_SQL_TYPE_TMP_SQL)))
            DBUG_RETURN(error_num);
        }
        result_list.use_union = FALSE;

        if ((error_num = append_limit_sql_part(
          result_list.internal_offset,
          result_list.limit_num,
          SPIDER_SQL_TYPE_SELECT_SQL)))
        {
          DBUG_RETURN(error_num);
        }
        if (
          (error_num = append_select_lock_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL))
        ) {
          DBUG_RETURN(error_num);
        }
      } else {
        result_list.limit_num =
          result_list.internal_limit - result_list.record_num;
        if (
          (error_num = init_union_table_name_pos_sql()) ||
          (error_num =
            append_union_all_start_sql_part(SPIDER_SQL_TYPE_SELECT_SQL))
        )
          DBUG_RETURN(error_num);
        do
        {
          multi_range_keys[multi_range_cnt] = mrr_cur_range.ptr;
          if ((error_num = spider_db_append_select(this)))
            DBUG_RETURN(error_num);
          if ((error_num = append_multi_range_cnt_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL, multi_range_cnt, TRUE)))
            DBUG_RETURN(error_num);
          if (
            (error_num = spider_db_append_select_columns(this)) ||
            (error_num = set_union_table_name_pos_sql())
          )
            DBUG_RETURN(error_num);
          wide_handler->high_priority = FALSE;
          if (
            share->key_hint &&
            (error_num = append_hint_after_table_sql_part(
              SPIDER_SQL_TYPE_SELECT_SQL))
          )
            DBUG_RETURN(error_num);
          set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
          if (
            (error_num = spider_db_append_key_where(
              &mrr_cur_range.start_key,
              SPIDER_TEST(mrr_cur_range.range_flag & EQ_RANGE) ?
              NULL : &mrr_cur_range.end_key, this))
          )
            DBUG_RETURN(error_num);
          if (result_list.direct_order_limit)
          {
            if ((error_num =
              append_key_order_for_direct_order_limit_with_alias_sql_part(
                NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
              DBUG_RETURN(error_num);
          } else {
            if ((error_num = append_key_order_with_alias_sql_part(
              NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
              DBUG_RETURN(error_num);
          }
          if ((error_num = append_limit_sql_part(
            0,
            result_list.internal_offset + result_list.limit_num,
            SPIDER_SQL_TYPE_SELECT_SQL)))
          {
            DBUG_RETURN(error_num);
          }
          if ((error_num = append_select_lock_sql_part(
            SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
          if ((error_num =
            append_union_all_sql_part(SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
          multi_range_cnt++;
          if (multi_range_cnt >= (uint) result_list.multi_split_read)
            break;
          if (multi_range_cnt == 1)
          {
            if (have_multi_range)
            {
              memcpy(&mrr_cur_range, &mrr_second_range,
                sizeof(KEY_MULTI_RANGE));
              have_second_range = FALSE;
              range_res = 0;
            } else {
              range_res = 1;
            }
          } else {
            range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
            DBUG_PRINT("info",("spider range_res7=%d", range_res));
          }
        }
        while (!range_res);
        wide_handler->high_priority = tmp_high_priority;
        if ((error_num =
          append_union_all_end_sql_part(SPIDER_SQL_TYPE_SELECT_SQL)))
          DBUG_RETURN(error_num);
        result_list.use_union = TRUE;

        bool direct_aggregate_backup = result_list.direct_aggregate;
        result_list.direct_aggregate = FALSE;
        if (result_list.direct_order_limit)
        {
          if ((error_num =
            append_key_order_for_direct_order_limit_with_alias_sql_part(
              NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        } else {
          if ((error_num = append_key_order_with_alias_sql_part(
            NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        }
        result_list.direct_aggregate = direct_aggregate_backup;
        if ((error_num = append_limit_sql_part(
          result_list.internal_offset,
          result_list.limit_num,
          SPIDER_SQL_TYPE_SELECT_SQL)))
        {
          DBUG_RETURN(error_num);
        }
      }

      int roop_start, roop_end, roop_count, tmp_lock_mode, link_ok;
      tmp_lock_mode = spider_conn_lock_mode(this);
      if (tmp_lock_mode)
      {
        /* "for update" or "lock in share mode" */
        link_ok = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_OK);
        roop_start = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_end = share->link_count;
      } else {
        link_ok = search_link_idx;
        roop_start = search_link_idx;
        roop_end = search_link_idx + 1;
      }
      for (roop_count = roop_start; roop_count < roop_end;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          conn_link_idx, roop_count, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        if (result_list.bgs_phase > 0)
        {
          if ((error_num = spider_check_and_init_casual_read(
            wide_handler->trx->thd, this,
            roop_count)))
            DBUG_RETURN(error_num);
          if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
            TRUE, FALSE, (roop_count != link_ok))))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            break;
          }
        } else {
          ulong sql_type;
            conn = conns[roop_count];
            if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
            {
              sql_type = SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_TMP_SQL;
            } else {
              sql_type = SPIDER_SQL_TYPE_HANDLER;
            }
          spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
          if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          if ((error_num =
            dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
          {
            if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
            {
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
            DBUG_RETURN(error_num);
          }
          if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          sql_type &= ~SPIDER_SQL_TYPE_TMP_SQL;
          DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
            conn->need_mon = &need_mons[roop_count];
            DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = TRUE;
            conn->mta_conn_mutex_unlock_later = TRUE;
            if ((error_num = spider_db_set_names(this, conn,
              roop_count)))
            {
              DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
              conn->mta_conn_mutex_lock_already = FALSE;
              conn->mta_conn_mutex_unlock_later = FALSE;
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
              if (
                share->monitoring_kind[roop_count] &&
                need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    roop_count,
                    (uint32) share->monitoring_sid[roop_count],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[roop_count],
                    NULL,
                    0,
                    share->monitoring_kind[roop_count],
                    share->monitoring_limit[roop_count],
                    share->monitoring_flag[roop_count],
                    TRUE
                  );
              }
              break;
            }
            if (
              result_list.tmp_table_join && bka_mode != 2 &&
              spider_bit_is_set(result_list.tmp_table_join_first, roop_count)
            ) {
              spider_clear_bit(result_list.tmp_table_join_first, roop_count);
              spider_set_bit(result_list.tmp_table_created, roop_count);
              result_list.tmp_tables_created = TRUE;
              spider_conn_set_timeout_from_share(conn, roop_count,
                wide_handler->trx->thd, share);
              if (dbton_hdl->execute_sql(
                SPIDER_SQL_TYPE_TMP_SQL,
                conn,
                -1,
                &need_mons[roop_count])
              ) {
                DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                conn->mta_conn_mutex_lock_already = FALSE;
                conn->mta_conn_mutex_unlock_later = FALSE;
                error_num = spider_db_errorno(conn);
                if (
                  share->monitoring_kind[roop_count] &&
                  need_mons[roop_count]
                ) {
                  error_num = spider_ping_table_mon_from_table(
                      wide_handler->trx,
                      wide_handler->trx->thd,
                      share,
                      roop_count,
                      (uint32) share->monitoring_sid[roop_count],
                      share->table_name,
                      share->table_name_length,
                      conn_link_idx[roop_count],
                      NULL,
                      0,
                      share->monitoring_kind[roop_count],
                      share->monitoring_limit[roop_count],
                      share->monitoring_flag[roop_count],
                      TRUE
                    );
                }
                break;
              }
              spider_db_discard_multiple_result(this, roop_count, conn);
            }
            spider_conn_set_timeout_from_share(conn, roop_count,
              wide_handler->trx->thd, share);
            if (dbton_hdl->execute_sql(
              sql_type,
              conn,
              result_list.quick_mode,
              &need_mons[roop_count])
            ) {
              DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
              conn->mta_conn_mutex_lock_already = FALSE;
              conn->mta_conn_mutex_unlock_later = FALSE;
              error_num = spider_db_errorno(conn);
              if (
                share->monitoring_kind[roop_count] &&
                need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    roop_count,
                    (uint32) share->monitoring_sid[roop_count],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[roop_count],
                    NULL,
                    0,
                    share->monitoring_kind[roop_count],
                    share->monitoring_limit[roop_count],
                    share->monitoring_flag[roop_count],
                    TRUE
                  );
              }
              break;
            }
            connection_ids[roop_count] = conn->connection_id;
            DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = FALSE;
            conn->mta_conn_mutex_unlock_later = FALSE;
            if (roop_count == link_ok)
            {
              if ((error_num = spider_db_store_result(this, roop_count, table)))
              {
                if (
                  error_num != HA_ERR_END_OF_FILE &&
                  share->monitoring_kind[roop_count] &&
                  need_mons[roop_count]
                ) {
                  error_num = spider_ping_table_mon_from_table(
                      wide_handler->trx,
                      wide_handler->trx->thd,
                      share,
                      roop_count,
                      (uint32) share->monitoring_sid[roop_count],
                      share->table_name,
                      share->table_name_length,
                      conn_link_idx[roop_count],
                      NULL,
                      0,
                      share->monitoring_kind[roop_count],
                      share->monitoring_limit[roop_count],
                      share->monitoring_flag[roop_count],
                      TRUE
                    );
                }
                break;
              }
              result_link_idx = link_ok;
            } else {
              spider_db_discard_result(this, roop_count, conn);
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
        }
      }
      if (error_num)
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          !check_error_mode(error_num)
        )
          error_num = HA_ERR_END_OF_FILE;
        if (error_num == HA_ERR_END_OF_FILE)
        {
          if (multi_range_cnt >= (uint) result_list.multi_split_read)
          {
            range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
            DBUG_PRINT("info",("spider range_res8=%d", range_res));
          }
          if (
            range_res
          ) {
            table->status = STATUS_NOT_FOUND;
            DBUG_RETURN(error_num);
          }

          DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
          result_list.finish_flg = FALSE;
          result_list.record_num = 0;
          if (result_list.current)
          {
            DBUG_PRINT("info",
              ("spider result_list.current->finish_flg = FALSE"));
            result_list.current->finish_flg = FALSE;
            spider_db_free_one_result(&result_list,
              (SPIDER_RESULT *) result_list.current);
            if (result_list.current == result_list.first)
              result_list.current = NULL;
            else
              result_list.current = result_list.current->prev;
          }
          error_num = 0;
        } else
          DBUG_RETURN(error_num);
      } else {
        if (!(error_num = spider_db_fetch(table->record[0], this, table)))
        {
          *range_info = multi_range_keys[multi_range_hit_point];
          DBUG_RETURN(0);
        }
        if (
          error_num != HA_ERR_END_OF_FILE &&
          !check_error_mode(error_num)
        )
          error_num = HA_ERR_END_OF_FILE;
        if (error_num == HA_ERR_END_OF_FILE)
        {
          if (multi_range_cnt >= (uint) result_list.multi_split_read)
          {
            range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range);
            DBUG_PRINT("info",("spider range_res9=%d", range_res));
          }
          if (
            range_res
          ) {
            table->status = STATUS_NOT_FOUND;
            DBUG_RETURN(error_num);
          }

          DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
          result_list.finish_flg = FALSE;
          result_list.record_num = 0;
          if (result_list.current)
          {
            DBUG_PRINT("info",
              ("spider result_list.current->finish_flg = FALSE"));
            result_list.current->finish_flg = FALSE;
            spider_db_free_one_result(&result_list,
              (SPIDER_RESULT *) result_list.current);
            if (result_list.current == result_list.first)
              result_list.current = NULL;
            else
              result_list.current = result_list.current->prev;
          }
          error_num = 0;
        } else
          DBUG_RETURN(error_num);
      }
      if (check_no_where_cond())
      {
        DBUG_RETURN(check_error_mode_eof(0));
      }
      multi_range_cnt = 0;
      if ((error_num = reset_sql_sql(
        SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
        DBUG_RETURN(error_num);
    } while (!error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::rnd_init(
  bool scan
) {
  int error_num;
  DBUG_ENTER("ha_spider::rnd_init");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider scan=%s", scan ? "TRUE" : "FALSE"));
  if (!dml_inited)
  {
    if (unlikely((error_num = dml_init())))
    {
      DBUG_RETURN(error_num);
    }
  }
  pushed_pos = NULL;
/*
  if (wide_handler->external_lock_type == F_WRLCK)
    check_and_start_bulk_update(SPD_BU_START_BY_INDEX_OR_RND_INIT);
*/
  rnd_scan_and_first = scan;
  if (
    scan &&
    wide_handler->sql_command != SQLCOM_ALTER_TABLE
  ) {
    spider_set_result_list_param(this);
    pk_update = FALSE;
    if (
      result_list.current &&
      !result_list.low_mem_read &&
      prev_index_rnd_init == SPD_RND
    ) {
      result_list.current = result_list.first;
      spider_db_set_pos_to_first_row(&result_list);
      rnd_scan_and_first = FALSE;
    } else {
      spider_db_free_one_result_for_start_next(this);
      if (
        result_list.current &&
        result_list.low_mem_read
      ) {
        int roop_start, roop_end, roop_count, tmp_lock_mode;
        tmp_lock_mode = spider_conn_lock_mode(this);
        if (tmp_lock_mode)
        {
          /* "for update" or "lock in share mode" */
          roop_start = spider_conn_link_idx_next(share->link_statuses,
            conn_link_idx, -1, share->link_count,
            SPIDER_LINK_STATUS_RECOVERY);
          roop_end = share->link_count;
        } else {
          roop_start = search_link_idx;
          roop_end = search_link_idx + 1;
        }
        for (roop_count = roop_start; roop_count < roop_end;
          roop_count = spider_conn_link_idx_next(share->link_statuses,
            conn_link_idx, roop_count, share->link_count,
            SPIDER_LINK_STATUS_RECOVERY)
        ) {
          if (conns[roop_count] && result_list.bgs_working)
            spider_bg_conn_break(conns[roop_count], this);
          if (quick_targets[roop_count])
          {
            spider_db_free_one_quick_result(
              (SPIDER_RESULT *) result_list.current);
            DBUG_ASSERT(quick_targets[roop_count] ==
              conns[roop_count]->quick_target);
            DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL",
              conns[roop_count]));
            conns[roop_count]->quick_target = NULL;
            quick_targets[roop_count] = NULL;
          }
        }
        result_list.record_num = 0;
        DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
        result_list.finish_flg = FALSE;
        result_list.quick_phase = 0;
        result_list.bgs_phase = 0;
      }

      mrr_with_cnt = FALSE;
      use_spatial_index = FALSE;

      if (
        wide_handler->update_request &&
        share->have_recovery_link &&
        wide_handler->external_lock_type == F_WRLCK &&
        (pk_update = spider_check_pk_update(table))
      ) {
        bitmap_set_all(table->read_set);
        if (is_clone)
          memset(wide_handler->searched_bitmap, 0xFF,
            no_bytes_in_map(table->read_set));
      }

      set_select_column_mode();
      result_list.keyread = FALSE;

      init_rnd_handler = FALSE;
      if ((error_num = reset_sql_sql(
        SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER)))
        DBUG_RETURN(error_num);
      result_list.check_direct_order_limit = FALSE;
    }
  }
  prev_index_rnd_init = SPD_RND;
  DBUG_RETURN(0);
}


int ha_spider::rnd_end()
{
/*
  int error_num;
  backup_error_status();
*/
  DBUG_ENTER("ha_spider::rnd_end");
  DBUG_PRINT("info",("spider this=%p", this));
/*
#ifdef INFO_KIND_FORCE_LIMIT_BEGIN
  info_limit = 9223372036854775807LL;
#endif
  if (
    (error_num = check_and_end_bulk_update(
      SPD_BU_START_BY_INDEX_OR_RND_INIT)) ||
    (error_num = spider_trx_check_link_idx_failed(this))
  )
    DBUG_RETURN(check_error_mode(error_num));
*/
  DBUG_RETURN(0);
}


int ha_spider::rnd_next_internal(
  uchar *buf
) {
  int error_num;
  ha_spider *direct_limit_offset_spider =
    (ha_spider *) partition_handler->owner;
  backup_error_status();
  DBUG_ENTER("ha_spider::rnd_next_internal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  /* do not copy table data at alter table */
  if (wide_handler->sql_command == SQLCOM_ALTER_TABLE)
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  do_direct_update = FALSE;

  if (rnd_scan_and_first)
  {
    if ((error_num = spider_set_conn_bg_param(this)))
      DBUG_RETURN(error_num);
    if ((error_num = rnd_handler_init()))
      DBUG_RETURN(check_error_mode_eof(error_num));
    check_direct_order_limit();
    check_select_column(TRUE);

    if (this->result_list.direct_limit_offset)
    {
      if (direct_limit_offset_spider->direct_select_limit == 0)
      { // mean has got all result
        DBUG_RETURN(check_error_mode_eof(HA_ERR_END_OF_FILE));
      }
      if (
        partition_handler->handlers &&
        direct_limit_offset_spider->direct_current_offset > 0
      ) {
        longlong table_count = this->records();
        DBUG_PRINT("info",("spider table_count=%lld", table_count));
        if (table_count <= direct_limit_offset_spider->direct_current_offset)
        {
          // skip this spider(partition)
          direct_limit_offset_spider->direct_current_offset -= table_count;
          DBUG_PRINT("info",("spider direct_current_offset=%lld",
            direct_limit_offset_spider->direct_current_offset));
          DBUG_RETURN(check_error_mode_eof(HA_ERR_END_OF_FILE));
        }
      }

      // make the offset/limit statement
      DBUG_PRINT("info",("spider direct_current_offset=%lld",
        direct_limit_offset_spider->direct_current_offset));
      result_list.internal_offset = direct_limit_offset_spider->direct_current_offset;
      DBUG_PRINT("info",("spider direct_select_limit=%lld",
        direct_limit_offset_spider->direct_select_limit));
      result_list.internal_limit = direct_limit_offset_spider->direct_select_limit;
      result_list.split_read = direct_limit_offset_spider->direct_select_limit;

      // start with this spider(partition)
      direct_limit_offset_spider->direct_current_offset = 0;
    }

    DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
    result_list.finish_flg = FALSE;
    result_list.record_num = 0;
    if (
      (error_num = spider_db_append_select(this)) ||
      (error_num = spider_db_append_select_columns(this))
    )
      DBUG_RETURN(error_num);
    set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);

    /* append condition pushdown */
    if (spider_db_append_condition(this, NULL, 0, FALSE))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

    set_order_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
    if (result_list.direct_order_limit)
    {
      if ((error_num =
        append_key_order_for_direct_order_limit_with_alias_sql_part(
          NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    else if (result_list.direct_aggregate)
    {
      if ((error_num =
        append_group_by_sql_part(NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    result_list.desc_flg = FALSE;
    result_list.sorted = FALSE;
    result_list.key_info = NULL;
    result_list.limit_num =
      result_list.internal_limit >= result_list.split_read ?
      result_list.split_read : result_list.internal_limit;
    if (sql_kinds & SPIDER_SQL_KIND_SQL)
    {
      if ((error_num = append_limit_sql_part(
        result_list.internal_offset,
        result_list.limit_num,
        SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
      if (
        (error_num = append_select_lock_sql_part(
          SPIDER_SQL_TYPE_SELECT_SQL))
      ) {
        DBUG_RETURN(error_num);
      }
    }
    if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      if ((error_num = append_limit_sql_part(
        result_list.internal_offset,
        result_list.limit_num,
        SPIDER_SQL_TYPE_HANDLER)))
      {
        DBUG_RETURN(error_num);
      }
    }

    int roop_start, roop_end, roop_count, tmp_lock_mode, link_ok;
    tmp_lock_mode = spider_conn_lock_mode(this);
    if (tmp_lock_mode)
    {
      /* "for update" or "lock in share mode" */
      link_ok = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_OK);
      roop_start = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_end = share->link_count;
    } else {
      link_ok = search_link_idx;
      roop_start = search_link_idx;
      roop_end = search_link_idx + 1;
    }
    for (roop_count = roop_start; roop_count < roop_end;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (result_list.bgs_phase > 0)
      {
        if ((error_num = spider_check_and_init_casual_read(
          wide_handler->trx->thd, this,
          roop_count)))
          DBUG_RETURN(error_num);
        if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
          TRUE, FALSE, (roop_count != link_ok))))
        {
          if (
            error_num != HA_ERR_END_OF_FILE &&
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
      } else {
        SPIDER_CONN *conn = conns[roop_count];
        ulong sql_type;
        if (sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
        {
          sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
        } else {
          sql_type = SPIDER_SQL_TYPE_HANDLER;
        }
        spider_db_handler *dbton_hdl = dbton_handler[conn->dbton_id];
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if ((error_num =
          dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
        {
          if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
          {
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
          DBUG_RETURN(error_num);
        }
        if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
        conn->need_mon = &need_mons[roop_count];
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        if ((error_num = spider_db_set_names(this, conn,
          roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        spider_conn_set_timeout_from_share(conn, roop_count,
          wide_handler->trx->thd, share);
        if (dbton_hdl->execute_sql(
          sql_type,
          conn,
          result_list.quick_mode,
          &need_mons[roop_count])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          error_num = spider_db_errorno(conn);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        connection_ids[roop_count] = conn->connection_id;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        if (roop_count == link_ok)
        {
          if ((error_num = spider_db_store_result(this, roop_count, table)))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          result_link_idx = link_ok;
        } else {
          spider_db_discard_result(this, roop_count, conn);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
      }
    }
    rnd_scan_and_first = FALSE;

    if (this->result_list.direct_limit_offset)
    {
      if (buf && (error_num = spider_db_seek_next(buf, this, search_link_idx,
        table)))
        DBUG_RETURN(check_error_mode_eof(error_num));
      DBUG_RETURN(0);
    }
  }

  if (
    result_list.direct_limit_offset &&
    direct_limit_offset_spider->direct_select_offset > 0
  ) {
    // limit-- for each got row
    direct_limit_offset_spider->direct_select_offset--;
    DBUG_RETURN(0);
  }

  if (buf && (error_num = spider_db_seek_next(buf, this, search_link_idx,
    table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(0);
}

int ha_spider::pre_rnd_next(
  bool use_parallel
) {
  DBUG_ENTER("ha_spider::pre_rnd_next");
  DBUG_PRINT("info",("spider this=%p", this));
  check_pre_call(use_parallel);
  if (use_pre_call)
  {
    store_error_num =
      rnd_next_internal(NULL);
    DBUG_RETURN(store_error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::rnd_next(
  uchar *buf
) {
  int error_num;
  DBUG_ENTER("ha_spider::rnd_next");
  DBUG_PRINT("info",("spider this=%p", this));
  if (use_pre_call)
  {
    if (store_error_num)
    {
      if (store_error_num == HA_ERR_END_OF_FILE)
        table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(store_error_num);
    }
    if ((error_num = spider_bg_all_conn_pre_next(this, search_link_idx)))
      DBUG_RETURN(error_num);
    use_pre_call = FALSE;
  }
  DBUG_RETURN(rnd_next_internal(buf));
}

void ha_spider::position(
  const uchar *record
) {
  DBUG_ENTER("ha_spider::position");
  DBUG_PRINT("info",("spider this=%p", this));
  if (pushed_pos)
  {
    DBUG_PRINT("info",("spider pushed_pos=%p", pushed_pos));
    memcpy(ref, pushed_pos, ref_length);
    DBUG_VOID_RETURN;
  }
  if (pt_clone_last_searcher)
  {
    /* sercher is cloned handler */
    DBUG_PRINT("info",("spider cloned handler access"));
    pt_clone_last_searcher->position(record);
    memcpy(ref, pt_clone_last_searcher->ref, ref_length);
  } else {
    if (is_clone)
    {
      DBUG_PRINT("info",("spider set pt_clone_last_searcher (NULL) to %p",
        pt_clone_source_handler));
      pt_clone_source_handler->pt_clone_last_searcher = NULL;
    }
    memset(ref, '0', sizeof(SPIDER_POSITION));
    DBUG_PRINT("info",("spider self position"));
    DBUG_PRINT("info",
      ("spider current_row_num=%lld", result_list.current_row_num));
    if (!wide_handler->position_bitmap_init)
    {
      if (select_column_mode)
      {
        spider_db_handler *dbton_hdl =
          dbton_handler[result_list.current->dbton_id];
        dbton_hdl->copy_minimum_select_bitmap(wide_handler->position_bitmap);
      }
      wide_handler->position_bitmap_init = TRUE;
    }
    spider_db_create_position(this, (SPIDER_POSITION *) ref);
  }
  DBUG_VOID_RETURN;
}

int ha_spider::rnd_pos(
  uchar *buf,
  uchar *pos
) {
  DBUG_ENTER("ha_spider::rnd_pos");
  DBUG_PRINT("info",("spider this=%p", this));
#ifndef DBUG_OFF
  for (uint roop_count = 0; roop_count < ((table->s->fields + 7) / 8);
    roop_count++)
  {
    DBUG_PRINT("info",("spider roop_count=%d", roop_count));
    DBUG_PRINT("info",("spider read_set=%d",
      ((uchar *) table->read_set->bitmap)[roop_count]));
  }
#endif
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  DBUG_PRINT("info",("spider pos=%p", pos));
  DBUG_PRINT("info",("spider buf=%p", buf));
  pushed_pos_buf = *((SPIDER_POSITION *) pos);
  pushed_pos = &pushed_pos_buf;
  DBUG_RETURN(spider_db_seek_tmp(buf, &pushed_pos_buf, this, table));
}

int ha_spider::cmp_ref(
  const uchar *ref1,
  const uchar *ref2
) {
  int ret = 0;
  DBUG_ENTER("ha_spider::cmp_ref");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider ref1=%p", ref1));
  DBUG_PRINT("info",("spider ref2=%p", ref2));
  result_list.in_cmp_ref = TRUE;
  if (table_share->primary_key < MAX_KEY)
  {
    uchar table_key[MAX_KEY_LENGTH];
    KEY *key_info = &table->key_info[table_share->primary_key];
    DBUG_PRINT("info",("spider cmp by primary key"));
    rnd_pos(table->record[0], (uchar *) ref2);
    key_copy(
      table_key,
      table->record[0],
      key_info,
      key_info->key_length);
    rnd_pos(table->record[0], (uchar *) ref1);
    ret = key_cmp(key_info->key_part, table_key, key_info->key_length);
  } else {
    Field **field;
    my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(table->record[1], table->record[0]);
    DBUG_PRINT("info",("spider cmp by all rows"));
    rnd_pos(table->record[1], (uchar *) ref2);
    rnd_pos(table->record[0], (uchar *) ref1);
    for (
      field = table->field;
      *field;
      field++
    ) {
      if ((ret = (*field)->cmp_binary_offset((uint) ptr_diff)))
      {
        DBUG_PRINT("info",("spider different at %s",
          SPIDER_field_name_str(*field)));
        break;
      }
    }
  }
  result_list.in_cmp_ref = FALSE;
  DBUG_PRINT("info",("spider ret=%d", ret));
  DBUG_RETURN(ret);
}

float spider_ft_find_relevance(
  FT_INFO *handler,
  uchar *record,
  uint length
) {
  DBUG_ENTER("spider_ft_find_relevance");
  st_spider_ft_info *info = (st_spider_ft_info*) handler;
  DBUG_PRINT("info",("spider info=%p", info));
  DBUG_PRINT("info",("spider score=%f", info->score));
  DBUG_RETURN(info->score);
}

float spider_ft_get_relevance(
  FT_INFO *handler
) {
  DBUG_ENTER("spider_ft_get_relevance");
  st_spider_ft_info *info = (st_spider_ft_info*) handler;
  DBUG_PRINT("info",("spider info=%p", info));
  DBUG_PRINT("info",("spider score=%f", info->score));
  DBUG_RETURN(info->score);
}

void spider_ft_close_search(
  FT_INFO *handler
) {
  DBUG_ENTER("spider_ft_close_search");
  DBUG_VOID_RETURN;
}

_ft_vft spider_ft_vft = {
  NULL, // spider_ft_read_next
  spider_ft_find_relevance,
  spider_ft_close_search,
  spider_ft_get_relevance,
  NULL // spider_ft_reinit_search
};

int ha_spider::ft_init()
{
  int roop_count, error_num;
  DBUG_ENTER("ha_spider::ft_init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (store_error_num)
    DBUG_RETURN(store_error_num);
  if (active_index == MAX_KEY && inited == NONE)
  {
    st_spider_ft_info  *ft_info = ft_first;
    ft_init_without_index_init = TRUE;
    ft_init_idx = MAX_KEY;
    while (TRUE)
    {
      if (ft_info->used_in_where)
      {
        ft_init_idx = ft_info->inx;
        if ((error_num = index_init(ft_init_idx, FALSE)))
          DBUG_RETURN(error_num);
        active_index = MAX_KEY;
        break;
      }
      if (ft_info == ft_current)
        break;
      ft_info = ft_info->next;
    }
    if (ft_init_idx == MAX_KEY)
    {
      if ((error_num = rnd_init(TRUE)))
        DBUG_RETURN(error_num);
    }
  } else {
    ft_init_idx = active_index;
    ft_init_without_index_init = FALSE;
  }

  ft_init_and_first = TRUE;

  for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
    sql_kind[roop_count] = SPIDER_SQL_KIND_SQL;
  sql_kinds = SPIDER_SQL_KIND_SQL;
  DBUG_RETURN(0);
}

void ha_spider::ft_end()
{
  DBUG_ENTER("ha_spider::ft_end");
  DBUG_PRINT("info",("spider this=%p", this));
  if (ft_init_without_index_init)
  {
    if (ft_init_idx == MAX_KEY)
      store_error_num = rnd_end();
    else
      store_error_num = index_end();
  }
  ft_init_without_index_init = FALSE;
  handler::ft_end();
  DBUG_VOID_RETURN;
}

FT_INFO *ha_spider::ft_init_ext(
  uint flags,
  uint inx,
  String *key
) {
  st_spider_ft_info *tmp_ft_info;
  backup_error_status();
  DBUG_ENTER("ha_spider::ft_init_ext");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider flags=%u", flags));
  DBUG_PRINT("info",("spider inx=%u", inx));
  DBUG_PRINT("info",("spider key=%s", key->c_ptr_safe()));
  if (inx == NO_SUCH_KEY)
  {
    my_error(ER_FT_MATCHING_KEY_NOT_FOUND, MYF(0));
    DBUG_RETURN(NULL);
  }

  tmp_ft_info = ft_current;
  if (ft_current)
    ft_current = ft_current->next;
  else {
    ft_current = ft_first;
    set_ft_discard_bitmap();
  }

  if (!ft_current)
  {
    if (!(ft_current = (st_spider_ft_info *)
      spider_malloc(spider_current_trx, 2, sizeof(st_spider_ft_info),
        MYF(MY_WME | MY_ZEROFILL))))
    {
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      DBUG_RETURN(NULL);
    }
    if (tmp_ft_info)
      tmp_ft_info->next = ft_current;
    else
      ft_first = ft_current;
  }

  ft_current->please = &spider_ft_vft;
  ft_current->file = this;
  ft_current->used_in_where = (flags & FT_SORTED);
  ft_current->target = ft_count;
  ft_current->flags = flags;
  ft_current->inx = inx;
  ft_current->key = key;

  ft_count++;
  DBUG_RETURN((FT_INFO *) ft_current);
}

int ha_spider::ft_read_internal(
  uchar *buf
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::ft_read_internal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  if (ft_init_and_first)
  {
    ft_init_and_first = FALSE;
    spider_db_free_one_result_for_start_next(this);
    check_direct_order_limit();
    if ((error_num = spider_set_conn_bg_param(this)))
      DBUG_RETURN(error_num);
    check_select_column(FALSE);
    DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
    result_list.finish_flg = FALSE;
    result_list.record_num = 0;
    if (wide_handler->keyread)
      result_list.keyread = TRUE;
    else
      result_list.keyread = FALSE;
    if (
      (error_num = spider_db_append_select(this)) ||
      (error_num = spider_db_append_select_columns(this))
    )
      DBUG_RETURN(error_num);
    uint tmp_active_index = active_index;
    active_index = ft_init_idx;
    if (
      ft_init_idx < MAX_KEY &&
      share->key_hint &&
      (error_num =
        append_hint_after_table_sql_part(SPIDER_SQL_TYPE_SELECT_SQL))
    ) {
      active_index = tmp_active_index;
      DBUG_RETURN(error_num);
    }
    active_index = tmp_active_index;
    set_where_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
    result_list.desc_flg = FALSE;
    result_list.sorted = TRUE;
    if (ft_init_idx == MAX_KEY)
      result_list.key_info = NULL;
    else
      result_list.key_info = &table->key_info[ft_init_idx];
    result_list.key_order = 0;
    result_list.limit_num =
      result_list.internal_limit >= result_list.split_read ?
      result_list.split_read : result_list.internal_limit;
    if ((error_num = spider_db_append_match_where(this)))
      DBUG_RETURN(error_num);
    if (result_list.direct_order_limit)
    {
      if ((error_num =
        append_key_order_for_direct_order_limit_with_alias_sql_part(NULL, 0,
          SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    else if (result_list.direct_aggregate)
    {
      if ((error_num =
        append_group_by_sql_part(NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    if (sql_kinds & SPIDER_SQL_KIND_SQL)
    {
      if ((error_num = append_limit_sql_part(
        result_list.internal_offset,
        result_list.limit_num,
        SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
      if (
        (error_num = append_select_lock_sql_part(
          SPIDER_SQL_TYPE_SELECT_SQL))
      ) {
        DBUG_RETURN(error_num);
      }
    }
    if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      if ((error_num = append_limit_sql_part(
        result_list.internal_offset,
        result_list.limit_num,
        SPIDER_SQL_TYPE_HANDLER)))
      {
        DBUG_RETURN(error_num);
      }
    }

    int roop_start, roop_end, roop_count, tmp_lock_mode, link_ok;
    tmp_lock_mode = spider_conn_lock_mode(this);
    if (tmp_lock_mode)
    {
      /* "for update" or "lock in share mode" */
      link_ok = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_OK);
      roop_start = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_end = share->link_count;
    } else {
      link_ok = search_link_idx;
      roop_start = search_link_idx;
      roop_end = search_link_idx + 1;
    }
    for (roop_count = roop_start; roop_count < roop_end;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (result_list.bgs_phase > 0)
      {
        if ((error_num = spider_check_and_init_casual_read(
          wide_handler->trx->thd, this,
          roop_count)))
          DBUG_RETURN(error_num);
        if ((error_num = spider_bg_conn_search(this, roop_count, roop_start,
          TRUE, FALSE, (roop_count != link_ok))))
        {
          if (
            error_num != HA_ERR_END_OF_FILE &&
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
      } else {
        uint dbton_id = share->use_sql_dbton_ids[roop_count];
        spider_db_handler *dbton_hdl = dbton_handler[dbton_id];
        SPIDER_CONN *conn = conns[roop_count];
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        if (dbton_hdl->need_lock_before_set_sql_for_exec(
          SPIDER_SQL_TYPE_SELECT_SQL))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if ((error_num = dbton_hdl->set_sql_for_exec(
          SPIDER_SQL_TYPE_SELECT_SQL, roop_count)))
        {
          if (dbton_hdl->need_lock_before_set_sql_for_exec(
            SPIDER_SQL_TYPE_SELECT_SQL))
          {
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
          DBUG_RETURN(error_num);
        }
        if (!dbton_hdl->need_lock_before_set_sql_for_exec(
          SPIDER_SQL_TYPE_SELECT_SQL))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        conn->need_mon = &need_mons[roop_count];
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        if ((error_num = spider_db_set_names(this, conn, roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        spider_conn_set_timeout_from_share(conn, roop_count,
          wide_handler->trx->thd, share);
        if (dbton_hdl->execute_sql(
          SPIDER_SQL_TYPE_SELECT_SQL,
          conn,
          result_list.quick_mode,
          &need_mons[roop_count])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          error_num = spider_db_errorno(conn);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode_eof(error_num));
        }
        connection_ids[roop_count] = conn->connection_id;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        if (roop_count == link_ok)
        {
          if ((error_num = spider_db_store_result(this, roop_count, table)))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode_eof(error_num));
          }
          result_link_idx = link_ok;
        } else {
          spider_db_discard_result(this, roop_count, conn);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
      }
    }
  }

  if (is_clone)
  {
    DBUG_PRINT("info",("spider set pt_clone_last_searcher to %p",
      pt_clone_source_handler));
    pt_clone_source_handler->pt_clone_last_searcher = this;
  }
  if (buf && (error_num = spider_db_seek_next(buf, this, search_link_idx,
    table)))
    DBUG_RETURN(check_error_mode_eof(error_num));
  DBUG_RETURN(0);
}

int ha_spider::pre_ft_read(
  bool use_parallel
) {
  DBUG_ENTER("ha_spider::pre_ft_read");
  DBUG_PRINT("info",("spider this=%p", this));
  check_pre_call(use_parallel);
  if (use_pre_call)
  {
    store_error_num =
      ft_read_internal(NULL);
    DBUG_RETURN(store_error_num);
  }
  DBUG_RETURN(0);
}

int ha_spider::ft_read(
  uchar *buf
) {
  int error_num;
  DBUG_ENTER("ha_spider::ft_read");
  DBUG_PRINT("info",("spider this=%p", this));
  if (use_pre_call)
  {
    if (store_error_num)
    {
      if (store_error_num == HA_ERR_END_OF_FILE)
        table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(store_error_num);
    }
    if ((error_num = spider_bg_all_conn_pre_next(this, search_link_idx)))
      DBUG_RETURN(error_num);
    use_pre_call = FALSE;
  }
  DBUG_RETURN(ft_read_internal(buf));
}

int ha_spider::info(
  uint flag
) {
  int error_num;
  THD *thd = ha_thd();
  double sts_interval = spider_param_sts_interval(thd, share->sts_interval);
  int sts_mode = spider_param_sts_mode(thd, share->sts_mode);
  int sts_sync = spider_param_sts_sync(thd, share->sts_sync);
  int sts_bg_mode = spider_param_sts_bg_mode(thd, share->sts_bg_mode);
  SPIDER_INIT_ERROR_TABLE *spider_init_error_table = NULL;
  set_error_mode();
  backup_error_status();
  DBUG_ENTER("ha_spider::info");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider flag=%x", flag));
#ifdef HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
  auto_inc_temporary = FALSE;
#endif
  wide_handler->sql_command = thd_sql_command(thd);
/*
  if (
    sql_command == SQLCOM_DROP_TABLE ||
    sql_command == SQLCOM_ALTER_TABLE ||
    sql_command == SQLCOM_SHOW_CREATE
  ) {
*/
    if (flag & HA_STATUS_AUTO)
    {
      if (share->lgtm_tblhnd_share->auto_increment_value)
        stats.auto_increment_value =
          share->lgtm_tblhnd_share->auto_increment_value;
      else {
        stats.auto_increment_value = 1;
#ifdef HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
        auto_inc_temporary = TRUE;
#endif
      }
    }
    if (
      wide_handler->sql_command == SQLCOM_DROP_TABLE ||
      wide_handler->sql_command == SQLCOM_ALTER_TABLE
    )
      DBUG_RETURN(0);
/*
  }
*/

  if (flag &
    (HA_STATUS_TIME | HA_STATUS_CONST | HA_STATUS_VARIABLE | HA_STATUS_AUTO))
  {
    time_t tmp_time = (time_t) time((time_t*) 0);
    DBUG_PRINT("info",
      ("spider difftime=%f", difftime(tmp_time, share->sts_get_time)));
    DBUG_PRINT("info",
      ("spider sts_interval=%f", sts_interval));
    int tmp_auto_increment_mode = 0;
    if (flag & HA_STATUS_AUTO)
    {
      tmp_auto_increment_mode = spider_param_auto_increment_mode(thd,
        share->auto_increment_mode);
      info_auto_called = TRUE;
    }
    if (!share->sts_init)
    {
      pthread_mutex_lock(&share->sts_mutex);
      if (share->sts_init)
        pthread_mutex_unlock(&share->sts_mutex);
      else {
        if ((spider_init_error_table =
          spider_get_init_error_table(wide_handler->trx, share, FALSE)))
        {
          DBUG_PRINT("info",("spider diff=%f",
            difftime(tmp_time, spider_init_error_table->init_error_time)));
          if (difftime(tmp_time,
            spider_init_error_table->init_error_time) <
            spider_param_table_init_error_interval())
          {
            pthread_mutex_unlock(&share->sts_mutex);
            if (wide_handler->sql_command == SQLCOM_SHOW_CREATE ||
                wide_handler->sql_command == SQLCOM_SHOW_FIELDS)
            {
              if (thd->is_error())
              {
                DBUG_PRINT("info", ("spider clear_error"));
                thd->clear_error();
              }
              DBUG_RETURN(0);
            }
            if (spider_init_error_table->init_error_with_message)
              my_message(spider_init_error_table->init_error,
                spider_init_error_table->init_error_msg, MYF(0));
            DBUG_RETURN(check_error_mode(spider_init_error_table->init_error));
          }
        }
        pthread_mutex_unlock(&share->sts_mutex);
        sts_interval = 0;
        if (tmp_auto_increment_mode == 1)
          sts_sync = 0;
      }
    }
    if (flag & HA_STATUS_AUTO)
    {
      if (
        share->wide_share &&
        tmp_auto_increment_mode == 1 &&
        !share->lgtm_tblhnd_share->auto_increment_init
      ) {
        sts_interval = 0;
        sts_sync = 0;
      }
    }
    if (difftime(tmp_time, share->sts_get_time) >= sts_interval)
    {
      if (
        sts_interval == 0 ||
        !pthread_mutex_trylock(&share->sts_mutex)
      ) {
        if (sts_interval == 0 || sts_bg_mode == 0)
        {
          if (sts_interval == 0)
            pthread_mutex_lock(&share->sts_mutex);
          if (difftime(tmp_time, share->sts_get_time) >= sts_interval)
          {
            if ((error_num = spider_check_trx_and_get_conn(ha_thd(), this,
              FALSE)))
            {
              pthread_mutex_unlock(&share->sts_mutex);
              if (!share->sts_init)
              {
                if (
                  spider_init_error_table ||
                  (spider_init_error_table =
                    spider_get_init_error_table(wide_handler->trx,
                      share, TRUE))
                ) {
                  spider_init_error_table->init_error = error_num;
                  if ((spider_init_error_table->init_error_with_message =
                    thd->is_error()))
                    strmov(spider_init_error_table->init_error_msg,
                      spider_stmt_da_message(thd));
                  spider_init_error_table->init_error_time =
                    (time_t) time((time_t*) 0);
                }
                share->init_error = TRUE;
                share->init = TRUE;
              }
              if (wide_handler->sql_command == SQLCOM_SHOW_CREATE ||
                  wide_handler->sql_command == SQLCOM_SHOW_FIELDS)
              {
                if (thd->is_error())
                {
                  DBUG_PRINT("info", ("spider clear_error"));
                  thd->clear_error();
                }
                DBUG_RETURN(0);
              }
              DBUG_RETURN(check_error_mode(error_num));
            }
            if ((error_num = spider_get_sts(share, search_link_idx, tmp_time,
                this, sts_interval, sts_mode,
                sts_sync,
                share->sts_init ? 2 : 1,
                flag | (share->sts_init ? 0 : HA_STATUS_AUTO)))
            ) {
              pthread_mutex_unlock(&share->sts_mutex);
              if (
                share->monitoring_kind[search_link_idx] &&
                need_mons[search_link_idx]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    search_link_idx,
                    (uint32) share->monitoring_sid[search_link_idx],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[search_link_idx],
                    NULL,
                    0,
                    share->monitoring_kind[search_link_idx],
                    share->monitoring_limit[search_link_idx],
                    share->monitoring_flag[search_link_idx],
                    TRUE
                  );
              }
              if (!share->sts_init)
              {
                if (
                  spider_init_error_table ||
                  (spider_init_error_table =
                    spider_get_init_error_table(wide_handler->trx,
                      share, TRUE))
                ) {
                  spider_init_error_table->init_error = error_num;
/*
                  if (!thd->is_error())
                    my_error(error_num, MYF(0), "");
*/
                  if ((spider_init_error_table->init_error_with_message =
                    thd->is_error()))
                    strmov(spider_init_error_table->init_error_msg,
                      spider_stmt_da_message(thd));
                  spider_init_error_table->init_error_time =
                    (time_t) time((time_t*) 0);
                }
                share->init_error = TRUE;
                share->init = TRUE;
              }
              if (wide_handler->sql_command == SQLCOM_SHOW_CREATE ||
                  wide_handler->sql_command == SQLCOM_SHOW_FIELDS)
              {
                if (thd->is_error())
                {
                  DBUG_PRINT("info", ("spider clear_error"));
                  thd->clear_error();
                }
                DBUG_RETURN(0);
              }
              DBUG_RETURN(check_error_mode(error_num));
            }
          }
        } else if (sts_bg_mode == 1) {
          /* background */
          if (!share->bg_sts_init || share->bg_sts_thd_wait)
          {
            share->bg_sts_thd_wait = FALSE;
            share->bg_sts_try_time = tmp_time;
            share->bg_sts_interval = sts_interval;
            share->bg_sts_mode = sts_mode;
            share->bg_sts_sync = sts_sync;
            if (!share->bg_sts_init)
            {
              if ((error_num = spider_create_sts_thread(share)))
              {
                pthread_mutex_unlock(&share->sts_mutex);
                if (wide_handler->sql_command == SQLCOM_SHOW_CREATE ||
                    wide_handler->sql_command == SQLCOM_SHOW_FIELDS)
                {
                  if (thd->is_error())
                  {
                    DBUG_PRINT("info", ("spider clear_error"));
                    thd->clear_error();
                  }
                  DBUG_RETURN(0);
                }
                DBUG_RETURN(error_num);
              }
            } else
              pthread_cond_signal(&share->bg_sts_cond);
          }
        } else {
          share->bg_sts_try_time = tmp_time;
          share->bg_sts_interval = sts_interval;
          share->bg_sts_mode = sts_mode;
          share->bg_sts_sync = sts_sync;
          spider_table_add_share_to_sts_thread(share);
        }
        pthread_mutex_unlock(&share->sts_mutex);
      }
    }
    if (flag & HA_STATUS_CONST)
    {
      if ((error_num = check_crd()))
      {
        if (wide_handler->sql_command == SQLCOM_SHOW_CREATE ||
            wide_handler->sql_command == SQLCOM_SHOW_FIELDS)
        {
          if (thd->is_error())
          {
            DBUG_PRINT("info", ("spider clear_error"));
            thd->clear_error();
          }
          DBUG_RETURN(0);
        }
        DBUG_RETURN(error_num);
      }
      spider_db_set_cardinarity(this, table);
    }

    if (flag & HA_STATUS_TIME)
      stats.update_time = (ulong) share->stat.update_time;
    if (flag & (HA_STATUS_CONST | HA_STATUS_VARIABLE))
    {
      stats.max_data_file_length = share->stat.max_data_file_length;
      stats.create_time = share->stat.create_time;
      stats.block_size = spider_param_block_size(thd);
    }
    if (flag & HA_STATUS_VARIABLE)
    {
      stats.data_file_length = share->stat.data_file_length;
      stats.index_file_length = share->stat.index_file_length;
      stats.records = share->stat.records;
      stats.mean_rec_length = share->stat.mean_rec_length;
      stats.check_time = share->stat.check_time;
      if (stats.records <= 1 /* && (flag & HA_STATUS_NO_LOCK) */ )
        stats.records = 2;
      stats.checksum = share->stat.checksum;
      stats.checksum_null = share->stat.checksum_null;
    }
    if (flag & HA_STATUS_AUTO)
    {
#ifdef HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
      auto_inc_temporary = FALSE;
#endif
      if (share->wide_share && table->next_number_field)
      {
        ulonglong first_value, nb_reserved_values;
        if (
          tmp_auto_increment_mode == 0 &&
          !(
            table->next_number_field->val_int() != 0 ||
            (table->auto_increment_field_not_null &&
              thd->variables.sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO)
          )
        ) {
          get_auto_increment(0, 0, 0, &first_value, &nb_reserved_values);
          share->lgtm_tblhnd_share->auto_increment_value = first_value;
          share->lgtm_tblhnd_share->auto_increment_lclval = first_value;
          share->lgtm_tblhnd_share->auto_increment_init = TRUE;
          DBUG_PRINT("info",("spider init auto_increment_lclval=%llu",
            share->lgtm_tblhnd_share->auto_increment_lclval));
          DBUG_PRINT("info",("spider auto_increment_value=%llu",
            share->lgtm_tblhnd_share->auto_increment_value));
          stats.auto_increment_value = first_value;
        } else if (tmp_auto_increment_mode == 1 &&
          !share->lgtm_tblhnd_share->auto_increment_init)
        {
          DBUG_PRINT("info",("spider auto_increment_value=%llu",
            share->lgtm_tblhnd_share->auto_increment_value));
          share->lgtm_tblhnd_share->auto_increment_lclval =
            share->lgtm_tblhnd_share->auto_increment_value;
          share->lgtm_tblhnd_share->auto_increment_init = TRUE;
          stats.auto_increment_value =
            share->lgtm_tblhnd_share->auto_increment_value;
        } else {
          DBUG_PRINT("info",("spider auto_increment_value=%llu",
            share->lgtm_tblhnd_share->auto_increment_value));
          stats.auto_increment_value =
            share->lgtm_tblhnd_share->auto_increment_value;
        }
      } else {
        stats.auto_increment_value =
          share->lgtm_tblhnd_share->auto_increment_value;
      }
    }
  }
  if (flag & HA_STATUS_ERRKEY)
    errkey = dup_key_idx;
  DBUG_RETURN(0);
}

ha_rows ha_spider::records_in_range(
  uint inx,
  const key_range *start_key,
  const key_range *end_key,
  page_range *pages)
{
  int error_num;
  THD *thd = ha_thd();
  double crd_interval = spider_param_crd_interval(thd, share->crd_interval);
  int crd_mode = spider_param_crd_mode(thd, share->crd_mode);
  int crd_type = spider_param_crd_type(thd, share->crd_type);
  int crd_sync = spider_param_crd_sync(thd, share->crd_sync);
  int crd_bg_mode = spider_param_crd_bg_mode(thd, share->crd_bg_mode);
  SPIDER_INIT_ERROR_TABLE *spider_init_error_table = NULL;
  uint dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::records_in_range");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider inx=%u", inx));
  time_t tmp_time = (time_t) time((time_t*) 0);
  if (!share->crd_init)
  {
    pthread_mutex_lock(&share->crd_mutex);
    if (share->crd_init)
      pthread_mutex_unlock(&share->crd_mutex);
    else {
      if ((spider_init_error_table =
        spider_get_init_error_table(wide_handler->trx, share, FALSE)))
      {
        DBUG_PRINT("info",("spider diff=%f",
          difftime(tmp_time, spider_init_error_table->init_error_time)));
        if (difftime(tmp_time,
          spider_init_error_table->init_error_time) <
          spider_param_table_init_error_interval())
        {
          pthread_mutex_unlock(&share->crd_mutex);
          if (spider_init_error_table->init_error_with_message)
            my_message(spider_init_error_table->init_error,
              spider_init_error_table->init_error_msg, MYF(0));
          if (check_error_mode(spider_init_error_table->init_error))
            my_errno = spider_init_error_table->init_error;
          DBUG_RETURN(HA_POS_ERROR);
        }
      }
      pthread_mutex_unlock(&share->crd_mutex);
      if (crd_mode == 3)
        crd_mode = 1;
      crd_interval = 0;
    }
  }
  dbton_id = share->sql_dbton_ids[search_link_idx];
  dbton_hdl = dbton_handler[dbton_id];
  crd_mode = dbton_hdl->crd_mode_exchange(crd_mode);
  if (crd_mode == 1 || crd_mode == 2)
  {
    DBUG_PRINT("info", ("spider static_key_cardinality[%u]=%lld", inx,
      share->static_key_cardinality[inx]));
    DBUG_PRINT("info",
      ("spider difftime=%f", difftime(tmp_time, share->crd_get_time)));
    DBUG_PRINT("info",
      ("spider crd_interval=%f", crd_interval));
    if (
      share->static_key_cardinality[inx] == -1 &&
      difftime(tmp_time, share->crd_get_time) >= crd_interval
    ) {
      if (!dml_inited)
      {
        if (unlikely((error_num = dml_init())))
        {
          if (check_error_mode(error_num))
            my_errno = error_num;
          DBUG_RETURN(HA_POS_ERROR);
        }
      }
      if (
        crd_interval == 0 ||
        !pthread_mutex_trylock(&share->crd_mutex)
      ) {
        if (crd_interval == 0 || crd_bg_mode == 0)
        {
          if (crd_interval == 0)
            pthread_mutex_lock(&share->crd_mutex);
          if (difftime(tmp_time, share->crd_get_time) >= crd_interval)
          {
            if ((error_num = spider_get_crd(share, search_link_idx, tmp_time,
              this, table, crd_interval, crd_mode,
              crd_sync,
              share->crd_init ? 2 : 1)))
            {
              pthread_mutex_unlock(&share->crd_mutex);
              if (
                share->monitoring_kind[search_link_idx] &&
                need_mons[search_link_idx]
              ) {
                error_num = spider_ping_table_mon_from_table(
                    wide_handler->trx,
                    wide_handler->trx->thd,
                    share,
                    search_link_idx,
                    (uint32) share->monitoring_sid[search_link_idx],
                    share->table_name,
                    share->table_name_length,
                    conn_link_idx[search_link_idx],
                    NULL,
                    0,
                    share->monitoring_kind[search_link_idx],
                    share->monitoring_limit[search_link_idx],
                    share->monitoring_flag[search_link_idx],
                    TRUE
                  );
              }
              if (!share->crd_init)
              {
                if (
                  spider_init_error_table ||
                  (spider_init_error_table =
                    spider_get_init_error_table(wide_handler->trx,
                      share, TRUE))
                ) {
                  spider_init_error_table->init_error = error_num;
/*
                  if (!thd->is_error())
                    my_error(error_num, MYF(0), "");
*/
                  if ((spider_init_error_table->init_error_with_message =
                    thd->is_error()))
                    strmov(spider_init_error_table->init_error_msg,
                      spider_stmt_da_message(thd));
                  spider_init_error_table->init_error_time =
                    (time_t) time((time_t*) 0);
                }
                share->init_error = TRUE;
                share->init = TRUE;
              }
              if (check_error_mode(error_num))
                my_errno = error_num;
              DBUG_RETURN(HA_POS_ERROR);
            }
          }
        } else if (crd_bg_mode == 1) {
          /* background */
          if (!share->bg_crd_init || share->bg_crd_thd_wait)
          {
            share->bg_crd_thd_wait = FALSE;
            share->bg_crd_try_time = tmp_time;
            share->bg_crd_interval = crd_interval;
            share->bg_crd_mode = crd_mode;
            share->bg_crd_sync = crd_sync;
            if (!share->bg_crd_init)
            {
              if ((error_num = spider_create_crd_thread(share)))
              {
                pthread_mutex_unlock(&share->crd_mutex);
                my_errno = error_num;
                DBUG_RETURN(HA_POS_ERROR);
              }
            } else
              pthread_cond_signal(&share->bg_crd_cond);
          }
        } else {
          share->bg_crd_try_time = tmp_time;
          share->bg_crd_interval = crd_interval;
          share->bg_crd_mode = crd_mode;
          share->bg_crd_sync = crd_sync;
          spider_table_add_share_to_crd_thread(share);
        }
        pthread_mutex_unlock(&share->crd_mutex);
      }
    }

    KEY *key_info = &table->key_info[inx];
    key_part_map full_key_part_map =
      make_prev_keypart_map(spider_user_defined_key_parts(key_info));
    key_part_map start_key_part_map;
    key_part_map end_key_part_map;
    key_part_map tgt_key_part_map;
    KEY_PART_INFO *key_part;
    Field *field = NULL;
    double rows = (double) share->stat.records;
    double weight, rate;
    DBUG_PRINT("info",("spider rows1=%f", rows));
    if (start_key)
      start_key_part_map = start_key->keypart_map & full_key_part_map;
    else
      start_key_part_map = 0;
    if (end_key)
      end_key_part_map = end_key->keypart_map & full_key_part_map;
    else
      end_key_part_map = 0;

    if (!start_key_part_map && !end_key_part_map)
    {
      DBUG_RETURN(HA_POS_ERROR);
    }
    else if (start_key_part_map >= end_key_part_map)
    {
      tgt_key_part_map = start_key_part_map;
    } else {
      tgt_key_part_map = end_key_part_map;
    }

    if (crd_type == 0)
      weight = spider_param_crd_weight(thd, share->crd_weight);
    else
      weight = 1;

    if (share->static_key_cardinality[inx] == -1)
    {
      for (
        key_part = key_info->key_part;
        tgt_key_part_map > 1;
        tgt_key_part_map >>= 1,
        key_part++
      ) {
        field = key_part->field;
        DBUG_PRINT("info",
          ("spider field_index=%u",
            field->field_index));
        DBUG_PRINT("info",
          ("spider cardinality=%lld", share->cardinality[field->field_index]));
        if (share->cardinality[field->field_index] == -1)
        {
          DBUG_PRINT("info",
            ("spider uninitialized column cardinality"));
          DBUG_RETURN(HA_POS_ERROR);
        }
        if ((rate =
          ((double) share->cardinality[field->field_index]) / weight) >= 1
        ) {
          if ((rows = rows / rate) < 2)
          {
            DBUG_PRINT("info",("spider rows2=%f then ret 2", rows));
            DBUG_RETURN((ha_rows) 2);
          }
        }
        if (crd_type == 1)
          weight += spider_param_crd_weight(thd, share->crd_weight);
        else if (crd_type == 2)
          weight *= spider_param_crd_weight(thd, share->crd_weight);
      }
      field = key_part->field;
      DBUG_PRINT("info",
        ("spider field_index=%u",
          field->field_index));
      DBUG_PRINT("info",
        ("spider cardinality=%lld", share->cardinality[field->field_index]));
      if (share->cardinality[field->field_index] == -1)
      {
        DBUG_PRINT("info",
          ("spider uninitialized column cardinality"));
        DBUG_RETURN(HA_POS_ERROR);
      }
    }
    if (
      start_key_part_map >= end_key_part_map &&
      start_key->flag == HA_READ_KEY_EXACT
    ) {
      if (share->static_key_cardinality[inx] == -1)
      {
        if ((rate =
          ((double) share->cardinality[field->field_index]) / weight) >= 1)
          rows = rows / rate;
      } else {
        rate = ((double) share->static_key_cardinality[inx]);
        rows = rows / rate;
      }
    } else if (start_key_part_map == end_key_part_map)
    {
      if (share->static_key_cardinality[inx] == -1)
      {
        if ((rate =
          ((double) share->cardinality[field->field_index]) / weight / 4) >= 1)
          rows = rows / rate;
      } else {
        if ((rate =
          ((double) share->static_key_cardinality[inx]) / 4) >= 1)
          rows = rows / rate;
      }
    } else {
      if (share->static_key_cardinality[inx] == -1)
      {
        if ((rate =
          ((double) share->cardinality[field->field_index]) / weight / 16) >= 1)
          rows = rows / rate;
      } else {
        if ((rate =
          ((double) share->static_key_cardinality[inx]) / 16) >= 1)
          rows = rows / rate;
      }
    }
    if (rows < 2)
    {
      DBUG_PRINT("info",("spider rows3=%f then ret 2", rows));
      DBUG_RETURN((ha_rows) 2);
    }
    DBUG_PRINT("info",("spider rows4=%f", rows));
    DBUG_RETURN((ha_rows) rows);
  } else if (crd_mode == 3)
  {
    if (!dml_inited)
    {
      if (unlikely((error_num = dml_init())))
      {
        if (check_error_mode(error_num))
          my_errno = error_num;
        DBUG_RETURN(HA_POS_ERROR);
      }
    }
    result_list.key_info = &table->key_info[inx];
    DBUG_RETURN(spider_db_explain_select(start_key, end_key, this,
      search_link_idx));
  }
  DBUG_RETURN((ha_rows) spider_param_crd_weight(thd, share->crd_weight));
}

int ha_spider::check_crd()
{
  int error_num;
  THD *thd = ha_thd();
  double crd_interval = spider_param_crd_interval(thd, share->crd_interval);
  int crd_mode = spider_param_crd_mode(thd, share->crd_mode);
  int crd_sync = spider_param_crd_sync(thd, share->crd_sync);
  int crd_bg_mode = spider_param_crd_bg_mode(thd, share->crd_bg_mode);
  SPIDER_INIT_ERROR_TABLE *spider_init_error_table = NULL;
  uint dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::check_crd");
  DBUG_PRINT("info",("spider this=%p", this));
  time_t tmp_time = (time_t) time((time_t*) 0);
  if (!share->crd_init)
  {
    pthread_mutex_lock(&share->crd_mutex);
    if (share->crd_init)
      pthread_mutex_unlock(&share->crd_mutex);
    else {
      if ((spider_init_error_table =
        spider_get_init_error_table(wide_handler->trx, share, FALSE)))
      {
        DBUG_PRINT("info",("spider diff=%f",
          difftime(tmp_time, spider_init_error_table->init_error_time)));
        if (difftime(tmp_time,
          spider_init_error_table->init_error_time) <
          spider_param_table_init_error_interval())
        {
          pthread_mutex_unlock(&share->crd_mutex);
          if (spider_init_error_table->init_error_with_message)
            my_message(spider_init_error_table->init_error,
              spider_init_error_table->init_error_msg, MYF(0));
          DBUG_RETURN(check_error_mode(spider_init_error_table->init_error));
        }
      }
      pthread_mutex_unlock(&share->crd_mutex);
      crd_interval = 0;
    }
  }
  if (crd_mode == 3)
    crd_mode = 1;
  if ((error_num = spider_check_trx_and_get_conn(ha_thd(), this, FALSE)))
  {
    DBUG_RETURN(check_error_mode(error_num));
  }
  dbton_id = share->sql_dbton_ids[search_link_idx];
  dbton_hdl = dbton_handler[dbton_id];
  crd_mode = dbton_hdl->crd_mode_exchange(crd_mode);
  DBUG_PRINT("info",
    ("spider difftime=%f", difftime(tmp_time, share->crd_get_time)));
  DBUG_PRINT("info",
    ("spider crd_interval=%f", crd_interval));
  if (difftime(tmp_time, share->crd_get_time) >= crd_interval)
  {
    if (
      crd_interval == 0 ||
      !pthread_mutex_trylock(&share->crd_mutex)
    ) {
      if (crd_interval == 0 || crd_bg_mode == 0)
      {
        if (crd_interval == 0)
          pthread_mutex_lock(&share->crd_mutex);
        if (difftime(tmp_time, share->crd_get_time) >= crd_interval)
        {
          if ((error_num = spider_get_crd(share, search_link_idx, tmp_time,
            this, table, crd_interval, crd_mode,
            crd_sync,
            share->crd_init ? 2 : 1)))
          {
            pthread_mutex_unlock(&share->crd_mutex);
            if (
              share->monitoring_kind[search_link_idx] &&
              need_mons[search_link_idx]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  search_link_idx,
                  (uint32) share->monitoring_sid[search_link_idx],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[search_link_idx],
                  NULL,
                  0,
                  share->monitoring_kind[search_link_idx],
                  share->monitoring_limit[search_link_idx],
                  share->monitoring_flag[search_link_idx],
                  TRUE
                );
            }
            if (!share->crd_init)
            {
              if (
                spider_init_error_table ||
                (spider_init_error_table =
                  spider_get_init_error_table(wide_handler->trx, share, TRUE))
              ) {
                spider_init_error_table->init_error = error_num;
                if ((spider_init_error_table->init_error_with_message =
                  thd->is_error()))
                  strmov(spider_init_error_table->init_error_msg,
                    spider_stmt_da_message(thd));
                spider_init_error_table->init_error_time =
                  (time_t) time((time_t*) 0);
              }
              share->init_error = TRUE;
              share->init = TRUE;
            }
            DBUG_RETURN(check_error_mode(error_num));
          }
        }
      } else if (crd_bg_mode == 1) {
        /* background */
        if (!share->bg_crd_init || share->bg_crd_thd_wait)
        {
          share->bg_crd_thd_wait = FALSE;
          share->bg_crd_try_time = tmp_time;
          share->bg_crd_interval = crd_interval;
          share->bg_crd_mode = crd_mode;
          share->bg_crd_sync = crd_sync;
          if (!share->bg_crd_init)
          {
            if ((error_num = spider_create_crd_thread(share)))
            {
              pthread_mutex_unlock(&share->crd_mutex);
              DBUG_RETURN(error_num);
            }
          } else
            pthread_cond_signal(&share->bg_crd_cond);
        }
      } else {
        share->bg_crd_try_time = tmp_time;
        share->bg_crd_interval = crd_interval;
        share->bg_crd_mode = crd_mode;
        share->bg_crd_sync = crd_sync;
        spider_table_add_share_to_crd_thread(share);
      }
      pthread_mutex_unlock(&share->crd_mutex);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::pre_records()
{
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::pre_records");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->sql_command == SQLCOM_ALTER_TABLE)
  {
    DBUG_RETURN(0);
  }
  if (!(share->additional_table_flags & HA_HAS_RECORDS))
  {
    DBUG_RETURN(0);
  }
  THD *thd = wide_handler->trx->thd;
  if (
    spider_param_sync_autocommit(thd) &&
    (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
  ) {
    result_list.casual_read[search_link_idx] =
      spider_param_casual_read(thd, share->casual_read);
  }
  if ((error_num = spider_db_simple_action(SPIDER_SIMPLE_RECORDS, this,
    search_link_idx, TRUE)))
  {
    DBUG_RETURN(check_error_mode(error_num));
  }
  use_pre_action = TRUE;
  DBUG_RETURN(0);
}

ha_rows ha_spider::records()
{
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::records");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->sql_command == SQLCOM_ALTER_TABLE)
  {
    use_pre_action = FALSE;
    DBUG_RETURN(0);
  }
  if (!(share->additional_table_flags & HA_HAS_RECORDS) && !this->result_list.direct_limit_offset)
  {
    DBUG_RETURN(handler::records());
  }
  if (!use_pre_action && !this->result_list.direct_limit_offset)
  {
    THD *thd = wide_handler->trx->thd;
    if (
      spider_param_sync_autocommit(thd) &&
      (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
    ) {
      result_list.casual_read[search_link_idx] =
        spider_param_casual_read(thd, share->casual_read);
    }
  }
  if ((error_num = spider_db_simple_action(SPIDER_SIMPLE_RECORDS, this,
    search_link_idx, FALSE)))
  {
    use_pre_action = FALSE;
    check_error_mode(error_num);
    DBUG_RETURN(HA_POS_ERROR);
  }
  use_pre_action = FALSE;
  share->stat.records = table_rows;
  DBUG_RETURN(table_rows);
}

int ha_spider::pre_calculate_checksum()
{
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::pre_calculate_checksum");
  DBUG_PRINT("info",("spider this=%p", this));
  THD *thd = wide_handler->trx->thd;
  if (!dml_inited)
  {
    if (unlikely((error_num = dml_init())))
    {
      DBUG_RETURN(error_num);
    }
  }
  if (
    spider_param_sync_autocommit(thd) &&
    (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
  ) {
    result_list.casual_read[search_link_idx] =
      spider_param_casual_read(thd, share->casual_read);
  }
  action_flags = T_EXTEND;
  if ((error_num = spider_db_simple_action(SPIDER_SIMPLE_CHECKSUM_TABLE, this,
    search_link_idx, TRUE)))
  {
    DBUG_RETURN(check_error_mode(error_num));
  }
  use_pre_action = TRUE;
  DBUG_RETURN(0);
}

int ha_spider::calculate_checksum()
{
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::calculate_checksum");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!dml_inited)
  {
    if (unlikely((error_num = dml_init())))
    {
      DBUG_RETURN(error_num);
    }
  }
  if (!use_pre_action && !this->result_list.direct_limit_offset)
  {
    THD *thd = wide_handler->trx->thd;
    if (
      spider_param_sync_autocommit(thd) &&
      (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
    ) {
      result_list.casual_read[search_link_idx] =
        spider_param_casual_read(thd, share->casual_read);
    }
  }
  action_flags = T_EXTEND;
  if ((error_num = spider_db_simple_action(SPIDER_SIMPLE_CHECKSUM_TABLE, this,
    search_link_idx, FALSE)))
  {
    use_pre_action = FALSE;
    DBUG_RETURN(check_error_mode(error_num));
  }
  use_pre_action = FALSE;
  if (checksum_null)
  {
    share->stat.checksum_null = TRUE;
    share->stat.checksum = 0;
    stats.checksum_null = TRUE;
    stats.checksum = 0;
  } else {
    share->stat.checksum_null = FALSE;
    share->stat.checksum = (ha_checksum) checksum_val;
    stats.checksum_null = FALSE;
    stats.checksum = (ha_checksum) checksum_val;
  }
  DBUG_RETURN(0);
}

const char *ha_spider::table_type() const
{
  DBUG_ENTER("ha_spider::table_type");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN("SPIDER");
}

ulonglong ha_spider::table_flags() const
{
  DBUG_ENTER("ha_spider::table_flags");
  DBUG_PRINT("info",("spider this=%p", this));
  ulonglong flags =
    HA_REC_NOT_IN_SEQ |
    HA_CAN_GEOMETRY |
    HA_NULL_IN_KEY |
    HA_CAN_INDEX_BLOBS |
    HA_AUTO_PART_KEY |
    HA_CAN_RTREEKEYS |
    HA_PRIMARY_KEY_REQUIRED_FOR_DELETE |
    /* HA_NO_PREFIX_CHAR_KEYS | */
    HA_CAN_FULLTEXT |
    HA_CAN_SQL_HANDLER |
    HA_FILE_BASED |
    HA_CAN_INSERT_DELAYED |
    HA_CAN_BIT_FIELD |
    HA_NO_COPY_ON_ALTER |
    HA_BINLOG_ROW_CAPABLE |
    HA_BINLOG_STMT_CAPABLE |
    HA_PARTIAL_COLUMN_READ |
#ifdef HA_SLOW_CMP_REF
    HA_SLOW_CMP_REF |
#endif
#ifdef SPIDER_ENGINE_CONDITION_PUSHDOWN_IS_ALWAYS_ON
    HA_CAN_TABLE_CONDITION_PUSHDOWN |
#endif
    SPIDER_CAN_BG_SEARCH |
    SPIDER_CAN_BG_INSERT |
    SPIDER_CAN_BG_UPDATE |
#ifdef HA_CAN_DIRECT_UPDATE_AND_DELETE
    HA_CAN_DIRECT_UPDATE_AND_DELETE |
#endif
#ifdef HA_CAN_FORCE_BULK_UPDATE
    (share && share->force_bulk_update ? HA_CAN_FORCE_BULK_UPDATE : 0) |
#endif
#ifdef HA_CAN_FORCE_BULK_DELETE
    (share && share->force_bulk_delete ? HA_CAN_FORCE_BULK_DELETE : 0) |
#endif
    (share ? share->additional_table_flags : 0)
  ;
  DBUG_RETURN(flags);
}

ulong ha_spider::table_flags_for_partition()
{
  DBUG_ENTER("ha_spider::table_flags_for_partition");
  DBUG_PRINT("info",("spider this=%p", this));
  ulong flags =
#ifdef HA_PT_CALL_AT_ONCE_STORE_LOCK
    HA_PT_CALL_AT_ONCE_STORE_LOCK |
#endif
#ifdef HA_PT_CALL_AT_ONCE_EXTERNAL_LOCK
    HA_PT_CALL_AT_ONCE_EXTERNAL_LOCK |
#endif
#ifdef HA_PT_CALL_AT_ONCE_START_STMT
    HA_PT_CALL_AT_ONCE_START_STMT |
#endif
#ifdef HA_PT_CALL_AT_ONCE_EXTRA
    HA_PT_CALL_AT_ONCE_EXTRA |
#endif
#ifdef HA_PT_CALL_AT_ONCE_COND_PUSH
    HA_PT_CALL_AT_ONCE_COND_PUSH |
#endif
#ifdef HA_PT_CALL_AT_ONCE_INFO_PUSH
    HA_PT_CALL_AT_ONCE_INFO_PUSH |
#endif
#ifdef HA_PT_CALL_AT_ONCE_TOP_TABLE
    HA_PT_CALL_AT_ONCE_TOP_TABLE |
#endif
    0;
  DBUG_RETURN(flags);
}

const char *ha_spider::index_type(
  uint key_number
) {
  KEY *key_info = &table->key_info[key_number];
  DBUG_ENTER("ha_spider::index_type");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider flags=%ld", key_info->flags));
  DBUG_PRINT("info",("spider algorithm=%d", key_info->algorithm));
  DBUG_RETURN(
    (key_info->flags & HA_FULLTEXT) ? "FULLTEXT" :
    (key_info->flags & HA_SPATIAL) ? "SPATIAL" :
    (key_info->algorithm == HA_KEY_ALG_HASH) ? "HASH" :
    (key_info->algorithm == HA_KEY_ALG_RTREE) ? "RTREE" :
    "BTREE"
  );
}

ulong ha_spider::index_flags(
  uint idx,
  uint part,
  bool all_parts
) const {
  DBUG_ENTER("ha_spider::index_flags");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(
    (table_share->key_info[idx].algorithm == HA_KEY_ALG_FULLTEXT) ?
      0 :
    (table_share->key_info[idx].algorithm == HA_KEY_ALG_HASH) ?
      HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR :
    HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
    HA_KEYREAD_ONLY
  );
}

uint ha_spider::max_supported_record_length() const
{
  DBUG_ENTER("ha_spider::max_supported_record_length");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_MAX_REC_LENGTH);
}

uint ha_spider::max_supported_keys() const
{
  DBUG_ENTER("ha_spider::max_supported_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(MAX_KEY);
}

uint ha_spider::max_supported_key_parts() const
{
  DBUG_ENTER("ha_spider::max_supported_key_parts");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(MAX_REF_PARTS);
}

uint ha_spider::max_supported_key_length() const
{
  DBUG_ENTER("ha_spider::max_supported_key_length");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(SPIDER_MAX_KEY_LENGTH);
}

uint ha_spider::max_supported_key_part_length() const
{
  DBUG_ENTER("ha_spider::max_supported_key_part_length");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(SPIDER_MAX_KEY_LENGTH);
}

uint8 ha_spider::table_cache_type()
{
  DBUG_ENTER("ha_spider::table_cache_type");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_CACHE_TBL_NOCACHE);
}

bool ha_spider::need_info_for_auto_inc()
{
  THD *thd = ha_thd();
  DBUG_ENTER("ha_spider::need_info_for_auto_inc");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider return=%s", (
    !share->lgtm_tblhnd_share->auto_increment_init ||
    (
      !spider_param_auto_increment_mode(thd, share->auto_increment_mode) &&
      !info_auto_called
    )
  ) ? "TRUE" : "FALSE"));
  DBUG_RETURN((
    !share->lgtm_tblhnd_share->auto_increment_init ||
    (
      !spider_param_auto_increment_mode(thd, share->auto_increment_mode) &&
      !info_auto_called
    )
  ));
}

#ifdef HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
bool ha_spider::can_use_for_auto_inc_init()
{
  DBUG_ENTER("ha_spider::can_use_for_auto_inc_init");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider return=%s", (
    !auto_inc_temporary
  ) ? "TRUE" : "FALSE"));
  DBUG_RETURN((
    !auto_inc_temporary
  ));
}
#endif

int ha_spider::update_auto_increment()
{
  int error_num;
  THD *thd = ha_thd();
  int auto_increment_mode = spider_param_auto_increment_mode(thd,
    share->auto_increment_mode);
  bool lock_here = FALSE;
  backup_error_status();
  DBUG_ENTER("ha_spider::update_auto_increment");
  DBUG_PRINT("info",("spider this=%p", this));
  force_auto_increment = TRUE;
/*
  if (
    next_insert_id >= auto_inc_interval_for_cur_row.maximum() &&
    wide_handler->trx->thd->auto_inc_intervals_forced.get_current()
  ) {
    force_auto_increment = TRUE;
    DBUG_PRINT("info",("spider force_auto_increment=TRUE"));
  } else {
    force_auto_increment = FALSE;
    DBUG_PRINT("info",("spider force_auto_increment=FALSE"));
  }
*/
  DBUG_PRINT("info",("spider auto_increment_mode=%d",
    auto_increment_mode));
  DBUG_PRINT("info",("spider next_number_field=%lld",
    table->next_number_field->val_int()));
  if (
    auto_increment_mode == 1 &&
    !(
      table->next_number_field->val_int() != 0 ||
      (table->auto_increment_field_not_null &&
        thd->variables.sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO)
    )
  ) {
    lock_here = TRUE;
    pthread_mutex_lock(&share->lgtm_tblhnd_share->auto_increment_mutex);
    next_insert_id = share->lgtm_tblhnd_share->auto_increment_value;
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      share->lgtm_tblhnd_share->auto_increment_value));
  }
  if ((error_num = handler::update_auto_increment()))
  {
    if (lock_here)
      pthread_mutex_unlock(&share->lgtm_tblhnd_share->auto_increment_mutex);
    DBUG_RETURN(check_error_mode(error_num));
  }
  if (lock_here)
  {
    if (insert_id_for_cur_row)
    {
      share->lgtm_tblhnd_share->auto_increment_lclval =
        insert_id_for_cur_row + 1;
      share->lgtm_tblhnd_share->auto_increment_value = next_insert_id;
      DBUG_PRINT("info",("spider after auto_increment_lclval=%llu",
        share->lgtm_tblhnd_share->auto_increment_lclval));
      DBUG_PRINT("info",("spider auto_increment_value=%llu",
        share->lgtm_tblhnd_share->auto_increment_value));
    }
    pthread_mutex_unlock(&share->lgtm_tblhnd_share->auto_increment_mutex);
  }
  if (!store_last_insert_id)
  {
    store_last_insert_id = table->next_number_field->val_int();
  }
  DBUG_RETURN(0);
}

void ha_spider::get_auto_increment(
  ulonglong offset,
  ulonglong increment,
  ulonglong nb_desired_values,
  ulonglong *first_value,
  ulonglong *nb_reserved_values
) {
  THD *thd = ha_thd();
  int auto_increment_mode = spider_param_auto_increment_mode(thd,
    share->auto_increment_mode);
  bool rev= table->key_info[table->s->next_number_index].
              key_part[table->s->next_number_keypart].key_part_flag &
                HA_REVERSE_SORT;
  DBUG_ENTER("ha_spider::get_auto_increment");
  DBUG_PRINT("info",("spider this=%p", this));
  *nb_reserved_values = ULONGLONG_MAX;
  if (auto_increment_mode == 0)
  {
    /* strict mode */
    int error_num;
    extra(HA_EXTRA_KEYREAD);
    if (index_init(table_share->next_number_index, TRUE))
      goto error_index_init;
    result_list.internal_limit = 1;
    if (table_share->next_number_keypart)
    {
      uchar key[MAX_KEY_LENGTH];
      key_copy(key, table->record[0],
        &table->key_info[table_share->next_number_index],
        table_share->next_number_key_offset);
      error_num = index_read_last_map(table->record[1], key,
        make_prev_keypart_map(table_share->next_number_keypart));
    } else if (rev)
      error_num = index_first(table->record[1]);
    else
      error_num = index_last(table->record[1]);

    if (error_num)
      *first_value = 1;
    else
      *first_value = ((ulonglong) table->next_number_field->
        val_int_offset(table_share->rec_buff_length) + 1);
    index_end();
    extra(HA_EXTRA_NO_KEYREAD);
    DBUG_VOID_RETURN;

error_index_init:
    extra(HA_EXTRA_NO_KEYREAD);
    *first_value = ~(ulonglong)0;
    DBUG_VOID_RETURN;
  } else {
    if (auto_increment_mode != 1)
      pthread_mutex_lock(&share->lgtm_tblhnd_share->auto_increment_mutex);
    DBUG_PRINT("info",("spider before auto_increment_lclval=%llu",
      share->lgtm_tblhnd_share->auto_increment_lclval));
    *first_value = share->lgtm_tblhnd_share->auto_increment_lclval;
    share->lgtm_tblhnd_share->auto_increment_lclval +=
      nb_desired_values * increment;
    DBUG_PRINT("info",("spider after auto_increment_lclval=%llu",
      share->lgtm_tblhnd_share->auto_increment_lclval));
    if (auto_increment_mode != 1)
      pthread_mutex_unlock(&share->lgtm_tblhnd_share->auto_increment_mutex);
  }
  DBUG_VOID_RETURN;
}

int ha_spider::reset_auto_increment(
  ulonglong value
) {
  DBUG_ENTER("ha_spider::reset_auto_increment");
  DBUG_PRINT("info",("spider this=%p", this));
  if (table->next_number_field)
  {
    pthread_mutex_lock(&share->lgtm_tblhnd_share->auto_increment_mutex);
    share->lgtm_tblhnd_share->auto_increment_lclval = value;
    share->lgtm_tblhnd_share->auto_increment_init = TRUE;
    DBUG_PRINT("info",("spider init auto_increment_lclval=%llu",
      share->lgtm_tblhnd_share->auto_increment_lclval));
    pthread_mutex_unlock(&share->lgtm_tblhnd_share->auto_increment_mutex);
  }
  DBUG_RETURN(0);
}

void ha_spider::release_auto_increment()
{
  DBUG_ENTER("ha_spider::release_auto_increment");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

void ha_spider::start_bulk_insert(
  ha_rows rows,
  uint flags
)
{
  DBUG_ENTER("ha_spider::start_bulk_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  bulk_insert = TRUE;
  bulk_size = -1;
  store_last_insert_id = 0;
  bzero(&copy_info, sizeof(copy_info));
  DBUG_VOID_RETURN;
}

int ha_spider::end_bulk_insert()
{
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::end_bulk_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  bulk_insert = FALSE;
  if (bulk_size == -1)
    DBUG_RETURN(0);
  if ((error_num = spider_db_bulk_insert(this, table, &copy_info, TRUE)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

int ha_spider::write_row(
  const uchar *buf
) {
  int error_num;
  THD *thd = ha_thd();
  int auto_increment_mode = spider_param_auto_increment_mode(thd,
    share->auto_increment_mode);
  bool auto_increment_flag =
    table->next_number_field && buf == table->record[0];
  backup_error_status();
  DBUG_ENTER("ha_spider::write_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_read_only_mode(thd, share->read_only_mode))
  {
    my_printf_error(ER_SPIDER_READ_ONLY_NUM, ER_SPIDER_READ_ONLY_STR, MYF(0),
      table_share->db.str, table_share->table_name.str);
    DBUG_RETURN(ER_SPIDER_READ_ONLY_NUM);
  }
  if (!dml_inited)
  {
    if (unlikely((error_num = dml_init())))
    {
      DBUG_RETURN(error_num);
    }
  }
#ifndef SPIDER_WITHOUT_HA_STATISTIC_INCREMENT
  ha_statistic_increment(&SSV::ha_write_count);
#endif
  if (!bulk_insert)
    store_last_insert_id = 0;
  if (auto_increment_flag)
  {
    if (auto_increment_mode == 3)
    {
      if (!table->auto_increment_field_not_null)
      {
#ifndef DBUG_OFF
        MY_BITMAP *tmp_map =
          dbug_tmp_use_all_columns(table, &table->write_set);
#endif
        table->next_number_field->store((longlong) 0, TRUE);
#ifndef DBUG_OFF
        dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
        force_auto_increment = FALSE;
        table->file->insert_id_for_cur_row = 0;
      }
    } else if (auto_increment_mode == 2)
    {
#ifndef DBUG_OFF
      MY_BITMAP *tmp_map =
        dbug_tmp_use_all_columns(table, &table->write_set);
#endif
      table->next_number_field->store((longlong) 0, TRUE);
      table->auto_increment_field_not_null = FALSE;
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
      force_auto_increment = FALSE;
      table->file->insert_id_for_cur_row = 0;
    } else {
      if (!share->lgtm_tblhnd_share->auto_increment_init)
      {
        pthread_mutex_lock(&share->lgtm_tblhnd_share->auto_increment_mutex);
        if (!share->lgtm_tblhnd_share->auto_increment_init)
        {
          info(HA_STATUS_AUTO);
          share->lgtm_tblhnd_share->auto_increment_lclval =
            stats.auto_increment_value;
          share->lgtm_tblhnd_share->auto_increment_init = TRUE;
          DBUG_PRINT("info",("spider init auto_increment_lclval=%llu",
            share->lgtm_tblhnd_share->auto_increment_lclval));
        }
        pthread_mutex_unlock(&share->lgtm_tblhnd_share->auto_increment_mutex);
      }
      if ((error_num = update_auto_increment()))
        DBUG_RETURN(error_num);
    }
  }
  if (!bulk_insert || bulk_size < 0)
  {
    direct_dup_insert =
      spider_param_direct_dup_insert(wide_handler->trx->thd,
        share->direct_dup_insert);
    DBUG_PRINT("info",("spider direct_dup_insert=%d", direct_dup_insert));
    if ((error_num = spider_db_bulk_insert_init(this, table)))
      DBUG_RETURN(check_error_mode(error_num));
    if (bulk_insert)
      bulk_size =
        (wide_handler->insert_with_update &&
          !result_list.insert_dup_update_pushdown) ||
        (!direct_dup_insert && wide_handler->ignore_dup_key) ?
        0 : spider_param_bulk_size(wide_handler->trx->thd, share->bulk_size);
    else
      bulk_size = 0;
  }
  if ((error_num = spider_db_bulk_insert(this, table, &copy_info, FALSE)))
    DBUG_RETURN(check_error_mode(error_num));

  DBUG_RETURN(0);
}


void ha_spider::direct_update_init(
  THD *thd,
  bool hs_request
) {
  DBUG_ENTER("ha_spider::direct_update_init");
  DBUG_PRINT("info",("spider this=%p", this));
  do_direct_update = TRUE;
  DBUG_VOID_RETURN;
}

bool ha_spider::start_bulk_update(
) {
  DBUG_ENTER("ha_spider::start_bulk_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(check_and_start_bulk_update(SPD_BU_START_BY_BULK_INIT));
}

int ha_spider::exec_bulk_update(
  ha_rows *dup_key_found
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::exec_bulk_update");
  DBUG_PRINT("info",("spider this=%p", this));
  *dup_key_found = 0;
  if ((error_num = spider_db_bulk_update_end(this, dup_key_found)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

int ha_spider::end_bulk_update(
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::end_bulk_update");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = check_and_end_bulk_update(SPD_BU_START_BY_BULK_INIT)))
  {
    if (check_error_mode(error_num))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

#ifdef SPIDER_UPDATE_ROW_HAS_CONST_NEW_DATA
int ha_spider::bulk_update_row(
  const uchar *old_data,
  const uchar *new_data,
  ha_rows *dup_key_found
)
#else
int ha_spider::bulk_update_row(
  const uchar *old_data,
  uchar *new_data,
  ha_rows *dup_key_found
)
#endif
{
  DBUG_ENTER("ha_spider::bulk_update_row");
  DBUG_PRINT("info",("spider this=%p", this));
  *dup_key_found = 0;
  DBUG_RETURN(update_row(old_data, new_data));
}

#ifdef SPIDER_UPDATE_ROW_HAS_CONST_NEW_DATA
int ha_spider::update_row(
  const uchar *old_data,
  const uchar *new_data
)
#else
int ha_spider::update_row(
  const uchar *old_data,
  uchar *new_data
)
#endif
{
  int error_num;
  THD *thd = ha_thd();
  backup_error_status();
  DBUG_ENTER("ha_spider::update_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_read_only_mode(thd, share->read_only_mode))
  {
    my_printf_error(ER_SPIDER_READ_ONLY_NUM, ER_SPIDER_READ_ONLY_STR, MYF(0),
      table_share->db.str, table_share->table_name.str);
    DBUG_RETURN(ER_SPIDER_READ_ONLY_NUM);
  }
#ifndef SPIDER_WITHOUT_HA_STATISTIC_INCREMENT
  ha_statistic_increment(&SSV::ha_update_count);
#endif
  do_direct_update = FALSE;
  if ((error_num = spider_db_update(this, table, old_data)))
    DBUG_RETURN(check_error_mode(error_num));
  if (table->found_next_number_field &&
    new_data == table->record[0] &&
    !table->s->next_number_keypart
  ) {
    pthread_mutex_lock(&share->lgtm_tblhnd_share->auto_increment_mutex);
    if (!share->lgtm_tblhnd_share->auto_increment_init)
    {
      info(HA_STATUS_AUTO);
      share->lgtm_tblhnd_share->auto_increment_lclval =
        stats.auto_increment_value;
      share->lgtm_tblhnd_share->auto_increment_init = TRUE;
      DBUG_PRINT("info",("spider init auto_increment_lclval=%llu",
        share->lgtm_tblhnd_share->auto_increment_lclval));
    }
    ulonglong tmp_auto_increment;
    if (((Field_num *) table->found_next_number_field)->unsigned_flag)
    {
      tmp_auto_increment =
        (ulonglong) table->found_next_number_field->val_int();
    } else {
      longlong tmp_auto_increment2 =
        table->found_next_number_field->val_int();
      if (tmp_auto_increment2 > 0)
        tmp_auto_increment = tmp_auto_increment2;
      else
        tmp_auto_increment = 0;
    }
    if (tmp_auto_increment >= share->lgtm_tblhnd_share->auto_increment_lclval)
    {
      share->lgtm_tblhnd_share->auto_increment_lclval = tmp_auto_increment + 1;
      share->lgtm_tblhnd_share->auto_increment_value = tmp_auto_increment + 1;
      DBUG_PRINT("info",("spider after auto_increment_lclval=%llu",
        share->lgtm_tblhnd_share->auto_increment_lclval));
      DBUG_PRINT("info",("spider auto_increment_value=%llu",
        share->lgtm_tblhnd_share->auto_increment_value));
    }
    pthread_mutex_unlock(&share->lgtm_tblhnd_share->auto_increment_mutex);
  }
  DBUG_RETURN(0);
}

bool ha_spider::check_direct_update_sql_part(
  st_select_lex *select_lex,
  longlong select_limit,
  longlong offset_limit
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::check_direct_update_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      dbton_hdl->check_direct_update(select_lex, select_limit, offset_limit)
    ) {
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}

#ifdef SPIDER_MDEV_16246
/**
  Perform initialization for a direct update request.

  @param  update fields       Pointer to the list of fields to update.

  @return >0                  Error.
          0                   Success.
*/

int ha_spider::direct_update_rows_init(
  List<Item> *update_fields
)
#else
int ha_spider::direct_update_rows_init()
#endif
{
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  List_iterator<Item> it(*wide_handler->direct_update_fields);
  Item *item;
  Field *field;
  THD *thd = wide_handler->trx->thd;
  DBUG_ENTER("ha_spider::direct_update_rows_init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (thd->variables.time_zone != UTC)
  {
    while ((item = it++))
    {
      if (item->type() == Item::FIELD_ITEM)
      {
        field = ((Item_field *)item)->field;

        if (field->type() == FIELD_TYPE_TIMESTAMP &&
            field->flags & UNIQUE_KEY_FLAG)
        {
          /*
            Spider cannot perform direct update on unique timestamp fields.
            To avoid false duplicate key errors, the table needs to be
            updated one row at a time.
          */
          DBUG_RETURN(HA_ERR_WRONG_COMMAND);
        }
      }
    }
  }
  if (!dml_inited)
  {
    if (unlikely(dml_init()))
    {
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
  direct_update_init(
    thd,
    FALSE
  );
  if (!wide_handler->condition)
    wide_handler->cond_check = FALSE;
  spider_get_select_limit(this, &select_lex, &select_limit, &offset_limit);
  if (wide_handler->direct_update_fields)
  {
    if (
#ifdef SPIDER_ENGINE_CONDITION_PUSHDOWN_IS_ALWAYS_ON
#else
      !(thd->variables.optimizer_switch &
        OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN) ||
#endif
      !select_lex ||
      select_lex->table_list.elements != 1 ||
      check_update_columns_sql_part() ||
      check_direct_update_sql_part(select_lex, select_limit, offset_limit) ||
      spider_db_append_condition(this, NULL, 0, TRUE)
    ) {
      DBUG_PRINT("info",("spider FALSE by condition"));
      do_direct_update = FALSE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
    if (select_lex->order_list.elements)
    {
      ORDER *order;
      for (order = (ORDER *) select_lex->order_list.first; order;
        order = order->next)
      {
        if (check_item_type_sql((*order->item)))
        {
          DBUG_PRINT("info",("spider FALSE by order"));
          do_direct_update = FALSE;
          DBUG_RETURN(HA_ERR_WRONG_COMMAND);
        }
      }
      result_list.direct_order_limit = TRUE;
    }
    wide_handler->trx->direct_update_count++;
    DBUG_PRINT("info",("spider OK"));
    DBUG_RETURN(0);
  }

  DBUG_PRINT("info",("spider offset_limit=%lld", offset_limit));
  DBUG_PRINT("info",("spider sql_command=%u", wide_handler->sql_command));
  DBUG_PRINT("info",("spider do_direct_update=%s",
    do_direct_update ? "TRUE" : "FALSE"));
  if (
    !offset_limit &&
    do_direct_update
  ) {
    wide_handler->trx->direct_update_count++;
    DBUG_PRINT("info",("spider OK"));
    DBUG_RETURN(0);
  }
  DBUG_PRINT("info",("spider FALSE by default"));
  do_direct_update = FALSE;
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


int ha_spider::direct_update_rows(
  ha_rows *update_rows,
  ha_rows *found_rows
) {
  int error_num;
  THD *thd = ha_thd();
  backup_error_status();
  DBUG_ENTER("ha_spider::direct_update_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_read_only_mode(thd, share->read_only_mode))
  {
    my_printf_error(ER_SPIDER_READ_ONLY_NUM, ER_SPIDER_READ_ONLY_STR, MYF(0),
      table_share->db.str, table_share->table_name.str);
    DBUG_RETURN(ER_SPIDER_READ_ONLY_NUM);
  }
  if (
    (active_index != MAX_KEY && (error_num = index_handler_init())) ||
    (active_index == MAX_KEY && (error_num = rnd_handler_init())) ||
    (error_num = spider_db_direct_update(this, table, update_rows, found_rows))
  )
    DBUG_RETURN(check_error_mode(error_num));

  DBUG_RETURN(0);
}


bool ha_spider::start_bulk_delete(
) {
  DBUG_ENTER("ha_spider::start_bulk_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(check_and_start_bulk_update(SPD_BU_START_BY_BULK_INIT));
}

int ha_spider::end_bulk_delete(
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::end_bulk_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = check_and_end_bulk_update(SPD_BU_START_BY_BULK_INIT)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

int ha_spider::delete_row(
  const uchar *buf
) {
  THD *thd = ha_thd();
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::delete_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_read_only_mode(thd, share->read_only_mode))
  {
    my_printf_error(ER_SPIDER_READ_ONLY_NUM, ER_SPIDER_READ_ONLY_STR, MYF(0),
      table_share->db.str, table_share->table_name.str);
    DBUG_RETURN(ER_SPIDER_READ_ONLY_NUM);
  }
#ifndef SPIDER_WITHOUT_HA_STATISTIC_INCREMENT
  ha_statistic_increment(&SSV::ha_delete_count);
#endif
  do_direct_update = FALSE;
  if ((error_num = spider_db_delete(this, table, buf)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

bool ha_spider::check_direct_delete_sql_part(
  st_select_lex *select_lex,
  longlong select_limit,
  longlong offset_limit
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::check_direct_delete_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      dbton_hdl->check_direct_delete(select_lex, select_limit, offset_limit)
    ) {
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}

int ha_spider::direct_delete_rows_init()
{
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  THD *thd = wide_handler->trx->thd;
  DBUG_ENTER("ha_spider::direct_delete_rows_init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!dml_inited)
  {
    if (unlikely(dml_init()))
    {
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
  direct_update_init(
    thd,
    FALSE
  );
  if (!wide_handler->condition)
    wide_handler->cond_check = FALSE;
  spider_get_select_limit(this, &select_lex, &select_limit, &offset_limit);
  if (
#ifdef SPIDER_ENGINE_CONDITION_PUSHDOWN_IS_ALWAYS_ON
#else
    !(thd->variables.optimizer_switch &
      OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN) ||
#endif
    !select_lex ||
    select_lex->table_list.elements != 1 ||
    check_direct_delete_sql_part(select_lex, select_limit, offset_limit) ||
    spider_db_append_condition(this, NULL, 0, TRUE)
  ) {
    DBUG_PRINT("info",("spider FALSE by condition"));
    do_direct_update = FALSE;
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  if (select_lex->order_list.elements)
  {
    ORDER *order;
    for (order = (ORDER *) select_lex->order_list.first; order;
      order = order->next)
    {
      if (check_item_type_sql((*order->item)))
      {
        DBUG_PRINT("info",("spider FALSE by order"));
        do_direct_update = FALSE;
        DBUG_RETURN(HA_ERR_WRONG_COMMAND);
      }
    }
    result_list.direct_order_limit = TRUE;
  }
  wide_handler->trx->direct_delete_count++;
  DBUG_PRINT("info",("spider OK"));
  DBUG_RETURN(0);
}


int ha_spider::direct_delete_rows(
  ha_rows *delete_rows
) {
  int error_num;
  THD *thd = ha_thd();
  backup_error_status();
  DBUG_ENTER("ha_spider::direct_delete_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_read_only_mode(thd, share->read_only_mode))
  {
    my_printf_error(ER_SPIDER_READ_ONLY_NUM, ER_SPIDER_READ_ONLY_STR, MYF(0),
      table_share->db.str, table_share->table_name.str);
    DBUG_RETURN(ER_SPIDER_READ_ONLY_NUM);
  }
  if (
    (active_index != MAX_KEY && (error_num = index_handler_init())) ||
    (active_index == MAX_KEY && (error_num = rnd_handler_init())) ||
    (error_num = spider_db_direct_delete(this, table, delete_rows))
  )
    DBUG_RETURN(check_error_mode(error_num));

  DBUG_RETURN(0);
}


int ha_spider::delete_all_rows()
{
  int error_num, roop_count;
  THD *thd = ha_thd();
  backup_error_status();
  DBUG_ENTER("ha_spider::delete_all_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_delete_all_rows_type(thd, share->delete_all_rows_type))
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  if (spider_param_read_only_mode(thd, share->read_only_mode))
  {
    my_printf_error(ER_SPIDER_READ_ONLY_NUM, ER_SPIDER_READ_ONLY_STR, MYF(0),
      table_share->db.str, table_share->table_name.str);
    DBUG_RETURN(ER_SPIDER_READ_ONLY_NUM);
  }
  do_direct_update = FALSE;
  sql_kinds = SPIDER_SQL_KIND_SQL;
  for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
    sql_kind[roop_count] = SPIDER_SQL_KIND_SQL;
  if ((error_num = spider_db_delete_all_rows(this)))
    DBUG_RETURN(check_error_mode(error_num));
  if (wide_handler->sql_command == SQLCOM_TRUNCATE &&
    table->found_next_number_field)
  {
    DBUG_PRINT("info",("spider reset auto increment"));
    pthread_mutex_lock(&share->lgtm_tblhnd_share->auto_increment_mutex);
    share->lgtm_tblhnd_share->auto_increment_lclval = 1;
    share->lgtm_tblhnd_share->auto_increment_init = FALSE;
    share->lgtm_tblhnd_share->auto_increment_value = 1;
    DBUG_PRINT("info",("spider init auto_increment_lclval=%llu",
      share->lgtm_tblhnd_share->auto_increment_lclval));
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      share->lgtm_tblhnd_share->auto_increment_value));
    pthread_mutex_unlock(&share->lgtm_tblhnd_share->auto_increment_mutex);
  }
  DBUG_RETURN(0);
}

int ha_spider::truncate()
{
  int error_num, roop_count;
  THD *thd = ha_thd();
  backup_error_status();
  DBUG_ENTER("ha_spider::truncate");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_read_only_mode(thd, share->read_only_mode))
  {
    my_printf_error(ER_SPIDER_READ_ONLY_NUM, ER_SPIDER_READ_ONLY_STR, MYF(0),
      table_share->db.str, table_share->table_name.str);
    DBUG_RETURN(ER_SPIDER_READ_ONLY_NUM);
  }
  wide_handler->sql_command = SQLCOM_TRUNCATE;
  if ((error_num = spider_check_trx_and_get_conn(thd, this, FALSE)))
  {
    DBUG_RETURN(error_num);
  }
  do_direct_update = FALSE;
  sql_kinds = SPIDER_SQL_KIND_SQL;
  for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
    sql_kind[roop_count] = SPIDER_SQL_KIND_SQL;
  if ((error_num = spider_db_delete_all_rows(this)))
    DBUG_RETURN(check_error_mode(error_num));
  if (wide_handler->sql_command == SQLCOM_TRUNCATE &&
    table->found_next_number_field)
  {
    DBUG_PRINT("info",("spider reset auto increment"));
    pthread_mutex_lock(&share->lgtm_tblhnd_share->auto_increment_mutex);
    share->lgtm_tblhnd_share->auto_increment_lclval = 1;
    share->lgtm_tblhnd_share->auto_increment_init = FALSE;
    share->lgtm_tblhnd_share->auto_increment_value = 1;
    DBUG_PRINT("info",("spider init auto_increment_lclval=%llu",
      share->lgtm_tblhnd_share->auto_increment_lclval));
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      share->lgtm_tblhnd_share->auto_increment_value));
    pthread_mutex_unlock(&share->lgtm_tblhnd_share->auto_increment_mutex);
  }
  DBUG_RETURN(0);
}


double ha_spider::scan_time()
{
  DBUG_ENTER("ha_spider::scan_time");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider scan_time = %.6f",
    share->scan_rate * share->stat.records * share->stat.mean_rec_length + 2));
  DBUG_RETURN(share->scan_rate * share->stat.records *
    share->stat.mean_rec_length + 2);
}

double ha_spider::read_time(
  uint index,
  uint ranges,
  ha_rows rows
) {
  DBUG_ENTER("ha_spider::read_time");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler->keyread)
  {
    DBUG_PRINT("info",("spider read_time(keyread) = %.6f",
      share->read_rate * table->key_info[index].key_length *
      rows / 2 + 2));
    DBUG_RETURN(share->read_rate * table->key_info[index].key_length *
      rows / 2 + 2);
  } else {
    DBUG_PRINT("info",("spider read_time = %.6f",
      share->read_rate * share->stat.mean_rec_length * rows + 2));
    DBUG_RETURN(share->read_rate * share->stat.mean_rec_length * rows + 2);
  }
}

const key_map *ha_spider::keys_to_use_for_scanning()
{
  DBUG_ENTER("ha_spider::keys_to_use_for_scanning");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(&key_map_full);
}

ha_rows ha_spider::estimate_rows_upper_bound()
{
  DBUG_ENTER("ha_spider::estimate_rows_upper_bound");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_POS_ERROR);
}

void ha_spider::print_error(
  int error,
  myf errflag
) {
  DBUG_ENTER("ha_spider::print_error");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!current_thd->is_error())
  {
    switch (error)
    {
      case ER_SPIDER_CON_COUNT_ERROR:
        my_message(error, ER_SPIDER_CON_COUNT_ERROR_STR, MYF(0));
        break;
      default:
        handler::print_error(error, errflag);
        break;
    }
  }
  DBUG_VOID_RETURN;
}

bool ha_spider::get_error_message(
  int error,
  String *buf
) {
  DBUG_ENTER("ha_spider::get_error_message");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (error)
  {
    case ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM:
      if (buf->reserve(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_LEN))
        DBUG_RETURN(TRUE);
      buf->q_append(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR,
        ER_SPIDER_REMOTE_SERVER_GONE_AWAY_LEN);
      break;
    default:
      if (buf->reserve(ER_SPIDER_UNKNOWN_LEN))
        DBUG_RETURN(TRUE);
      buf->q_append(ER_SPIDER_UNKNOWN_STR, ER_SPIDER_UNKNOWN_LEN);
      break;
  }
  DBUG_RETURN(FALSE);
}

int ha_spider::create(
  const char *name,
  TABLE *form,
  HA_CREATE_INFO *info
) {
  int error_num, dummy;
  SPIDER_SHARE tmp_share;
  THD *thd = ha_thd();
  uint sql_command = thd_sql_command(thd), roop_count;
  SPIDER_TRX *trx;
  TABLE *table_tables = NULL;
  SPIDER_Open_tables_backup open_tables_backup;
  bool need_lock = FALSE;
  DBUG_ENTER("ha_spider::create");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider name=%s", name));
  DBUG_PRINT("info",
    ("spider form->s->connect_string=%s", form->s->connect_string.str));
  DBUG_PRINT("info",
    ("spider info->connect_string=%s", info->connect_string.str));
  if (
    sql_command == SQLCOM_CREATE_INDEX ||
    sql_command == SQLCOM_DROP_INDEX
  )
    DBUG_RETURN(0);
  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
    goto error_get_trx;
  if (
    trx->locked_connections &&
    sql_command == SQLCOM_ALTER_TABLE
  ) {
    my_message(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM,
      ER_SPIDER_ALTER_BEFORE_UNLOCK_STR, MYF(0));
    error_num = ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM;
    goto error_alter_before_unlock;
  }
  memset((void*)&tmp_share, 0, sizeof(SPIDER_SHARE));
  tmp_share.table_name = (char*) name;
  tmp_share.table_name_length = strlen(name);
  tmp_share.table_name_hash_value = my_calc_hash(&trx->trx_alter_table_hash,
    (uchar*) tmp_share.table_name, tmp_share.table_name_length);
  tmp_share.lgtm_tblhnd_share = spider_get_lgtm_tblhnd_share(
    name, tmp_share.table_name_length, tmp_share.table_name_hash_value,
    FALSE, TRUE, &error_num);
  if (!tmp_share.lgtm_tblhnd_share)
  {
    goto error;
  }
  if (form->s->keys > 0)
  {
    if (!(tmp_share.static_key_cardinality = (longlong *)
      spider_bulk_malloc(spider_current_trx, 246, MYF(MY_WME),
        &tmp_share.static_key_cardinality,
          (uint) (sizeof(*tmp_share.static_key_cardinality) * form->s->keys),
        NullS))
    ) {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    if (!(tmp_share.key_hint = new spider_string[form->s->keys]))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
  }
  for (roop_count = 0; roop_count < form->s->keys; roop_count++)
    tmp_share.key_hint[roop_count].init_calc_mem(85);
  DBUG_PRINT("info",("spider tmp_share.key_hint=%p", tmp_share.key_hint));
  if ((error_num = spider_parse_connect_info(&tmp_share, form->s,
    form->part_info,
    1)))
    goto error;
  DBUG_PRINT("info",("spider tmp_table=%d", form->s->tmp_table));
  if (
    (sql_command == SQLCOM_CREATE_TABLE &&
      !(info->options & HA_LEX_CREATE_TMP_TABLE))
  ) {
    if (
      !(table_tables = spider_open_sys_table(
        current_thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
        SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, FALSE,
        &error_num))
    ) {
      goto error;
    }
    if (
      thd->lex->create_info.or_replace() &&
      (error_num = spider_delete_tables(
        table_tables, tmp_share.table_name, &dummy))
    ) {
      goto error;
    }
    if (
      (error_num = spider_insert_tables(table_tables, &tmp_share))
    ) {
      goto error;
    }
    spider_close_sys_table(current_thd, table_tables,
      &open_tables_backup, FALSE);
    table_tables = NULL;
  } else if (
    sql_command == SQLCOM_ALTER_TABLE
  ) {
    SPIDER_ALTER_TABLE *alter_table;
    if (trx->query_id != thd->query_id)
    {
      spider_free_trx_alter_table(trx);
      trx->query_id = thd->query_id;
    }
    if (!(alter_table =
      (SPIDER_ALTER_TABLE*) my_hash_search_using_hash_value(
      &trx->trx_alter_table_hash, tmp_share.table_name_hash_value,
      (uchar*) tmp_share.table_name, tmp_share.table_name_length)))
    {
      if ((error_num = spider_create_trx_alter_table(trx, &tmp_share, TRUE)))
        goto error;
    }
    trx->tmp_flg = TRUE;

    DBUG_PRINT("info",
      ("spider alter_info.flags: %llu  alter_info.partition_flags: %lu",
        thd->lex->alter_info.flags, thd->lex->alter_info.partition_flags));
    if ((thd->lex->alter_info.partition_flags &
        (
          SPIDER_ALTER_PARTITION_ADD | SPIDER_ALTER_PARTITION_DROP |
          SPIDER_ALTER_PARTITION_COALESCE | SPIDER_ALTER_PARTITION_REORGANIZE |
          SPIDER_ALTER_PARTITION_TABLE_REORG | SPIDER_ALTER_PARTITION_REBUILD
        )
      ) &&
      memcmp(name + strlen(name) - 5, "#TMP#", 5)
    ) {
      need_lock = TRUE;
      if (
        !(table_tables = spider_open_sys_table(
          current_thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
          SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, TRUE,
          &error_num))
      ) {
        goto error;
      }
      if (
        (error_num = spider_insert_tables(table_tables, &tmp_share))
      ) {
        goto error;
      }
      spider_close_sys_table(current_thd, table_tables,
        &open_tables_backup, TRUE);
      table_tables = NULL;
    }
  }

  if (
    (
      (info->used_fields & HA_CREATE_USED_AUTO) ||
      sql_command == SQLCOM_ALTER_TABLE ||
      sql_command == SQLCOM_CREATE_INDEX ||
      sql_command == SQLCOM_RENAME_TABLE
    ) &&
    info->auto_increment_value > 0
  ) {
    pthread_mutex_lock(&tmp_share.lgtm_tblhnd_share->auto_increment_mutex);
    tmp_share.lgtm_tblhnd_share->auto_increment_value =
      info->auto_increment_value;
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      tmp_share.lgtm_tblhnd_share->auto_increment_value));
    pthread_mutex_unlock(&tmp_share.lgtm_tblhnd_share->auto_increment_mutex);
  }

  if (tmp_share.static_key_cardinality)
    spider_free(spider_current_trx, tmp_share.static_key_cardinality, MYF(0));
  spider_free_share_alloc(&tmp_share);
  DBUG_RETURN(0);

error:
  if (table_tables)
    spider_close_sys_table(current_thd, table_tables,
      &open_tables_backup, need_lock);
  if (tmp_share.lgtm_tblhnd_share)
    spider_free_lgtm_tblhnd_share_alloc(tmp_share.lgtm_tblhnd_share, FALSE);
  if (tmp_share.static_key_cardinality)
    spider_free(spider_current_trx, tmp_share.static_key_cardinality, MYF(0));
  spider_free_share_alloc(&tmp_share);
error_alter_before_unlock:
error_get_trx:
  DBUG_RETURN(error_num);
}

void ha_spider::update_create_info(
  HA_CREATE_INFO* create_info
) {
  DBUG_ENTER("ha_spider::update_create_info");
  DBUG_PRINT("info",("spider this=%p", this));
  if (wide_handler && wide_handler->sql_command == SQLCOM_ALTER_TABLE)
  {
    SPIDER_TRX *trx = wide_handler->trx;
    THD *thd = trx->thd;
    if (trx->query_id != thd->query_id)
    {
      spider_free_trx_alter_table(trx);
      trx->query_id = thd->query_id;
      trx->tmp_flg = FALSE;
    }
    if (!(SPIDER_ALTER_TABLE*) my_hash_search(&trx->trx_alter_table_hash,
      (uchar*) share->table_name, share->table_name_length))
    {
      if (spider_create_trx_alter_table(trx, share, FALSE))
      {
        store_error_num = HA_ERR_OUT_OF_MEM;
        DBUG_VOID_RETURN;
      }
    }
  }

  if (!create_info->connect_string.str)
  {
    create_info->connect_string.str = table->s->connect_string.str;
    create_info->connect_string.length = table->s->connect_string.length;
  }
  DBUG_PRINT("info",
    ("spider create_info->connect_string=%s",
    create_info->connect_string.str));
  if (
    !(create_info->used_fields & HA_CREATE_USED_AUTO)
  ) {
    info(HA_STATUS_AUTO);
    create_info->auto_increment_value = stats.auto_increment_value;
  }
  DBUG_VOID_RETURN;
}

int ha_spider::rename_table(
  const char *from,
  const char *to
) {
  int error_num, roop_count, old_link_count, from_len = strlen(from),
    to_len = strlen(to), tmp_error_num;
  my_hash_value_type from_hash_value = my_calc_hash(&spider_open_tables,
    (uchar*) from, from_len);
  my_hash_value_type to_hash_value = my_calc_hash(&spider_open_tables,
    (uchar*) to, to_len);
  THD *thd = ha_thd();
  uint sql_command = thd_sql_command(thd);
  SPIDER_TRX *trx;
  TABLE *table_tables = NULL;
  SPIDER_ALTER_TABLE *alter_table_from, *alter_table_to;
  SPIDER_LGTM_TBLHND_SHARE *from_lgtm_tblhnd_share, *to_lgtm_tblhnd_share;
  SPIDER_Open_tables_backup open_tables_backup;
  bool need_lock = FALSE;
  DBUG_ENTER("ha_spider::rename_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider from=%s", from));
  DBUG_PRINT("info",("spider to=%s", to));
  if (
    sql_command == SQLCOM_CREATE_INDEX ||
    sql_command == SQLCOM_DROP_INDEX
  )
    DBUG_RETURN(0);
  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
    goto error;
  if (
    trx->locked_connections &&
    /* SQLCOM_RENAME_TABLE doesn't come here */
    sql_command == SQLCOM_ALTER_TABLE
  ) {
    my_message(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM,
      ER_SPIDER_ALTER_BEFORE_UNLOCK_STR, MYF(0));
    error_num = ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM;
    goto error;
  }
  if (
    sql_command == SQLCOM_RENAME_TABLE ||
    (sql_command == SQLCOM_ALTER_TABLE && !trx->tmp_flg) ||
    !(alter_table_from =
      (SPIDER_ALTER_TABLE*) my_hash_search(&trx->trx_alter_table_hash,
      (uchar*) from, from_len))
  ) {
    if (
      !(table_tables = spider_open_sys_table(
        current_thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
        SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, FALSE,
        &error_num))
    ) {
      goto error;
    }
    if (
      (error_num = spider_update_tables_name(
        table_tables, from, to, &old_link_count))
    ) {
      goto error;
    }
    spider_close_sys_table(current_thd, table_tables,
      &open_tables_backup, FALSE);
    table_tables = NULL;

    /* release table mon list */
    for (roop_count = 0; roop_count < old_link_count; roop_count++)
    {
      if ((error_num =
        spider_release_ping_table_mon_list(from, from_len, roop_count)))
      {
        goto error;
      }
    }
  } else if (sql_command == SQLCOM_ALTER_TABLE)
  {
    DBUG_PRINT("info",("spider alter_table_from=%p", alter_table_from));
    if ((alter_table_to =
      (SPIDER_ALTER_TABLE*) my_hash_search(&trx->trx_alter_table_hash,
      (uchar*) to, to_len))
    ) {
      DBUG_PRINT("info",("spider copy link_statuses"));
      uint all_link_count = alter_table_from->all_link_count;
      if (all_link_count > alter_table_to->all_link_count)
        all_link_count = alter_table_to->all_link_count;
      for (roop_count = 0; roop_count < (int) all_link_count; roop_count++)
      {
        if (alter_table_from->tmp_link_statuses[roop_count] <=
          SPIDER_LINK_STATUS_NO_CHANGE)
        {
          DBUG_PRINT("info",("spider copy %d", roop_count));
          alter_table_from->tmp_link_statuses[roop_count] =
            alter_table_to->tmp_link_statuses[roop_count];
        }
        DBUG_PRINT("info",("spider link_status_from[%d]=%ld", roop_count,
          alter_table_from->tmp_link_statuses[roop_count]));
        DBUG_PRINT("info",("spider link_status_to[%d]=%ld", roop_count,
          alter_table_to->tmp_link_statuses[roop_count]));
      }
    }

    DBUG_PRINT("info",
      ("spider alter_info.flags: %llu  alter_info.partition_flags: %lu",
        thd->lex->alter_info.flags, thd->lex->alter_info.partition_flags));
    if (
      (thd->lex->alter_info.partition_flags &
        (
          SPIDER_ALTER_PARTITION_ADD | SPIDER_ALTER_PARTITION_DROP |
          SPIDER_ALTER_PARTITION_COALESCE | SPIDER_ALTER_PARTITION_REORGANIZE |
          SPIDER_ALTER_PARTITION_TABLE_REORG | SPIDER_ALTER_PARTITION_REBUILD
        )
      )
    )
      need_lock = TRUE;

    if (
      !(table_tables = spider_open_sys_table(
        current_thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
        SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, need_lock,
        &error_num))
    ) {
      goto error;
    }

    if (alter_table_from->now_create)
    {
      SPIDER_SHARE tmp_share;
      tmp_share.table_name = (char*) to;
      tmp_share.table_name_length = to_len;
      tmp_share.priority = alter_table_from->tmp_priority;
      tmp_share.link_count = alter_table_from->link_count;
      tmp_share.all_link_count = alter_table_from->all_link_count;
      memcpy(&tmp_share.alter_table, alter_table_from,
        sizeof(*alter_table_from));
      if (
        (error_num = spider_insert_tables(table_tables, &tmp_share))
      ) {
        goto error;
      }
    } else {
      if (
        (error_num = spider_update_tables_priority(
          table_tables, alter_table_from, to, &old_link_count))
      ) {
        goto error;
      }
    }
    spider_close_sys_table(current_thd, table_tables,
      &open_tables_backup, need_lock);
    table_tables = NULL;

    if (!alter_table_from->now_create)
    {
      /* release table mon list */
      for (roop_count = 0; roop_count < (int) alter_table_from->all_link_count;
        roop_count++)
      {
        if ((error_num =
          spider_release_ping_table_mon_list(from, from_len, roop_count)))
        {
          goto error;
        }
      }
      for (roop_count = 0; roop_count < old_link_count; roop_count++)
      {
        if ((error_num =
          spider_release_ping_table_mon_list(to, to_len, roop_count)))
        {
          goto error;
        }
      }
    }
/*
    spider_free_trx_alter_table_alloc(trx, alter_table_from);
*/
  }

  pthread_mutex_lock(&spider_lgtm_tblhnd_share_mutex);
  from_lgtm_tblhnd_share = spider_get_lgtm_tblhnd_share(
    from, from_len, from_hash_value, TRUE, FALSE, &error_num);
  if (from_lgtm_tblhnd_share)
  {
    to_lgtm_tblhnd_share = spider_get_lgtm_tblhnd_share(
      to, to_len, to_hash_value, TRUE, TRUE, &error_num);
    if (!to_lgtm_tblhnd_share)
    {
      pthread_mutex_unlock(&spider_lgtm_tblhnd_share_mutex);
      goto error;
    }
    DBUG_PRINT("info",
      ("spider auto_increment_init=%s",
        from_lgtm_tblhnd_share->auto_increment_init ? "TRUE" : "FALSE"));
    to_lgtm_tblhnd_share->auto_increment_init =
      from_lgtm_tblhnd_share->auto_increment_init;
    to_lgtm_tblhnd_share->auto_increment_lclval =
      from_lgtm_tblhnd_share->auto_increment_lclval;
    to_lgtm_tblhnd_share->auto_increment_value =
      from_lgtm_tblhnd_share->auto_increment_value;
    spider_free_lgtm_tblhnd_share_alloc(from_lgtm_tblhnd_share, TRUE);
  }
  pthread_mutex_unlock(&spider_lgtm_tblhnd_share_mutex);
  spider_delete_init_error_table(from);
  DBUG_RETURN(0);

error:
  if (table_tables)
    spider_close_sys_table(current_thd, table_tables,
      &open_tables_backup, need_lock);
  pthread_mutex_lock(&spider_lgtm_tblhnd_share_mutex);
  to_lgtm_tblhnd_share = spider_get_lgtm_tblhnd_share(
    to, to_len, to_hash_value, TRUE, FALSE, &tmp_error_num);
  if (to_lgtm_tblhnd_share)
    spider_free_lgtm_tblhnd_share_alloc(to_lgtm_tblhnd_share, TRUE);
  pthread_mutex_unlock(&spider_lgtm_tblhnd_share_mutex);
  DBUG_RETURN(error_num);
}

int ha_spider::delete_table(
  const char *name
) {
  int error_num;
  THD *thd = ha_thd();
  SPIDER_TRX *trx;
  TABLE *table_tables = NULL;
  uint sql_command = thd_sql_command(thd);
  SPIDER_ALTER_TABLE *alter_table;
  SPIDER_Open_tables_backup open_tables_backup;
  bool need_lock = FALSE;
  DBUG_ENTER("ha_spider::delete_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider name=%s", name));
  if (
    sql_command == SQLCOM_CREATE_INDEX ||
    sql_command == SQLCOM_DROP_INDEX
  )
    DBUG_RETURN(0);
  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
    goto error;
  if (
    trx->locked_connections &&
    /* SQLCOM_DROP_DB doesn't come here */
    (
      sql_command == SQLCOM_DROP_TABLE ||
      sql_command == SQLCOM_ALTER_TABLE
    )
  ) {
    my_message(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM,
      ER_SPIDER_ALTER_BEFORE_UNLOCK_STR, MYF(0));
    error_num = ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM;
    goto error;
  }
  if (sql_command == SQLCOM_DROP_TABLE ||
    sql_command == SQLCOM_DROP_DB ||
    sql_command == SQLCOM_ALTER_TABLE ||
    sql_command == SQLCOM_CREATE_TABLE)
  {
    SPIDER_LGTM_TBLHND_SHARE *lgtm_tblhnd_share;
    int roop_count, old_link_count = 0, name_len = strlen(name);
    my_hash_value_type hash_value = my_calc_hash(&spider_open_tables,
      (uchar*) name, name_len);
    if (
      sql_command == SQLCOM_ALTER_TABLE &&
      (alter_table =
        (SPIDER_ALTER_TABLE*) my_hash_search_using_hash_value(
        &trx->trx_alter_table_hash,
        hash_value, (uchar*) name, name_len)) &&
      alter_table->now_create
    )
      DBUG_RETURN(0);

    DBUG_PRINT("info",
      ("spider alter_info.flags: %llu  alter_info.partition_flags: %lu",
        thd->lex->alter_info.flags, thd->lex->alter_info.partition_flags));
    if (
      sql_command == SQLCOM_ALTER_TABLE &&
      (thd->lex->alter_info.partition_flags &
        (
          SPIDER_ALTER_PARTITION_ADD | SPIDER_ALTER_PARTITION_DROP |
          SPIDER_ALTER_PARTITION_COALESCE | SPIDER_ALTER_PARTITION_REORGANIZE |
          SPIDER_ALTER_PARTITION_TABLE_REORG | SPIDER_ALTER_PARTITION_REBUILD
        )
      )
    )
      need_lock = TRUE;

    if ((error_num = spider_sys_delete_table_sts(
      current_thd, name, name_len, need_lock)))
      goto error;
    if ((error_num = spider_sys_delete_table_crd(
      current_thd, name, name_len, need_lock)))
      goto error;
    if (
      !(table_tables = spider_open_sys_table(
        current_thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
        SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, need_lock,
        &error_num))
    ) {
      goto error;
    }
    if (
      (error_num = spider_delete_tables(
        table_tables, name, &old_link_count))
    ) {
      goto error;
    }
    spider_close_sys_table(current_thd, table_tables,
      &open_tables_backup, need_lock);
    table_tables = NULL;

    /* release table mon list */
    for (roop_count = 0; roop_count < old_link_count; roop_count++)
    {
      if ((error_num =
        spider_release_ping_table_mon_list(name, name_len, roop_count)))
        goto error;
    }

    pthread_mutex_lock(&spider_lgtm_tblhnd_share_mutex);
    lgtm_tblhnd_share = spider_get_lgtm_tblhnd_share(
      name, name_len, hash_value, TRUE, FALSE, &error_num);
    if (lgtm_tblhnd_share)
      spider_free_lgtm_tblhnd_share_alloc(lgtm_tblhnd_share, TRUE);
    pthread_mutex_unlock(&spider_lgtm_tblhnd_share_mutex);
  }

  spider_delete_init_error_table(name);
  DBUG_RETURN(0);

error:
  if (table_tables)
    spider_close_sys_table(current_thd, table_tables,
      &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

bool ha_spider::is_crashed() const
{
  DBUG_ENTER("ha_spider::is_crashed");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

#ifdef SPIDER_HANDLER_AUTO_REPAIR_HAS_ERROR
bool ha_spider::auto_repair(int error) const
#else
bool ha_spider::auto_repair() const
#endif
{
  DBUG_ENTER("ha_spider::auto_repair");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int ha_spider::disable_indexes(
  uint mode
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::disable_indexes");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_disable_keys(this)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

int ha_spider::enable_indexes(
  uint mode
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::enable_indexes");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_enable_keys(this)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}


int ha_spider::check(
  THD* thd,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::check");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_check_table(this, check_opt)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

int ha_spider::repair(
  THD* thd,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::repair");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_repair_table(this, check_opt)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

bool ha_spider::check_and_repair(
  THD *thd
) {
  HA_CHECK_OPT check_opt;
  DBUG_ENTER("ha_spider::check_and_repair");
  DBUG_PRINT("info",("spider this=%p", this));
  check_opt.init();
  check_opt.flags = T_MEDIUM;
  if (spider_db_check_table(this, &check_opt))
  {
    check_opt.flags = T_QUICK;
    if (spider_db_repair_table(this, &check_opt))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

int ha_spider::analyze(
  THD* thd,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::analyze");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_analyze_table(this)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

int ha_spider::optimize(
  THD* thd,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  backup_error_status();
  DBUG_ENTER("ha_spider::optimize");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_optimize_table(this)))
    DBUG_RETURN(check_error_mode(error_num));
  DBUG_RETURN(0);
}

bool ha_spider::is_fatal_error(
  int error_num,
  uint flags
) {
  DBUG_ENTER("ha_spider::is_fatal_error");
  DBUG_PRINT("info",("spider error_num=%d", error_num));
  DBUG_PRINT("info",("spider flags=%u", flags));
  if (
    !handler::is_fatal_error(error_num, flags)
  ) {
    DBUG_PRINT("info",("spider FALSE"));
    DBUG_RETURN(FALSE);
  }
  DBUG_PRINT("info",("spider TRUE"));
  DBUG_RETURN(TRUE);
}

Field *ha_spider::field_exchange(
  Field *field
) {
  DBUG_ENTER("ha_spider::field_exchange");
  DBUG_PRINT("info",("spider in field=%p", field));
  DBUG_PRINT("info",("spider in field->table=%p", field->table));
  DBUG_PRINT("info",("spider table=%p", table));
  if (field->table != table)
    DBUG_RETURN(NULL);
  DBUG_PRINT("info",("spider out field=%p", field));
  DBUG_RETURN(field);
}

const COND *ha_spider::cond_push(
  const COND *cond
) {
  DBUG_ENTER("ha_spider::cond_push");
  if (
    wide_handler->stage == SPD_HND_STAGE_COND_PUSH &&
    wide_handler->stage_executor != this)
  {
    DBUG_RETURN(NULL);
  }
  wide_handler->stage = SPD_HND_STAGE_COND_PUSH;
  wide_handler->stage_executor = this;
  wide_handler->cond_check = FALSE;
  if (cond)
  {
    SPIDER_CONDITION *tmp_cond;
    if (!(tmp_cond = (SPIDER_CONDITION *)
      spider_malloc(spider_current_trx, 3, sizeof(*tmp_cond), MYF(MY_WME)))
    )
      DBUG_RETURN(cond);
    tmp_cond->cond = (COND *) cond;
    tmp_cond->next = wide_handler->condition;
    wide_handler->condition = tmp_cond;
  }
  DBUG_RETURN(NULL);
}

void ha_spider::cond_pop()
{
  DBUG_ENTER("ha_spider::cond_pop");
  if (
    wide_handler->stage == SPD_HND_STAGE_COND_POP &&
    wide_handler->stage_executor != this)
  {
    DBUG_VOID_RETURN;
  }
  wide_handler->stage = SPD_HND_STAGE_COND_POP;
  wide_handler->stage_executor = this;
  if (wide_handler->condition)
  {
    SPIDER_CONDITION *tmp_cond = wide_handler->condition->next;
    spider_free(spider_current_trx, wide_handler->condition, MYF(0));
    wide_handler->condition = tmp_cond;
  }
  DBUG_VOID_RETURN;
}

int ha_spider::info_push(
  uint info_type,
  void *info
) {
  int error_num = 0;
  DBUG_ENTER("ha_spider::info_push");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    wide_handler->stage == SPD_HND_STAGE_INFO_PUSH &&
    wide_handler->stage_executor != this)
  {
    DBUG_RETURN(0);
  }
  wide_handler->stage = SPD_HND_STAGE_INFO_PUSH;
  wide_handler->stage_executor = this;

  switch (info_type)
  {
#ifdef INFO_KIND_UPDATE_FIELDS
    case INFO_KIND_UPDATE_FIELDS:
      DBUG_PRINT("info",("spider INFO_KIND_UPDATE_FIELDS"));
      wide_handler->direct_update_fields = (List<Item> *) info;
      wide_handler->update_request = TRUE;
      if (wide_handler->keyread && check_partitioned())
        wide_handler->keyread = FALSE;
      break;
#endif
#ifdef INFO_KIND_UPDATE_VALUES
    case INFO_KIND_UPDATE_VALUES:
      DBUG_PRINT("info",("spider INFO_KIND_UPDATE_VALUES"));
      wide_handler->direct_update_values = (List<Item> *) info;
      break;
#endif
#ifdef INFO_KIND_FORCE_LIMIT_BEGIN
    case INFO_KIND_FORCE_LIMIT_BEGIN:
      DBUG_PRINT("info",("spider INFO_KIND_FORCE_LIMIT_BEGIN"));
      wide_handler->info_limit = *((longlong *) info);
      break;
    case INFO_KIND_FORCE_LIMIT_END:
      DBUG_PRINT("info",("spider INFO_KIND_FORCE_LIMIT_END"));
      wide_handler->info_limit = 9223372036854775807LL;
      break;
#endif
    default:
      break;
  }
  DBUG_RETURN(error_num);
}

void ha_spider::return_record_by_parent()
{
  DBUG_ENTER("ha_spider::return_record_by_parent");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_db_refetch_for_item_sum_funcs(this);
  DBUG_VOID_RETURN;
}

TABLE *ha_spider::get_table()
{
  DBUG_ENTER("ha_spider::get_table");
  DBUG_RETURN(table);
}

void ha_spider::set_ft_discard_bitmap()
{
  DBUG_ENTER("ha_spider::set_ft_discard_bitmap");
  TABLE_LIST *table_list = spider_get_parent_table_list(this);
  if (table_list)
  {
    st_select_lex *select_lex = table_list->select_lex;
    if (select_lex && select_lex->ftfunc_list)
    {
      uint roop_count;
      Field *field;
      Item *item, *item_next;
      Item_func_match *item_func_match;
      Item_field *item_field;
      {
        List_iterator_fast<Item_func_match> fmi(*select_lex->ftfunc_list);
        while ((item_func_match = fmi++))
        {
          DBUG_PRINT("info",("spider item_func_match=%p", item_func_match));
          uint item_count = item_func_match->argument_count();
          Item **item_list = item_func_match->arguments();
          for (roop_count = 1; roop_count < item_count; roop_count++)
          {
            item_field = (Item_field *) item_list[roop_count];
            DBUG_PRINT("info",("spider item_field=%p", item_field));
            field = item_field->field;
            DBUG_PRINT("info",("spider field=%p", field));
            if (!field || !(field = field_exchange(field)))
              continue;
            DBUG_PRINT("info",("spider clear_bit=%u", field->field_index));
            spider_clear_bit(wide_handler->ft_discard_bitmap,
              field->field_index);
          }
        }
      }
      THD *thd = ha_thd();
      Statement *stmt = thd->stmt_map.find(thd->id);
      if (stmt && stmt->free_list)
      {
        DBUG_PRINT("info",("spider item from stmt"));
        item_next = stmt->free_list;
      } else {
        DBUG_PRINT("info",("spider item from thd"));
        item_next = thd->free_list;
      }
      while ((item = item_next))
      {
        DBUG_PRINT("info",("spider item=%p", item));
        DBUG_PRINT("info",("spider itemtype=%u", item->type()));
        item_next = item->next;
        if (item->type() != Item::FIELD_ITEM)
          continue;
        field = ((Item_field *) item)->field;
        DBUG_PRINT("info",("spider field=%p", field));
        if (!field || !(field = field_exchange(field)))
          continue;
        DBUG_PRINT("info",("spider field_index=%u", field->field_index));
        if (!spider_bit_is_set(wide_handler->ft_discard_bitmap,
          field->field_index))
        {
          bool match_flag = FALSE;
          List_iterator_fast<Item_func_match> fmi(*select_lex->ftfunc_list);
          while ((item_func_match = fmi++))
          {
            DBUG_PRINT("info",("spider item_func_match=%p", item_func_match));
            uint item_count = item_func_match->argument_count();
            Item **item_list = item_func_match->arguments();
            for (roop_count = 1; roop_count < item_count; roop_count++)
            {
              DBUG_PRINT("info",("spider item_list[%u]=%p", roop_count,
                item_list[roop_count]));
              if (item == item_list[roop_count])
              {
                DBUG_PRINT("info",("spider matched"));
                match_flag = TRUE;
                break;
              }
            }
            if (match_flag)
              break;
          }
          if (!match_flag)
          {
            DBUG_PRINT("info",("spider set_bit=%u", field->field_index));
            spider_set_bit(wide_handler->ft_discard_bitmap,
              field->field_index);
          }
        }
      }
    }
  }
  DBUG_VOID_RETURN;
}

void ha_spider::set_searched_bitmap()
{
  int roop_count;
  DBUG_ENTER("ha_spider::set_searched_bitmap");
  for (roop_count = 0; roop_count < (int) ((table_share->fields + 7) / 8);
    roop_count++)
  {
    wide_handler->searched_bitmap[roop_count] =
      ((uchar *) table->read_set->bitmap)[roop_count] |
      ((uchar *) table->write_set->bitmap)[roop_count];
    DBUG_PRINT("info",("spider roop_count=%d", roop_count));
    DBUG_PRINT("info",("spider searched_bitmap=%d",
      wide_handler->searched_bitmap[roop_count]));
    DBUG_PRINT("info",("spider read_set=%d",
      ((uchar *) table->read_set->bitmap)[roop_count]));
    DBUG_PRINT("info",("spider write_set=%d",
      ((uchar *) table->write_set->bitmap)[roop_count]));
  }
  if (wide_handler->sql_command == SQLCOM_UPDATE ||
    wide_handler->sql_command == SQLCOM_UPDATE_MULTI)
  {
    DBUG_PRINT("info",("spider update option start"));
    Item *item;
    st_select_lex *select_lex = spider_get_select_lex(this);
    List_iterator_fast<Item> fi(select_lex->item_list);
    while ((item = fi++))
    {
      if (item->type() == Item::FIELD_ITEM)
      {
        Field *field = ((Item_field *)item)->field;
        if (!(field = field_exchange(field)))
        {
          DBUG_PRINT("info",("spider field is for different table"));
          continue;
        }
        spider_set_bit(wide_handler->searched_bitmap, field->field_index);
        DBUG_PRINT("info",("spider set searched_bitmap=%u",
          field->field_index));
      } else {
        DBUG_PRINT("info",("spider item type is not field"));
      }
    }
  }
  DBUG_VOID_RETURN;
}

void ha_spider::set_clone_searched_bitmap()
{
  DBUG_ENTER("ha_spider::set_clone_searched_bitmap");
  DBUG_PRINT("info",("spider searched_bitmap=%p",
    wide_handler->searched_bitmap));
#ifndef DBUG_OFF
  int roop_count;
  for (roop_count = 0; roop_count < (int) ((table_share->fields + 7) / 8);
    roop_count++)
    DBUG_PRINT("info", ("spider before searched_bitmap is %x",
      ((uchar *) wide_handler->searched_bitmap)[roop_count]));
#endif
  memcpy(wide_handler->searched_bitmap,
    pt_clone_source_handler->wide_handler->searched_bitmap,
    (table_share->fields + 7) / 8);
#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < (int) ((table_share->fields + 7) / 8);
    roop_count++)
    DBUG_PRINT("info", ("spider after searched_bitmap is %x",
      ((uchar *) wide_handler->searched_bitmap)[roop_count]));
#endif
  memcpy(wide_handler->ft_discard_bitmap,
    pt_clone_source_handler->wide_handler->ft_discard_bitmap,
    (table_share->fields + 7) / 8);
  DBUG_VOID_RETURN;
}

void ha_spider::set_searched_bitmap_from_item_list()
{
  DBUG_ENTER("ha_spider::set_searched_bitmap_from_item_list");
  Field *field;
  Item *item, *item_next;
  THD *thd = ha_thd();
  Statement *stmt = thd->stmt_map.find(thd->id);
  if (stmt && stmt->free_list)
  {
    DBUG_PRINT("info",("spider item from stmt"));
    item_next = stmt->free_list;
  } else {
    DBUG_PRINT("info",("spider item from thd"));
    item_next = thd->free_list;
  }
  while ((item = item_next))
  {
    DBUG_PRINT("info",("spider item=%p", item));
    DBUG_PRINT("info",("spider itemtype=%u", item->type()));
    item_next = item->next;
    if (item->type() != Item::FIELD_ITEM)
      continue;
    field = ((Item_field *) item)->field;
    DBUG_PRINT("info",("spider field=%p", field));
    if (!field || !(field = field_exchange(field)))
      continue;
    DBUG_PRINT("info",("spider field_index=%u", field->field_index));
    spider_set_bit(wide_handler->searched_bitmap, field->field_index);
  }
  DBUG_VOID_RETURN;
}

void ha_spider::set_select_column_mode()
{
  int roop_count;
  KEY *key_info;
  KEY_PART_INFO *key_part;
  Field *field;
  THD *thd = wide_handler->trx->thd;
  DBUG_ENTER("ha_spider::set_select_column_mode");
  wide_handler->position_bitmap_init = FALSE;
#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < (int) ((table_share->fields + 7) / 8);
    roop_count++)
    DBUG_PRINT("info", ("spider bitmap is %x",
      ((uchar *) table->read_set->bitmap)[roop_count]));
#endif
  select_column_mode = spider_param_select_column_mode(thd,
    share->select_column_mode);
  if (select_column_mode)
  {
    DBUG_PRINT("info",("spider searched_bitmap=%p",
      wide_handler->searched_bitmap));
    set_searched_bitmap();
    set_searched_bitmap_from_item_list();
    if (wide_handler->external_lock_type == F_WRLCK &&
      wide_handler->sql_command != SQLCOM_SELECT)
    {
      uint part_num = 0;
      if (wide_handler->update_request)
        part_num = check_partitioned();
      if (
        part_num ||
        table_share->primary_key == MAX_KEY
      ) {
        /* need all columns */
        for (roop_count = 0; roop_count < (int) table_share->fields;
          roop_count++)
          spider_set_bit(wide_handler->searched_bitmap, roop_count);
      } else {
        /* need primary key columns */
        key_info = &table_share->key_info[table_share->primary_key];
        key_part = key_info->key_part;
        for (roop_count = 0;
          roop_count < (int) spider_user_defined_key_parts(key_info);
          roop_count++)
        {
          field = key_part[roop_count].field;
          spider_set_bit(wide_handler->searched_bitmap, field->field_index);
        }
      }
#ifndef DBUG_OFF
      for (roop_count = 0;
        roop_count < (int) ((table_share->fields + 7) / 8);
        roop_count++)
        DBUG_PRINT("info", ("spider change bitmap is %x",
          wide_handler->searched_bitmap[roop_count]));
#endif
    }
  }
  DBUG_VOID_RETURN;
}

void ha_spider::check_select_column(bool rnd)
{
  THD *thd = wide_handler->trx->thd;
  DBUG_ENTER("ha_spider::check_select_column");
  select_column_mode = spider_param_select_column_mode(thd,
    share->select_column_mode);
  if (select_column_mode)
  {
    if (!rnd)
    {
      if (wide_handler->between_flg)
      {
        memcpy(wide_handler->idx_read_bitmap,
          table->read_set->bitmap, (table_share->fields + 7) / 8);
        memcpy(wide_handler->idx_write_bitmap,
          table->write_set->bitmap, (table_share->fields + 7) / 8);
        wide_handler->between_flg = FALSE;
        wide_handler->idx_bitmap_is_set = TRUE;
        DBUG_PRINT("info",("spider set idx_bitmap"));
      } else if (wide_handler->idx_bitmap_is_set)
      {
        memcpy(table->read_set->bitmap,
          wide_handler->idx_read_bitmap,
          (table_share->fields + 7) / 8);
        memcpy(table->write_set->bitmap,
          wide_handler->idx_write_bitmap,
          (table_share->fields + 7) / 8);
        DBUG_PRINT("info",("spider copy idx_bitmap"));
      }
    } else {
      if (
        !wide_handler->rnd_bitmap_is_set &&
        (
          wide_handler->between_flg ||
          wide_handler->idx_bitmap_is_set
        )
      ) {
        memcpy(wide_handler->rnd_read_bitmap,
          table->read_set->bitmap, (table_share->fields + 7) / 8);
        memcpy(wide_handler->rnd_write_bitmap,
          table->write_set->bitmap, (table_share->fields + 7) / 8);
        wide_handler->between_flg = FALSE;
        wide_handler->rnd_bitmap_is_set = TRUE;
        DBUG_PRINT("info",("spider set rnd_bitmap"));
      } else if (wide_handler->rnd_bitmap_is_set)
      {
        memcpy(table->read_set->bitmap,
          wide_handler->rnd_read_bitmap,
          (table_share->fields + 7) / 8);
        memcpy(table->write_set->bitmap,
          wide_handler->rnd_write_bitmap,
          (table_share->fields + 7) / 8);
        DBUG_PRINT("info",("spider copy rnd_bitmap"));
      }
    }
  }
  DBUG_VOID_RETURN;
}

bool ha_spider::check_and_start_bulk_update(
  spider_bulk_upd_start bulk_upd_start
) {
  DBUG_ENTER("ha_spider::check_and_start_bulk_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider bulk_update_start=%d",
    result_list.bulk_update_start));
  if (
    result_list.bulk_update_start == SPD_BU_NOT_START ||
    (
      !result_list.bulk_update_mode &&
      bulk_upd_start == SPD_BU_START_BY_BULK_INIT
    )
  ) {
    THD *thd = ha_thd();
    int bulk_update_mode = spider_param_bulk_update_mode(thd,
      share->bulk_update_mode);
/*
    longlong split_read = spider_split_read_param(this);
*/
    result_list.bulk_update_size = spider_param_bulk_update_size(thd,
      share->bulk_update_size);

    if (!support_bulk_update_sql())
    {
      result_list.bulk_update_mode = 0;
      DBUG_PRINT("info",("spider result_list.bulk_update_mode=%d 1",
        result_list.bulk_update_mode));
/*
    } else if (
      split_read != 9223372036854775807LL
    ) {
      result_list.bulk_update_mode = 2;
      DBUG_PRINT("info",("spider result_list.bulk_update_mode=%d 2",
        result_list.bulk_update_mode));
*/
    } else {
      if (result_list.bulk_update_start == SPD_BU_NOT_START)
      {
        result_list.bulk_update_mode = bulk_update_mode;
        DBUG_PRINT("info",("spider result_list.bulk_update_mode=%d 3",
          result_list.bulk_update_mode));
      } else {
        result_list.bulk_update_mode = 1;
        DBUG_PRINT("info",("spider result_list.bulk_update_mode=%d 4",
          result_list.bulk_update_mode));
      }
    }
    result_list.bulk_update_start = bulk_upd_start;
    DBUG_RETURN(FALSE);
  }
  DBUG_RETURN(TRUE);
}

int ha_spider::check_and_end_bulk_update(
  spider_bulk_upd_start bulk_upd_start
) {
  int error_num = 0;
  ha_rows dup_key_found = 0;
  DBUG_ENTER("ha_spider::check_and_end_bulk_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider bulk_update_start=%d",
    result_list.bulk_update_start));
  DBUG_PRINT("info",("spider bulk_update_mode=%d",
    result_list.bulk_update_mode));
  if (result_list.bulk_update_start == bulk_upd_start)
  {
    if (result_list.bulk_update_mode)
      error_num = spider_db_bulk_update_end(this, &dup_key_found);
    result_list.bulk_update_size = 0;
    result_list.bulk_update_mode = 0;
    result_list.bulk_update_start = SPD_BU_NOT_START;
  }
  DBUG_RETURN(error_num);
}

uint ha_spider::check_partitioned()
{
  uint part_num;
  DBUG_ENTER("ha_spider::check_partitioned");
  DBUG_PRINT("info",("spider this=%p", this));
  table->file->get_no_parts("", &part_num);
  if (part_num)
    DBUG_RETURN(part_num);

  TABLE_LIST *tmp_table_list = table->pos_in_table_list;
  while ((tmp_table_list = tmp_table_list->parent_l))
  {
    tmp_table_list->table->file->get_no_parts("", &part_num);
    if (part_num)
      DBUG_RETURN(part_num);
  }
  DBUG_RETURN(0);
}

void ha_spider::check_direct_order_limit()
{
  int roop_count;
  DBUG_ENTER("ha_spider::check_direct_order_limit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!result_list.check_direct_order_limit)
  {
    if (spider_check_direct_order_limit(this))
    {
      result_list.direct_order_limit = TRUE;
      sql_kinds = SPIDER_SQL_KIND_SQL;
      for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
        sql_kind[roop_count] = SPIDER_SQL_KIND_SQL;
    } else
      result_list.direct_order_limit = FALSE;

    spider_set_direct_limit_offset(this);
    result_list.check_direct_order_limit = TRUE;
  }
  DBUG_VOID_RETURN;
}

/********************************************************************
 * Check whether the current query is a SELECT DISTINCT using an
 * index in a non-partitioned Spider configuration, with a
 * projection list that consists solely of the first key prefix
 * column.
 *
 * For a SELECT DISTINCT query using an index in a non-partitioned
 * Spider configuration, with a projection list that consists
 * solely of the first key prefix, set the internal row retrieval
 * limit to avoid visiting each row multiple times.
 ********************************************************************/
void ha_spider::check_distinct_key_query()
{
  DBUG_ENTER( "ha_spider::check_distinct_key_query" );

  if ( result_list.direct_distinct && !partition_handler->handlers &&
       result_list.keyread && result_list.check_direct_order_limit )
  {
    // SELECT DISTINCT query using an index in a non-partitioned configuration
    KEY_PART_INFO*  key_part = result_list.key_info->key_part;
    Field*          key_field = key_part->field;

    if ( is_sole_projection_field( key_field->field_index ) )
    {
      // Projection list consists solely of the first key prefix column

      // Set the internal row retrieval limit to avoid visiting each row
      // multiple times.  This fixes a Spider performance bug that
      // caused each row to be visited multiple times.
      result_list.internal_limit = 1;
    }
  }

  DBUG_VOID_RETURN;
}

/********************************************************************
 * Determine whether the current query's projection list
 * consists solely of the specified column.
 *
 * Params   IN      - field_index:
 *                    Field index of the column of interest within
 *                    its table.
 *
 * Returns  TRUE    - if the query's projection list consists
 *                    solely of the specified column.
 *          FALSE   - otherwise.
 ********************************************************************/
bool ha_spider::is_sole_projection_field(
  uint16 field_index
) {
  // NOTE: It is assumed that spider_db_append_select_columns() has already been called
  //       to build the bitmap of projection fields
  bool                is_ha_sole_projection_field;
  uint                loop_index, dbton_id;
  spider_db_handler*  dbton_hdl;
  DBUG_ENTER( "ha_spider::is_sole_projection_field" );

  for ( loop_index = 0; loop_index < share->use_sql_dbton_count; loop_index++ )
  {
    dbton_id    = share->use_sql_dbton_ids[ loop_index ];
    dbton_hdl   = dbton_handler[ dbton_id ];

    if ( dbton_hdl->first_link_idx >= 0 )
    {
      is_ha_sole_projection_field = dbton_hdl->is_sole_projection_field( field_index );
      if ( !is_ha_sole_projection_field )
      {
        DBUG_RETURN( FALSE );
      }
    }
  }

  DBUG_RETURN( TRUE );
}

int ha_spider::check_ha_range_eof()
{
  DBUG_ENTER("ha_spider::check_ha_range_eof");
  DBUG_PRINT("info",("spider this=%p", this));
  const key_range *end_key = result_list.end_key;
  DBUG_PRINT("info",("spider use_both_key=%s",
    result_list.use_both_key ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("spider sql_kind[%u]=%u",
    search_link_idx, sql_kind[search_link_idx]));
  DBUG_PRINT("info",("spider sql_command=%u", wide_handler->sql_command));
  if (
    result_list.use_both_key &&
    (sql_kind[search_link_idx] & SPIDER_SQL_KIND_HANDLER) &&
    wide_handler->sql_command != SQLCOM_HA_READ
  ) {
    int cmp_result = key_cmp(result_list.key_info->key_part,
      end_key->key, end_key->length);
    DBUG_PRINT("info",("spider cmp_result=%d", cmp_result));
    if (
      cmp_result > 0 ||
      (end_key->flag == HA_READ_BEFORE_KEY && !cmp_result)
    ) {
      table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::drop_tmp_tables()
{
  int error_num = 0, tmp_error_num, need_mon;
  DBUG_ENTER("ha_spider::drop_tmp_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  if (result_list.tmp_tables_created)
  {
    int roop_start, roop_end, roop_count, tmp_lock_mode;
    tmp_lock_mode = spider_conn_lock_mode(this);
    if (tmp_lock_mode)
    {
      /* "for update" or "lock in share mode" */
      roop_start = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_end = share->link_count;
    } else {
      roop_start = search_link_idx;
      roop_end = search_link_idx + 1;
    }

    for (roop_count = roop_start; roop_count < roop_end;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (spider_bit_is_set(result_list.tmp_table_created, roop_count))
      {
        uint dbton_id = share->use_sql_dbton_ids[roop_count];
        spider_db_handler *dbton_hdl = dbton_handler[dbton_id];
        SPIDER_CONN *conn = conns[roop_count];
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        if (dbton_hdl->need_lock_before_set_sql_for_exec(
          SPIDER_SQL_TYPE_TMP_SQL))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if ((error_num = dbton_hdl->set_sql_for_exec(
          SPIDER_SQL_TYPE_TMP_SQL, roop_count)))
        {
          if (dbton_hdl->need_lock_before_set_sql_for_exec(
            SPIDER_SQL_TYPE_TMP_SQL))
          {
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
          DBUG_RETURN(error_num);
        }
        if (!dbton_hdl->need_lock_before_set_sql_for_exec(
          SPIDER_SQL_TYPE_TMP_SQL))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        conn->need_mon = &need_mon;
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        if ((tmp_error_num = spider_db_set_names(this, conn, roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            tmp_error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          error_num = tmp_error_num;
        }
        if (!tmp_error_num)
        {
          spider_conn_set_timeout_from_share(conn, roop_count,
            wide_handler->trx->thd, share);
          if (dbton_hdl->execute_sql(
            SPIDER_SQL_TYPE_DROP_TMP_TABLE_SQL,
            conn,
            -1,
            &need_mons[roop_count])
          ) {
            DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = FALSE;
            conn->mta_conn_mutex_unlock_later = FALSE;
            tmp_error_num = spider_db_errorno(conn);
            if (
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              tmp_error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            error_num = tmp_error_num;
          } else {
            DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
            conn->mta_conn_mutex_lock_already = FALSE;
            conn->mta_conn_mutex_unlock_later = FALSE;
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
        }
        spider_clear_bit(result_list.tmp_table_created, roop_count);
      }
    }
    result_list.tmp_tables_created = FALSE;
  }
  DBUG_RETURN(error_num);
}

bool ha_spider::handler_opened(
  int link_idx,
  uint tgt_conn_kind
) {
  DBUG_ENTER("ha_spider::handler_opened");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider link_idx=%d", link_idx));
  DBUG_PRINT("info",("spider tgt_conn_kind=%u", tgt_conn_kind));
  if (
      spider_bit_is_set(m_handler_opened, link_idx)
  ) {
    DBUG_PRINT("info",("spider TRUE"));
    DBUG_RETURN(TRUE);
  }
  DBUG_PRINT("info",("spider FALSE"));
  DBUG_RETURN(FALSE);
}

void ha_spider::set_handler_opened(
  int link_idx
) {
  DBUG_ENTER("ha_spider::set_handler_opened");
  DBUG_PRINT("info",("spider this=%p", this));
    spider_set_bit(m_handler_opened, link_idx);
  DBUG_VOID_RETURN;
}

void ha_spider::clear_handler_opened(
  int link_idx,
  uint tgt_conn_kind
) {
  DBUG_ENTER("ha_spider::clear_handler_opened");
  DBUG_PRINT("info",("spider this=%p", this));
    spider_clear_bit(m_handler_opened, link_idx);
  DBUG_VOID_RETURN;
}

int ha_spider::close_opened_handler(
  int link_idx,
  bool release_conn
) {
  int error_num = 0, error_num2;
  DBUG_ENTER("ha_spider::close_opened_handler");
  DBUG_PRINT("info",("spider this=%p", this));

  if (spider_bit_is_set(m_handler_opened, link_idx))
  {
    if ((error_num2 = spider_db_close_handler(this,
      conns[link_idx], link_idx, SPIDER_CONN_KIND_MYSQL))
    ) {
      if (
        share->monitoring_kind[link_idx] &&
        need_mons[link_idx]
      ) {
        error_num2 = spider_ping_table_mon_from_table(
          wide_handler->trx,
          wide_handler->trx->thd,
          share,
          link_idx,
          (uint32) share->monitoring_sid[link_idx],
          share->table_name,
          share->table_name_length,
          conn_link_idx[link_idx],
          NULL,
          0,
          share->monitoring_kind[link_idx],
          share->monitoring_limit[link_idx],
          share->monitoring_flag[link_idx],
          TRUE
        );
      }
      error_num = error_num2;
    }
    spider_clear_bit(m_handler_opened, link_idx);
    if (release_conn && !conns[link_idx]->join_trx)
    {
      spider_free_conn_from_trx(wide_handler->trx, conns[link_idx],
        FALSE, FALSE, NULL);
      conns[link_idx] = NULL;
    }
  }
  DBUG_RETURN(error_num);
}

int ha_spider::index_handler_init()
{
  int lock_mode, error_num;
  int roop_start, roop_end, roop_count;
  DBUG_ENTER("ha_spider::index_handler_init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!init_index_handler)
  {
    init_index_handler = TRUE;
    lock_mode = spider_conn_lock_mode(this);
    if (lock_mode)
    {
      /* "for update" or "lock in share mode" */
      roop_start = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_end = share->link_count;
    } else {
      roop_start = search_link_idx;
      roop_end = search_link_idx + 1;
    }
    sql_kinds = 0;
    direct_update_kinds = 0;
    for (roop_count = roop_start; roop_count < roop_end;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (
        spider_conn_use_handler(this, lock_mode, roop_count) &&
        spider_conn_need_open_handler(this, active_index, roop_count)
      ) {
        if ((error_num = spider_db_open_handler(this,
            conns[roop_count]
          , roop_count))
        ) {
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
        set_handler_opened(roop_count);
      }
    }
    if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      st_select_lex *select_lex;
      longlong select_limit;
      longlong offset_limit;
      spider_get_select_limit(this, &select_lex, &select_limit, &offset_limit);
      DBUG_PRINT("info",("spider SPIDER_SQL_KIND_HANDLER"));
      result_list.semi_split_read = 1;
      result_list.semi_split_read_limit = 9223372036854775807LL;
      if (select_limit == 9223372036854775807LL)
      {
        DBUG_PRINT("info",("spider set limit to 1"));
        result_list.semi_split_read_base = 1;
        result_list.split_read = 1;
      } else {
        DBUG_PRINT("info",("spider set limit to %lld", select_limit));
        result_list.semi_split_read_base = select_limit;
        result_list.split_read = select_limit;
      }
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::rnd_handler_init()
{
  int error_num, lock_mode;
  int roop_start, roop_end, roop_count;
  DBUG_ENTER("ha_spider::rnd_handler_init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!init_rnd_handler)
  {
    init_rnd_handler = TRUE;
    lock_mode = spider_conn_lock_mode(this);
    if (lock_mode)
    {
      /* "for update" or "lock in share mode" */
      roop_start = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_end = share->link_count;
    } else {
      roop_start = search_link_idx;
      roop_end = search_link_idx + 1;
    }
    sql_kinds = 0;
    direct_update_kinds = 0;
    for (roop_count = roop_start; roop_count < roop_end;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (
        spider_conn_use_handler(this, lock_mode, roop_count) &&
        spider_conn_need_open_handler(this, MAX_KEY, roop_count)
      ) {
        if ((error_num = spider_db_open_handler(this,
            conns[roop_count]
          , roop_count))
        ) {
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
        set_handler_opened(roop_count);
      }
    }
    if (sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      st_select_lex *select_lex;
      longlong select_limit;
      longlong offset_limit;
      spider_get_select_limit(this, &select_lex, &select_limit, &offset_limit);
      DBUG_PRINT("info",("spider SPIDER_SQL_KIND_HANDLER"));
      result_list.semi_split_read = 1;
      result_list.semi_split_read_limit = 9223372036854775807LL;
      if (select_limit == 9223372036854775807LL)
      {
        DBUG_PRINT("info",("spider set limit to 1"));
        result_list.semi_split_read_base = 1;
        result_list.split_read = 1;
      } else {
        DBUG_PRINT("info",("spider set limit to %lld", select_limit));
        result_list.semi_split_read_base = select_limit;
        result_list.split_read = select_limit;
      }
    }
  }
  DBUG_RETURN(0);
}

void ha_spider::set_error_mode()
{
  THD *thd = ha_thd();
  DBUG_ENTER("ha_spider::set_error_mode");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (thd_sql_command(thd))
  {
    case SQLCOM_SELECT:
    case SQLCOM_SHOW_DATABASES:
    case SQLCOM_SHOW_TABLES:
    case SQLCOM_SHOW_FIELDS:
    case SQLCOM_SHOW_KEYS:
    case SQLCOM_SHOW_VARIABLES:
    case SQLCOM_SHOW_STATUS:
    case SQLCOM_SHOW_ENGINE_LOGS:
    case SQLCOM_SHOW_ENGINE_STATUS:
    case SQLCOM_SHOW_ENGINE_MUTEX:
    case SQLCOM_SHOW_PROCESSLIST:
    case SQLCOM_SHOW_BINLOG_STAT:
    case SQLCOM_SHOW_SLAVE_STAT:
    case SQLCOM_SHOW_GRANTS:
    case SQLCOM_SHOW_CREATE:
    case SQLCOM_SHOW_CHARSETS:
    case SQLCOM_SHOW_COLLATIONS:
    case SQLCOM_SHOW_CREATE_DB:
    case SQLCOM_SHOW_TABLE_STATUS:
    case SQLCOM_SHOW_TRIGGERS:
    case SQLCOM_CHANGE_DB:
    case SQLCOM_HA_OPEN:
    case SQLCOM_HA_CLOSE:
    case SQLCOM_HA_READ:
    case SQLCOM_SHOW_SLAVE_HOSTS:
    case SQLCOM_SHOW_BINLOG_EVENTS:
    case SQLCOM_SHOW_WARNS:
    case SQLCOM_EMPTY_QUERY:
    case SQLCOM_SHOW_ERRORS:
    case SQLCOM_SHOW_STORAGE_ENGINES:
    case SQLCOM_SHOW_PRIVILEGES:
    case SQLCOM_HELP:
    case SQLCOM_SHOW_CREATE_PROC:
    case SQLCOM_SHOW_CREATE_FUNC:
    case SQLCOM_SHOW_STATUS_PROC:
    case SQLCOM_SHOW_STATUS_FUNC:
    case SQLCOM_SHOW_PROC_CODE:
    case SQLCOM_SHOW_FUNC_CODE:
    case SQLCOM_SHOW_AUTHORS:
    case SQLCOM_SHOW_PLUGINS:
    case SQLCOM_SHOW_CONTRIBUTORS:
    case SQLCOM_SHOW_CREATE_EVENT:
    case SQLCOM_SHOW_EVENTS:
    case SQLCOM_SHOW_CREATE_TRIGGER:
    case SQLCOM_SHOW_PROFILE:
    case SQLCOM_SHOW_PROFILES:
      error_mode = spider_param_error_read_mode(thd, share->error_read_mode);
      DBUG_PRINT("info",("spider read error_mode=%d", error_mode));
      break;
    default:
      error_mode = spider_param_error_write_mode(thd, share->error_write_mode);
      DBUG_PRINT("info",("spider write error_mode=%d", error_mode));
      break;
  }
  DBUG_VOID_RETURN;
}

void ha_spider::backup_error_status()
{
  THD *thd = ha_thd();
  DBUG_ENTER("ha_spider::backup_error_status");
  if (thd)
    da_status = thd->is_error();
  DBUG_VOID_RETURN;
}

int ha_spider::check_error_mode(
  int error_num
) {
  THD *thd = ha_thd();
  DBUG_ENTER("ha_spider::check_error_mode");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider error_num=%d", error_num));
  if (!thd || !error_mode)
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider error reset"));
  SPIDER_RESTORE_DASTATUS;
  DBUG_RETURN(0);
}

int ha_spider::check_error_mode_eof(
  int error_num
) {
  DBUG_ENTER("ha_spider::check_error_mode_eof");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider error_num=%d", error_num));
  if (error_num == HA_ERR_END_OF_FILE)
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  if (check_error_mode(error_num))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider result_list.finish_flg = TRUE"));
  result_list.finish_flg = TRUE;
  if (result_list.current)
  {
    DBUG_PRINT("info",("spider result_list.current->finish_flg = TRUE"));
    result_list.current->finish_flg = TRUE;
  }
  table->status = STATUS_NOT_FOUND;
  DBUG_RETURN(HA_ERR_END_OF_FILE);
}

void ha_spider::check_pre_call(
  bool use_parallel
) {
  THD* thd = ha_thd();
  LEX *lex = thd->lex;
  st_select_lex *select_lex = spider_get_select_lex(this);
  int skip_parallel_search =
    spider_param_skip_parallel_search(thd, share->skip_parallel_search);
  DBUG_ENTER("ha_spider::check_pre_call");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    (
      (skip_parallel_search & 1) &&
      lex->sql_command != SQLCOM_SELECT // such like insert .. select ..
    ) ||
    (
      (skip_parallel_search & 2) &&
      lex->sql_cache == LEX::SQL_NO_CACHE //  for mysqldump
    )
  ) {
    use_pre_call = FALSE;
    DBUG_VOID_RETURN;
  }
  if (
    use_parallel &&
    thd->query_id != partition_handler->parallel_search_query_id
  ) {
    partition_handler->parallel_search_query_id = thd->query_id;
    ++wide_handler->trx->parallel_search_count;
  }
  use_pre_call = use_parallel;
  if (!use_pre_call)
  {
    longlong select_limit;
    longlong offset_limit;
    spider_get_select_limit_from_select_lex(
      select_lex, &select_limit, &offset_limit);
    if (
      select_lex &&
      (!select_lex->limit_params.explicit_limit || !select_limit)
    ) {
      use_pre_call = TRUE;
    }
  }
  DBUG_VOID_RETURN;
}

void ha_spider::check_insert_dup_update_pushdown()
{
  THD *thd = wide_handler->trx->thd;
  DBUG_ENTER("ha_spider::check_insert_dup_update_pushdown");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!spider_param_direct_dup_insert(thd, share->direct_dup_insert))
  {
    DBUG_PRINT("info",("spider FALSE by direct_dup_insert"));
    DBUG_VOID_RETURN;
  }
  wide_handler->direct_update_fields = &thd->lex->update_list;
  wide_handler->direct_update_values = &thd->lex->value_list;
  if (!append_dup_update_pushdown_sql_part(NULL, 0))
  {
    result_list.insert_dup_update_pushdown = TRUE;
  }
  DBUG_VOID_RETURN;
}


void ha_spider::sync_from_clone_source_base(
  ha_spider *spider
) {
  uint roop_count2, dbton_id;
  spider_db_handler *dbton_hdl, *dbton_hdl2;
  DBUG_ENTER("ha_spider::sync_from_clone_source_base");
  for (roop_count2 = 0; roop_count2 < share->use_dbton_count; roop_count2++)
  {
    dbton_id = share->use_dbton_ids[roop_count2];
    dbton_hdl = dbton_handler[dbton_id];
    dbton_hdl2 = spider->dbton_handler[dbton_id];
    dbton_hdl->first_link_idx = dbton_hdl2->first_link_idx;
    dbton_hdl->strict_group_by = dbton_hdl2->strict_group_by;
  }
  DBUG_VOID_RETURN;
}

void ha_spider::set_first_link_idx()
{
  int roop_count, all_link_idx;
  uint roop_count2, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::set_first_link_idx");
  for (roop_count2 = 0; roop_count2 < share->use_dbton_count; roop_count2++)
  {
    dbton_id = share->use_dbton_ids[roop_count2];
    dbton_hdl = dbton_handler[dbton_id];
    dbton_hdl->first_link_idx = -1;
    dbton_hdl->strict_group_by = FALSE;
  }
  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count, SPIDER_LINK_STATUS_RECOVERY);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    all_link_idx = conn_link_idx[roop_count];
    dbton_id = share->sql_dbton_ids[all_link_idx];
    if (dbton_id < SPIDER_DBTON_SIZE)
    {
      dbton_hdl = dbton_handler[dbton_id];
      if (dbton_hdl->first_link_idx == -1)
      {
        dbton_hdl->first_link_idx = roop_count;
      }
      if (share->strict_group_bys[all_link_idx])
      {
        dbton_hdl->strict_group_by = TRUE;
      }
    }
  }
  DBUG_VOID_RETURN;
}

void ha_spider::reset_first_link_idx()
{
  int all_link_idx;
  uint roop_count2, dbton_id;
  spider_db_handler *dbton_hdl;
  int lock_mode = spider_conn_lock_mode(this);
  DBUG_ENTER("ha_spider::reset_first_link_idx");
  if (!lock_mode)
  {
    DBUG_PRINT("info",("spider use only search_link_idx"));
    for (roop_count2 = 0; roop_count2 < share->use_dbton_count; roop_count2++)
    {
      dbton_id = share->use_dbton_ids[roop_count2];
      dbton_hdl = dbton_handler[dbton_id];
      dbton_hdl->first_link_idx = -1;
    }
    all_link_idx = conn_link_idx[search_link_idx];
    dbton_id = share->sql_dbton_ids[all_link_idx];
    if (dbton_id < SPIDER_DBTON_SIZE)
    {
      dbton_hdl = dbton_handler[dbton_id];
      if (dbton_hdl->first_link_idx == -1)
      {
        dbton_hdl->first_link_idx = search_link_idx;
      }
    }
  }
  DBUG_VOID_RETURN;
}

int ha_spider::reset_sql_sql(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  DBUG_ENTER("ha_spider::reset_sql_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    if ((error_num = dbton_handler[dbton_id]->reset_sql(sql_type)))
    {
      DBUG_RETURN(error_num);
    }
  }

  if (sql_type & SPIDER_SQL_TYPE_BULK_UPDATE_SQL)
  {
    for (roop_count = 0; roop_count < share->link_count; roop_count++)
    {
      result_list.update_sqls[roop_count].length(0);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_tmp_table_and_sql_for_bka(
  const key_range *start_key
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_tmp_table_and_sql_for_bka");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_tmp_table_and_sql_for_bka(start_key))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::reuse_tmp_table_and_sql_for_bka()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::reuse_tmp_table_and_sql_for_bka");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->reuse_tmp_table_and_sql_for_bka())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_union_table_and_sql_for_bka(
  const key_range *start_key
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_union_table_and_sql_for_bka");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_union_table_and_sql_for_bka(start_key))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::reuse_union_table_and_sql_for_bka()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::reuse_union_table_and_sql_for_bka");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->reuse_union_table_and_sql_for_bka())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_insert_sql_part()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_insert_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_insert_part())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_update_sql_part()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_update_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_update_part())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_update_set_sql_part()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_update_set_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_update_set_part())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_direct_update_set_sql_part()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_direct_update_set_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_direct_update_set_part())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_dup_update_pushdown_sql_part(
  const char *alias,
  uint alias_length
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_dup_update_pushdown_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_dup_update_pushdown_part(
        alias, alias_length))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_update_columns_sql_part(
  const char *alias,
  uint alias_length
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_update_columns_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_update_columns_part(
        alias, alias_length))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::check_update_columns_sql_part()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::check_update_columns_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->check_update_columns_part())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_delete_sql_part()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_delete_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_delete_part())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_select_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_select_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_select_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_table_select_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_table_select_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_table_select_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_key_select_sql_part(
  ulong sql_type,
  uint idx
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_key_select_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_key_select_part(sql_type, idx))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_minimum_select_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_minimum_select_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_minimum_select_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_from_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_from_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_from_part(sql_type,
        dbton_hdl->first_link_idx))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_hint_after_table_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_hint_after_table_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_hint_after_table_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

void ha_spider::set_where_pos_sql(
  ulong sql_type
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::set_where_pos_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
      dbton_hdl->set_where_pos(sql_type);
  }
  DBUG_VOID_RETURN;
}

void ha_spider::set_where_to_pos_sql(
  ulong sql_type
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::set_where_to_pos_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
      dbton_hdl->set_where_to_pos(sql_type);
  }
  DBUG_VOID_RETURN;
}

int ha_spider::check_item_type_sql(
  Item *item
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::check_item_type_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->check_item_type(item))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_values_connector_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_values_connector_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num =
        dbton_hdl->append_values_connector_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_values_terminator_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_values_terminator_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num =
        dbton_hdl->append_values_terminator_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_union_table_connector_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_union_table_connector_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num =
        dbton_hdl->append_union_table_connector_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_union_table_terminator_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_union_table_terminator_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num =
        dbton_hdl->append_union_table_terminator_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_key_column_values_sql_part(
  const key_range *start_key,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_key_column_values_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num =
        dbton_hdl->append_key_column_values_part(start_key, sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_key_column_values_with_name_sql_part(
  const key_range *start_key,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_key_column_values_with_name_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num =
        dbton_hdl->append_key_column_values_with_name_part(
          start_key, sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_key_where_sql_part(
  const key_range *start_key,
  const key_range *end_key,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_key_where_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_key_where_part(start_key, end_key,
        sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_match_where_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_match_where_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_match_where_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_condition_sql_part(
  const char *alias,
  uint alias_length,
  ulong sql_type,
  bool test_flg
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_condition_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_condition_part(alias, alias_length,
        sql_type, test_flg))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_sum_select_sql_part(
  ulong sql_type,
  const char *alias,
  uint alias_length
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_sum_select_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_sum_select_part(sql_type,
        alias, alias_length))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  wide_handler->trx->direct_aggregate_count++;
  DBUG_RETURN(0);
}

int ha_spider::append_match_select_sql_part(
  ulong sql_type,
  const char *alias,
  uint alias_length
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_match_select_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_match_select_part(sql_type,
        alias, alias_length))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

void ha_spider::set_order_pos_sql(
  ulong sql_type
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::set_order_pos_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
      dbton_hdl->set_order_pos(sql_type);
  }
  DBUG_VOID_RETURN;
}

void ha_spider::set_order_to_pos_sql(
  ulong sql_type
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::set_order_to_pos_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
      dbton_hdl->set_order_to_pos(sql_type);
  }
  DBUG_VOID_RETURN;
}

int ha_spider::append_group_by_sql_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_group_by_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_group_by_part(
        alias, alias_length, sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_key_order_for_merge_with_alias_sql_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_key_order_for_merge_with_alias_sql_part");
  if (result_list.direct_aggregate)
  {
    st_select_lex *select_lex = spider_get_select_lex(this);
    ORDER *group = (ORDER *) select_lex->group_list.first;
    if (!group && *(select_lex->join->sum_funcs))
    {
      DBUG_PRINT("info",("spider skip order by"));
      DBUG_RETURN(0);
    }
  }
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_key_order_for_merge_with_alias_part(
        alias, alias_length, sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_key_order_for_direct_order_limit_with_alias_sql_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_key_order_for_direct_order_limit_with_alias_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num =
        dbton_hdl->append_key_order_for_direct_order_limit_with_alias_part(
          alias, alias_length, sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_key_order_with_alias_sql_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_key_order_with_alias_sql_part");
  if (result_list.direct_aggregate)
  {
    st_select_lex *select_lex = spider_get_select_lex(this);
    ORDER *group = (ORDER *) select_lex->group_list.first;
    if (!group && *(select_lex->join->sum_funcs))
    {
      DBUG_PRINT("info",("spider skip order by"));
      DBUG_RETURN(0);
    }
  }
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_key_order_with_alias_part(
        alias, alias_length, sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_limit_sql_part(
  longlong offset,
  longlong limit,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_limit_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_limit_part(offset, limit, sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::reappend_limit_sql_part(
  longlong offset,
  longlong limit,
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::reappend_limit_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->reappend_limit_part(offset, limit, sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_insert_terminator_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_insert_terminator_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_insert_terminator_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_insert_values_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_insert_values_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_insert_values_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_into_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_into_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_into_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

void ha_spider::set_insert_to_pos_sql(
  ulong sql_type
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::set_insert_to_pos_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
      dbton_hdl->set_insert_to_pos(sql_type);
  }
  DBUG_VOID_RETURN;
}

bool ha_spider::is_bulk_insert_exec_period(
  bool bulk_end
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::is_bulk_insert_exec_period");
    for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
    {
      dbton_id = share->use_sql_dbton_ids[roop_count];
      dbton_hdl = dbton_handler[dbton_id];
      if (
        dbton_hdl->first_link_idx >= 0 &&
        dbton_hdl->is_bulk_insert_exec_period(bulk_end)
      ) {
        DBUG_RETURN(TRUE);
      }
    }
  DBUG_RETURN(FALSE);
}

int ha_spider::append_select_lock_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_select_lock_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_select_lock_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_union_all_start_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_union_all_start_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_union_all_start_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_union_all_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_union_all_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_union_all_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_union_all_end_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_union_all_end_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_union_all_end_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_multi_range_cnt_sql_part(
  ulong sql_type,
  uint multi_range_cnt,
  bool with_comma
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_multi_range_cnt_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_multi_range_cnt_part(
        sql_type, multi_range_cnt, with_comma))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_multi_range_cnt_with_name_sql_part(
  ulong sql_type,
  uint multi_range_cnt
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_multi_range_cnt_with_name_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_multi_range_cnt_with_name_part(
        sql_type, multi_range_cnt))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_delete_all_rows_sql_part(
  ulong sql_type
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_delete_all_rows_sql_part");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_delete_all_rows_part(sql_type))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_update_sql(
  const TABLE *table,
  my_ptrdiff_t ptr_diff,
  bool bulk
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_update");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_update(table, ptr_diff))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  if (!bulk)
  {
    DBUG_RETURN(0);
  }

  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_count < share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    dbton_id = share->sql_dbton_ids[conn_link_idx[roop_count]];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      dbton_hdl->need_copy_for_update(roop_count)
    ) {
      if ((error_num = dbton_hdl->append_update(table, ptr_diff, roop_count)))
      {
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_delete_sql(
  const TABLE *table,
  my_ptrdiff_t ptr_diff,
  bool bulk
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::append_delete");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->append_delete(table, ptr_diff))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  if (!bulk)
  {
    DBUG_RETURN(0);
  }

  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, -1, share->link_count, SPIDER_LINK_STATUS_RECOVERY);
    roop_count < share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    dbton_id = share->sql_dbton_ids[conn_link_idx[roop_count]];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      dbton_hdl->need_copy_for_update(roop_count)
    ) {
      if ((error_num = dbton_hdl->append_delete(table, ptr_diff, roop_count)))
      {
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

bool ha_spider::sql_is_filled_up(
  ulong sql_type
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::sql_is_filled_up");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      dbton_hdl->sql_is_filled_up(sql_type)
    ) {
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}

bool ha_spider::sql_is_empty(
  ulong sql_type
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::sql_is_empty");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      !dbton_hdl->sql_is_empty(sql_type)
    ) {
      DBUG_RETURN(FALSE);
    }
  }
  DBUG_RETURN(TRUE);
}

bool ha_spider::support_multi_split_read_sql()
{
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::support_multi_split_read_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      !dbton_hdl->support_multi_split_read()
    ) {
      DBUG_RETURN(FALSE);
    }
  }
  DBUG_RETURN(TRUE);
}

bool ha_spider::support_bulk_update_sql()
{
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::support_bulk_update_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      !dbton_hdl->support_bulk_update()
    ) {
      DBUG_RETURN(FALSE);
    }
  }
  DBUG_RETURN(TRUE);
}

int ha_spider::bulk_tmp_table_insert()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  TABLE **tmp_table = result_list.upd_tmp_tbls;
  DBUG_ENTER("ha_spider::bulk_tmp_table_insert");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->bulk_tmp_table_insert())
    ) {
      DBUG_RETURN(error_num);
    }
  }

  for (roop_count = 0; roop_count < share->link_count; roop_count++)
  {
    if (tmp_table[roop_count])
    {
      dbton_id = share->sql_dbton_ids[conn_link_idx[roop_count]];
      dbton_hdl = dbton_handler[dbton_id];
      if (
        dbton_hdl->first_link_idx >= 0 &&
        (error_num = dbton_hdl->bulk_tmp_table_insert(roop_count))
      ) {
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::bulk_tmp_table_end_bulk_insert()
{
  int error_num = 0, error_num2;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  TABLE **tmp_table = result_list.upd_tmp_tbls;
  DBUG_ENTER("ha_spider::bulk_tmp_table_end_bulk_insert");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num2 = dbton_hdl->bulk_tmp_table_end_bulk_insert())
    ) {
      error_num = error_num2;
    }
  }

  for (roop_count = 0; roop_count < share->link_count; roop_count++)
  {
    if (tmp_table[roop_count])
    {
      if (
        (error_num2 = tmp_table[roop_count]->file->ha_end_bulk_insert())
      ) {
        error_num = error_num2;
      }
    }
  }
  DBUG_RETURN(error_num);
}

int ha_spider::bulk_tmp_table_rnd_init()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  TABLE **tmp_table = result_list.upd_tmp_tbls;
  DBUG_ENTER("ha_spider::bulk_tmp_table_rnd_init");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->bulk_tmp_table_rnd_init())
    ) {
      goto error_1;
    }
  }

  for (roop_count = 0; roop_count < share->link_count; roop_count++)
  {
    if (tmp_table[roop_count])
    {
      tmp_table[roop_count]->file->extra(HA_EXTRA_CACHE);
      if (
        (error_num = tmp_table[roop_count]->file->ha_rnd_init(TRUE))
      )
        goto error_2;
    }
  }
  DBUG_RETURN(0);

error_2:
  for (; roop_count > 0; roop_count--)
  {
    if (tmp_table[roop_count - 1])
    {
      tmp_table[roop_count - 1]->file->ha_rnd_end();
    }
  }
  roop_count = share->use_sql_dbton_count;
error_1:
  for (; roop_count > 0; roop_count--)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count - 1];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
      dbton_hdl->bulk_tmp_table_rnd_end();
  }
  DBUG_RETURN(error_num);
}

int ha_spider::bulk_tmp_table_rnd_next()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  TABLE **tmp_table = result_list.upd_tmp_tbls;
  DBUG_ENTER("ha_spider::bulk_tmp_table_rnd_next");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->bulk_tmp_table_rnd_next())
    ) {
      DBUG_RETURN(error_num);
    }
  }

  for (roop_count = 0; roop_count < share->link_count; roop_count++)
  {
    if (tmp_table[roop_count])
    {
      if (
        !(error_num = tmp_table[roop_count]->file->ha_rnd_next(
          tmp_table[roop_count]->record[0]))
      ) {
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::bulk_tmp_table_rnd_end()
{
  int error_num = 0, error_num2;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  TABLE **tmp_table = result_list.upd_tmp_tbls;
  DBUG_ENTER("ha_spider::bulk_tmp_table_rnd_end");
  for (roop_count = share->link_count; roop_count > 0; roop_count--)
  {
    if (tmp_table[roop_count - 1])
    {
      if ((error_num2 = tmp_table[roop_count - 1]->file->ha_rnd_end()))
      {
        error_num = error_num2;
      }
    }
  }

  for (roop_count = share->use_sql_dbton_count; roop_count > 0; roop_count--)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count - 1];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num2 = dbton_hdl->bulk_tmp_table_rnd_end())
    ) {
      error_num = error_num2;
    }
  }
  DBUG_RETURN(error_num);
}

int ha_spider::mk_bulk_tmp_table_and_bulk_start()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  TABLE **tmp_table = result_list.upd_tmp_tbls;
  DBUG_ENTER("ha_spider::mk_bulk_tmp_table_and_bulk_start");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
    {
      if (dbton_hdl->bulk_tmp_table_created())
      {
        DBUG_RETURN(0);
      } else {
        break;
      }
    }
  }

  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->mk_bulk_tmp_table_and_bulk_start())
    ) {
      goto error_1;
    }
  }

  for (roop_count = 0; roop_count < share->link_count; roop_count++)
  {
    dbton_id = share->sql_dbton_ids[conn_link_idx[roop_count]];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      dbton_hdl->need_copy_for_update(roop_count)
    ) {
#ifdef SPIDER_use_LEX_CSTRING_for_Field_blob_constructor
      LEX_CSTRING field_name = {STRING_WITH_LEN("a")};
      if (
        !tmp_table[roop_count] &&
        !(tmp_table[roop_count] = spider_mk_sys_tmp_table(
          wide_handler->trx->thd, table,
          &result_list.upd_tmp_tbl_prms[roop_count],
          &field_name, result_list.update_sqls[roop_count].charset()))
      )
#else
      if (
        !tmp_table[roop_count] &&
        !(tmp_table[roop_count] = spider_mk_sys_tmp_table(
          wide_handler->trx->thd, table,
          &result_list.upd_tmp_tbl_prms[roop_count], "a",
          result_list.update_sqls[roop_count].charset()))
      )
#endif
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error_2;
      }
      tmp_table[roop_count]->file->extra(HA_EXTRA_WRITE_CACHE);
      tmp_table[roop_count]->file->ha_start_bulk_insert((ha_rows) 0);
    }
  }
  DBUG_RETURN(0);

error_2:
  for (; roop_count > 0; roop_count--)
  {
    if (tmp_table[roop_count - 1])
    {
      tmp_table[roop_count - 1]->file->ha_end_bulk_insert();
      spider_rm_sys_tmp_table(wide_handler->trx->thd,
        tmp_table[roop_count - 1],
        &result_list.upd_tmp_tbl_prms[roop_count - 1]);
      tmp_table[roop_count - 1] = NULL;
    }
  }
  roop_count = share->use_sql_dbton_count;
error_1:
  for (; roop_count > 0; roop_count--)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count - 1];
    if (dbton_hdl->first_link_idx >= 0)
    {
      dbton_handler[dbton_id]->bulk_tmp_table_end_bulk_insert();
      dbton_handler[dbton_id]->rm_bulk_tmp_table();
    }
  }
  DBUG_RETURN(error_num);
}

void ha_spider::rm_bulk_tmp_table()
{
  uint roop_count, dbton_id;
  TABLE **tmp_table = result_list.upd_tmp_tbls;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::bulk_tmp_table_rnd_end");
  for (roop_count = share->link_count; roop_count > 0; roop_count--)
  {
    if (tmp_table[roop_count - 1])
    {
      spider_rm_sys_tmp_table(wide_handler->trx->thd,
        tmp_table[roop_count - 1],
        &result_list.upd_tmp_tbl_prms[roop_count - 1]);
      tmp_table[roop_count - 1] = NULL;
    }
  }

  for (roop_count = share->use_sql_dbton_count; roop_count > 0; roop_count--)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count - 1];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
      dbton_hdl->rm_bulk_tmp_table();
  }
  DBUG_VOID_RETURN;
}

bool ha_spider::bulk_tmp_table_created()
{
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::bulk_tmp_table_created");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (dbton_hdl->first_link_idx >= 0)
    {
      if (dbton_hdl->bulk_tmp_table_created())
      {
        DBUG_RETURN(TRUE);
      }
    }
  }
  DBUG_RETURN(FALSE);
}

int ha_spider::print_item_type(
  Item *item,
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::print_item_type");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = spider_db_print_item_type(item, NULL, this, str,
        alias, alias_length, dbton_id, FALSE, NULL))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

bool ha_spider::support_use_handler_sql(
  int use_handler
) {
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::support_use_handler_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      !dbton_hdl->support_use_handler(use_handler)
    ) {
      DBUG_RETURN(FALSE);
    }
  }
  DBUG_RETURN(TRUE);
}

int ha_spider::init_union_table_name_pos_sql()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::init_union_table_name_pos_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->init_union_table_name_pos())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::set_union_table_name_pos_sql()
{
  int error_num;
  uint roop_count, dbton_id;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("ha_spider::set_union_table_name_pos_sql");
  for (roop_count = 0; roop_count < share->use_sql_dbton_count; roop_count++)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    dbton_hdl = dbton_handler[dbton_id];
    if (
      dbton_hdl->first_link_idx >= 0 &&
      (error_num = dbton_hdl->set_union_table_name_pos())
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::append_lock_tables_list()
{
  int error_num, roop_count;
  DBUG_ENTER("ha_spider::append_lock_tables_list");
  DBUG_PRINT("info",("spider lock_table_type=%u",
    wide_handler->lock_table_type));

  if ((error_num = spider_check_trx_and_get_conn(wide_handler->trx->thd, this,
    FALSE)))
  {
    DBUG_RETURN(error_num);
  }

  if (wide_handler->lock_table_type == 1)
  {
    for (
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_count < (int) share->link_count;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      SPIDER_CONN *conn = conns[roop_count];
      int appended = 0;
      if ((error_num = dbton_handler[conn->dbton_id]->
        append_lock_tables_list(conn, roop_count, &appended)))
      {
        DBUG_RETURN(error_num);
      }
      if (appended)
      {
        conn->table_lock = 2;
      }
    }
  } else if (wide_handler->lock_table_type == 2)
  {
    for (
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_count < (int) share->link_count;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (
        conns[roop_count] &&
        conns[roop_count]->table_lock != 1 &&
        spider_param_semi_table_lock(wide_handler->trx->thd,
          share->semi_table_lock)
      ) {
        SPIDER_CONN *conn = conns[roop_count];
        int appended = 0;
        if ((error_num = dbton_handler[conn->dbton_id]->
          append_lock_tables_list(conn, roop_count, &appended)))
        {
          DBUG_RETURN(error_num);
        }
        if (appended)
        {
          conn->table_lock = 3;
        }
      }
    }
  }
  DBUG_RETURN(0);
}

int ha_spider::lock_tables()
{
  int error_num, roop_count;
  DBUG_ENTER("ha_spider::lock_tables");
  DBUG_PRINT("info",("spider lock_table_type=%u",
    wide_handler->lock_table_type));

    if (!conns[search_link_idx])
    {
      my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
        ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM);
    }
    for (
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_count < (int) share->link_count;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if (wide_handler->sql_command != SQLCOM_UNLOCK_TABLES)
      {
        DBUG_PRINT("info",("spider conns[%d]->join_trx=%u",
          roop_count, conns[roop_count]->join_trx));
        if (
          (!conns[roop_count]->join_trx &&
            (error_num = spider_internal_start_trx_for_connection(this,
              conns[roop_count],
              roop_count)))
        ) {
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(check_error_mode(error_num));
        }
        reset_first_link_idx();
      }
      if (conns[roop_count]->table_lock >= 2)
      {
        if (
          conns[roop_count]->db_conn->have_lock_table_list() &&
          (error_num = spider_db_lock_tables(this, roop_count))
        ) {
          if (
            share->monitoring_kind[roop_count] &&
            need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                wide_handler->trx,
                wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          conns[roop_count]->table_lock = 0;
          DBUG_RETURN(check_error_mode(error_num));
        }
        if (conns[roop_count]->table_lock == 2)
          conns[roop_count]->table_lock = 1;
      } else if (wide_handler->sql_command == SQLCOM_UNLOCK_TABLES ||
        spider_param_internal_unlock(wide_handler->trx->thd) == 1)
      {
        if (conns[roop_count]->table_lock == 1)
        {
          conns[roop_count]->table_lock = 0;
          if (!conns[roop_count]->trx_start)
            conns[roop_count]->disable_reconnect = FALSE;
          if ((error_num = spider_db_unlock_tables(this, roop_count)))
          {
            if (
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  wide_handler->trx,
                  wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode(error_num));
          }
        }
      }
    }
  DBUG_RETURN(0);
}

int ha_spider::dml_init()
{
  int error_num, roop_count;
  SPIDER_TRX *trx = wide_handler->trx;
  THD *thd = trx->thd;
  bool sync_trx_isolation = spider_param_sync_trx_isolation(thd);
  DBUG_ENTER("ha_spider::dml_init");
  if (wide_handler->lock_mode == -2)
  {
    wide_handler->lock_mode = spider_param_selupd_lock_mode(thd,
      share->selupd_lock_mode);
  }
  if ((error_num = check_access_kind_for_connection(thd,
    (wide_handler->lock_type >= TL_WRITE_ALLOW_WRITE))))
  {
    DBUG_RETURN(error_num);
  }
    if (!conns[search_link_idx])
    {
      my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
        ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM);
    }
    if (wide_handler->sql_command == SQLCOM_TRUNCATE)
      DBUG_RETURN(0);
    for (
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_count < (int) share->link_count;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      DBUG_PRINT("info",("spider conns[%d]->join_trx=%u",
        roop_count, conns[roop_count]->join_trx));
      if (
        (!conns[roop_count]->join_trx &&
          (error_num = spider_internal_start_trx_for_connection(this,
            conns[roop_count],
            roop_count)))
      ) {
        if (
          share->monitoring_kind[roop_count] &&
          need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              trx,
              trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(check_error_mode(error_num));
      }
      reset_first_link_idx();
      if (
        conns[roop_count]->semi_trx_isolation == -2 &&
        conns[roop_count]->semi_trx_isolation_chk == TRUE &&
        sync_trx_isolation &&
        spider_param_semi_trx_isolation(trx->thd) >= 0
      ) {
        spider_conn_queue_semi_trx_isolation(conns[roop_count],
          spider_param_semi_trx_isolation(trx->thd));
      } else {
        if (sync_trx_isolation)
        {
          if ((error_num = spider_check_and_set_trx_isolation(
            conns[roop_count], &need_mons[roop_count])))
          {
            if (
              share->monitoring_kind[roop_count] &&
              need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  trx,
                  trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(check_error_mode(error_num));
          }
        }
        conns[roop_count]->semi_trx_isolation = -1;
      }
    }
  if (wide_handler->insert_with_update)
  {
    check_insert_dup_update_pushdown();
  }
  dml_inited = TRUE;
  DBUG_RETURN(0);
}

