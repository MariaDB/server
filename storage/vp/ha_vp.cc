/* Copyright (C) 2009-2019 Kentoku Shiba
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
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation
#endif

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "vp_environ.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "key.h"
#include "sql_select.h"
#ifdef HANDLER_HAS_PRUNE_PARTITIONS_FOR_CHILD
#include "opt_range.h"
#endif
#endif
#include "vp_param.h"
#include "vp_err.h"
#include "vp_include.h"
#include "ha_vp.h"
#include "vp_table.h"

#ifdef HA_CAN_BG_SEARCH
#define VP_CAN_BG_SEARCH HA_CAN_BG_SEARCH
#else
#define VP_CAN_BG_SEARCH 0
#endif
#ifdef HA_CAN_BG_INSERT
#define VP_CAN_BG_INSERT HA_CAN_BG_INSERT
#else
#define VP_CAN_BG_INSERT 0
#endif
#ifdef HA_CAN_BG_UPDATE
#define VP_CAN_BG_UPDATE HA_CAN_BG_UPDATE
#else
#define VP_CAN_BG_UPDATE 0
#endif

static longlong vp_base_table_flags =
  (
    HA_HAS_RECORDS |
    HA_BINLOG_ROW_CAPABLE |
    HA_BINLOG_STMT_CAPABLE |
#ifdef HA_CAN_BULK_ACCESS
    HA_CAN_BULK_ACCESS |
#endif
#ifdef HA_CAN_DIRECT_UPDATE_AND_DELETE
    HA_CAN_DIRECT_UPDATE_AND_DELETE |
#endif
    VP_CAN_BG_SEARCH | VP_CAN_BG_INSERT | VP_CAN_BG_UPDATE
  );


extern handlerton *vp_hton_ptr;
#ifndef WITHOUT_VP_BG_ACCESS
extern pthread_attr_t vp_pt_attr;
#endif

#ifdef HAVE_PSI_INTERFACE
#ifndef WITHOUT_VP_BG_ACCESS
extern PSI_mutex_key vp_key_mutex_bg_sync;
extern PSI_mutex_key vp_key_mutex_bg;
extern PSI_cond_key vp_key_cond_bg_sync;
extern PSI_cond_key vp_key_cond_bg;
extern PSI_thread_key vp_key_thd_bg;
#endif
#endif

ha_vp::ha_vp(
) : handler(vp_hton_ptr, NULL)
#ifdef HA_CAN_BULK_ACCESS
  , bulk_access_started(FALSE)
  , bulk_access_executing(FALSE)
  , bulk_access_pre_called(FALSE)
  , bulk_access_info_first(NULL)
  , bulk_access_info_current(NULL)
  , bulk_access_info_exec_tgt(NULL)
  , bulk_access_exec_bitmap(NULL)
#endif
#ifdef HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
  , handler_close(FALSE)
#endif
  , mr_init(FALSE)
{
  DBUG_ENTER("ha_vp::ha_vp");
  DBUG_PRINT("info",("vp this=%p", this));
  share = NULL;
  part_tables = NULL;
  use_tables = NULL;
  work_bitmap = NULL;
  ref_length = 0;
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  allocated_top_table_fields = 0;
#endif
  additional_table_flags = vp_base_table_flags;
  ins_child_bitmaps[0] = NULL;
  condition = NULL;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_handler_share = NULL;
  pt_handler_share_creator = NULL;
  clone_partition_handler_share = NULL;
#endif
  is_clone = FALSE;
  pt_clone_source_handler = NULL;
  ft_first = NULL;
  ft_current = NULL;
  ft_inited = FALSE;
  ft_count = 0;
#if MYSQL_VERSION_ID < 50500
#else
  children_l = NULL;
  children_attached = FALSE;
#endif
  suppress_autoinc = FALSE;
  use_pre_call = FALSE;
#ifdef VP_SUPPORT_MRR
  m_mrr_range_first = NULL;
  m_child_mrr_range_first = NULL;
  m_range_info = NULL;
  m_mrr_full_buffer = NULL;
  m_mrr_full_buffer_size = 0;
  m_mrr_new_full_buffer_size = 0;
#endif
  DBUG_VOID_RETURN;
}

ha_vp::ha_vp(
  handlerton *hton,
  TABLE_SHARE *table_arg
) : handler(hton, table_arg)
#ifdef HA_CAN_BULK_ACCESS
  , bulk_access_started(FALSE)
  , bulk_access_executing(FALSE)
  , bulk_access_pre_called(FALSE)
  , bulk_access_info_first(NULL)
  , bulk_access_info_current(NULL)
  , bulk_access_info_exec_tgt(NULL)
  , bulk_access_exec_bitmap(NULL)
#endif
#ifdef HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
  , handler_close(FALSE)
#endif
  , mr_init(FALSE)
{
  DBUG_ENTER("ha_vp::ha_vp");
  DBUG_PRINT("info",("vp this=%p", this));
  share = NULL;
  part_tables = NULL;
  use_tables = NULL;
  work_bitmap = NULL;
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  allocated_top_table_fields = 0;
#endif
  additional_table_flags = vp_base_table_flags;
  ins_child_bitmaps[0] = NULL;
  condition = NULL;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_handler_share = NULL;
  pt_handler_share_creator = NULL;
  clone_partition_handler_share = NULL;
#endif
  is_clone = FALSE;
  pt_clone_source_handler = NULL;
  ref_length = 0;
  ft_first = NULL;
  ft_current = NULL;
  ft_inited = FALSE;
  ft_count = 0;
#if MYSQL_VERSION_ID < 50500
#else
  children_l = NULL;
  children_attached = FALSE;
#endif
  suppress_autoinc = FALSE;
  use_pre_call = FALSE;
#ifdef VP_SUPPORT_MRR
  m_mrr_range_first = NULL;
  m_child_mrr_range_first = NULL;
  m_range_info = NULL;
  m_mrr_full_buffer = NULL;
  m_mrr_full_buffer_size = 0;
  m_mrr_new_full_buffer_size = 0;
#endif
  DBUG_VOID_RETURN;
}

static const char *ha_vp_exts[] = {
  NullS
};

handler *ha_vp::clone(
  const char *name,
  MEM_ROOT *mem_root
) {
  ha_vp *vp;
  DBUG_ENTER("ha_vp::clone");
  DBUG_PRINT("info",("vp this=%p", this));
  if (
    !(vp = (ha_vp *)
      get_new_handler(table->s, mem_root, vp_hton_ptr)) ||
    !(vp->ref = (uchar*) alloc_root(mem_root, ALIGN_SIZE(ref_length) * 2))
  )
    DBUG_RETURN(NULL);
  vp->is_clone = TRUE;
  vp->pt_clone_source_handler = this;
  if (vp->ha_open(table, name, table->db_stat,
    HA_OPEN_IGNORE_IF_LOCKED))
    DBUG_RETURN(NULL);

  DBUG_RETURN((handler *) vp);
}

const char **ha_vp::bas_ext() const
{
  return ha_vp_exts;
}

int ha_vp::open(
  const char* name,
  int mode,
  uint test_if_locked
) {
  int error_num, roop_count;
  THD *thd = ha_thd();
#ifdef WITH_PARTITION_STORAGE_ENGINE
  VP_PARTITION_SHARE *partition_share;
  my_bitmap_map *tmp_idx_read_bitmap, *tmp_idx_write_bitmap,
    *tmp_rnd_read_bitmap, *tmp_rnd_write_bitmap,
    *tmp_idx_init_read_bitmap, *tmp_idx_init_write_bitmap,
    *tmp_rnd_init_read_bitmap, *tmp_rnd_init_write_bitmap;
  uint part_num;
  bool create_pt_handler_share = FALSE, pt_handler_mutex = FALSE,
    may_be_clone = FALSE;
  ha_vp **pt_handler_share_handlers;
#endif
  TABLE *clone_tables = NULL;
#ifndef DBUG_OFF
  TABLE *tmp_table;
#endif
  DBUG_ENTER("ha_vp::open");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  table_lock_count = 0;
  bitmap_map_size =
    sizeof(my_bitmap_map) * ((table_share->fields +
    sizeof(my_bitmap_map) * 8 - 1) / sizeof(my_bitmap_map) / 8);
  DBUG_PRINT("info",("vp bitmap_map_size=%d", bitmap_map_size));
  sql_command = thd_sql_command(thd);
  DBUG_PRINT("info",("vp sql_command=%u", sql_command));
  ref_buf = NULL;
  ref_buf_length = 0;
  VP_INIT_ALLOC_ROOT(&mr, 1024, 0, MYF(MY_WME));

  if (!vp_get_share(name, table, thd, this, &error_num))
    goto error_get_share;
  thr_lock_data_init(&share->lock, &lock, NULL);

#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_share = share->partition_share;
  table->file->get_no_parts("", &part_num);
  if (partition_share)
  {
    pt_handler_mutex = TRUE;
    pthread_mutex_lock(&partition_share->pt_handler_mutex);
/*
    if (
      !partition_share->partition_handler_share ||
      partition_share->partition_handler_share->table != table
    )
      create_pt_handler_share = TRUE;
*/
    if (
      sql_command == SQLCOM_ALTER_TABLE ||
      !(partition_handler_share = (VP_PARTITION_HANDLER_SHARE*)
        my_hash_search(&partition_share->pt_handler_hash, (uchar*) &table,
        sizeof(TABLE *)))
    )
      create_pt_handler_share = TRUE;
  }

#if MYSQL_VERSION_ID < 50500
#else
  init_correspond_columns = FALSE;
#endif
  if (create_pt_handler_share)
  {
    if (!(part_tables = (TABLE_LIST*)
      my_multi_malloc(MYF(MY_WME),
        &part_tables, sizeof(TABLE_LIST) * share->table_count,
#if MYSQL_VERSION_ID < 50500
#else
        &children_info, sizeof(VP_CHILD_INFO) * share->table_count,
#endif
        &use_tables, sizeof(uchar) * share->use_tables_size,
        &use_tables2, sizeof(uchar) * share->use_tables_size,
        &use_tables3, sizeof(uchar) * share->use_tables_size,
        &sel_key_init_use_tables, sizeof(uchar) * share->use_tables_size,
        &sel_key_use_tables, sizeof(uchar) * share->use_tables_size,
        &sel_rnd_use_tables, sizeof(uchar) * share->use_tables_size,
        &upd_target_tables, sizeof(uchar) * share->use_tables_size,
        &key_inited_tables, sizeof(uchar) * share->use_tables_size,
        &rnd_inited_tables, sizeof(uchar) * share->use_tables_size,
        &ft_inited_tables, sizeof(uchar) * share->use_tables_size,
        &select_ignore, sizeof(uchar) * share->use_tables_size,
        &select_ignore_with_lock, sizeof(uchar) * share->use_tables_size,
        &update_ignore, sizeof(uchar) * share->use_tables_size,
        &pruned_tables, sizeof(uchar) * share->use_tables_size,
#ifdef HA_CAN_BULK_ACCESS
        &bulk_access_exec_bitmap, sizeof(uchar) * share->use_tables_size,
#endif
        &work_bitmap, sizeof(uchar) * ((table_share->fields + 7) / 8),
        &work_bitmap2, sizeof(uchar) * ((table_share->fields + 7) / 8),
        &work_bitmap3, sizeof(uchar) * ((table_share->fields + 7) / 8),
        &work_bitmap4, sizeof(uchar) * ((table_share->fields + 7) / 8),
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
        &top_table_field_for_childs, sizeof(Field **) * share->table_count,
#endif
#ifndef WITHOUT_VP_BG_ACCESS
        &bg_base, sizeof(VP_BG_BASE) * share->table_count,
#endif
        &child_cond_count, sizeof(uint) * share->table_count,
        &child_record0, sizeof(uchar *) * share->table_count,
        &child_record1, sizeof(uchar *) * share->table_count,
        &idx_init_read_bitmap, bitmap_map_size,
        &idx_init_write_bitmap, bitmap_map_size,
        &rnd_init_read_bitmap, bitmap_map_size,
        &rnd_init_write_bitmap, bitmap_map_size,
        &idx_read_bitmap, bitmap_map_size,
        &idx_write_bitmap, bitmap_map_size,
        &rnd_read_bitmap, bitmap_map_size,
        &rnd_write_bitmap, bitmap_map_size,
        &partition_handler_share, sizeof(VP_PARTITION_HANDLER_SHARE),
        &tmp_idx_init_read_bitmap, bitmap_map_size,
        &tmp_idx_init_write_bitmap, bitmap_map_size,
        &tmp_rnd_init_read_bitmap, bitmap_map_size,
        &tmp_rnd_init_write_bitmap, bitmap_map_size,
        &rnd_init_write_bitmap, bitmap_map_size,
        &tmp_idx_read_bitmap, bitmap_map_size,
        &tmp_idx_write_bitmap, bitmap_map_size,
        &tmp_rnd_read_bitmap, bitmap_map_size,
        &tmp_rnd_write_bitmap, bitmap_map_size,
#if defined(HAVE_HANDLERSOCKET)
        &child_multi_range, sizeof(KEY_MULTI_RANGE) * share->table_count,
        &child_key_buff, MAX_KEY_LENGTH * share->table_count,
#endif
#ifdef VP_SUPPORT_MRR
        &m_range_info, sizeof(range_id_t) * share->table_count,
        &m_stock_range_seq, sizeof(uint) * share->table_count,
        &m_mrr_buffer, sizeof(HANDLER_BUFFER) * share->table_count,
        &m_mrr_buffer_size, sizeof(uint) * share->table_count,
        &m_child_mrr_range_length, sizeof(uint) * share->table_count,
        &m_child_mrr_range_first,
          sizeof(VP_CHILD_KEY_MULTI_RANGE *) * share->table_count,
        &m_child_mrr_range_current,
          sizeof(VP_CHILD_KEY_MULTI_RANGE *) * share->table_count,
        &m_child_key_multi_range_hld,
          sizeof(VP_CHILD_KEY_MULTI_RANGE_HLD) * share->table_count,
#endif
        &pt_handler_share_handlers, sizeof(ha_vp *) * part_num,
        NullS))
    ) {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_tables_alloc;
    }
    DBUG_PRINT("info",("vp create partition_handler_share"));
    partition_handler_share->use_count = 1;
/*
    if (partition_handler_share->use_count < part_num)
      partition_share->partition_handler_share = partition_handler_share;
*/
    DBUG_PRINT("info",("vp table=%p", table));
    partition_handler_share->table = table;
    partition_handler_share->idx_init_read_bitmap = tmp_idx_init_read_bitmap;
    partition_handler_share->idx_init_write_bitmap = tmp_idx_init_write_bitmap;
    partition_handler_share->rnd_init_read_bitmap = tmp_rnd_init_read_bitmap;
    partition_handler_share->rnd_init_write_bitmap = tmp_rnd_init_write_bitmap;
    partition_handler_share->idx_read_bitmap = tmp_idx_read_bitmap;
    partition_handler_share->idx_write_bitmap = tmp_idx_write_bitmap;
    partition_handler_share->rnd_read_bitmap = tmp_rnd_read_bitmap;
    partition_handler_share->rnd_write_bitmap = tmp_rnd_write_bitmap;
    partition_handler_share->idx_init_flg = FALSE;
    partition_handler_share->rnd_init_flg = FALSE;
    partition_handler_share->idx_bitmap_is_set = FALSE;
    partition_handler_share->rnd_bitmap_is_set = FALSE;
    partition_handler_share->creator = this;
    if (part_num)
    {
      partition_handler_share->handlers = (void **) pt_handler_share_handlers;
      partition_handler_share->handlers[0] = this;
    }
    pt_handler_share_creator = this;
    if (my_hash_insert(&partition_share->pt_handler_hash,
      (uchar*) partition_handler_share))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_hash_insert;
    }
    pthread_mutex_unlock(&partition_share->pt_handler_mutex);
    pt_handler_mutex = FALSE;
  } else {
#endif
    if (!(part_tables = (TABLE_LIST*)
      my_multi_malloc(MYF(MY_WME),
        &part_tables, sizeof(TABLE_LIST) * share->table_count,
#if MYSQL_VERSION_ID < 50500
#else
        &children_info, sizeof(VP_CHILD_INFO) * share->table_count,
#endif
        &use_tables, sizeof(uchar) * share->use_tables_size,
        &use_tables2, sizeof(uchar) * share->use_tables_size,
        &use_tables3, sizeof(uchar) * share->use_tables_size,
        &sel_key_init_use_tables, sizeof(uchar) * share->use_tables_size,
        &sel_key_use_tables, sizeof(uchar) * share->use_tables_size,
        &sel_rnd_use_tables, sizeof(uchar) * share->use_tables_size,
        &upd_target_tables, sizeof(uchar) * share->use_tables_size,
        &key_inited_tables, sizeof(uchar) * share->use_tables_size,
        &rnd_inited_tables, sizeof(uchar) * share->use_tables_size,
        &ft_inited_tables, sizeof(uchar) * share->use_tables_size,
        &select_ignore, sizeof(uchar) * share->use_tables_size,
        &select_ignore_with_lock, sizeof(uchar) * share->use_tables_size,
        &update_ignore, sizeof(uchar) * share->use_tables_size,
        &pruned_tables, sizeof(uchar) * share->use_tables_size,
#ifdef HA_CAN_BULK_ACCESS
        &bulk_access_exec_bitmap, sizeof(uchar) * share->use_tables_size,
#endif
        &work_bitmap, sizeof(uchar) * ((table_share->fields + 7) / 8),
        &work_bitmap2, sizeof(uchar) * ((table_share->fields + 7) / 8),
        &work_bitmap3, sizeof(uchar) * ((table_share->fields + 7) / 8),
        &work_bitmap4, sizeof(uchar) * ((table_share->fields + 7) / 8),
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
        &top_table_field_for_childs, sizeof(Field **) * share->table_count,
#endif
#ifndef WITHOUT_VP_BG_ACCESS
        &bg_base, sizeof(VP_BG_BASE) * share->table_count,
#endif
        &child_cond_count, sizeof(uint) * share->table_count,
        &child_record0, sizeof(uchar *) * share->table_count,
        &child_record1, sizeof(uchar *) * share->table_count,
        &idx_init_read_bitmap, bitmap_map_size,
        &idx_init_write_bitmap, bitmap_map_size,
        &rnd_init_read_bitmap, bitmap_map_size,
        &rnd_init_write_bitmap, bitmap_map_size,
        &idx_read_bitmap, bitmap_map_size,
        &idx_write_bitmap, bitmap_map_size,
        &rnd_read_bitmap, bitmap_map_size,
        &rnd_write_bitmap, bitmap_map_size,
#if defined(HAVE_HANDLERSOCKET)
        &child_multi_range, sizeof(KEY_MULTI_RANGE) * share->table_count,
        &child_key_buff, MAX_KEY_LENGTH * share->table_count,
#endif
#ifdef VP_SUPPORT_MRR
        &m_range_info, sizeof(range_id_t) * share->table_count,
        &m_stock_range_seq, sizeof(uint) * share->table_count,
        &m_mrr_buffer, sizeof(HANDLER_BUFFER) * share->table_count,
        &m_mrr_buffer_size, sizeof(uint) * share->table_count,
        &m_child_mrr_range_length, sizeof(uint) * share->table_count,
        &m_child_mrr_range_first,
          sizeof(VP_CHILD_KEY_MULTI_RANGE *) * share->table_count,
        &m_child_mrr_range_current,
          sizeof(VP_CHILD_KEY_MULTI_RANGE *) * share->table_count,
        &m_child_key_multi_range_hld,
          sizeof(VP_CHILD_KEY_MULTI_RANGE_HLD) * share->table_count,
#endif
        NullS))
    ) {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_tables_alloc;
    }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (partition_share)
    {
      DBUG_PRINT("info",("vp copy partition_handler_share"));
/*
      partition_handler_share = (VP_PARTITION_HANDLER_SHARE *)
        partition_share->partition_handler_share;
*/
      if (part_num)
      {
        if (partition_handler_share->use_count >= part_num)
          may_be_clone = TRUE;
        else {
          partition_handler_share->handlers[
            partition_handler_share->use_count] = this;
          partition_handler_share->use_count++;
        }
      }
/*
      if (partition_handler_share->use_count == part_num)
        partition_share->partition_handler_share = NULL;
*/
      pthread_mutex_unlock(&partition_share->pt_handler_mutex);
      pt_handler_mutex = FALSE;
    }
  }
#endif
  memcpy(part_tables, share->part_tables,
    sizeof(TABLE_LIST) * share->table_count);
  memcpy(select_ignore, share->select_ignore,
    sizeof(uchar) * share->use_tables_size);
  memcpy(select_ignore_with_lock, share->select_ignore_with_lock,
    sizeof(uchar) * share->use_tables_size);
  memset((uchar *) update_ignore, 0,
    sizeof(uchar) * share->use_tables_size);
#ifdef HA_CAN_BULK_ACCESS
  memset(bulk_access_exec_bitmap, 0, sizeof(uchar) * share->use_tables_size);
#endif
  memset(idx_read_bitmap, 0, bitmap_map_size);
  memset(idx_write_bitmap, 0, bitmap_map_size);
  memset(rnd_read_bitmap, 0, bitmap_map_size);
  memset(rnd_write_bitmap, 0, bitmap_map_size);
#ifdef VP_SUPPORT_MRR
  bzero(m_mrr_buffer, share->table_count * sizeof(HANDLER_BUFFER));
  bzero(m_child_mrr_range_first,
    share->table_count * sizeof(VP_CHILD_KEY_MULTI_RANGE *));
#endif

#if MYSQL_VERSION_ID < 50500
#ifdef WITH_PARTITION_STORAGE_ENGINE
  /* for table partitioning */
  if (table->child_l)
    *(table->child_last_l) = part_tables;
#endif
#endif

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
#if MYSQL_VERSION_ID < 50500
    part_tables[roop_count].lock_type = (thr_lock_type) mode;
    if (roop_count < share->table_count - 1)
      part_tables[roop_count].next_global = &part_tables[roop_count + 1];
#endif
    part_tables[roop_count].parent_l = NULL;
#ifndef WITHOUT_VP_BG_ACCESS
    bg_base[roop_count].table_idx = roop_count;
    bg_base[roop_count].part_table = &part_tables[roop_count];
    bg_base[roop_count].parent = this;
    bg_base[roop_count].bg_init = FALSE;
    bg_base[roop_count].bg_caller_sync_wait = FALSE;
#endif
  }

#if MYSQL_VERSION_ID < 50500
#ifdef WITH_PARTITION_STORAGE_ENGINE
  /* for table partitioning */
  if (!table->child_l)
#endif
    table->child_l = part_tables;

  table->child_last_l = &part_tables[share->table_count - 1].next_global;
#else
  children_l = part_tables;
  children_last_l = &part_tables[share->table_count - 1].next_global;
#endif

  DBUG_PRINT("info",("vp blob_fields=%d", table_share->blob_fields));
  if (table_share->blob_fields)
  {
    if (!(blob_buff = new (&mr) String[table_share->fields]))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_blob_buff;
    }
    for (roop_count = 0; roop_count < (int) table_share->fields; roop_count++)
      blob_buff[roop_count].set_charset(table->field[roop_count]->charset());
  }

  child_table_idx = share->table_count;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (may_be_clone)
    is_clone = TRUE;
#endif
  if (is_clone)
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (part_num)
    {
      for (roop_count = 0; roop_count < (int) part_num; roop_count++)
      {
        if (((ha_vp *) partition_handler_share->handlers[roop_count])->share ==
          share)
        {
          pt_clone_source_handler =
            (ha_vp *) partition_handler_share->handlers[roop_count];
          break;
        }
      }
    }
#endif

    sql_command = pt_clone_source_handler->sql_command;
    lock_type_sto = pt_clone_source_handler->lock_type_sto;
    lock_mode = pt_clone_source_handler->lock_mode;
    update_request = pt_clone_source_handler->update_request;

    pt_clone_source_handler->init_select_column(FALSE);
    clone_init_select_column();

#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (
      partition_handler_share->clone_partition_handler_share &&
      partition_handler_share->clone_partition_handler_share->use_count <
        part_num
    ) {
      clone_partition_handler_share =
        partition_handler_share->clone_partition_handler_share;
      clone_partition_handler_share->handlers[
        clone_partition_handler_share->use_count] = this;
      clone_partition_handler_share->use_count++;
#endif
      if (!(clone_tables = (TABLE *)
        my_multi_malloc(MYF(MY_WME),
          &clone_tables, sizeof(TABLE) * share->table_count,
          NullS))
      ) {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error_clone_tables_alloc;
      }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    } else {
      if (!(clone_tables = (TABLE *)
        my_multi_malloc(MYF(MY_WME),
          &clone_tables, sizeof(TABLE) * share->table_count,
          &clone_partition_handler_share,
            sizeof(VP_CLONE_PARTITION_HANDLER_SHARE),
          &pt_handler_share_handlers, sizeof(ha_vp *) * part_num,
          &tmp_idx_read_bitmap, bitmap_map_size,
          &tmp_idx_write_bitmap, bitmap_map_size,
          NullS))
      ) {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error_clone_tables_alloc;
      }
      clone_partition_handler_share->use_count = 1;
      clone_partition_handler_share->handlers =
        (void **) pt_handler_share_handlers;
      clone_partition_handler_share->idx_read_bitmap = tmp_idx_read_bitmap;
      clone_partition_handler_share->idx_write_bitmap = tmp_idx_write_bitmap;
      clone_partition_handler_share->idx_bitmap_is_set = FALSE;
      clone_partition_handler_share->handlers[0] = this;
      partition_handler_share->clone_partition_handler_share =
        clone_partition_handler_share;
    }
#endif
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      part_tables[roop_count].table =
        pt_clone_source_handler->part_tables[roop_count].table;
      clear_child_bitmap(roop_count);
      set_child_bitmap((uchar *) idx_init_write_bitmap, roop_count, TRUE);
      set_child_bitmap((uchar *) idx_init_read_bitmap, roop_count, FALSE);

      part_tables[roop_count].table = &clone_tables[roop_count];
      memcpy(part_tables[roop_count].table,
        pt_clone_source_handler->part_tables[roop_count].table, sizeof(TABLE));
      if (!(part_tables[roop_count].table->file = pt_clone_source_handler->
        part_tables[roop_count].table->file->clone(
          part_tables[roop_count].table->s->normalized_path.str,
          thd->mem_root)))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error_clone;
      }
    }

    table_lock_count = pt_clone_source_handler->table_lock_count;
    child_ref_length = pt_clone_source_handler->child_ref_length;
    ref_length = (child_ref_length * share->table_count) +
      sizeof(ha_vp *) +
      table->key_info[table_share->primary_key].key_length;
    additional_table_flags = pt_clone_source_handler->additional_table_flags;

    ins_child_bitmaps[0] = pt_clone_source_handler->ins_child_bitmaps[0];
    ins_child_bitmaps[1] = pt_clone_source_handler->ins_child_bitmaps[1];
    upd_child_bitmaps[0] = pt_clone_source_handler->upd_child_bitmaps[0];
    upd_child_bitmaps[1] = pt_clone_source_handler->upd_child_bitmaps[1];
    del_child_bitmaps[0] = pt_clone_source_handler->del_child_bitmaps[0];
    del_child_bitmaps[1] = pt_clone_source_handler->del_child_bitmaps[1];
    add_from_child_bitmaps[0] =
      pt_clone_source_handler->add_from_child_bitmaps[0];
    add_from_child_bitmaps[1] =
      pt_clone_source_handler->add_from_child_bitmaps[1];
    sel_key_init_child_bitmaps[0] =
      pt_clone_source_handler->sel_key_init_child_bitmaps[0];
    sel_key_init_child_bitmaps[1] =
      pt_clone_source_handler->sel_key_init_child_bitmaps[1];
    sel_key_child_bitmaps[0] =
      pt_clone_source_handler->sel_key_child_bitmaps[0];
    sel_key_child_bitmaps[1] =
      pt_clone_source_handler->sel_key_child_bitmaps[1];
    sel_rnd_child_bitmaps[0] =
      pt_clone_source_handler->sel_rnd_child_bitmaps[0];
    sel_rnd_child_bitmaps[1] =
      pt_clone_source_handler->sel_rnd_child_bitmaps[1];

    cached_table_flags = table_flags();
  }
  if (reset())
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }
#ifndef DBUG_OFF
  tmp_table = thd->open_tables;
  while (tmp_table)
  {
    DBUG_PRINT("info",("vp tmp_table=%p", tmp_table));
    DBUG_PRINT("info",("vp tmp_table->file=%p", tmp_table->file));
    if (tmp_table->file)
      DBUG_PRINT("info",("vp tmp_table->file->inited=%x",
        tmp_table->file->inited));
    tmp_table = tmp_table->next;
  }
#endif
  child_multi_range_first = NULL;

  DBUG_RETURN(0);

error:
error_clone:
  if (clone_tables)
    vp_my_free(clone_tables, MYF(0));
error_clone_tables_alloc:
error_init_blob_buff:
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (
    partition_handler_share &&
    pt_handler_share_creator == this
  ) {
    partition_share = share->partition_share;
    if (!pt_handler_mutex)
      pthread_mutex_lock(&partition_share->pt_handler_mutex);
/*
    if (partition_share->partition_handler_share == partition_handler_share)
      partition_share->partition_handler_share = NULL;
*/
    my_hash_delete(&partition_share->pt_handler_hash,
      (uchar*) partition_handler_share);
    pthread_mutex_unlock(&partition_share->pt_handler_mutex);
    pt_handler_mutex = FALSE;
  }
error_hash_insert:
  partition_handler_share = NULL;
  pt_handler_share_creator = NULL;
#endif
  vp_my_free(part_tables, MYF(0));
  part_tables = NULL;
#ifdef VP_SUPPORT_MRR
  m_range_info = NULL;
#endif
error_tables_alloc:
  if (pt_handler_mutex)
    pthread_mutex_unlock(&partition_share->pt_handler_mutex);
  vp_free_share(share);
  share = NULL;
error_get_share:
  DBUG_RETURN(error_num);
}

int ha_vp::close()
{
#ifndef WITHOUT_VP_BG_ACCESS
  int roop_count;
  VP_BG_BASE *base;
#endif
#ifdef WITH_PARTITION_STORAGE_ENGINE
  VP_PARTITION_SHARE *partition_share;
#endif
  DBUG_ENTER("ha_vp::close");
  DBUG_PRINT("info",("vp this=%p", this));

#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_info_first)
  {
    do {
      bulk_access_info_current = bulk_access_info_first->next;
      delete_bulk_access_info(bulk_access_info_first);
      bulk_access_info_first = bulk_access_info_current;
    } while (bulk_access_info_first);
  }
#endif
#ifdef VP_SUPPORT_MRR
  int i;
  if (m_child_mrr_range_first)
  {
    for (i = 0; i < share->table_count; i++)
    {
      if (m_child_mrr_range_first[i])
      {
        VP_CHILD_KEY_MULTI_RANGE *tmp_mrr_range_first =
          m_child_mrr_range_first[i];
        VP_CHILD_KEY_MULTI_RANGE *tmp_mrr_range_current;
        do {
          tmp_mrr_range_current = tmp_mrr_range_first;
          tmp_mrr_range_first = tmp_mrr_range_first->next;
          vp_my_free(tmp_mrr_range_current, MYF(0));
        } while (tmp_mrr_range_first);
      }
    }
    m_child_mrr_range_first = NULL;
  }
  if (m_mrr_range_first)
  {
    do {
      m_mrr_range_current = m_mrr_range_first;
      m_mrr_range_first = m_mrr_range_first->next;
      if (m_mrr_range_current->key[0])
        vp_my_free(m_mrr_range_current->key[0], MYF(0));
      if (m_mrr_range_current->key[1])
        vp_my_free(m_mrr_range_current->key[1], MYF(0));
      vp_my_free(m_mrr_range_current, MYF(0));
    } while (m_mrr_range_first);
  }
  if (m_mrr_full_buffer)
  {
    vp_my_free(m_mrr_full_buffer, MYF(0));
    m_mrr_full_buffer = NULL;
    m_mrr_full_buffer_size = 0;
  }
#endif
  if (is_clone)
  {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
#ifdef VP_HANDLER_HAS_HA_CLOSE
      part_tables[roop_count].table->file->ha_close();
#else
      part_tables[roop_count].table->file->close();
#endif
    }
    vp_my_free(part_tables[0].table, MYF(0));
  }

#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    base = &bg_base[roop_count];
    free_bg_thread(base);
  }
#endif
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  if (allocated_top_table_fields)
  {
    vp_my_free(top_table_field_for_childs[0], MYF(0));
    allocated_top_table_fields = 0;
  }
#endif
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (
    partition_handler_share &&
    pt_handler_share_creator == this
  ) {
    partition_share = share->partition_share;
    pthread_mutex_lock(&partition_share->pt_handler_mutex);
/*
    if (partition_share->partition_handler_share == partition_handler_share)
      partition_share->partition_handler_share = NULL;
*/
    my_hash_delete(&partition_share->pt_handler_hash,
      (uchar*) partition_handler_share);
    pthread_mutex_unlock(&partition_share->pt_handler_mutex);
  }
  partition_handler_share = NULL;
  pt_handler_share_creator = NULL;
  clone_partition_handler_share = NULL;
#endif
  if (part_tables)
  {
    vp_my_free(part_tables, MYF(0));
    part_tables = NULL;
#ifdef VP_SUPPORT_MRR
    m_range_info = NULL;
#endif
  }
  if (ref_buf)
  {
    vp_my_free(ref_buf, MYF(0));
    ref_buf = NULL;
  }
  if (share)
  {
    vp_free_share(share);
    share = NULL;
  }
#if MYSQL_VERSION_ID < 50500
  table->child_l = NULL;
#else
  children_l = NULL;
#endif
  if (!is_clone)
    free_child_bitmap_buff();
  is_clone = FALSE;
  pt_clone_source_handler = NULL;
  while (ft_first)
  {
    ft_current = ft_first;
    ft_first = ft_current->next;
    vp_my_free(ft_current, MYF(0));
  }
  ft_current = NULL;
  if (child_multi_range_first)
  {
    vp_my_free(child_multi_range_first, MYF(0));
    child_multi_range_first = NULL;
  }
  if (mr_init)
  {
    free_root(&mr, MYF(0));
    mr_init = FALSE;
  }
  DBUG_RETURN(0);
}

uint ha_vp::lock_count() const
{
  DBUG_ENTER("ha_vp::lock_count");
#if MYSQL_VERSION_ID < 50500
  DBUG_RETURN(table_lock_count);
#else
  if (
    thd_sql_command(ha_thd()) == SQLCOM_HA_OPEN ||
    thd_sql_command(ha_thd()) == SQLCOM_HA_READ
  ) {
    DBUG_PRINT("info",("vp table_lock_count=%u", table_lock_count));
    DBUG_RETURN(table_lock_count);
  }
  DBUG_RETURN(0);
#endif
}

#ifdef HA_CAN_BULK_ACCESS
int ha_vp::additional_lock(
  THD *thd,
  enum thr_lock_type lock_type
) {
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::additional_lock");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num = part_tables[roop_count].table->file->additional_lock(thd,
      lock_type)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}
#endif

THR_LOCK_DATA **ha_vp::store_lock(
  THD *thd,
  THR_LOCK_DATA **to,
  enum thr_lock_type lock_type
) {
  int roop_count;
  DBUG_ENTER("ha_vp::store_lock");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp part_tables[0].table=%p", part_tables[0].table));
  sql_command = thd_sql_command(thd);
  DBUG_PRINT("info",("vp sql_command=%u", sql_command));
  lock_type_sto = lock_type;
  DBUG_PRINT("info",("vp lock_type=%u", lock_type));
  switch (sql_command)
  {
    case SQLCOM_SELECT:
      if (lock_type == TL_READ_WITH_SHARED_LOCKS)
        lock_mode = 1;
      else if (lock_type <= TL_READ_NO_INSERT)
        lock_mode = 0;
      else
        lock_mode = -1;
      break;
    case SQLCOM_CREATE_TABLE:
    case SQLCOM_UPDATE:
    case SQLCOM_INSERT:
    case SQLCOM_INSERT_SELECT:
    case SQLCOM_DELETE:
    case SQLCOM_LOAD:
    case SQLCOM_REPLACE:
    case SQLCOM_REPLACE_SELECT:
    case SQLCOM_DELETE_MULTI:
    case SQLCOM_UPDATE_MULTI:
      if (lock_type >= TL_READ && lock_type <= TL_READ_NO_INSERT)
        lock_mode = 1;
      else
        lock_mode = -1;
      break;
    default:
      lock_mode = -1;
      break;
  }
#if MYSQL_VERSION_ID < 50500
#else
  if (sql_command == SQLCOM_HA_OPEN || sql_command == SQLCOM_HA_READ)
  {
#endif
    if (table_lock_count > 0)
    {
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        to = part_tables[roop_count].table->file->store_lock(thd, to,
          lock_type);
        DBUG_PRINT("info",("vp lock->type=%u", to ? (*(to - 1))->type : 0));
      }
    }
#if MYSQL_VERSION_ID < 50500
#else
  }
#endif
  DBUG_RETURN(to);
}

int ha_vp::external_lock(
  THD *thd,
  int lock_type
) {
  int error_num, error_num2, roop_count;
  DBUG_ENTER("ha_vp::external_lock");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp part_tables[0].table=%p", part_tables[0].table));
  DBUG_PRINT("info",("vp lock_type=%x", lock_type));
  sql_command = thd_sql_command(thd);
  if (
    /* SQLCOM_RENAME_TABLE and SQLCOM_DROP_DB don't come here */
    sql_command == SQLCOM_DROP_TABLE ||
    sql_command == SQLCOM_ALTER_TABLE
  ) {
    if (store_error_num)
      DBUG_RETURN(store_error_num);
    DBUG_RETURN(0);
  }
#if MYSQL_VERSION_ID < 50500
  DBUG_PRINT("info",("vp thd->options=%x", (int) thd->options));
  if (table_lock_count == 0)
#else
  if (!children_attached && !is_clone && lock_type != F_UNLCK)
#endif
  {
    my_error(ER_WRONG_OBJECT, MYF(0), table->s->db.str,
      table->s->table_name.str, "BASE TABLE");
    DBUG_RETURN(ER_WRONG_OBJECT);
  }
  if (store_error_num)
    DBUG_RETURN(store_error_num);

#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  if (
    !set_top_table_fields &&
    (error_num = set_top_table_and_fields(table, table->field,
      table_share->fields, TRUE))
  )
    DBUG_RETURN(error_num);

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num = part_tables[roop_count].table->file->
      set_top_table_and_fields(
      top_table,
      top_table_field_for_childs[roop_count],
      top_table_fields)))
      DBUG_RETURN(error_num);
  }
#endif

  lock_type_ext = lock_type;
  if (lock_type == F_WRLCK)
    update_request = TRUE;
  else
    update_request = FALSE;
  error_num = 0;
#if MYSQL_VERSION_ID < 50500
#else
  if (
    is_clone ||
    sql_command == SQLCOM_HA_READ
  ) {
#endif
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if ((error_num2 =
        part_tables[roop_count].table->file->ha_external_lock(thd, lock_type)))
      {
        if (lock_type != F_UNLCK)
          goto error_external_lock;
        else
          error_num = error_num2;
      }
    }
#if MYSQL_VERSION_ID < 50500
#else
  }
#endif
  DBUG_RETURN(error_num);

error_external_lock:
#if MYSQL_VERSION_ID < 50500
#else
  if (
    is_clone ||
    sql_command == SQLCOM_HA_READ
  ) {
#endif
    while (roop_count)
    {
      roop_count--;
      part_tables[roop_count].table->file->ha_external_lock(thd, F_UNLCK);
    }
#if MYSQL_VERSION_ID < 50500
#else
  }
#endif
  DBUG_RETURN(error_num2);
}

int ha_vp::reset()
{
  int error_num, error_num2, roop_count;
  VP_CONDITION *tmp_cond;
  DBUG_ENTER("ha_vp::reset");
  DBUG_PRINT("info",("vp this=%p", this));
  bulk_insert = FALSE;
  init_sel_key_init_bitmap = FALSE;
  init_sel_key_bitmap = FALSE;
  init_sel_rnd_bitmap = FALSE;
  init_ins_bitmap = FALSE;
  init_upd_bitmap = FALSE;
  init_del_bitmap = FALSE;
  cb_state = CB_NO_SET;
  child_keyread = FALSE;
#ifdef HA_EXTRA_HAS_STARTING_ORDERED_INDEX_SCAN
  extra_use_cmp_ref = FALSE;
#else
  extra_use_cmp_ref = TRUE;
#endif
  rnd_scan = FALSE;
  child_table_idx = 0;
  error_num = 0;
  store_error_num = 0;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (partition_handler_share)
  {
    if (!is_clone)
    {
      partition_handler_share->idx_init_flg = FALSE;
      partition_handler_share->clone_partition_handler_share = NULL;
    } else
      clone_partition_handler_share->idx_bitmap_is_set = FALSE;
    partition_handler_share->rnd_init_flg = FALSE;
    partition_handler_share->idx_bitmap_is_set = FALSE;
    partition_handler_share->rnd_bitmap_is_set = FALSE;
  }
#endif
  if (!is_clone)
    idx_bitmap_init_flg = FALSE;
  rnd_bitmap_init_flg = FALSE;
  idx_bitmap_is_set = FALSE;
  rnd_bitmap_is_set = FALSE;
#if MYSQL_VERSION_ID < 50500
  if (table->children_attached || is_clone)
#else
  if (children_attached || is_clone)
#endif
  {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if ((error_num2 = part_tables[roop_count].table->file->ha_reset()))
        error_num = error_num2;
    }
  }
  while (condition)
  {
    tmp_cond = condition->next;
    vp_my_free(condition, MYF(0));
    condition = tmp_cond;
  }
  memset(pruned_tables, 0, sizeof(uchar) * share->use_tables_size);
  pruned = FALSE;
  ft_current = NULL;
  ft_inited = FALSE;
  ft_count = 0;
  use_pre_call = FALSE;

#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_info_first)
  {
    VP_BULK_ACCESS_INFO *bulk_access_info = bulk_access_info_first;
    while (
      bulk_access_info &&
      bulk_access_info->used
    ) {
      bulk_access_info->idx_bitmap_init_flg = FALSE;
      bulk_access_info->rnd_bitmap_init_flg = FALSE;
      bulk_access_info->idx_bitmap_is_set = FALSE;
      bulk_access_info->rnd_bitmap_is_set = FALSE;
      bulk_access_info->child_keyread = FALSE;
      bulk_access_info->single_table = FALSE;
      bulk_access_info->set_used_table = FALSE;
      bulk_access_info->init_sel_key_init_bitmap = FALSE;
      bulk_access_info->init_sel_key_bitmap = FALSE;
      bulk_access_info->init_sel_rnd_bitmap = FALSE;
      bulk_access_info->init_ins_bitmap = FALSE;
      bulk_access_info->used = FALSE;
#ifdef WITH_PARTITION_STORAGE_ENGINE
      if (partition_handler_share && partition_handler_share->creator == this)
      {
        VP_PARTITION_HANDLER_SHARE *tmp_partition_handler_share =
          bulk_access_info->partition_handler_share;
        if (tmp_partition_handler_share)
        {
          tmp_partition_handler_share->idx_init_flg = FALSE;
          tmp_partition_handler_share->rnd_init_flg = FALSE;
          tmp_partition_handler_share->idx_bitmap_is_set = FALSE;
          tmp_partition_handler_share->rnd_bitmap_is_set = FALSE;
        }
        VP_CLONE_PARTITION_HANDLER_SHARE *tmp_clone_partition_handler_share =
          bulk_access_info->clone_partition_handler_share;
        if (tmp_clone_partition_handler_share)
        {
          tmp_clone_partition_handler_share->idx_bitmap_is_set = FALSE;
        }
      }
#endif
      bulk_access_info = bulk_access_info->next;
    }
    memset(bulk_access_exec_bitmap, 0, sizeof(uchar) * share->use_tables_size);
  }
  bulk_access_started = FALSE;
  bulk_access_executing = FALSE;
  bulk_access_pre_called = FALSE;
  bulk_access_info_current = NULL;
  bulk_access_info_exec_tgt = NULL;
#endif
#ifdef VP_SUPPORT_MRR
  m_mrr_new_full_buffer_size = 0;
#endif
#ifdef HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
  handler_close = FALSE;
#endif
  DBUG_RETURN(error_num);
}

int ha_vp::extra(
  enum ha_extra_function operation
) {
  int error_num, error_num2, roop_count;
  TABLE_LIST *part_table;
  bool reinit;
  longlong additional_table_flags_for_neg;
#if MYSQL_VERSION_ID < 50500
#else
  THD *thd;
  TABLE_LIST *tmp_table_list;
#endif
  DBUG_ENTER("ha_vp::extra");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp operation=%d", (int) operation));
  switch (operation)
  {
    case HA_EXTRA_CACHE:
      DBUG_PRINT("info",("vp HA_EXTRA_CACHE"));
      if (child_table_idx < share->table_count)
      {
        if ((error_num =
          part_tables[child_table_idx].table->file->extra(operation)))
          DBUG_RETURN(error_num);
      } else {
        if ((error_num =
          part_tables[0].table->file->extra(operation)))
          DBUG_RETURN(error_num);
      }
      break;
    case HA_EXTRA_KEYREAD:
      DBUG_PRINT("info",("vp HA_EXTRA_KEYREAD"));
      break;
    case HA_EXTRA_NO_KEYREAD:
      DBUG_PRINT("info",("vp HA_EXTRA_NO_KEYREAD"));
      break;
    case HA_EXTRA_ATTACH_CHILDREN:
      DBUG_PRINT("info",("vp HA_EXTRA_ATTACH_CHILDREN"));
      if (!is_clone)
      {
        reinit = FALSE;
        table_lock_count = 0;
        child_ref_length = 0;
        key_used_on_scan = MAX_KEY;
        additional_table_flags = vp_base_table_flags;
        if (share->same_all_columns)
          additional_table_flags_for_neg = 0;
        else
          additional_table_flags_for_neg = HA_PARTIAL_COLUMN_READ;
        for (roop_count = 0; roop_count < share->table_count; roop_count++)
        {
          part_table = &part_tables[roop_count];
          DBUG_PRINT("info",("vp part_tables[%d].table=%p", roop_count,
            part_table->table));
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
          if (set_top_table_fields)
            clear_top_table_fields();
          part_table->table->file->clear_top_table_fields();
#endif
          if ((error_num = part_table->table->file->extra(operation)))
            DBUG_RETURN(error_num);
          DBUG_PRINT("info",("vp part_tables[%d] lock_count=%u", roop_count,
            part_table->table->file->lock_count()));
          table_lock_count += part_table->table->file->lock_count();
          if (child_ref_length < part_table->table->file->ref_length)
          {
            child_ref_length = part_table->table->file->ref_length;
          }
#if MYSQL_VERSION_ID < 50500
          if (part_table->get_child_def_version() !=
            part_table->table->s->get_table_def_version())
          {
            reinit = TRUE;
            part_table->set_child_def_version(
              part_table->table->s->get_table_def_version());
          }
#else
          DBUG_PRINT("info",("vp init=%s", share->init ? "TRUE" : "FALSE"));
          if (!share->init ||
            !init_correspond_columns ||
            children_info[roop_count].child_table_ref_type !=
              part_table->table->s->get_table_ref_type() ||
            children_info[roop_count].child_def_version !=
              part_table->table->s->get_table_def_version()
          ) {
            reinit = TRUE;
            children_info[roop_count].child_table_ref_type =
              part_table->table->s->get_table_ref_type();
            children_info[roop_count].child_def_version =
              part_table->table->s->get_table_def_version();
          }
#endif
          additional_table_flags &=
            part_table->table->file->ha_table_flags();
          additional_table_flags_for_neg |=
            (part_table->table->file->ha_table_flags() &
              (HA_PARTIAL_COLUMN_READ | HA_PRIMARY_KEY_IN_READ_INDEX));
          if (key_used_on_scan > part_table->table->file->key_used_on_scan)
            key_used_on_scan = part_table->table->file->key_used_on_scan;
        }
        ref_length = (child_ref_length * share->table_count) +
          sizeof(ha_vp *) +
          table->key_info[table_share->primary_key].key_length;
        if (ref_length > ref_buf_length)
        {
          if (ref_buf)
            vp_my_free(ref_buf, MYF(0));
          if (!(ref_buf =
            (uchar*) my_malloc(ALIGN_SIZE(ref_length) * 2, MYF(MY_WME))))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          ref_buf_length = ref_length;
        }
        ref = ref_buf;
        dup_ref = ref + ALIGN_SIZE(ref_length);
        DBUG_PRINT("info",("vp ref_length=%u", ref_length));
        additional_table_flags |= additional_table_flags_for_neg;
        if (!share->init || reinit)
        {
          free_child_bitmap_buff();
          if (
            (error_num = create_child_bitmap_buff()) ||
            (error_num =
              vp_correspond_columns(this, table, share, table_share,
              part_tables, reinit))
          )
            DBUG_RETURN(error_num);
        }
        cached_table_flags = table_flags();
#if MYSQL_VERSION_ID < 50500
#else
        children_attached = TRUE;
        init_correspond_columns = TRUE;
#endif
      }
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (vp_bit_is_set(share->same_columns, roop_count))
        {
          TABLE *child_table = part_tables[roop_count].table;
          bool done_already =
            (child_table->record[0] == table->record[0]);
          if (!done_already)
          {
            my_ptrdiff_t ptr_diff =
              PTR_BYTE_DIFF(table->record[0], child_table->record[0]);
            child_record0[roop_count] = child_table->record[0];
            child_table->record[0] = table->record[0];
            child_record1[roop_count] = child_table->record[1];
            child_table->record[1] = table->record[1];
            Field **field = child_table->field;
            for (; *field; field++)
              (*field)->move_field_offset(ptr_diff);
          }
          if ((error_num = child_table->file->
            extra(HA_EXTRA_INIT_AFTER_ATTACH_CHILDREN))
          ) {
            DBUG_RETURN(error_num);
          }
        }
      }
      if (
        !table->pos_in_table_list->parent_l &&
        (error_num =
          info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST))
      ) {
        for (roop_count = 0; roop_count < share->table_count; roop_count++)
        {
          if (vp_bit_is_set(share->same_columns, roop_count))
          {
            TABLE *child_table = part_tables[roop_count].table;
            bool done_already =
              (child_table->record[0] == child_record0[roop_count]);
            if (!done_already)
            {
              child_table->record[0] = child_record0[roop_count];
              child_table->record[1] = child_record1[roop_count];
              my_ptrdiff_t ptr_diff =
                PTR_BYTE_DIFF(table->record[0], child_table->record[0]);
              Field **field = child_table->field;
              for (; *field; field++)
                (*field)->move_field_offset(-ptr_diff);
            }
          }
        }
        DBUG_RETURN(error_num);
      }
      break;
    case HA_EXTRA_DETACH_CHILDREN:
      DBUG_PRINT("info",("vp HA_EXTRA_DETACH_CHILDREN"));
#if MYSQL_VERSION_ID < 50500
      if (table->children_attached)
#else
      if (children_attached)
#endif
      {
        for (roop_count = 0; roop_count < share->table_count; roop_count++)
        {
          if (vp_bit_is_set(share->same_columns, roop_count))
          {
            TABLE *child_table = part_tables[roop_count].table;
            bool done_already =
              (child_table->record[0] == child_record0[roop_count]);
            if (!done_already)
            {
              child_table->record[0] = child_record0[roop_count];
              child_table->record[1] = child_record1[roop_count];
              my_ptrdiff_t ptr_diff =
                PTR_BYTE_DIFF(table->record[0], child_table->record[0]);
              Field **field = child_table->field;
              for (; *field; field++)
                (*field)->move_field_offset(-ptr_diff);
            }
          }
        }
      }
#if MYSQL_VERSION_ID < 50500
      if (table->children_attached && !is_clone)
#else
      if (children_attached && !is_clone)
#endif
      {
        error_num = 0;
        additional_table_flags = vp_base_table_flags;
        cached_table_flags = table_flags();
        for (roop_count = 0; roop_count < share->table_count; roop_count++)
        {
          TABLE *child_table = part_tables[roop_count].table;
#if MYSQL_VERSION_ID < 50500
#else
          if (child_table)
          {
#endif
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
            child_table->file->clear_top_table_fields();
#endif
            if ((error_num2 = child_table->file->extra(operation)))
              error_num = error_num2;
#if MYSQL_VERSION_ID < 50500
#else
          }
#endif
        }
#if MYSQL_VERSION_ID < 50500
        table->children_attached = FALSE;
#else
        children_attached = FALSE;
#ifdef HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
        if (!handler_close)
        {
#endif
          thd = ha_thd();
          tmp_table_list = table->pos_in_table_list;
          tmp_table_list->next_global = *children_last_l;
          if (*children_last_l)
            (*children_last_l)->prev_global = &tmp_table_list->next_global;
          if (thd->lex->query_tables_last == children_last_l)
            thd->lex->query_tables_last = &tmp_table_list->next_global;
          if (thd->lex->query_tables_own_last == children_last_l)
            thd->lex->query_tables_own_last = &tmp_table_list->next_global;
#ifdef HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
        }
#endif
#endif
        if (error_num)
          DBUG_RETURN(error_num);
      }
      break;
#if MYSQL_VERSION_ID < 50500
#else
    case HA_EXTRA_ADD_CHILDREN_LIST:
      DBUG_PRINT("info",("vp HA_EXTRA_ADD_CHILDREN_LIST"));
      thd = ha_thd();
      tmp_table_list = table->pos_in_table_list;
      DBUG_PRINT("info",("vp tmp_table_list=%p", tmp_table_list));
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        part_tables[roop_count].parent_l = tmp_table_list;
        if (roop_count == 0)
          part_tables[roop_count].prev_global = &tmp_table_list->next_global;
        else
          part_tables[roop_count].prev_global =
            &part_tables[roop_count - 1].next_global;
        part_tables[roop_count].next_global = part_tables + roop_count + 1;
        part_tables[roop_count].select_lex = tmp_table_list->select_lex;

        part_tables[roop_count].table = NULL;
        part_tables[roop_count].lock_type = tmp_table_list->lock_type;
        part_tables[roop_count].mdl_request.init(
          MDL_key::TABLE, VP_TABLE_LIST_db_str(&part_tables[roop_count]),
          VP_TABLE_LIST_table_name_str(&part_tables[roop_count]),
          (tmp_table_list->lock_type >= TL_WRITE_ALLOW_WRITE) ?
            MDL_SHARED_WRITE : MDL_SHARED_READ,
          MDL_TRANSACTION);

        if (!thd->locked_tables_mode &&
          tmp_table_list->mdl_request.type == MDL_SHARED_NO_WRITE)
          part_tables[roop_count].mdl_request.set_type(MDL_SHARED_NO_WRITE);
      }

      if (tmp_table_list->next_global)
      {
        DBUG_PRINT("info",("vp parent->next_global=%p",
          tmp_table_list->next_global));
        tmp_table_list->next_global->prev_global = children_last_l;
      }
      *children_last_l = tmp_table_list->next_global;
      tmp_table_list->next_global = part_tables;

      if (thd->lex->query_tables_last == &tmp_table_list->next_global)
        thd->lex->query_tables_last = children_last_l;
      if (thd->lex->query_tables_own_last == &tmp_table_list->next_global)
        thd->lex->query_tables_own_last = children_last_l;
      break;
    case HA_EXTRA_IS_ATTACHED_CHILDREN:
      DBUG_PRINT("info",("vp HA_EXTRA_IS_ATTACHED_CHILDREN"));
      DBUG_RETURN(is_clone || children_attached);
#endif
#ifdef HA_EXTRA_HAS_STARTING_ORDERED_INDEX_SCAN
    case HA_EXTRA_STARTING_ORDERED_INDEX_SCAN:
      DBUG_PRINT("info",("vp HA_EXTRA_STARTING_ORDERED_INDEX_SCAN"));
      extra_use_cmp_ref = TRUE;
      add_pk_bitmap_to_child();
      break;
#endif
    default:
      DBUG_PRINT("info",("vp default"));
#if MYSQL_VERSION_ID < 50500
      if (table->children_attached || is_clone)
#else
      if (children_attached || is_clone)
#endif
      {
        for (roop_count = 0; roop_count < share->table_count; roop_count++)
        {
          if ((error_num =
            part_tables[roop_count].table->file->extra(operation)))
            DBUG_RETURN(error_num);
        }
      }
      break;
  }
  DBUG_RETURN(0);
}

int ha_vp::extra_opt(
  enum ha_extra_function operation,
  ulong cachesize
) {
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::extra_opt");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp operation=%d", (int) operation));
  DBUG_PRINT("info",("vp cachesize=%lu", cachesize));
  switch (operation)
  {
    case HA_EXTRA_CACHE:
      if (child_table_idx < share->table_count)
      {
        if ((error_num =
          part_tables[child_table_idx].table->file->extra_opt(operation,
            cachesize)))
          DBUG_RETURN(error_num);
      } else {
        if ((error_num =
          part_tables[0].table->file->extra_opt(operation,
            cachesize)))
          DBUG_RETURN(error_num);
      }
      break;
    default:
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if ((error_num =
          part_tables[roop_count].table->file->extra_opt(operation,
            cachesize)))
          DBUG_RETURN(error_num);
      }
      break;
  }
  DBUG_RETURN(0);
}

int ha_vp::index_init(
  uint idx,
  bool sorted
) {
  int error_num, roop_count;
  TABLE *child_table;
  DBUG_ENTER("ha_vp::index_init");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp part_tables[0].table=%p", part_tables[0].table));
  DBUG_PRINT("info",("vp table=%p", table));
  DBUG_PRINT("info",("vp idx=%u", idx));
#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
  if (!table->pos_in_table_list->parent_l && lock_type_ext == F_WRLCK)
  {
    check_and_set_bitmap_for_update(FALSE);
  }
#endif
  init_select_column(FALSE);
  DBUG_PRINT("info",("vp init_sel_key_init_bitmap=%s",
    init_sel_key_init_bitmap ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  memset(key_inited_tables, 0, sizeof(uchar) * share->use_tables_size);
  memset(pruned_tables, 0, sizeof(uchar) * share->use_tables_size);
  pruned = FALSE;
  active_index = idx;
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_executing && bulk_access_info_exec_tgt->called)
  {
    memcpy(use_tables, bulk_access_info_exec_tgt->sel_key_init_use_tables,
      sizeof(uchar) * share->use_tables_size);
    child_keyread = bulk_access_info_exec_tgt->child_keyread;
    single_table = bulk_access_info_exec_tgt->single_table;
    set_used_table = bulk_access_info_exec_tgt->set_used_table;
    child_table_idx = bulk_access_info_exec_tgt->child_table_idx;
    child_key_idx = bulk_access_info_exec_tgt->child_key_idx;
  } else
#endif
  if (!init_sel_key_init_bitmap)
  {
    memset(use_tables, 0, sizeof(uchar) * share->use_tables_size);
    child_keyread = FALSE;
    single_table = FALSE;
    set_used_table = FALSE;
    child_table_idx = share->table_count;
    child_key_idx = MAX_KEY;
    if (
      (lock_mode > 0 || lock_type_ext == F_WRLCK) &&
      (sql_command == SQLCOM_UPDATE || sql_command == SQLCOM_UPDATE_MULTI)
    ) {
      if (check_partitioned())
      {
        /* need all columns */
        for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        {
          idx_init_read_bitmap[roop_count] = ~((uchar) 0);
          idx_init_write_bitmap[roop_count] = ~((uchar) 0);
        }
      } else if (share->zero_record_update_mode)
      {
        for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        {
          idx_init_read_bitmap[roop_count] |=
            share->cpy_clm_bitmap[roop_count];
          idx_init_write_bitmap[roop_count] |=
            share->cpy_clm_bitmap[roop_count];
        }
      }
    }
    memcpy(work_bitmap3, idx_init_read_bitmap,
      sizeof(uchar) * share->bitmap_size);
    memcpy(work_bitmap4, idx_init_write_bitmap,
      sizeof(uchar) * share->bitmap_size);

    if (
      (error_num = choose_child_index(idx, work_bitmap3, work_bitmap4,
        &child_table_idx, &child_key_idx)) ||
      (error_num = choose_child_ft_tables(work_bitmap3, work_bitmap4)) ||
      (!single_table && !ft_correspond_flag &&
      (error_num = choose_child_tables(work_bitmap3, work_bitmap4)))
    ) {
      DBUG_RETURN(error_num);
    }
    set_child_pt_bitmap();
    memcpy(sel_key_init_use_tables, use_tables,
      sizeof(uchar) * share->use_tables_size);
  } else if (cb_state != CB_SEL_KEY_INIT)
    memcpy(use_tables, sel_key_init_use_tables,
      sizeof(uchar) * share->use_tables_size);
  DBUG_PRINT("info",("vp child_table_idx=%d", child_table_idx));
  DBUG_PRINT("info",("vp child_key_idx=%d", child_key_idx));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(use_tables, roop_count))
    {
#ifdef HA_CAN_BULK_ACCESS
      if (bulk_access_executing && bulk_access_info_exec_tgt->called)
      {
        child_table = part_tables[roop_count].table;
        memcpy(child_table->read_set->bitmap,
          bulk_access_info_exec_tgt->sel_key_init_child_bitmaps[0][roop_count],
          child_table->s->column_bitmap_size);
        memcpy(child_table->write_set->bitmap,
          bulk_access_info_exec_tgt->sel_key_init_child_bitmaps[1][roop_count],
          child_table->s->column_bitmap_size);
        DBUG_PRINT("info",("vp table_count=%d", roop_count));
        DBUG_PRINT("info",("vp init child table bitmaps"));
      } else
#endif
      if (init_sel_key_init_bitmap)
      {
        if (cb_state != CB_SEL_KEY_INIT)
        {
          child_table = part_tables[roop_count].table;
          memcpy(child_table->read_set->bitmap,
            sel_key_init_child_bitmaps[0][roop_count],
            child_table->s->column_bitmap_size);
          memcpy(child_table->write_set->bitmap,
            sel_key_init_child_bitmaps[1][roop_count],
            child_table->s->column_bitmap_size);
          DBUG_PRINT("info",("vp table_count=%d", roop_count));
          DBUG_PRINT("info",("vp init child table bitmaps"));
        }
      } else {
        child_table = part_tables[roop_count].table;
        memcpy(sel_key_init_child_bitmaps[0][roop_count],
          child_table->read_set->bitmap,
          child_table->s->column_bitmap_size);
        memcpy(sel_key_init_child_bitmaps[1][roop_count],
          child_table->write_set->bitmap,
          child_table->s->column_bitmap_size);
        DBUG_PRINT("info",("vp init sel_key_init_child_bitmaps"));
      }
      DBUG_PRINT("info",("vp table_count=%d", roop_count));
      if (roop_count == child_table_idx)
      {
        DBUG_ASSERT(!ft_inited);
        if (
          child_keyread &&
          !(table_share->key_info[active_index].flags & HA_SPATIAL) &&
          (error_num =
            part_tables[roop_count].table->file->extra(HA_EXTRA_KEYREAD))
        )
          DBUG_RETURN(error_num);
        DBUG_PRINT("info",("vp call child[%d] ha_index_init",
          roop_count));
        vp_set_bit(key_inited_tables, roop_count);
        if ((error_num =
          part_tables[roop_count].table->file->ha_index_init(
            child_key_idx, sorted)))
          DBUG_RETURN(error_num);
      } else {
        DBUG_PRINT("info",("vp call child[%d] ha_index_init",
          roop_count));
        vp_set_bit(key_inited_tables, roop_count);
        if ((error_num =
          part_tables[roop_count].table->file->ha_index_init(
            share->correspond_pk[roop_count]->key_idx, FALSE)))
          DBUG_RETURN(error_num);
      }
    }
  }
  init_sel_key_init_bitmap = TRUE;
  cb_state = CB_SEL_KEY_INIT;
  DBUG_RETURN(0);
}

#ifdef HA_CAN_BULK_ACCESS
int ha_vp::pre_index_init(
  uint idx,
  bool sorted
) {
  int error_num, roop_count;
  TABLE *child_table;
  DBUG_ENTER("ha_vp::pre_index_init");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp part_tables[0].table=%p", part_tables[0].table));
  DBUG_PRINT("info",("vp table=%p", table));
  DBUG_PRINT("info",("vp idx=%u", idx));
  bulk_access_pre_called = TRUE;
#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
  if (!table->pos_in_table_list->parent_l && lock_type_ext == F_WRLCK)
  {
    check_and_set_bitmap_for_update(FALSE);
  }
#endif
  init_select_column(FALSE);
  DBUG_PRINT("info",("vp init_sel_key_init_bitmap=%s",
    init_sel_key_init_bitmap ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  memset(key_inited_tables, 0, sizeof(uchar) * share->use_tables_size);
  memset(pruned_tables, 0, sizeof(uchar) * share->use_tables_size);
  pruned = FALSE;
  active_index = idx;
  if (!bulk_access_info_current->init_sel_key_init_bitmap)
  {
    memset(use_tables, 0, sizeof(uchar) * share->use_tables_size);
    child_keyread = FALSE;
    single_table = FALSE;
    set_used_table = FALSE;
    child_table_idx = share->table_count;
    child_key_idx = MAX_KEY;
    if (
      (lock_mode > 0 || lock_type_ext == F_WRLCK) &&
      (sql_command == SQLCOM_UPDATE || sql_command == SQLCOM_UPDATE_MULTI)
    ) {
      if (check_partitioned())
      {
        /* need all columns */
        for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        {
          bulk_access_info_current->idx_init_read_bitmap[roop_count] =
            ~((uchar) 0);
          bulk_access_info_current->idx_init_write_bitmap[roop_count] =
            ~((uchar) 0);
        }
      } else if (share->zero_record_update_mode)
      {
        for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        {
          bulk_access_info_current->idx_init_read_bitmap[roop_count] |=
            share->cpy_clm_bitmap[roop_count];
          bulk_access_info_current->idx_init_write_bitmap[roop_count] |=
            share->cpy_clm_bitmap[roop_count];
        }
      }
    }
    memcpy(work_bitmap3, bulk_access_info_current->idx_init_read_bitmap,
      sizeof(uchar) * share->bitmap_size);
    memcpy(work_bitmap4, bulk_access_info_current->idx_init_write_bitmap,
      sizeof(uchar) * share->bitmap_size);

    if (
      (error_num = choose_child_index(idx, work_bitmap3, work_bitmap4,
        &child_table_idx, &child_key_idx)) ||
      (error_num = choose_child_ft_tables(work_bitmap3, work_bitmap4)) ||
      (!single_table && !ft_correspond_flag &&
      (error_num = choose_child_tables(work_bitmap3, work_bitmap4)))
    ) {
      DBUG_RETURN(error_num);
    }
    set_child_pt_bitmap();
    memcpy(bulk_access_info_current->sel_key_init_use_tables, use_tables,
      sizeof(uchar) * share->use_tables_size);
    bulk_access_info_current->child_keyread = child_keyread;
    bulk_access_info_current->single_table = single_table;
    bulk_access_info_current->set_used_table = set_used_table;
    bulk_access_info_current->child_table_idx = child_table_idx;
    bulk_access_info_current->child_key_idx = child_key_idx;
  } else if (cb_state != CB_SEL_KEY_INIT)
    memcpy(use_tables, bulk_access_info_current->sel_key_init_use_tables,
      sizeof(uchar) * share->use_tables_size);
  DBUG_PRINT("info",("vp child_table_idx=%d", child_table_idx));
  DBUG_PRINT("info",("vp child_key_idx=%d", child_key_idx));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(use_tables, roop_count))
    {
      if (bulk_access_info_current->init_sel_key_init_bitmap)
      {
        if (cb_state != CB_SEL_KEY_INIT)
        {
          child_table = part_tables[roop_count].table;
          memcpy(child_table->read_set->bitmap,
            bulk_access_info_current->
              sel_key_init_child_bitmaps[0][roop_count],
            child_table->s->column_bitmap_size);
          memcpy(child_table->write_set->bitmap,
            bulk_access_info_current->
              sel_key_init_child_bitmaps[1][roop_count],
            child_table->s->column_bitmap_size);
          DBUG_PRINT("info",("vp table_count=%d", roop_count));
          DBUG_PRINT("info",("vp init child table bitmaps"));
        }
      } else {
        child_table = part_tables[roop_count].table;
        memcpy(bulk_access_info_current->
            sel_key_init_child_bitmaps[0][roop_count],
          child_table->read_set->bitmap,
          child_table->s->column_bitmap_size);
        memcpy(bulk_access_info_current->
            sel_key_init_child_bitmaps[1][roop_count],
          child_table->write_set->bitmap,
          child_table->s->column_bitmap_size);
        DBUG_PRINT("info",("vp init sel_key_init_child_bitmaps"));
      }
      DBUG_PRINT("info",("vp table_count=%d", roop_count));
      if (roop_count == child_table_idx)
      {
        DBUG_ASSERT(!ft_inited);
        if (
          child_keyread &&
          !(table_share->key_info[active_index].flags & HA_SPATIAL) &&
          (error_num =
            part_tables[roop_count].table->file->extra(HA_EXTRA_KEYREAD))
        )
          DBUG_RETURN(error_num);
        vp_set_bit(key_inited_tables, roop_count);
        if ((error_num =
          part_tables[roop_count].table->file->ha_pre_index_init(
            child_key_idx, sorted)))
          DBUG_RETURN(error_num);
        vp_set_bit(bulk_access_exec_bitmap, roop_count);
      } else if (update_request) {
        vp_set_bit(key_inited_tables, roop_count);
        if ((error_num =
          part_tables[roop_count].table->file->ha_pre_index_init(
            share->correspond_pk[roop_count]->key_idx, FALSE)))
          DBUG_RETURN(error_num);
      }
    }
  }
  if (single_table)
    need_bulk_access_finish = FALSE;
  else {
    need_bulk_access_finish = TRUE;
    DBUG_RETURN(pre_index_end());
  }
  bulk_access_info_current->init_sel_key_init_bitmap = TRUE;
  cb_state = CB_SEL_KEY_INIT;
  DBUG_RETURN(0);
}
#endif

int ha_vp::index_end()
{
  int error_num, error_num2, roop_count;
  DBUG_ENTER("ha_vp::index_end");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  error_num = 0;
  active_index = MAX_KEY;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(key_inited_tables, roop_count))
    {
      DBUG_PRINT("info",("vp table_count=%d", roop_count));
      DBUG_PRINT("info",("vp child_table=%p", part_tables[roop_count].table));
      if ((error_num2 =
        part_tables[roop_count].table->file->ha_index_end()))
        error_num = error_num2;
    }
  }
  DBUG_RETURN(error_num);
}

#ifdef HA_CAN_BULK_ACCESS
int ha_vp::pre_index_end()
{
  int error_num, error_num2, roop_count;
  DBUG_ENTER("ha_vp::pre_index_end");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  error_num = 0;
  active_index = MAX_KEY;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(key_inited_tables, roop_count))
    {
      DBUG_PRINT("info",("vp table_count=%d", roop_count));
      DBUG_PRINT("info",("vp child_table=%p", part_tables[roop_count].table));
      if ((error_num2 =
        part_tables[roop_count].table->file->ha_pre_index_end()))
        error_num = error_num2;
    }
  }
  bulk_access_pre_called = FALSE;
  if (!error_num && need_bulk_access_finish)
    DBUG_RETURN(ER_NOT_SUPPORTED_YET);
  DBUG_RETURN(error_num);
}
#endif

void ha_vp::index_read_map_init(
  const uchar *key,
  key_part_map keypart_map,
  enum ha_rkey_function find_flag
) {
  DBUG_ENTER("ha_vp::index_read_map_init");
  DBUG_PRINT("info",("vp this=%p", this));
  check_select_column(FALSE);
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  uint key_length = calculate_key_len(table, active_index, key, keypart_map);
  child_key = create_child_key(key, child_key_different, keypart_map,
    key_length, &child_key_length);
  DBUG_VOID_RETURN;
}

int ha_vp::pre_index_read_map(
  const uchar *key,
  key_part_map keypart_map,
  enum ha_rkey_function find_flag,
  bool use_parallel
) {
  TABLE *table2;
  DBUG_ENTER("ha_vp::pre_index_read_map");
  DBUG_PRINT("info",("vp this=%p", this));
  use_pre_call = TRUE;
  index_read_map_init(key, keypart_map, find_flag);
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
#endif

  table2 = part_tables[child_table_idx].table;
  DBUG_RETURN(table2->file->pre_index_read_map(
    child_key, keypart_map, find_flag, use_parallel));
}

int ha_vp::index_read_map(
  uchar *buf,
  const uchar *key,
  key_part_map keypart_map,
  enum ha_rkey_function find_flag
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::index_read_map");
  DBUG_PRINT("info",("vp this=%p", this));
  if (use_pre_call)
    use_pre_call = FALSE;
  else
    index_read_map_init(key, keypart_map, find_flag);

  table2 = part_tables[child_table_idx].table;
  if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    (error_num = table2->file->ha_index_read_map(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0], child_key, keypart_map, find_flag)) ||
#else
    (error_num = table2->file->index_read_map(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0], child_key, keypart_map, find_flag)) ||
#endif
    (error_num = get_child_record_by_idx(child_table_idx, ptr_diff))
  ) {
    table->status = table2->status;
    DBUG_RETURN(error_num);
  }
  DBUG_PRINT("info",("vp single_table=%s", single_table ? "TRUE" : "FALSE"));
  if (
    !single_table &&
    (error_num = get_child_record_by_pk(ptr_diff))
  ) {
    if (
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    )
      DBUG_RETURN(error_num);
    else
      DBUG_RETURN(index_next(buf));
  }
  table->status = table2->status;
  DBUG_RETURN(0);
}

#ifdef VP_HANDLER_HAS_HA_INDEX_READ_LAST_MAP
void ha_vp::index_read_last_map_init(
  const uchar *key,
  key_part_map keypart_map
) {
  DBUG_ENTER("ha_vp::index_read_last_map_init");
  DBUG_PRINT("info",("vp this=%p", this));
  check_select_column(FALSE);
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  uint key_length = calculate_key_len(table, active_index, key, keypart_map);
  child_key = create_child_key(key, child_key_different, keypart_map,
    key_length, &child_key_length);
  DBUG_VOID_RETURN;
}

int ha_vp::pre_index_read_last_map(
  const uchar *key,
  key_part_map keypart_map,
  bool use_parallel
) {
  TABLE *table2;
  DBUG_ENTER("ha_vp::pre_index_read_last_map");
  DBUG_PRINT("info",("vp this=%p", this));
  use_pre_call = TRUE;
  index_read_last_map_init(key, keypart_map);
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
#endif

  table2 = part_tables[child_table_idx].table;
  DBUG_RETURN(table2->file->pre_index_read_last_map(child_key, keypart_map,
    use_parallel));
}

int ha_vp::index_read_last_map(
  uchar *buf,
  const uchar *key,
  key_part_map keypart_map
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::index_read_last_map");
  DBUG_PRINT("info",("vp this=%p", this));
  if (use_pre_call)
    use_pre_call = FALSE;
  else
    index_read_last_map_init(key, keypart_map);

  table2 = part_tables[child_table_idx].table;
  if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    (error_num = table2->file->ha_index_read_last_map(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0], child_key, keypart_map)) ||
#else
    (error_num = table2->file->index_read_last_map(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0], child_key, keypart_map)) ||
#endif
    (error_num = get_child_record_by_idx(child_table_idx, ptr_diff))
  ) {
    table->status = table2->status;
    DBUG_RETURN(error_num);
  }
  if (
    !single_table &&
    (error_num = get_child_record_by_pk(ptr_diff))
  ) {
    if (
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    )
      DBUG_RETURN(error_num);
    else
      DBUG_RETURN(index_prev(buf));
  }
  table->status = table2->status;
  DBUG_RETURN(0);
}
#endif

int ha_vp::index_next(
  uchar *buf
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::index_next");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }

  table2 = part_tables[child_table_idx].table;
  do {
    if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
      (error_num = table2->file->ha_index_next(
        vp_bit_is_set(share->same_columns, child_table_idx) ?
        buf : table2->record[0])) ||
#else
      (error_num = table2->file->index_next(
        vp_bit_is_set(share->same_columns, child_table_idx) ?
        buf : table2->record[0])) ||
#endif
      (error_num = get_child_record_by_idx(child_table_idx, ptr_diff))
    ) {
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if (
      !single_table &&
      (error_num = get_child_record_by_pk(ptr_diff)) &&
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    ) {
      DBUG_RETURN(error_num);
    }
  } while (error_num);
  table->status = table2->status;
  DBUG_RETURN(0);
}

int ha_vp::index_prev(
  uchar *buf
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::index_prev");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }

  table2 = part_tables[child_table_idx].table;
  do {
    if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
      (error_num = table2->file->ha_index_prev(
        vp_bit_is_set(share->same_columns, child_table_idx) ?
        buf : table2->record[0])) ||
#else
      (error_num = table2->file->index_prev(
        vp_bit_is_set(share->same_columns, child_table_idx) ?
        buf : table2->record[0])) ||
#endif
      (error_num = get_child_record_by_idx(child_table_idx, ptr_diff))
    ) {
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if (
      !single_table &&
      (error_num = get_child_record_by_pk(ptr_diff)) &&
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    ) {
      DBUG_RETURN(error_num);
    }
  } while (error_num);
  table->status = table2->status;
  DBUG_RETURN(0);
}

void ha_vp::index_first_init()
{
  DBUG_ENTER("ha_vp::index_first_init");
  DBUG_PRINT("info",("vp this=%p", this));
  check_select_column(FALSE);
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  DBUG_VOID_RETURN;
}

int ha_vp::pre_index_first(
  bool use_parallel
) {
  TABLE *table2;
  DBUG_ENTER("ha_vp::pre_index_first");
  DBUG_PRINT("info",("vp this=%p", this));
  use_pre_call = TRUE;
  index_first_init();
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
#endif

  table2 = part_tables[child_table_idx].table;
  DBUG_RETURN(table2->file->pre_index_first(use_parallel));
}

int ha_vp::index_first(
  uchar *buf
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::index_first");
  DBUG_PRINT("info",("vp this=%p", this));
  if (use_pre_call)
    use_pre_call = FALSE;
  else
    index_first_init();

  table2 = part_tables[child_table_idx].table;
  if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    (error_num = table2->file->ha_index_first(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0])) ||
#else
    (error_num = table2->file->index_first(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0])) ||
#endif
    (error_num = get_child_record_by_idx(child_table_idx, ptr_diff))
  ) {
    table->status = table2->status;
    DBUG_RETURN(error_num);
  }
  if (
    !single_table &&
    (error_num = get_child_record_by_pk(ptr_diff))
  ) {
    if (
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    )
      DBUG_RETURN(error_num);
    else
      DBUG_RETURN(index_next(buf));
  }
  table->status = table2->status;
  DBUG_RETURN(0);
}

void ha_vp::index_last_init()
{
  DBUG_ENTER("ha_vp::index_last_init");
  DBUG_PRINT("info",("vp this=%p", this));
  check_select_column(FALSE);
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  DBUG_VOID_RETURN;
}

int ha_vp::pre_index_last(
  bool use_parallel
) {
  TABLE *table2;
  DBUG_ENTER("ha_vp::pre_index_last");
  DBUG_PRINT("info",("vp this=%p", this));
  use_pre_call = TRUE;
  index_last_init();
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
#endif

  table2 = part_tables[child_table_idx].table;
  DBUG_RETURN(table2->file->pre_index_last(use_pre_call));
}

int ha_vp::index_last(
  uchar *buf
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::index_last");
  DBUG_PRINT("info",("vp this=%p", this));
  if (use_pre_call)
    use_pre_call = FALSE;
  else
    index_last_init();

  table2 = part_tables[child_table_idx].table;
  if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    (error_num = table2->file->ha_index_last(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0])) ||
#else
    (error_num = table2->file->index_last(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0])) ||
#endif
    (error_num = get_child_record_by_idx(child_table_idx, ptr_diff))
  ) {
    table->status = table2->status;
    DBUG_RETURN(error_num);
  }
  if (
    !single_table &&
    (error_num = get_child_record_by_pk(ptr_diff))
  ) {
    if (
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    )
      DBUG_RETURN(error_num);
    else
      DBUG_RETURN(index_prev(buf));
  }
  table->status = table2->status;
  DBUG_RETURN(0);
}

int ha_vp::index_next_same(
  uchar *buf,
  const uchar *key,
  uint keylen
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::index_next_same");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  table2 = part_tables[child_table_idx].table;
  if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    (error_num = table2->file->ha_index_next_same(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0], child_key, child_key_length)) ||
#else
    (error_num = table2->file->index_next_same(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0], child_key, child_key_length)) ||
#endif
    (error_num = get_child_record_by_idx(child_table_idx, ptr_diff))
  ) {
    table->status = table2->status;
    DBUG_RETURN(error_num);
  }
  if (
    !single_table &&
    (error_num = get_child_record_by_pk(ptr_diff))
  ) {
    if (
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    )
      DBUG_RETURN(error_num);
    else
      DBUG_RETURN(index_next(buf));
  }
  table->status = table2->status;
  DBUG_RETURN(0);
}

void ha_vp::read_range_first_init(
  const key_range *start_key,
  const key_range *end_key,
  bool eq_range,
  bool sorted
) {
  DBUG_ENTER("ha_vp::read_range_first_init");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp start_key=%p", start_key));
  DBUG_PRINT("info",("vp end_key=%p", end_key));
  DBUG_PRINT("info",("vp eq_range=%s", eq_range ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp sorted=%s", sorted ? "TRUE" : "FALSE"));
  check_select_column(FALSE);
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  if (start_key)
  {
    DBUG_PRINT("info",("vp start_key->flag=%d", start_key->flag));
    DBUG_PRINT("info",("vp start_key->length=%d", start_key->length));
    child_start_key.keypart_map = start_key->keypart_map;
    child_start_key.flag = start_key->flag;
    child_start_key.key = create_child_key(
      start_key->key, child_key_different, start_key->keypart_map,
      start_key->length, &child_start_key.length
    );
  }
  if (end_key)
  {
    DBUG_PRINT("info",("vp end_key->flag=%d", end_key->flag));
    DBUG_PRINT("info",("vp end_key->length=%d", end_key->length));
    child_end_key.keypart_map = end_key->keypart_map;
    child_end_key.flag = end_key->flag;
    child_end_key.key =  create_child_key(
      end_key->key, child_end_key_different, end_key->keypart_map,
      end_key->length, &child_end_key.length
    );

    key_compare_result_on_equal =
      ((end_key->flag == HA_READ_BEFORE_KEY) ? 1 :
      (end_key->flag == HA_READ_AFTER_KEY) ? -1 : 0);
  }
  range_key_part = table->key_info[active_index].key_part;
  DBUG_VOID_RETURN;
}

int ha_vp::pre_read_range_first(
  const key_range *start_key,
  const key_range *end_key,
  bool eq_range,
  bool sorted,
  bool use_parallel
) {
  TABLE *table2;
  DBUG_ENTER("ha_vp::pre_read_range_first");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp start_key=%p", start_key));
  DBUG_PRINT("info",("vp end_key=%p", end_key));
  DBUG_PRINT("info",("vp eq_range=%s", eq_range ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp sorted=%s", sorted ? "TRUE" : "FALSE"));
  use_pre_call = TRUE;
  read_range_first_init(start_key, end_key, eq_range, sorted);
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
#endif

  table2 = part_tables[child_table_idx].table;
  DBUG_RETURN(table2->file->pre_read_range_first(
    start_key ? &child_start_key : NULL,
    end_key ? &child_end_key : NULL, eq_range, sorted, use_parallel));
}

int ha_vp::read_range_first(
  const key_range *start_key,
  const key_range *end_key,
  bool eq_range,
  bool sorted
) {
  int error_num;
  TABLE *table2;
  DBUG_ENTER("ha_vp::read_range_first");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp start_key=%p", start_key));
  DBUG_PRINT("info",("vp end_key=%p", end_key));
  DBUG_PRINT("info",("vp eq_range=%s", eq_range ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp sorted=%s", sorted ? "TRUE" : "FALSE"));
  if (use_pre_call)
    use_pre_call = FALSE;
  else
    read_range_first_init(start_key, end_key, eq_range, sorted);

  table2 = part_tables[child_table_idx].table;
  if (
    (error_num = table2->file->read_range_first(
      start_key ? &child_start_key : NULL,
      end_key ? &child_end_key : NULL, eq_range, sorted)) ||
    (error_num = get_child_record_by_idx(child_table_idx, 0))
  ) {
    table->status = table2->status;
    DBUG_RETURN(error_num);
  }
#ifndef DBUG_OFF
  int roop_count, roop_count2;
  uint store_length;
  uchar tmp_key[MAX_KEY_LENGTH], *tmp_ptr;
  KEY *key_info;
  KEY_PART_INFO *key_part;
  key_part_map tmp_key_part_map, tmp_key_part_map2 = 0;
  key_info = &table2->key_info[child_key_idx];
  key_part = key_info->key_part;
  if (start_key)
    tmp_key_part_map2 |= start_key->keypart_map;
  if (end_key)
    tmp_key_part_map2 |= end_key->keypart_map;
  tmp_key_part_map =
    make_prev_keypart_map(vp_user_defined_key_parts(key_info));
  tmp_key_part_map &= tmp_key_part_map2;
  key_copy(
    tmp_key,
    table2->record[0],
    key_info,
    key_info->key_length);
  for (
    roop_count = 0, tmp_ptr = tmp_key,
    store_length = key_part->store_length;
    tmp_key_part_map > 0;
    tmp_ptr += store_length, roop_count++, tmp_key_part_map >>= 1,
    key_part++, store_length = key_part->store_length
  ) {
    for (roop_count2 = 0; roop_count2 < (int) store_length; roop_count2++)
    {
      DBUG_PRINT("info",("vp tmp_key[%d][%d]=%x",
        roop_count, roop_count2, tmp_ptr[roop_count2]));
    }
  }
  /* primary key */
  key_info = &table2->key_info[0];
  key_part = key_info->key_part;
  tmp_key_part_map =
    make_prev_keypart_map(vp_user_defined_key_parts(key_info));
  key_copy(
    tmp_key,
    table2->record[0],
    key_info,
    key_info->key_length);
  for (
    roop_count = 0, tmp_ptr = tmp_key,
    store_length = key_part->store_length;
    tmp_key_part_map > 0;
    tmp_ptr += store_length, roop_count++, tmp_key_part_map >>= 1,
    key_part++, store_length = key_part->store_length
  ) {
    for (roop_count2 = 0; roop_count2 < (int) store_length; roop_count2++)
    {
      DBUG_PRINT("info",("vp tmp_pk[%d][%d]=%x",
        roop_count, roop_count2, tmp_ptr[roop_count2]));
    }
  }
#endif
  if (
    !single_table &&
    (error_num = get_child_record_by_pk(0))
  ) {
    if (
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    )
      DBUG_RETURN(error_num);
    else
      DBUG_RETURN(read_range_next());
  }
  table->status = table2->status;
  DBUG_RETURN(0);
}

int ha_vp::read_range_next()
{
  int error_num;
  TABLE *table2;
  DBUG_ENTER("ha_vp::read_range_next");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  table2 = part_tables[child_table_idx].table;
  do {
    if (
      (error_num = table2->file->read_range_next()) ||
      (error_num = get_child_record_by_idx(child_table_idx, 0))
    ) {
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if (
      !single_table &&
      (error_num = get_child_record_by_pk(0)) &&
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    ) {
      DBUG_RETURN(error_num);
    }
  } while (error_num);
  table->status = table2->status;
  DBUG_RETURN(0);
}

#ifdef VP_SUPPORT_MRR
int ha_vp::multi_range_key_create_key(
  RANGE_SEQ_IF *seq,
  range_seq_t seq_it,
  int target_table_idx
) {
  uint length;
  key_range *start_key, *end_key;
  KEY_MULTI_RANGE *range;
  DBUG_ENTER("ha_vp::multi_range_key_create_key");
  DBUG_PRINT("info",("vp this=%p", this));
  m_mrr_range_length = 0;
  m_child_mrr_range_length[target_table_idx] = 0;
  if (!m_mrr_range_first)
  {
    if (!(m_mrr_range_first = (VP_KEY_MULTI_RANGE *)
      my_multi_malloc(MYF(MY_WME),
        &m_mrr_range_current, sizeof(VP_KEY_MULTI_RANGE),
        NullS))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    m_mrr_range_first->id = 1;
    m_mrr_range_first->key[0] = NULL;
    m_mrr_range_first->key[1] = NULL;
    m_mrr_range_first->next = NULL;
  } else {
    m_mrr_range_current = m_mrr_range_first;
  }
  if (!m_child_mrr_range_first[target_table_idx])
  {
    if (!(m_child_mrr_range_first[target_table_idx] =
      (VP_CHILD_KEY_MULTI_RANGE *) my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
        &m_child_mrr_range_current[target_table_idx],
        sizeof(VP_CHILD_KEY_MULTI_RANGE),
        NullS))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  } else {
    m_child_mrr_range_current[target_table_idx] =
      m_child_mrr_range_first[target_table_idx];
    m_child_mrr_range_current[target_table_idx]->vp_key_multi_range = NULL;
  }

  while (!seq->next(seq_it, &m_mrr_range_current->key_multi_range))
  {
    m_mrr_range_length++;
    range = &m_mrr_range_current->key_multi_range;
    DBUG_PRINT("info",("vp range->range_flag=%u", range->range_flag));
    start_key = &range->start_key;
    DBUG_PRINT("info",("vp start_key->key=%p", start_key->key));
    DBUG_PRINT("info",("vp start_key->length=%u", start_key->length));
    DBUG_PRINT("info",("vp start_key->keypart_map=%lu", start_key->keypart_map));
    DBUG_PRINT("info",("vp start_key->flag=%u", start_key->flag));
    if (start_key->key)
    {
      length = start_key->length;
      if (
        !m_mrr_range_current->key[0] ||
        m_mrr_range_current->length[0] < length
      ) {
        if (m_mrr_range_current->key[0])
        {
          vp_my_free(m_mrr_range_current->key[0], MYF(0));
        }
        if (!(m_mrr_range_current->key[0] = (uchar *)
          my_multi_malloc(MYF(MY_WME),
            &m_mrr_range_current->key[0], length,
            NullS))
        ) {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        m_mrr_range_current->length[0] = length;
      }
      memcpy(m_mrr_range_current->key[0], start_key->key, length);
      start_key->key = m_mrr_range_current->key[0];
    }

    end_key = &range->end_key;
    DBUG_PRINT("info",("vp end_key->key=%p", end_key->key));
    DBUG_PRINT("info",("vp end_key->length=%u", end_key->length));
    DBUG_PRINT("info",("vp end_key->keypart_map=%lu", end_key->keypart_map));
    DBUG_PRINT("info",("vp end_key->flag=%u", end_key->flag));
    if (end_key->key)
    {
      length = end_key->length;
      if (
        !m_mrr_range_current->key[1] ||
        m_mrr_range_current->length[1] < length
      ) {
        if (m_mrr_range_current->key[1])
        {
          vp_my_free(m_mrr_range_current->key[1], MYF(0));
        }
        if (!(m_mrr_range_current->key[1] = (uchar *)
          my_multi_malloc(MYF(MY_WME),
            &m_mrr_range_current->key[1], length,
            NullS))
        ) {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        m_mrr_range_current->length[1] = length;
      }
      memcpy(m_mrr_range_current->key[1], end_key->key, length);
      end_key->key = m_mrr_range_current->key[1];
    }
    m_mrr_range_current->ptr = m_mrr_range_current->key_multi_range.ptr;
    m_mrr_range_current->key_multi_range.ptr = m_mrr_range_current;

    m_child_mrr_range_length[target_table_idx]++;
    m_child_mrr_range_current[target_table_idx]->vp_key_multi_range =
      m_mrr_range_current;

    if (!m_child_mrr_range_current[target_table_idx]->next)
    {
      VP_CHILD_KEY_MULTI_RANGE *tmp_child_mrr_range;
      if (!(tmp_child_mrr_range = (VP_CHILD_KEY_MULTI_RANGE *)
        my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
          &tmp_child_mrr_range, sizeof(VP_CHILD_KEY_MULTI_RANGE),
          NullS))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      m_child_mrr_range_current[target_table_idx]->next = tmp_child_mrr_range;
      m_child_mrr_range_current[target_table_idx] = tmp_child_mrr_range;
    } else {
      m_child_mrr_range_current[target_table_idx] =
        m_child_mrr_range_current[target_table_idx]->next;
      m_child_mrr_range_current[target_table_idx]->vp_key_multi_range = NULL;
    }

    if (!m_mrr_range_current->next)
    {
      VP_KEY_MULTI_RANGE *tmp_mrr_range;
      if (!(tmp_mrr_range = (VP_KEY_MULTI_RANGE *)
        my_multi_malloc(MYF(MY_WME),
          &tmp_mrr_range, sizeof(VP_KEY_MULTI_RANGE),
          NullS))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      tmp_mrr_range->id = m_mrr_range_current->id + 1;
      tmp_mrr_range->key[0] = NULL;
      tmp_mrr_range->key[1] = NULL;
      tmp_mrr_range->next = NULL;
      m_mrr_range_current->next = tmp_mrr_range;
      m_mrr_range_current = tmp_mrr_range;
    } else {
      m_mrr_range_current = m_mrr_range_current->next;
    }
  }

  m_child_key_multi_range_hld[target_table_idx].vp = this;
  m_child_key_multi_range_hld[target_table_idx].child_table_idx =
    target_table_idx;
  m_child_key_multi_range_hld[target_table_idx].vp_child_key_multi_range =
    m_child_mrr_range_first[target_table_idx];
  DBUG_PRINT("info",("vp OK"));
  DBUG_RETURN(0);
}

static void vp_multi_range_key_get_key_info(
  void *init_params,
  uint *length,
  key_part_map *map
) {
  DBUG_ENTER("vp_multi_range_key_get_key_info");
  VP_CHILD_KEY_MULTI_RANGE_HLD *hld=
    (VP_CHILD_KEY_MULTI_RANGE_HLD *)init_params;
  ha_vp *vp = hld->vp;
  key_range *start_key =
    &vp->m_mrr_range_first->key_multi_range.start_key;
  *length = start_key->length;
  *map = start_key->keypart_map;
  DBUG_VOID_RETURN;
}

static range_seq_t vp_multi_range_key_init(
  void *init_params,
  uint n_ranges,
  uint flags
) {
  DBUG_ENTER("vp_multi_range_key_init");
  VP_CHILD_KEY_MULTI_RANGE_HLD *hld =
    (VP_CHILD_KEY_MULTI_RANGE_HLD *)init_params;
  ha_vp *vp = hld->vp;
  int i = hld->child_table_idx;
  vp->m_mrr_range_init_flags = flags;
  hld->vp_child_key_multi_range = vp->m_child_mrr_range_first[i];
  DBUG_RETURN(init_params);
}

static bool vp_multi_range_key_next(
  range_seq_t seq,
  KEY_MULTI_RANGE *range
) {
  DBUG_ENTER("vp_multi_range_key_next");
  VP_CHILD_KEY_MULTI_RANGE_HLD *hld =
    (VP_CHILD_KEY_MULTI_RANGE_HLD *)seq;
  VP_KEY_MULTI_RANGE *vp_key_multi_range =
    hld->vp_child_key_multi_range->vp_key_multi_range;
  if (!vp_key_multi_range)
    DBUG_RETURN(TRUE);
  *range = vp_key_multi_range->key_multi_range;
  DBUG_PRINT("info",("vp range->range_flag=%u", range->range_flag));
  DBUG_PRINT("info",("vp start_key->key=%p", range->start_key.key));
  DBUG_PRINT("info",("vp start_key->length=%u", range->start_key.length));
  DBUG_PRINT("info",("vp start_key->keypart_map=%lu", range->start_key.keypart_map));
  DBUG_PRINT("info",("vp start_key->flag=%u", range->start_key.flag));
  DBUG_PRINT("info",("vp end_key->key=%p", range->end_key.key));
  DBUG_PRINT("info",("vp end_key->length=%u", range->end_key.length));
  DBUG_PRINT("info",("vp end_key->keypart_map=%lu", range->end_key.keypart_map));
  DBUG_PRINT("info",("vp end_key->flag=%u", range->end_key.flag));
  hld->vp_child_key_multi_range =
    hld->vp_child_key_multi_range->next;
  DBUG_RETURN(FALSE);
}

static bool vp_multi_range_key_skip_record(
  range_seq_t seq,
  range_id_t range_info,
  uchar *rowid
) {
  DBUG_ENTER("vp_multi_range_key_skip_record");
  VP_CHILD_KEY_MULTI_RANGE_HLD *hld =
    (VP_CHILD_KEY_MULTI_RANGE_HLD *)seq;
  DBUG_RETURN(hld->vp->m_seq_if->skip_record(
    hld->vp->m_seq, range_info, rowid));
}

static bool vp_multi_range_key_skip_index_tuple(
  range_seq_t seq,
  range_id_t range_info
) {
  DBUG_ENTER("vp_multi_range_key_skip_index_tuple");
  VP_CHILD_KEY_MULTI_RANGE_HLD *hld =
    (VP_CHILD_KEY_MULTI_RANGE_HLD *)seq;
  DBUG_RETURN(hld->vp->m_seq_if->skip_index_tuple(
    hld->vp->m_seq, range_info));
}

ha_rows ha_vp::multi_range_read_info_const(
  uint keyno,
  RANGE_SEQ_IF *seq,
  void *seq_init_param, 
  uint n_ranges,
  uint *bufsz,
  uint *mrr_mode,
  Cost_estimate *cost
) {
  int error_num;
  ha_rows rows;
  range_seq_t seq_it;
  DBUG_ENTER("ha_vp::multi_range_read_info_const");
  DBUG_PRINT("info",("vp this=%p", this));
  child_keyread = FALSE;
  single_table = FALSE;
  memcpy(work_bitmap3, table->read_set->bitmap,
    sizeof(uchar) * share->bitmap_size);
  memcpy(work_bitmap4, table->write_set->bitmap,
    sizeof(uchar) * share->bitmap_size);
  if (
    (error_num = choose_child_index(keyno, work_bitmap3, work_bitmap4,
      &child_table_idx, &child_key_idx))
  )
    DBUG_RETURN(HA_POS_ERROR);
  set_child_pt_bitmap();

  TABLE *table2 = part_tables[child_table_idx].table;
  if (!vp_bit_is_set(share->need_converting, child_table_idx))
  {
    rows =
      table2->file->multi_range_read_info_const(
        child_key_idx,
        seq,
        seq_init_param,
        n_ranges,
        bufsz,
        mrr_mode,
        cost
      );
    DBUG_PRINT("info",("vp rows=%llu", rows));
    DBUG_RETURN(rows);
  }
  m_mrr_new_full_buffer_size = 0;
  seq_it = seq->init(seq_init_param, n_ranges, *mrr_mode);
  if ((error_num = multi_range_key_create_key(seq, seq_it, child_table_idx)))
  {
    DBUG_RETURN(HA_POS_ERROR);
  }
  m_child_seq_if.get_key_info =
    seq->get_key_info ? vp_multi_range_key_get_key_info : NULL;
  m_child_seq_if.init =
    vp_multi_range_key_init;
  m_child_seq_if.next =
    vp_multi_range_key_next;
  m_child_seq_if.skip_record =
    seq->skip_record ? vp_multi_range_key_skip_record : NULL;
  m_child_seq_if.skip_index_tuple =
    seq->skip_index_tuple ? vp_multi_range_key_skip_index_tuple : NULL;

  m_mrr_buffer_size[child_table_idx] = 0;
  rows = table2->file->multi_range_read_info_const(
    child_key_idx,
    &m_child_seq_if,
    &m_child_key_multi_range_hld[child_table_idx],
    m_child_mrr_range_length[child_table_idx],
    &m_mrr_buffer_size[child_table_idx],
    mrr_mode,
    cost
  );
  if (rows == HA_POS_ERROR)
    DBUG_RETURN(HA_POS_ERROR);
  m_mrr_new_full_buffer_size += m_mrr_buffer_size[child_table_idx];

  DBUG_PRINT("info",("vp rows=%llu", rows));
  DBUG_RETURN(rows);
}

ha_rows ha_vp::multi_range_read_info(
  uint keyno,
  uint n_ranges,
  uint keys,
  uint key_parts,
  uint *bufsz, 
  uint *mrr_mode,
  Cost_estimate *cost
) {
  int error_num;
  ha_rows rows;
  DBUG_ENTER("ha_vp::multi_range_read_info");
  DBUG_PRINT("info",("vp this=%p", this));
  child_keyread = FALSE;
  single_table = FALSE;
  memcpy(work_bitmap3, table->read_set->bitmap,
    sizeof(uchar) * share->bitmap_size);
  memcpy(work_bitmap4, table->write_set->bitmap,
    sizeof(uchar) * share->bitmap_size);
  if (
    (error_num = choose_child_index(keyno, work_bitmap3, work_bitmap4,
      &child_table_idx, &child_key_idx))
  )
    DBUG_RETURN(HA_POS_ERROR);
  set_child_pt_bitmap();

  TABLE *table2 = part_tables[child_table_idx].table;
  if (!vp_bit_is_set(share->need_converting, child_table_idx))
  {
    rows =
      table2->file->multi_range_read_info(
        child_key_idx,
        n_ranges,
        keys,
        key_parts,
        bufsz,
        mrr_mode,
        cost
      );
    DBUG_PRINT("info",("vp rows=%llu", rows));
    DBUG_RETURN(rows);
  }
  m_mrr_new_full_buffer_size = 0;
  m_mrr_buffer_size[child_table_idx] = 0;
  rows = table2->file->multi_range_read_info(
    child_key_idx,
    n_ranges,
    keys,
    key_parts,
    &m_mrr_buffer_size[child_table_idx],
    mrr_mode,
    cost
  );
  if (rows == HA_POS_ERROR)
    DBUG_RETURN(HA_POS_ERROR);
  m_mrr_new_full_buffer_size += m_mrr_buffer_size[child_table_idx];
  DBUG_PRINT("info",("vp rows=%llu", rows));
  DBUG_RETURN(0);
}

int ha_vp::multi_range_read_init(
  RANGE_SEQ_IF *seq,
  void *seq_init_param,
  uint n_ranges,
  uint mrr_mode, 
  HANDLER_BUFFER *buf
) {
  int error_num;
  uchar *tmp_buffer;
  DBUG_ENTER("ha_vp::multi_range_read_init");
  DBUG_PRINT("info",("vp this=%p", this));
  TABLE *table2 = part_tables[child_table_idx].table;
  mrr_iter = seq->init(seq_init_param, n_ranges, mrr_mode);
  mrr_funcs = *seq;
  mrr_is_output_sorted = MY_TEST(mrr_mode & HA_MRR_SORTED);
  mrr_have_range = FALSE;
  if (!vp_bit_is_set(share->need_converting, child_table_idx))
  {
    DBUG_PRINT("info",("vp call child[%d] multi_range_read_init directly",
      child_table_idx));
    error_num = table2->file->multi_range_read_init(
        seq,
        seq_init_param,
        n_ranges,
        mrr_mode,
        buf
      );
    DBUG_RETURN(error_num);
  }

  m_seq_if = seq;
  m_seq = seq->init(seq_init_param, n_ranges, mrr_mode);
  if ((error_num = multi_range_key_create_key(seq, m_seq, child_table_idx)))
  {
    DBUG_RETURN(error_num);
  }
  m_child_seq_if.get_key_info =
    seq->get_key_info ? vp_multi_range_key_get_key_info : NULL;
  m_child_seq_if.init =
    vp_multi_range_key_init;
  m_child_seq_if.next =
    vp_multi_range_key_next;
  m_child_seq_if.skip_record =
    seq->skip_record ? vp_multi_range_key_skip_record : NULL;
  m_child_seq_if.skip_index_tuple =
    seq->skip_index_tuple ? vp_multi_range_key_skip_index_tuple : NULL;
  if (m_mrr_full_buffer_size < m_mrr_new_full_buffer_size)
  {
    if (m_mrr_full_buffer)
      vp_my_free(m_mrr_full_buffer, MYF(0));
    m_mrr_full_buffer_size = 0;
    if (!(m_mrr_full_buffer = (uchar *)
      my_multi_malloc(MYF(MY_WME),
        &m_mrr_full_buffer, m_mrr_new_full_buffer_size,
        NullS))
    ) {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    m_mrr_full_buffer_size = m_mrr_new_full_buffer_size;
  }

  if (m_mrr_new_full_buffer_size)
  {
    tmp_buffer = m_mrr_full_buffer;
    if (m_mrr_buffer_size[child_table_idx])
    {
      m_mrr_buffer[child_table_idx].buffer = tmp_buffer;
      m_mrr_buffer[child_table_idx].end_of_used_area = tmp_buffer;
      tmp_buffer += m_mrr_buffer_size[child_table_idx];
      m_mrr_buffer[child_table_idx].buffer_end = tmp_buffer;
    }
  } else {
    m_mrr_buffer[child_table_idx] = *buf;
  }
  DBUG_PRINT("info",("vp call child[%d] multi_range_read_init",
    child_table_idx));
  if ((error_num = table2->file->multi_range_read_init(
    &m_child_seq_if,
    &m_child_key_multi_range_hld[child_table_idx],
    m_child_mrr_range_length[child_table_idx],
    mrr_mode,
    &m_mrr_buffer[child_table_idx])))
    goto error;
  m_stock_range_seq[child_table_idx] = 0;

  m_mrr_range_current = m_mrr_range_first;
  DBUG_RETURN(0);

error:
  DBUG_RETURN(error_num);
}

int ha_vp::pre_multi_range_read_next(
  bool use_parallel
) {
  int error_num;
  TABLE *table2;
  THD *thd = ha_thd();
  int multi_range_mode =
    vp_param_multi_range_mode(thd, share->multi_range_mode);
  DBUG_ENTER("ha_vp::pre_multi_range_read_next");
  DBUG_PRINT("info",("vp this=%p", this));
  use_pre_call = TRUE;
  check_select_column(FALSE);
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  if (multi_range_mode == 0)
    DBUG_RETURN(handler::pre_multi_range_read_next(use_parallel));
  table2 = part_tables[child_table_idx].table;
  error_num = table2->file->pre_multi_range_read_next(use_parallel);
  DBUG_RETURN(error_num);
}

int ha_vp::multi_range_read_next(
  range_id_t *range_info
) {
  int error_num;
  DBUG_ENTER("ha_vp::multi_range_read_next");
  DBUG_PRINT("info",("vp this=%p", this));
  if (!mrr_have_range)
  {
    mrr_have_range = TRUE;
    error_num = multi_range_read_next_first(range_info);
  } else
    error_num = multi_range_read_next_next(range_info);
  DBUG_RETURN(error_num);
}

int ha_vp::multi_range_read_explain_info(
  uint mrr_mode,
  char *str,
  size_t size
) {
  DBUG_ENTER("ha_vp::multi_range_read_explain_info");
  DBUG_PRINT("info",("vp this=%p", this));
#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
  if (!table->pos_in_table_list->parent_l && lock_type_ext == F_WRLCK)
  {
    check_and_set_bitmap_for_update(TRUE);
  }
#endif
  init_select_column(FALSE);
  DBUG_RETURN(
    part_tables[0].table->file->multi_range_read_explain_info(
      mrr_mode, str, size)
  );
}

int ha_vp::multi_range_read_next_first(
  range_id_t *range_info
) {
  int error_num;
  TABLE *table2;
  THD *thd = ha_thd();
  int multi_range_mode =
    vp_param_multi_range_mode(thd, share->multi_range_mode);
  DBUG_ENTER("ha_vp::multi_range_read_next_first");
  DBUG_PRINT("info",("vp this=%p", this));
  if (!use_pre_call)
  {
    check_select_column(FALSE);
    DBUG_PRINT("info",("vp cb_state=%d", cb_state));
    if (cb_state != CB_SEL_KEY || is_clone)
    {
      prune_child();
      cb_state = CB_SEL_KEY;
    }
  } else {
    use_pre_call = FALSE;
  }
  if (multi_range_mode == 0)
  {
    DBUG_PRINT("info",("vp call handler::multi_range_read_next"));
    DBUG_RETURN(handler::multi_range_read_next(range_info));
  }
  table2 = part_tables[child_table_idx].table;
  if (vp_bit_is_set(share->need_converting, child_table_idx))
  {
    DBUG_PRINT("info",("vp call child[%d] multi_range_read_next",
      child_table_idx));
    error_num = table2->file->multi_range_read_next(
      &m_range_info[child_table_idx]);
    if (!error_num)
      *range_info =
        ((VP_KEY_MULTI_RANGE *) m_range_info[child_table_idx])->ptr;
  } else {
    DBUG_PRINT("info",("vp call child[%d] multi_range_read_next directly",
      child_table_idx));
    error_num = table2->file->multi_range_read_next(range_info);
  }
  if (
    error_num ||
    (error_num = get_child_record_by_idx(child_table_idx, 0))
  ) {
    table->status = table2->status;
    DBUG_RETURN(error_num);
  }
  if (
    !single_table &&
    (error_num = get_child_record_by_pk(0))
  ) {
    if (
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    )
      DBUG_RETURN(error_num);
    else {
      DBUG_RETURN(multi_range_read_next(range_info));
    }
  }

  table->status = table2->status;
  DBUG_RETURN(0);
}

int ha_vp::multi_range_read_next_next(
  range_id_t *range_info
) {
  int error_num;
  TABLE *table2;
  THD *thd = ha_thd();
  int multi_range_mode =
    vp_param_multi_range_mode(thd, share->multi_range_mode);
  DBUG_ENTER("ha_vp::multi_range_read_next_next");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  if (multi_range_mode == 0)
  {
    DBUG_PRINT("info",("vp call handler::multi_range_read_next"));
    DBUG_RETURN(handler::multi_range_read_next(range_info));
  }

  table2 = part_tables[child_table_idx].table;
  do {
    if (vp_bit_is_set(share->need_converting, child_table_idx))
    {
      DBUG_PRINT("info",("vp call child[%d] multi_range_read_next",
        child_table_idx));
      error_num = table2->file->multi_range_read_next(
        &m_range_info[child_table_idx]);
      if (!error_num)
        *range_info =
          ((VP_KEY_MULTI_RANGE *) m_range_info[child_table_idx])->ptr;
    } else {
      DBUG_PRINT("info",("vp call child[%d] multi_range_read_next directly",
        child_table_idx));
      error_num = table2->file->multi_range_read_next(range_info);
    }
    if (
      error_num ||
      (error_num = get_child_record_by_idx(child_table_idx, 0))
    ) {
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if (
      !single_table &&
      (error_num = get_child_record_by_pk(0)) &&
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    ) {
      DBUG_RETURN(error_num);
    }
  } while (error_num);

  table->status = table2->status;
  DBUG_RETURN(0);
}
#else
int ha_vp::read_multi_range_first_init(
  KEY_MULTI_RANGE **found_range_p,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  HANDLER_BUFFER *buffer
) {
  THD *thd = ha_thd();
  KEY_MULTI_RANGE *multi_range, *tmp_multi_range;
  uchar *keys;
  int multi_range_mode =
    vp_param_multi_range_mode(thd, share->multi_range_mode);
  DBUG_ENTER("ha_vp::read_multi_range_first_init");
  DBUG_PRINT("info",("vp this=%p", this));
  check_select_column(FALSE);
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  if (multi_range_mode == 0)
    DBUG_RETURN(0);

  multi_range_sorted = sorted;
  multi_range_buffer = buffer;
  if (vp_bit_is_set(share->need_converting, child_table_idx))
  {
    if (child_multi_range_first)
      vp_my_free(child_multi_range_first, MYF(0));
    /* copy multi range for child */
    if (!(child_multi_range_first =
      (KEY_MULTI_RANGE *) my_multi_malloc(MYF(MY_WME),
        &multi_range, sizeof(KEY_MULTI_RANGE) * range_count,
        &keys, sizeof(uchar) * MAX_KEY_LENGTH * range_count * 2,
        NullS)))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    memcpy(multi_range, ranges, sizeof(KEY_MULTI_RANGE) * range_count);
    for (
      tmp_multi_range = multi_range,
      multi_range_curr = ranges,
      multi_range_end = ranges + range_count;
      multi_range_curr < multi_range_end;
      tmp_multi_range++,
      multi_range_curr++
    ) {
      DBUG_PRINT("info",("vp multi_range->start_key.key=%p",
        multi_range->start_key.key));
      tmp_multi_range->start_key.key = create_child_key(
        multi_range_curr->start_key.key, keys,
        multi_range_curr->start_key.keypart_map,
        multi_range_curr->start_key.length,
        &tmp_multi_range->start_key.length
      );
      keys += MAX_KEY_LENGTH;
      DBUG_PRINT("info",("vp multi_range->end_key.key=%p",
        multi_range->start_key.key));
      tmp_multi_range->end_key.key = create_child_key(
        multi_range_curr->end_key.key, keys,
        multi_range_curr->end_key.keypart_map,
        multi_range_curr->end_key.length,
        &tmp_multi_range->end_key.length
      );
      keys += MAX_KEY_LENGTH;
    }
  } else
    multi_range = ranges;

  multi_range_curr = ranges;
  multi_range_end = ranges + range_count;

  child_found_range = multi_range;
  DBUG_RETURN(0);
}

int ha_vp::pre_read_multi_range_first(
  KEY_MULTI_RANGE **found_range_p,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  HANDLER_BUFFER *buffer,
  bool use_parallel
) {
  int error_num;
  TABLE *table2;
  THD *thd = ha_thd();
  int multi_range_mode =
    vp_param_multi_range_mode(thd, share->multi_range_mode);
  DBUG_ENTER("ha_vp::pre_read_multi_range_first");
  DBUG_PRINT("info",("vp this=%p", this));
  use_pre_call = TRUE;
  if ((error_num =
    read_multi_range_first_init(found_range_p, ranges, range_count, sorted,
      buffer)))
  {
    DBUG_RETURN(error_num);
  }
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
#endif

  if (multi_range_mode == 0)
    DBUG_RETURN(handler::pre_read_multi_range_first(
      found_range_p, ranges, range_count, sorted, buffer, use_parallel));
  table2 = part_tables[child_table_idx].table;
  DBUG_RETURN(table2->file->pre_read_multi_range_first(&child_found_range,
    child_found_range, range_count, sorted, buffer, use_parallel));
}

int ha_vp::read_multi_range_first(
  KEY_MULTI_RANGE **found_range_p,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  HANDLER_BUFFER *buffer
) {
  int error_num;
  TABLE *table2;
  THD *thd = ha_thd();
  int multi_range_mode =
    vp_param_multi_range_mode(thd, share->multi_range_mode);
  KEY_MULTI_RANGE *multi_range;
  DBUG_ENTER("ha_vp::read_multi_range_first");
  DBUG_PRINT("info",("vp this=%p", this));
  if (use_pre_call)
    use_pre_call = FALSE;
  else if ((error_num =
    read_multi_range_first_init(found_range_p, ranges, range_count, sorted,
      buffer)))
  {
    DBUG_RETURN(error_num);
  }

  if (multi_range_mode == 0)
    DBUG_RETURN(handler::read_multi_range_first(
      found_range_p, ranges, range_count, sorted, buffer));
  multi_range = child_found_range;
  table2 = part_tables[child_table_idx].table;
  if (
    (error_num = table2->file->read_multi_range_first(
      &child_found_range, multi_range, range_count, sorted, buffer)) ||
    (error_num = get_child_record_by_idx(child_table_idx, 0))
  ) {
    table->status = table2->status;
    DBUG_RETURN(error_num);
  }
#ifndef DBUG_OFF
  int roop_count, roop_count2;
  uint store_length;
  uchar tmp_key[MAX_KEY_LENGTH], *tmp_ptr;
  KEY *key_info;
  KEY_PART_INFO *key_part;
  key_part_map tmp_key_part_map, tmp_key_part_map2 = 0;
  key_info = &table2->key_info[child_key_idx];
  key_part = key_info->key_part;
  if (multi_range->start_key.key)
    tmp_key_part_map2 |= multi_range->start_key.keypart_map;
  if (multi_range->end_key.key)
    tmp_key_part_map2 |= multi_range->end_key.keypart_map;
  tmp_key_part_map = make_prev_keypart_map(key_info->key_parts);
  tmp_key_part_map &= tmp_key_part_map2;
  key_copy(
    tmp_key,
    table2->record[0],
    key_info,
    key_info->key_length);
  for (
    roop_count = 0, tmp_ptr = tmp_key,
    store_length = key_part->store_length;
    tmp_key_part_map > 0;
    tmp_ptr += store_length, roop_count++, tmp_key_part_map >>= 1,
    key_part++, store_length = key_part->store_length
  ) {
    for (roop_count2 = 0; roop_count2 < (int) store_length; roop_count2++)
    {
      DBUG_PRINT("info",("vp tmp_key[%d][%d]=%x",
        roop_count, roop_count2, tmp_ptr[roop_count2]));
    }
  }
  /* primary key */
  key_info = &table2->key_info[0];
  key_part = key_info->key_part;
  tmp_key_part_map = make_prev_keypart_map(key_info->key_parts);
  key_copy(
    tmp_key,
    table2->record[0],
    key_info,
    key_info->key_length);
  for (
    roop_count = 0, tmp_ptr = tmp_key,
    store_length = key_part->store_length;
    tmp_key_part_map > 0;
    tmp_ptr += store_length, roop_count++, tmp_key_part_map >>= 1,
    key_part++, store_length = key_part->store_length
  ) {
    for (roop_count2 = 0; roop_count2 < (int) store_length; roop_count2++)
    {
      DBUG_PRINT("info",("vp tmp_pk[%d][%d]=%x",
        roop_count, roop_count2, tmp_ptr[roop_count2]));
    }
  }
#endif
  if (
    !single_table &&
    (error_num = get_child_record_by_pk(0))
  ) {
    if (
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    )
      DBUG_RETURN(error_num);
    else {
      DBUG_RETURN(read_multi_range_next(&child_found_range));
    }
  }
  *found_range_p = child_found_range - multi_range + ranges;

  table->status = table2->status;
  DBUG_RETURN(0);
}

int ha_vp::read_multi_range_next(
  KEY_MULTI_RANGE **found_range_p
) {
  int error_num;
  TABLE *table2;
  THD *thd = ha_thd();
  KEY_MULTI_RANGE *tmp_multi_range;
  int multi_range_mode =
    vp_param_multi_range_mode(thd, share->multi_range_mode);
  DBUG_ENTER("ha_vp::read_multi_range_next");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (cb_state != CB_SEL_KEY || is_clone)
  {
    prune_child();
    cb_state = CB_SEL_KEY;
  }
  if (multi_range_mode == 0)
    DBUG_RETURN(handler::read_multi_range_next(found_range_p));

  table2 = part_tables[child_table_idx].table;
  tmp_multi_range = child_found_range;
  do {
    if (
      (error_num = table2->file->read_multi_range_next(&child_found_range)) ||
      (error_num = get_child_record_by_idx(child_table_idx, 0))
    ) {
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if (
      !single_table &&
      (error_num = get_child_record_by_pk(0)) &&
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    ) {
      DBUG_RETURN(error_num);
    }
  } while (error_num);
  *found_range_p = child_found_range - tmp_multi_range + *found_range_p;

  table->status = table2->status;
  DBUG_RETURN(0);
}
#endif

int ha_vp::rnd_init(
  bool scan
) {
  DBUG_ENTER("ha_vp::rnd_init");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  rnd_scan = scan;
  DBUG_PRINT("info",("vp rnd_scan=%s", rnd_scan ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp init_rnd_bitmap=%s",
    init_sel_rnd_bitmap ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (sql_command == SQLCOM_ALTER_TABLE)
    DBUG_RETURN(0);

#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
  if (
    !table->pos_in_table_list->parent_l &&
    rnd_scan &&
    lock_type_ext == F_WRLCK
  ) {
    check_and_set_bitmap_for_update(TRUE);
  }
#endif
  init_select_column(TRUE);
  memset(rnd_inited_tables, 0, sizeof(uchar) * share->use_tables_size);
  if (rnd_scan)
  {
    memset(pruned_tables, 0, sizeof(uchar) * share->use_tables_size);
    pruned = FALSE;
  }
  rnd_init_and_first = TRUE;
  DBUG_RETURN(0);
}

#ifdef HA_CAN_BULK_ACCESS
int ha_vp::pre_rnd_init(
  bool scan
) {
  DBUG_ENTER("ha_vp::pre_rnd_init");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  rnd_scan = scan;
  DBUG_PRINT("info",("vp rnd_scan=%s", rnd_scan ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp init_rnd_bitmap=%s",
    init_sel_rnd_bitmap ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (sql_command == SQLCOM_ALTER_TABLE)
    DBUG_RETURN(0);

#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
  if (
    !table->pos_in_table_list->parent_l &&
    rnd_scan &&
    lock_type_ext == F_WRLCK
  ) {
    check_and_set_bitmap_for_update(TRUE);
  }
#endif
  init_select_column(TRUE);
  memset(rnd_inited_tables, 0, sizeof(uchar) * share->use_tables_size);
  if (rnd_scan)
  {
    memset(pruned_tables, 0, sizeof(uchar) * share->use_tables_size);
    pruned = FALSE;
  }
  rnd_init_and_first = TRUE;
  bulk_access_pre_called = TRUE;
  need_bulk_access_finish = FALSE;
  DBUG_RETURN(0);
}
#endif

int ha_vp::rnd_end()
{
  int error_num, error_num2, roop_count;
  DBUG_ENTER("ha_vp::rnd_end");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  rnd_scan = FALSE;
  if (sql_command == SQLCOM_ALTER_TABLE)
    DBUG_RETURN(0);

  error_num = 0;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(rnd_inited_tables, roop_count))
    {
      DBUG_PRINT("info",("vp table_count=%d", roop_count));
      DBUG_PRINT("info",("vp child_table=%p", part_tables[roop_count].table));
      if ((error_num2 =
        part_tables[roop_count].table->file->ha_index_or_rnd_end()))
        error_num = error_num2;
    }
  }
  DBUG_RETURN(error_num);
}

#ifdef HA_CAN_BULK_ACCESS
int ha_vp::pre_rnd_end()
{
  int error_num;
  DBUG_ENTER("ha_vp::pre_rnd_end");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  rnd_scan = FALSE;
  if (sql_command == SQLCOM_ALTER_TABLE)
    DBUG_RETURN(0);

  error_num = 0;
  if (vp_bit_is_set(rnd_inited_tables, child_table_idx))
  {
    DBUG_PRINT("info",("vp table_count=%d", child_table_idx));
    DBUG_PRINT("info",("vp child_table=%p",
      part_tables[child_table_idx].table));
    error_num =
      part_tables[child_table_idx].table->file->ha_pre_rnd_end();
  }
  bulk_access_pre_called = FALSE;
  if (!error_num && need_bulk_access_finish)
    DBUG_RETURN(ER_NOT_SUPPORTED_YET);
  DBUG_RETURN(error_num);
}
#endif

int ha_vp::rnd_next_init()
{
  int error_num;
  DBUG_ENTER("ha_vp::rnd_next_init");
  DBUG_PRINT("info",("vp this=%p", this));
  check_select_column(TRUE);
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
  if (rnd_init_and_first)
  {
    if ((error_num = set_rnd_bitmap()))
      DBUG_RETURN(error_num);
    rnd_init_and_first = FALSE;
    cb_state = CB_SEL_RND;
  } else if (cb_state != CB_SEL_RND)
  {
    reset_rnd_bitmap();
    cb_state = CB_SEL_RND;
  }
  DBUG_RETURN(0);
}

int ha_vp::pre_rnd_next(
  bool use_parallel
) {
  int error_num;
  TABLE *table2;
  DBUG_ENTER("ha_vp::pre_rnd_next");
  DBUG_PRINT("info",("vp this=%p", this));
  /* do not copy table data at alter table */
  if (sql_command == SQLCOM_ALTER_TABLE)
    DBUG_RETURN(0);

  use_pre_call = TRUE;
  if ((error_num = rnd_next_init()))
    DBUG_RETURN(error_num);

#ifdef HA_CAN_BULK_ACCESS
  if (!rnd_scan || single_table)
    need_bulk_access_finish = FALSE;
  else
    need_bulk_access_finish = TRUE;
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
#endif

  table2 = part_tables[child_table_idx].table;
  DBUG_RETURN(table2->file->pre_rnd_next(use_parallel));
}

int ha_vp::rnd_next(
  uchar *buf
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::rnd_next");
  DBUG_PRINT("info",("vp this=%p", this));
  /* do not copy table data at alter table */
  if (sql_command == SQLCOM_ALTER_TABLE)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  if (use_pre_call)
    use_pre_call = FALSE;
  else if ((error_num = rnd_next_init()))
    DBUG_RETURN(error_num);

  table2 = part_tables[child_table_idx].table;
  do {
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    if ((error_num = table2->file->ha_rnd_next(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0])))
#else
    if ((error_num = table2->file->rnd_next(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0])))
#endif
    {
      if (error_num == HA_ERR_RECORD_DELETED)
        continue;
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if ((error_num = get_child_record_by_idx(child_table_idx, ptr_diff)))
    {
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if (
      !single_table &&
      (error_num = get_child_record_by_pk(ptr_diff)) &&
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    ) {
      DBUG_RETURN(error_num);
    }
  } while (error_num);
  table->status = table2->status;
  DBUG_RETURN(0);
}

void ha_vp::position(
  const uchar *record
) {
  int roop_count;
  uint roop_count2;
  TABLE *table2;
  handler *file2;
  ha_vp *tmp_vp = this;
  DBUG_ENTER("ha_vp::position");
  DBUG_PRINT("info",("vp this=%p", this));
#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp read_bitmap is %d-%u", roop_count,
      ((uchar *) table->read_set->bitmap)[roop_count]));
    DBUG_PRINT("info",("vp write_bitmap is %d-%u", roop_count,
      ((uchar *) table->write_set->bitmap)[roop_count]));
  }
#endif
  DBUG_PRINT("info",("vp ref=%p", ref));
  DBUG_PRINT("info",("vp ref_length=%u", ref_length));
  memcpy(ref + (child_ref_length * share->table_count), &tmp_vp,
    sizeof(ha_vp *));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(use_tables, roop_count))
    {
      DBUG_PRINT("info",("vp roop_count=%d is set", roop_count));
      table2 = part_tables[roop_count].table;
#ifndef DBUG_OFF
      int roop_count2;
      for (roop_count2 = 0; roop_count2 < (int) table2->s->column_bitmap_size;
        roop_count2++)
      {
        DBUG_PRINT("info",("vp child read_bitmap is %d-%d-%u",
          roop_count, roop_count2,
          ((uchar *) table2->read_set->bitmap)[roop_count2]));
        DBUG_PRINT("info",("vp child write_bitmap is %d-%d-%u",
          roop_count, roop_count2,
          ((uchar *) table2->write_set->bitmap)[roop_count2]));
      }
#endif
      file2 = table2->file;
      file2->position(table2->record[0]);
      memcpy(ref + (child_ref_length * roop_count), file2->ref,
        file2->ref_length);
      DBUG_PRINT("info",("vp ref copy=%d", roop_count));
#ifndef DBUG_OFF
/*
      for (roop_count2 = 0; roop_count2 < (int) file2->ref_length;
        roop_count2++)
        DBUG_PRINT("info",("vp ref[roop_count2]=%x", file2->ref[roop_count2]));
*/
#endif
    }
  }
  /* for cmp_ref */
  KEY *key_info = &table->key_info[table_share->primary_key];
  KEY_PART_INFO *key_part = key_info->key_part;
  if (record != table->record[0])
  {
    my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(record, table->record[0]);
    for (roop_count2 = 0; roop_count2 < vp_user_defined_key_parts(key_info);
      ++roop_count2)
    {
      key_part[roop_count2].field->move_field_offset(ptr_diff);
    }
    key_copy(
      ref + (child_ref_length * share->table_count) + sizeof(ha_vp *),
      (uchar *) record,
      key_info,
      key_info->key_length);
    for (roop_count2 = 0; roop_count2 < vp_user_defined_key_parts(key_info);
      ++roop_count2)
    {
      key_part[roop_count2].field->move_field_offset(-ptr_diff);
    }
  } else {
    key_copy(
      ref + (child_ref_length * share->table_count) + sizeof(ha_vp *),
      (uchar *) record,
      key_info,
      key_info->key_length);
  }
  DBUG_PRINT("info",("vp ref=%p", ref));
  DBUG_PRINT("info",("vp ref_length=%u", ref_length));
  DBUG_VOID_RETURN;
}

int ha_vp::rnd_pos(
  uchar *buf,
  uchar *pos
) {
  int error_num = 0, error_num2, roop_count;
  ha_vp *tmp_vp;
  TABLE *table2 = NULL;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::rnd_pos");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp cb_state=%d", cb_state));
/*
  if (rnd_init_and_first)
  {
    if ((error_num = set_rnd_bitmap()))
      DBUG_RETURN(error_num);
    rnd_init_and_first = FALSE;
    cb_state = CB_SEL_RND;
  } else if (cb_state != CB_SEL_RND)
  {
    reset_rnd_bitmap();
    cb_state = CB_SEL_RND;
  }
*/
  DBUG_PRINT("info",("vp pos=%p", pos));
  DBUG_PRINT("info",("vp buf=%p", buf));
  memcpy(&tmp_vp, pos + (child_ref_length * share->table_count),
    sizeof(ha_vp *));
  DBUG_PRINT("info",("vp tmp_vp=%p", tmp_vp));
  if (tmp_vp == this)
  {
    if (rnd_init_and_first)
    {
      if ((error_num = set_rnd_bitmap()))
        DBUG_RETURN(error_num);
      rnd_init_and_first = FALSE;
      cb_state = CB_SEL_RND;
    } else if (cb_state != CB_SEL_RND)
    {
      reset_rnd_bitmap();
      cb_state = CB_SEL_RND;
    }
  } else {
    if ((error_num = set_rnd_bitmap_from_another(tmp_vp)))
      DBUG_RETURN(error_num);
  }

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    DBUG_PRINT("info",("vp use_tables[%d]=%s", roop_count,
      vp_bit_is_set(use_tables, roop_count) ? "TRUE" : "FALSE"));
    DBUG_PRINT("info",("vp pruned_tables[%d]=%s", roop_count,
      vp_bit_is_set(pruned_tables, roop_count) ? "TRUE" : "FALSE"));
    if (
      vp_bit_is_set(use_tables, roop_count) &&
      !vp_bit_is_set(pruned_tables, roop_count)
    ) {
      DBUG_PRINT("info",("vp roop_count=%d is set", roop_count));
      table2 = part_tables[roop_count].table;
      if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
        (error_num = table2->file->ha_rnd_pos(table2->record[0],
          pos + (child_ref_length * roop_count))) ||
#else
        (error_num = table2->file->rnd_pos(table2->record[0],
          pos + (child_ref_length * roop_count))) ||
#endif
        (error_num = get_child_record_by_idx(roop_count, ptr_diff))
      ) {
        table->status = table2->status;
        DBUG_RETURN(error_num);
      }
    }
  }

  DBUG_PRINT("info",("vp pruned=%s", pruned ? "TRUE" : "FALSE"));
  if (pruned)
  {
    uchar *use_tables_bak = use_tables;
    use_tables = pruned_tables;
    if ((error_num = get_child_record_by_pk(ptr_diff)))
    {
      use_tables = use_tables_bak;
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    use_tables = use_tables_bak;
  }
  table->status = table2->status;

  if (tmp_vp != this && inited == NONE)
  {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (vp_bit_is_set(rnd_inited_tables, roop_count))
      {
        DBUG_PRINT("info",("vp table_count=%d", roop_count));
        DBUG_PRINT("info",("vp child_table=%p",
          part_tables[roop_count].table));
        if ((error_num2 =
          part_tables[roop_count].table->file->ha_rnd_end()))
          error_num = error_num2;
      }
    }
  }
  DBUG_RETURN(error_num);
}

int ha_vp::cmp_ref(
  const uchar *ref1,
  const uchar *ref2
) {
  int ret;
  KEY *key_info = &table->key_info[table_share->primary_key];
  uint store_length, roop_count;
  KEY_PART_INFO *key_part = key_info->key_part;
  const uchar *key1 = ref1 + (child_ref_length * share->table_count) +
    sizeof(ha_vp *);
  const uchar *key2 = ref2 + (child_ref_length * share->table_count) +
    sizeof(ha_vp *);
  DBUG_ENTER("ha_vp::cmp_ref");
  DBUG_PRINT("info",("vp ref1=%p", ref1));
  DBUG_PRINT("info",("vp ref2=%p", ref2));
  for (roop_count = 0; roop_count < vp_user_defined_key_parts(key_info);
    ++roop_count)
  {
    store_length = key_part[roop_count].store_length;
    /* columns of primary key have no null bit */
    if ((ret = key_part[roop_count].field->key_cmp(key1, key2)))
    {
      DBUG_PRINT("info",("vp ret=%d", ret));
      DBUG_RETURN(ret);
    }
    key1 += store_length;
    key2 += store_length;
  }
/*
  if (key_buf_cmp(
    key_info,
    vp_user_defined_key_parts(key_info),
    ref1 + (child_ref_length * share->table_count) + sizeof(ha_vp *),
    ref2 + (child_ref_length * share->table_count) + sizeof(ha_vp *)
  ))
    ret = 1;
*/
  DBUG_PRINT("info",("vp ret=0"));
  DBUG_RETURN(0);
}

float vp_ft_find_relevance(
  FT_INFO *handler,
  uchar *record,
  uint length
) {
  st_vp_ft_info *info = (st_vp_ft_info*) handler;
  return info->file->ft_find_relevance(handler, record, length);
}

float ha_vp::ft_find_relevance(
  FT_INFO *handler,
  uchar *record,
  uint length
) {
  st_vp_ft_info *info = (st_vp_ft_info*) handler;
  TABLE *child_table = part_tables[info->target->table_idx].table;
  DBUG_ENTER("ha_vp::ft_find_relevance");
  if (
    info->ft_handler &&
    info->ft_handler->please &&
    info->ft_handler->please->find_relevance
  ) {
    if (length)
      DBUG_RETURN(info->ft_handler->please->find_relevance(
        info->ft_handler, record, length));
    else
      DBUG_RETURN(info->ft_handler->please->find_relevance(
        info->ft_handler, child_table->record[0], 0));
  }
  DBUG_RETURN((float) -1.0);
}

float vp_ft_get_relevance(
  FT_INFO *handler
) {
  st_vp_ft_info *info = (st_vp_ft_info*) handler;
  return info->file->ft_get_relevance(handler);
}

float ha_vp::ft_get_relevance(
  FT_INFO *handler
) {
  st_vp_ft_info *info = (st_vp_ft_info*) handler;
  DBUG_ENTER("ha_vp::ft_get_relevance");
  if (
    info->ft_handler &&
    info->ft_handler->please &&
    info->ft_handler->please->get_relevance
  ) {
    DBUG_RETURN(info->ft_handler->please->get_relevance(info->ft_handler));
  }
  DBUG_RETURN((float) -1.0);
}

void vp_ft_close_search(
  FT_INFO *handler
) {
  st_vp_ft_info *info = (st_vp_ft_info*) handler;
  info->file->ft_close_search(handler);
}

void ha_vp::ft_close_search(
  FT_INFO *handler
) {
  st_vp_ft_info *info = (st_vp_ft_info*) handler;
  DBUG_ENTER("ha_vp::ft_close_search");
  if (
    info->ft_handler &&
    info->ft_handler->please &&
    info->ft_handler->please->close_search
  ) {
    info->ft_handler->please->close_search(info->ft_handler);
  }
  DBUG_VOID_RETURN;
}

_ft_vft vp_ft_vft = {
  NULL, // vp_ft_read_next
  vp_ft_find_relevance,
  vp_ft_close_search,
  vp_ft_get_relevance,
  NULL // vp_ft_reinit_search
};

FT_INFO *ha_vp::ft_init_ext(
  uint flags,
  uint inx,
  String *key
) {
  VP_CORRESPOND_KEY *tmp_correspond_key;
  uchar *tmp_select_ignore;
  st_vp_ft_info *tmp_ft_info;
  DBUG_ENTER("ha_vp::ft_init_ext");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp flags=%u", flags));
  DBUG_PRINT("info",("vp inx=%u", inx));
  DBUG_PRINT("info",("vp key=%s", key->c_ptr_safe()));
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
    memset(ft_inited_tables, 0, sizeof(uchar) * share->use_tables_size);
  }

  if (!ft_current)
  {
    if (!(ft_current = (st_vp_ft_info *)
      my_malloc(sizeof(st_vp_ft_info), MYF(MY_WME | MY_ZEROFILL))))
    {
      store_error_num = HA_ERR_OUT_OF_MEM;
      DBUG_RETURN(NULL);
    }
    if (tmp_ft_info)
      tmp_ft_info->next = ft_current;
    else
      ft_first = ft_current;
  }

  if (lock_mode > 0 || lock_type_ext == F_WRLCK)
  {
    tmp_select_ignore = select_ignore_with_lock;
  } else {
    tmp_select_ignore = select_ignore;
  }

  tmp_correspond_key = share->keys[inx].correspond_key;
  while (vp_bit_is_set(tmp_select_ignore, tmp_correspond_key->table_idx))
  {
    if (!(tmp_correspond_key = tmp_correspond_key->next))
    {
      my_printf_error(ER_VP_IGNORED_CORRESPOND_KEY_NUM,
        ER_VP_IGNORED_CORRESPOND_KEY_STR, MYF(0), inx);
      store_error_num = ER_VP_IGNORED_CORRESPOND_KEY_NUM;
      DBUG_RETURN(NULL);
    }
  }
  ft_current->target = tmp_correspond_key;

  ft_current->please = &vp_ft_vft;
  ft_current->file = this;
  ft_current->used_in_where = (flags & FT_SORTED);
  ft_current->flags = flags;
  ft_current->inx = inx;
  ft_current->key = key;

  ft_current->ft_handler =
    part_tables[ft_current->target->table_idx].table->file->ft_init_ext(
      flags, ft_current->target->key_idx, key);
  part_tables[ft_current->target->table_idx].table->file->ft_handler =
    ft_current->ft_handler;
  vp_set_bit(ft_inited_tables, ft_current->target->table_idx);

  ft_count++;

  DBUG_RETURN((FT_INFO *) ft_current);
}

int ha_vp::ft_init()
{
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::ft_init");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  ft_inited = TRUE;
  if (store_error_num)
    DBUG_RETURN(store_error_num);
  if (active_index == MAX_KEY && inited == NONE)
  {
    rnd_scan = TRUE;
    DBUG_PRINT("info",("vp init_rnd_bitmap=%s",
      init_sel_rnd_bitmap ? "TRUE" : "FALSE"));
    DBUG_PRINT("info",("vp cb_state=%d", cb_state));
#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
    if (!table->pos_in_table_list->parent_l && lock_type_ext == F_WRLCK)
    {
      check_and_set_bitmap_for_update(TRUE);
    }
#endif
    init_select_column(TRUE);
    memset(rnd_inited_tables, 0, sizeof(uchar) * share->use_tables_size);
    memset(pruned_tables, 0, sizeof(uchar) * share->use_tables_size);
    pruned = FALSE;
    rnd_init_and_first = TRUE;
    ft_init_without_index_init = TRUE;
  } else {
    ft_init_idx = active_index;
    ft_init_without_index_init = FALSE;
    check_select_column(FALSE);
  }
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      vp_bit_is_set(ft_inited_tables, roop_count) &&
      (error_num = part_tables[roop_count].table->file->ft_init())
    )
      goto error;
  }
  DBUG_RETURN(0);

error:
  for (roop_count--; roop_count >= 0; roop_count--)
  {
    if (vp_bit_is_set(ft_inited_tables, roop_count))
      part_tables[roop_count].table->file->ft_end();
  }
  DBUG_RETURN(error_num);
}

#ifdef HA_CAN_BULK_ACCESS
int ha_vp::pre_ft_init()
{
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::pre_ft_init");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  ft_inited = TRUE;
  if (store_error_num)
    DBUG_RETURN(store_error_num);
  if (active_index == MAX_KEY && inited == NONE)
  {
    rnd_scan = TRUE;
    DBUG_PRINT("info",("vp init_rnd_bitmap=%s",
      init_sel_rnd_bitmap ? "TRUE" : "FALSE"));
    DBUG_PRINT("info",("vp cb_state=%d", cb_state));
#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
    if (!table->pos_in_table_list->parent_l && lock_type_ext == F_WRLCK)
    {
      check_and_set_bitmap_for_update(TRUE);
    }
#endif
    init_select_column(TRUE);
    memset(rnd_inited_tables, 0, sizeof(uchar) * share->use_tables_size);
    memset(pruned_tables, 0, sizeof(uchar) * share->use_tables_size);
    pruned = FALSE;
    rnd_init_and_first = TRUE;
    ft_init_without_index_init = TRUE;
  } else {
    ft_init_idx = active_index;
    ft_init_without_index_init = FALSE;
    check_select_column(FALSE);
  }
  bulk_access_pre_called = TRUE;
  need_bulk_access_finish = FALSE;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      vp_bit_is_set(ft_inited_tables, roop_count) &&
      (error_num = part_tables[roop_count].table->file->pre_ft_init())
    )
      goto error;
  }
  DBUG_RETURN(0);

error:
  for (roop_count--; roop_count >= 0; roop_count--)
  {
    if (vp_bit_is_set(ft_inited_tables, roop_count))
      part_tables[roop_count].table->file->pre_ft_end();
  }
  DBUG_RETURN(error_num);
}
#endif

void ha_vp::ft_end()
{
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::ft_end");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  rnd_scan = FALSE;
  ft_inited = FALSE;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(ft_inited_tables, roop_count))
    {
      part_tables[roop_count].table->file->ft_end();
    }
    if (
      ft_init_without_index_init &&
      vp_bit_is_set(rnd_inited_tables, roop_count)
    ) {
      DBUG_PRINT("info",("vp table_count=%d", roop_count));
      DBUG_PRINT("info",("vp child_table=%p", part_tables[roop_count].table));
      if ((error_num =
        part_tables[roop_count].table->file->ha_index_or_rnd_end()))
        store_error_num = error_num;
    }
  }
  handler::ft_end();
  DBUG_VOID_RETURN;
}

#ifdef HA_CAN_BULK_ACCESS
int ha_vp::pre_ft_end()
{
  int error_num;
  DBUG_ENTER("ha_vp::pre_ft_end");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp table=%p", table));
  DBUG_ASSERT(!vp_bit_is_set(rnd_inited_tables, child_table_idx));
  rnd_scan = FALSE;
  ft_inited = FALSE;
  error_num = 0;
  if (vp_bit_is_set(ft_inited_tables, child_table_idx))
  {
    DBUG_PRINT("info",("vp table_count=%d", child_table_idx));
    DBUG_PRINT("info",("vp child_table=%p",
      part_tables[child_table_idx].table));
    error_num =
      part_tables[child_table_idx].table->file->pre_ft_end();
  }
  if (
    !error_num &&
    ft_init_without_index_init &&
    vp_bit_is_set(rnd_inited_tables, child_table_idx)
  ) {
    error_num =
      part_tables[child_table_idx].table->file->ha_pre_rnd_end();
  }
  bulk_access_pre_called = FALSE;
  if (!error_num && need_bulk_access_finish)
    DBUG_RETURN(ER_NOT_SUPPORTED_YET);
  DBUG_RETURN(0);
}
#endif

int ha_vp::ft_read_init()
{
  int error_num;
  DBUG_ENTER("ha_vp::ft_read_init");
  DBUG_PRINT("info",("vp this=%p", this));
  if (ft_init_without_index_init && rnd_init_and_first)
  {
    check_select_column(TRUE);
    DBUG_PRINT("info",("vp cb_state=%d", cb_state));
    if ((error_num = set_rnd_bitmap()))
      DBUG_RETURN(error_num);
    cb_state = CB_SEL_RND;
    rnd_init_and_first = FALSE;
  }
  DBUG_RETURN(0);
}

int ha_vp::pre_ft_read(
  bool use_parallel
) {
  int error_num;
  TABLE *table2;
  DBUG_ENTER("ha_vp::pre_ft_read");
  DBUG_PRINT("info",("vp this=%p", this));
  use_pre_call = TRUE;
  if ((error_num = ft_read_init()))
    DBUG_RETURN(error_num);

#ifdef HA_CAN_BULK_ACCESS
  if (single_table)
    need_bulk_access_finish = FALSE;
  else
    need_bulk_access_finish = TRUE;
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
#endif

  table2 = part_tables[child_table_idx].table;
  DBUG_RETURN(table2->file->pre_ft_read(use_parallel));
}

int ha_vp::ft_read(
  uchar *buf
) {
  int error_num;
  TABLE *table2;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("ha_vp::ft_read");
  DBUG_PRINT("info",("vp this=%p", this));
  if (use_pre_call)
    use_pre_call = FALSE;
  else if ((error_num = ft_read_init()))
    DBUG_RETURN(error_num);

  table2 = part_tables[child_table_idx].table;
  do {
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    if ((error_num = table2->file->ha_ft_read(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0])))
#else
    if ((error_num = table2->file->ft_read(
      vp_bit_is_set(share->same_columns, child_table_idx) ?
      buf : table2->record[0])))
#endif
    {
      if (error_num == HA_ERR_RECORD_DELETED)
        continue;
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if ((error_num = get_child_record_by_idx(child_table_idx, ptr_diff)))
    {
      table->status = table2->status;
      DBUG_RETURN(error_num);
    }
    if (
      !single_table &&
      (error_num = get_child_record_by_pk(ptr_diff)) &&
      error_num != HA_ERR_KEY_NOT_FOUND &&
      error_num != HA_ERR_END_OF_FILE
    ) {
      DBUG_RETURN(error_num);
    }
  } while (error_num);
  table->status = table2->status;
  DBUG_RETURN(0);
}

int ha_vp::info(
  uint flag
) {
  int error_num, roop_count, roop_count2, info_src_table, table_idx, key_idx;
  handler *child_file;
  ha_statistics *child_stats;
  time_t create_time, check_time, update_time;
  ha_rows records;
  ulong mean_rec_length;
  uint block_size;
  ulonglong data_file_length, max_data_file_length, index_file_length,
    auto_increment_value;
  KEY *key_info, *key_info2;
  DBUG_ENTER("ha_vp::info");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp flag=%x", flag));

#if MYSQL_VERSION_ID < 50500
  if (table->children_attached || is_clone)
#else
  DBUG_PRINT("info",("vp children_attached=%d", children_attached));
  if (children_attached || is_clone)
#endif
  {
    DBUG_PRINT("info",("vp part_tables[0].table=%p", part_tables[0].table));
    memset(use_tables3, 0, sizeof(uchar) * share->use_tables_size);
    if (flag & HA_STATUS_ERRKEY)
    {
      DBUG_PRINT("info",("vp dup_table_idx=%d", dup_table_idx));
      child_file = part_tables[dup_table_idx].table->file;
      if ((error_num = child_file->info(flag)))
        DBUG_RETURN(error_num);
      vp_set_bit(use_tables3, dup_table_idx);

      errkey = child_file->errkey;
      DBUG_PRINT("info",("vp errkey=%d", errkey));
    }

    if (flag & (HA_STATUS_TIME | HA_STATUS_CONST | HA_STATUS_VARIABLE |
      HA_STATUS_AUTO))
    {
      if (share->info_src_table)
        info_src_table = share->info_src_table - 1;
      else
        info_src_table = 0;

      child_file = part_tables[info_src_table].table->file;
      if ((error_num = child_file->info(flag)))
        DBUG_RETURN(error_num);
      vp_set_bit(use_tables3, info_src_table);

      child_stats = &child_file->stats;
      update_time = child_stats->update_time;
      max_data_file_length = child_stats->max_data_file_length;
      create_time = child_stats->create_time;
      block_size = child_stats->block_size;
      data_file_length = child_stats->data_file_length;
      index_file_length = child_stats->index_file_length;
      records = child_stats->records;
      mean_rec_length = child_stats->mean_rec_length;
      check_time = child_stats->check_time;

      if (
        (flag & HA_STATUS_AUTO) &&
        info_src_table != share->auto_increment_table
      ) {
        child_file = part_tables[share->auto_increment_table].table->file;
        if ((error_num = child_file->info(flag)))
          DBUG_RETURN(error_num);
        vp_set_bit(use_tables3, share->auto_increment_table);
        child_stats = &child_file->stats;
        auto_increment_value = child_stats->auto_increment_value;
      } else
        auto_increment_value = child_stats->auto_increment_value;

      if (!share->info_src_table)
      {
        for (roop_count = 1; roop_count < share->table_count; roop_count++)
        {
          child_file = part_tables[roop_count].table->file;
          if ((error_num = child_file->info(flag)))
            DBUG_RETURN(error_num);
          vp_set_bit(use_tables3, roop_count);

          child_stats = &child_file->stats;
          if (difftime(child_stats->update_time, update_time) > 0)
            update_time = child_stats->update_time;
          max_data_file_length += child_stats->max_data_file_length;
          if (difftime(child_stats->create_time, create_time) > 0)
            create_time = child_stats->create_time;
          if (block_size < child_stats->block_size)
            block_size = child_stats->block_size;
          data_file_length += child_stats->data_file_length;
          index_file_length += child_stats->index_file_length;
          mean_rec_length += child_stats->mean_rec_length;
          if (difftime(child_stats->check_time, check_time) > 0)
            check_time = child_stats->check_time;
        }
      }
      if (flag & HA_STATUS_CONST)
      {
        for (roop_count = 0; roop_count < (int) table_share->keys;
          roop_count++)
        {
          table_idx = share->keys[roop_count].correspond_key->table_idx;
          key_idx = share->keys[roop_count].correspond_key->key_idx;
          if (vp_bit_is_set(use_tables3, table_idx))
          {
            child_file = part_tables[table_idx].table->file;
            if ((error_num = child_file->info(flag)))
              DBUG_RETURN(error_num);
            vp_set_bit(use_tables3, table_idx);
          }
          key_info = &table->key_info[roop_count];
          key_info2 = &part_tables[table_idx].table->key_info[key_idx];
          for (roop_count2 = 0;
            roop_count2 < (int) vp_user_defined_key_parts(key_info);
            roop_count2++)
            key_info->rec_per_key[roop_count2] =
              key_info2->rec_per_key[roop_count2];
        }
      }

      if (flag & HA_STATUS_TIME)
      {
#ifndef DBUG_OFF
        {
          struct tm *ts, tmp_ts;
          char buf[80];
          ts = localtime_r(&check_time, &tmp_ts);
          strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
          DBUG_PRINT("info",("vp update_time=%s", buf));
        }
#endif
        stats.update_time = (ulong) update_time;
      }
      if (flag & HA_STATUS_CONST)
      {
        DBUG_PRINT("info",("vp max_data_file_length=%llu",
          max_data_file_length));
        stats.max_data_file_length = max_data_file_length;
#ifndef DBUG_OFF
        {
          struct tm *ts, tmp_ts;
          char buf[80];
          ts = localtime_r(&check_time, &tmp_ts);
          strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
          DBUG_PRINT("info",("vp create_time=%s", buf));
        }
#endif
        stats.create_time = (ulong) create_time;
        DBUG_PRINT("info",("vp block_size=%u", block_size));
        stats.block_size = block_size;
      }
      if (flag & HA_STATUS_VARIABLE)
      {
        DBUG_PRINT("info",("vp data_file_length=%llu", data_file_length));
        stats.data_file_length = data_file_length;
        DBUG_PRINT("info",("vp index_file_length=%llu", index_file_length));
        stats.index_file_length = index_file_length;
        DBUG_PRINT("info",("vp records=%llu", records));
        stats.records = records;
        DBUG_PRINT("info",("vp mean_rec_length=%lu", mean_rec_length));
        stats.mean_rec_length = mean_rec_length;
#ifndef DBUG_OFF
        {
          struct tm *ts, tmp_ts;
          char buf[80];
          ts = localtime_r(&check_time, &tmp_ts);
          strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
          DBUG_PRINT("info",("vp check_time=%s", buf));
        }
#endif
        stats.check_time = (ulong) check_time;
      }
      if (flag & HA_STATUS_AUTO)
      {
        DBUG_PRINT("info",("vp block_size=%u", block_size));
        stats.auto_increment_value = auto_increment_value;
      }
    }
  }
  DBUG_RETURN(0);
}

ha_rows ha_vp::records()
{
  int info_src_table;
  DBUG_ENTER("ha_vp::records");
  DBUG_PRINT("info",("vp this=%p", this));
  if (share->info_src_table)
    info_src_table = share->info_src_table - 1;
  else
    info_src_table = 0;

  if (part_tables[info_src_table].table->file->ha_table_flags() |
    HA_HAS_RECORDS)
    DBUG_RETURN(part_tables[info_src_table].table->file->records());
  else {
    int roop_count;
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (part_tables[roop_count].table->file->ha_table_flags() |
        HA_HAS_RECORDS)
        DBUG_RETURN(part_tables[roop_count].table->file->records());
    }
  }
  DBUG_RETURN(HA_POS_ERROR);
}

ha_rows ha_vp::records_in_range(
  uint idx,
  key_range *start_key,
  key_range *end_key
) {
  int error_num, active_index_bak, roop_count;
  ha_rows res_rows;
  KEY *key_info, *key_info2;
  DBUG_ENTER("ha_vp::records_in_range");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp part_tables[0].table=%p", part_tables[0].table));
  DBUG_PRINT("info",("vp start_key=%p", start_key));
  DBUG_PRINT("info",("vp end_key=%p", end_key));
  child_keyread = FALSE;
  single_table = FALSE;
  memcpy(work_bitmap3, table->read_set->bitmap,
    sizeof(uchar) * share->bitmap_size);
  memcpy(work_bitmap4, table->write_set->bitmap,
    sizeof(uchar) * share->bitmap_size);
  if (
    (error_num = choose_child_index(idx, work_bitmap3, work_bitmap4,
      &child_table_idx, &child_key_idx))
  )
    DBUG_RETURN((ha_rows) 0);
  set_child_pt_bitmap();

  active_index_bak = active_index;
  active_index = idx;
  if (start_key)
  {
    child_start_key.keypart_map = start_key->keypart_map;
    child_start_key.flag = start_key->flag;
    child_start_key.key = create_child_key(
      start_key->key, child_key_different, start_key->keypart_map,
      start_key->length, &child_start_key.length);
  }
  if (end_key)
  {
    child_end_key.keypart_map = end_key->keypart_map;
    child_end_key.flag = end_key->flag;
    child_end_key.key =  create_child_key(
      end_key->key, child_end_key_different, end_key->keypart_map,
      end_key->length, &child_end_key.length);
  }
  active_index = active_index_bak;
  res_rows = part_tables[child_table_idx].table->file->records_in_range(
    child_key_idx,
    start_key ? &child_start_key : NULL,
    end_key ? &child_end_key : NULL);

  key_info = &table->key_info[idx];
  key_info2 = &part_tables[child_table_idx].table->key_info[child_key_idx];
  for (roop_count = 0;
    roop_count < (int) vp_user_defined_key_parts(key_info); roop_count++)
    key_info->rec_per_key[roop_count] = key_info2->rec_per_key[roop_count];
  DBUG_RETURN(res_rows);
}

const char *ha_vp::table_type() const
{
  DBUG_ENTER("ha_vp::table_type");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN("VP");
}

#if MYSQL_VERSION_ID < 50500
static ulonglong vp_table_flags_msm = 0;
#else
#ifdef HA_CAN_MULTISTEP_MERGE
static ulonglong vp_table_flags_msm = HA_CAN_MULTISTEP_MERGE;
#else
static ulonglong vp_table_flags_msm = 0;
#endif
#endif

ulonglong ha_vp::table_flags() const
{
  DBUG_ENTER("ha_vp::table_flags");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(
    HA_REC_NOT_IN_SEQ |
    HA_CAN_GEOMETRY |
    HA_NULL_IN_KEY |
    HA_CAN_INDEX_BLOBS |
    HA_AUTO_PART_KEY |
    HA_REQUIRE_PRIMARY_KEY |
    HA_CAN_RTREEKEYS |
    HA_PRIMARY_KEY_REQUIRED_FOR_DELETE |
    /* HA_NO_PREFIX_CHAR_KEYS | */
    HA_CAN_FULLTEXT |
    HA_CAN_SQL_HANDLER |
    HA_FILE_BASED |
    HA_CAN_INSERT_DELAYED |
    HA_CAN_BIT_FIELD |
    HA_NO_COPY_ON_ALTER |
    vp_table_flags_msm |
    additional_table_flags |
    (share ? share->additional_table_flags : 0)
  );
}

const char *ha_vp::index_type(
  uint key_number
) {
  KEY *key_info;
  DBUG_ENTER("ha_vp::index_type");
  DBUG_PRINT("info",("vp this=%p", this));
  key_info = &table_share->key_info[key_number];
  DBUG_RETURN(
    (key_info->flags & HA_FULLTEXT) ? "FULLTEXT" :
    (key_info->flags & HA_SPATIAL) ? "SPATIAL" :
    (key_info->algorithm == HA_KEY_ALG_HASH) ? "HASH" :
    (key_info->algorithm == HA_KEY_ALG_RTREE) ? "RTREE" :
    "BTREE"
  );
}

ulong ha_vp::index_flags(
  uint idx,
  uint part,
  bool all_parts
) const {
  KEY *key_info;
  DBUG_ENTER("ha_vp::index_flags");
  DBUG_PRINT("info",("vp this=%p", this));
  key_info = &table_share->key_info[idx];
  DBUG_RETURN(
    (key_info->algorithm == HA_KEY_ALG_FULLTEXT) ?
      0 :
    (key_info->algorithm == HA_KEY_ALG_HASH) ?
      HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR :
    HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
    HA_KEYREAD_ONLY
  );
}

uint ha_vp::max_supported_record_length() const
{
  DBUG_ENTER("ha_vp::max_supported_record_length");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(HA_MAX_REC_LENGTH);
}

uint ha_vp::max_supported_keys() const
{
  DBUG_ENTER("ha_vp::max_supported_keys");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(MAX_KEY);
}

uint ha_vp::max_supported_key_parts() const
{
  DBUG_ENTER("ha_vp::max_supported_key_parts");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(MAX_REF_PARTS);
}

uint ha_vp::max_supported_key_length() const
{
  DBUG_ENTER("ha_vp::max_supported_key_length");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(VP_MAX_KEY_LENGTH);
}

uint ha_vp::max_supported_key_part_length() const
{
  DBUG_ENTER("ha_vp::max_supported_key_part_length");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(VP_MAX_KEY_LENGTH);
}

uint8 ha_vp::table_cache_type()
{
  DBUG_ENTER("ha_vp::table_cache_type");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(share->support_table_cache);
}

#ifdef HANDLER_HAS_NEED_INFO_FOR_AUTO_INC
bool ha_vp::need_info_for_auto_inc()
{
  handler *file = part_tables[share->auto_increment_table].table->file;
  DBUG_ENTER("ha_vp::need_info_for_auto_inc");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(file->need_info_for_auto_inc());
}
#endif

int ha_vp::update_auto_increment()
{
  int error_num;
  handler *file = part_tables[share->auto_increment_table].table->file;
  DBUG_ENTER("ha_vp::update_auto_increment");
  DBUG_PRINT("info",("vp this=%p", this));
  error_num = file->update_auto_increment();
  insert_id_for_cur_row = file->insert_id_for_cur_row;
  DBUG_PRINT("info",("vp insert_id_for_cur_row=%llu", insert_id_for_cur_row));
  DBUG_RETURN(error_num);
}

void ha_vp::set_next_insert_id(
  ulonglong id
) {
  handler *file = part_tables[share->auto_increment_table].table->file;
  DBUG_ENTER("ha_vp::set_next_insert_id");
  DBUG_PRINT("info",("vp this=%p", this));
  file->set_next_insert_id(id);
  DBUG_VOID_RETURN;
}

void ha_vp::get_auto_increment(
  ulonglong offset,
  ulonglong increment,
  ulonglong nb_desired_values,
  ulonglong *first_value,
  ulonglong *nb_reserved_values
) {
  handler *file = part_tables[share->auto_increment_table].table->file;
  DBUG_ENTER("ha_vp::get_auto_increment");
  DBUG_PRINT("info",("vp this=%p", this));
  file->get_auto_increment(
    offset,
    increment,
    nb_desired_values,
    first_value,
    nb_reserved_values
  );
  DBUG_VOID_RETURN;
}

void ha_vp::restore_auto_increment(
  ulonglong prev_insert_id
) {
  handler *file = part_tables[share->auto_increment_table].table->file;
  DBUG_ENTER("ha_vp::restore_auto_increment");
  DBUG_PRINT("info",("vp this=%p", this));
  file->restore_auto_increment(prev_insert_id);
  DBUG_VOID_RETURN;
}

void ha_vp::release_auto_increment()
{
  int roop_count;
/*
  handler *file = part_tables[share->auto_increment_table].table->file;
*/
  DBUG_ENTER("ha_vp::release_auto_increment");
  DBUG_PRINT("info",("vp this=%p", this));
/*
  file->ha_release_auto_increment();
*/
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
    part_tables[roop_count].table->file->ha_release_auto_increment();
  DBUG_VOID_RETURN;
}

int ha_vp::reset_auto_increment(
  ulonglong value
) {
  handler *file = part_tables[share->auto_increment_table].table->file;
  DBUG_ENTER("ha_vp::reset_auto_increment");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(file->ha_reset_auto_increment(value));
}

#ifdef VP_HANDLER_START_BULK_INSERT_HAS_FLAGS
void ha_vp::start_bulk_insert(
  ha_rows rows,
  uint flags
)
#else
void ha_vp::start_bulk_insert(
  ha_rows rows
)
#endif
{
  int roop_count;
  DBUG_ENTER("ha_vp::start_bulk_insert");
  DBUG_PRINT("info",("vp this=%p", this));
  if (
    !table->next_number_field ||
    vp_param_allow_bulk_autoinc(ha_thd(), share->allow_bulk_autoinc)
  ) {
    bulk_insert = TRUE;
    if (!init_ins_bitmap)
    {
      int child_table_idx_bak = child_table_idx;
      child_table_idx = share->table_count;
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (!(vp_bit_is_set(update_ignore, roop_count)))
        {
          clear_child_bitmap(roop_count);
          set_child_bitmap(
            (uchar *) table->write_set->bitmap,
            roop_count, TRUE);
          set_child_bitmap(
            (uchar *) table->read_set->bitmap,
            roop_count, FALSE);
          memcpy(ins_child_bitmaps[0][roop_count], table->read_set->bitmap,
            part_tables[roop_count].table->s->column_bitmap_size);
          memcpy(ins_child_bitmaps[1][roop_count], table->write_set->bitmap,
            part_tables[roop_count].table->s->column_bitmap_size);
        }
      }
      child_table_idx = child_table_idx_bak;
      init_ins_bitmap = TRUE;
      cb_state = CB_INSERT;
    } else if (cb_state != CB_INSERT)
    {
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (!(vp_bit_is_set(update_ignore, roop_count)))
        {
          memcpy(table->read_set->bitmap, ins_child_bitmaps[0][roop_count],
            part_tables[roop_count].table->s->column_bitmap_size);
          memcpy(table->write_set->bitmap, ins_child_bitmaps[1][roop_count],
            part_tables[roop_count].table->s->column_bitmap_size);
        }
      }
      cb_state = CB_INSERT;
    }
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
#ifdef VP_HANDLER_START_BULK_INSERT_HAS_FLAGS
        part_tables[roop_count].table->file->ha_start_bulk_insert(rows, flags);
#else
        part_tables[roop_count].table->file->ha_start_bulk_insert(rows);
#endif
    }
  }
  DBUG_VOID_RETURN;
}

int ha_vp::end_bulk_insert()
{
  DBUG_ENTER("ha_vp::end_bulk_insert");
  DBUG_RETURN(end_bulk_insert(FALSE));
}

int ha_vp::end_bulk_insert(
  bool abort
) {
  int error_num = 0, error_num2, roop_count;
  DBUG_ENTER("ha_vp::end_bulk_insert");
  DBUG_PRINT("info",("vp this=%p", this));
  bulk_insert = FALSE;
  if (
    !table->next_number_field ||
    vp_param_allow_bulk_autoinc(ha_thd(), share->allow_bulk_autoinc)
  ) {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        if ((error_num2 =
          part_tables[roop_count].table->file->ha_end_bulk_insert()))
          error_num = error_num2;
      }
    }
  }
  DBUG_RETURN(error_num);
}

int ha_vp::write_row(
  uchar *buf
) {
  int error_num = 0, roop_count, roop_count2, first_insert = -1;
  uint16 field_index;
  THD *thd = table->in_use;
  TABLE *child_table;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  int child_binlog = vp_param_child_binlog(thd, share->child_binlog);
  ulonglong option_backup = 0;
  longlong auto_inc_val;
#ifndef WITHOUT_VP_BG_ACCESS
  int bgi_mode = vp_param_bgi_mode(thd, share->bgi_mode);
  VP_BG_BASE *base;
#endif
  DBUG_ENTER("ha_vp::write_row");
  DBUG_PRINT("info",("vp this=%p", this));
  dup_table_idx = share->table_count;

#ifdef HA_CAN_BULK_ACCESS
  if (bulk_access_executing && bulk_access_info_exec_tgt->called)
  {
    bgi_mode = 0;
    if (cb_state != CB_INSERT)
    {
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (!(vp_bit_is_set(update_ignore, roop_count)))
        {
          child_table = part_tables[roop_count].table;
          memcpy(child_table->read_set->bitmap,
            bulk_access_info_exec_tgt->ins_child_bitmaps[0][roop_count],
            child_table->s->column_bitmap_size);
          memcpy(child_table->write_set->bitmap,
            bulk_access_info_exec_tgt->ins_child_bitmaps[1][roop_count],
            child_table->s->column_bitmap_size);
        }
      }
      cb_state = CB_INSERT;
    }
  } else {
#endif
#ifndef VP_WITHOUT_HA_STATISTIC_INCREMENT
    ha_statistic_increment(&SSV::ha_write_count);
#endif
#ifdef VP_TABLE_HAS_TIMESTAMP_FIELD_TYPE
    if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_INSERT)
      table->timestamp_field->set_time();
#endif
    if (!bulk_insert && !init_ins_bitmap)
    {
      int child_table_idx_bak = child_table_idx;
      child_table_idx = share->table_count;
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (!(vp_bit_is_set(update_ignore, roop_count)))
        {
          clear_child_bitmap(roop_count);
          set_child_bitmap(
            (uchar *) table->write_set->bitmap,
            roop_count, TRUE);
          set_child_bitmap(
            (uchar *) table->read_set->bitmap,
            roop_count, FALSE);
          child_table = part_tables[roop_count].table;
          memcpy(ins_child_bitmaps[0][roop_count],
            child_table->read_set->bitmap,
            child_table->s->column_bitmap_size);
          memcpy(ins_child_bitmaps[1][roop_count],
            child_table->write_set->bitmap,
            child_table->s->column_bitmap_size);
        }
      }
      child_table_idx = child_table_idx_bak;
      init_ins_bitmap = TRUE;
      cb_state = CB_INSERT;
    } else if (cb_state != CB_INSERT)
    {
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (!(vp_bit_is_set(update_ignore, roop_count)))
        {
          child_table = part_tables[roop_count].table;
          memcpy(child_table->read_set->bitmap,
            ins_child_bitmaps[0][roop_count],
            child_table->s->column_bitmap_size);
          memcpy(child_table->write_set->bitmap,
            ins_child_bitmaps[1][roop_count],
            child_table->s->column_bitmap_size);
        }
      }
      cb_state = CB_INSERT;
    }
#ifdef HA_CAN_BULK_ACCESS
  }
#endif

  memset(use_tables, ~((uchar) 0), sizeof(uchar) * share->use_tables_size);
  memset(use_tables2, ~((uchar) 0), sizeof(uchar) * share->use_tables_size);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(share->same_columns, roop_count))
    {
      DBUG_PRINT("info",("vp child_table %d has same columns", roop_count));
      continue;
    }
    restore_record(part_tables[roop_count].table, s->default_values);
  }
  set_child_pt_bitmap();
  set_child_record_for_update(ptr_diff, 0, TRUE, FALSE);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
    set_child_record_for_insert(ptr_diff, roop_count);
  if (!child_binlog)
  {
#if MYSQL_VERSION_ID < 50500
    option_backup = thd->options;
    thd->options &= ~OPTION_BIN_LOG;
#else
    option_backup = thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_BIN_LOG;
#endif
  }

  if (
    table->next_number_field &&
    !(vp_bit_is_set(update_ignore, share->auto_increment_table))
  ) {
    first_insert = share->auto_increment_table;
  } else {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        first_insert = roop_count;
        break;
      }
    }
  }

  child_table = part_tables[first_insert].table;
  if (!suppress_autoinc)
  {
    child_table->next_number_field = child_table->found_next_number_field;
    if (table->next_number_field)
      child_table->auto_increment_field_not_null =
        table->auto_increment_field_not_null;
    else if (child_table->next_number_field)
      child_table->auto_increment_field_not_null =
        !child_table->next_number_field->is_null();
  }
  DBUG_PRINT("info",("vp child_table[%d]->record[0]=%p",
    first_insert, child_table->record[0]));
#ifndef DBUG_OFF
  my_bitmap_map *tmp_map =
    dbug_tmp_use_all_columns(table, child_table->read_set);
#endif
  error_num = child_table->file->ha_write_row(child_table->record[0]);
#ifndef DBUG_OFF
  dbug_tmp_restore_column_map(child_table->read_set, tmp_map);
#endif
  if (error_num)
  {
    child_table->next_number_field = NULL;
    child_table->auto_increment_field_not_null = FALSE;
    dup_table_idx = first_insert;
    if (!child_binlog)
    {
#if MYSQL_VERSION_ID < 50500
      thd->options = option_backup;
#else
      thd->variables.option_bits = option_backup;
#endif
    }
    goto error;
  }
  if (
    table->next_number_field &&
    child_table->next_number_field &&
    (
      !child_table->auto_increment_field_not_null ||
      (
        !table->next_number_field->val_int() &&
        !(thd->variables.sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO)
      )
    )
  ) {
    table->next_number_field->set_notnull();
    auto_inc_val = child_table->next_number_field->val_int();
    table->file->insert_id_for_cur_row = auto_inc_val;
    DBUG_PRINT("info",("vp auto_inc_val=%llu", auto_inc_val));
    if ((error_num = table->next_number_field->store(
      auto_inc_val, TRUE)))
    {
      child_table->next_number_field = NULL;
      child_table->auto_increment_field_not_null = FALSE;
      dup_table_idx = first_insert;
      if (!child_binlog)
      {
#if MYSQL_VERSION_ID < 50500
        thd->options = option_backup;
#else
        thd->variables.option_bits = option_backup;
#endif
      }
      goto error;
    }
    for (roop_count2 = 0; roop_count2 < share->table_count; roop_count2++)
    {
      if (first_insert == roop_count2)
        continue;
      field_index = share->correspond_columns_p[table_share->fields *
        roop_count2 + table->next_number_field->field_index];
      if (
        field_index < MAX_FIELDS &&
        (error_num = part_tables[roop_count2].table->field[field_index]->
          store(auto_inc_val, TRUE))
      ) {
        child_table->next_number_field = NULL;
        child_table->auto_increment_field_not_null = FALSE;
        dup_table_idx = roop_count2;
        if (!child_binlog)
        {
#if MYSQL_VERSION_ID < 50500
          thd->options = option_backup;
#else
          thd->variables.option_bits = option_backup;
#endif
        }
        goto error;
      }
    }
  }
  child_table->next_number_field = NULL;
  child_table->auto_increment_field_not_null = FALSE;

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (roop_count == first_insert)
      continue;
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      child_table = part_tables[roop_count].table;
#ifndef WITHOUT_VP_BG_ACCESS
      if (
        bgi_mode &&
        (child_table->file->ha_table_flags() & VP_CAN_BG_INSERT)
      ) {
        base = &bg_base[roop_count];
        if ((error_num = create_bg_thread(base)))
        {
          dup_table_idx = roop_count;
          if (!child_binlog)
          {
#if MYSQL_VERSION_ID < 50500
            thd->options = option_backup;
#else
            thd->variables.option_bits = option_backup;
#endif
          }
          goto error;
        }
        base->bg_command = VP_BG_COMMAND_INSERT;
        if (!suppress_autoinc)
        {
          child_table->next_number_field =
            child_table->found_next_number_field;
          if (table->next_number_field)
            child_table->auto_increment_field_not_null =
              table->auto_increment_field_not_null;
          else if (child_table->next_number_field)
            child_table->auto_increment_field_not_null =
              !child_table->next_number_field->is_null();
        }
        bg_kick(base);
      } else {
#endif
        if (!suppress_autoinc)
        {
          child_table->next_number_field =
            child_table->found_next_number_field;
          if (table->next_number_field)
            child_table->auto_increment_field_not_null =
            table->auto_increment_field_not_null;
          else if (child_table->next_number_field)
            child_table->auto_increment_field_not_null =
              !child_table->next_number_field->is_null();
        }
        DBUG_PRINT("info",("vp child_table[%d]->record[0]=%p",
          roop_count, child_table->record[0]));
#ifndef DBUG_OFF
        my_bitmap_map *tmp_map =
          dbug_tmp_use_all_columns(table, child_table->read_set);
#endif
        VP_DBUG_PRINT_FIELD_VALUES(child_table, 0);
        error_num =
          child_table->file->ha_write_row(child_table->record[0]);
#ifndef DBUG_OFF
        dbug_tmp_restore_column_map(child_table->read_set, tmp_map);
#endif
        if (error_num)
        {
          child_table->next_number_field = NULL;
          child_table->auto_increment_field_not_null = FALSE;
          dup_table_idx = roop_count;
          if (!child_binlog)
          {
#if MYSQL_VERSION_ID < 50500
            thd->options = option_backup;
#else
            thd->variables.option_bits = option_backup;
#endif
          }
          goto error;
        }
        child_table->next_number_field = NULL;
        child_table->auto_increment_field_not_null = FALSE;
#ifndef WITHOUT_VP_BG_ACCESS
      }
#endif
    }
  }

#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    child_table = part_tables[roop_count].table;
    if (
      bgi_mode &&
      (child_table->file->ha_table_flags() & VP_CAN_BG_INSERT)
    ) {
      base = &bg_base[roop_count];
      bg_wait(base);
      if (base->bg_error)
      {
        dup_table_idx = roop_count;
        if (!child_binlog)
        {
#if MYSQL_VERSION_ID < 50500
          thd->options = option_backup;
#else
          thd->variables.option_bits = option_backup;
#endif
        }
        error_num = base->bg_error;
        goto error;
      }
    }
  }
#endif

  if (!child_binlog)
  {
#if MYSQL_VERSION_ID < 50500
    thd->options = option_backup;
#else
    thd->variables.option_bits = option_backup;
#endif
  }
  DBUG_RETURN(error_num);

error:
#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    base = &bg_base[roop_count];
    if (base->bg_init)
      bg_wait(base);
  }
#endif
  DBUG_RETURN(error_num);
}

#ifdef HA_CAN_BULK_ACCESS
int ha_vp::pre_write_row(
  uchar *buf
) {
  int error_num = 0, roop_count, first_insert = -1;
  THD *thd = table->in_use;
  TABLE *child_table;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  int child_binlog = vp_param_child_binlog(thd, share->child_binlog);
  ulonglong option_backup = 0;
  DBUG_ENTER("ha_vp::pre_write_row");
  DBUG_PRINT("info",("vp this=%p", this));
  dup_table_idx = share->table_count;
#ifndef VP_WITHOUT_HA_STATISTIC_INCREMENT
  ha_statistic_increment(&SSV::ha_write_count);
#endif
#ifdef VP_TABLE_HAS_TIMESTAMP_FIELD_TYPE
  if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_INSERT)
    table->timestamp_field->set_time();
#endif

  if (!bulk_insert && !bulk_access_info_current->init_ins_bitmap)
  {
    int child_table_idx_bak = child_table_idx;
    child_table_idx = share->table_count;
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        clear_child_bitmap(roop_count);
        set_child_bitmap(
          (uchar *) table->write_set->bitmap,
          roop_count, TRUE);
        set_child_bitmap(
          (uchar *) table->read_set->bitmap,
          roop_count, FALSE);
        child_table = part_tables[roop_count].table;
        memcpy(bulk_access_info_current->ins_child_bitmaps[0][roop_count],
          child_table->read_set->bitmap,
          child_table->s->column_bitmap_size);
        memcpy(bulk_access_info_current->ins_child_bitmaps[1][roop_count],
          child_table->write_set->bitmap,
          child_table->s->column_bitmap_size);
      }
    }
    child_table_idx = child_table_idx_bak;
    bulk_access_info_current->init_ins_bitmap = TRUE;
    cb_state = CB_INSERT;
  } else if (cb_state != CB_INSERT)
  {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        child_table = part_tables[roop_count].table;
        memcpy(child_table->read_set->bitmap,
          bulk_access_info_current->ins_child_bitmaps[0][roop_count],
          child_table->s->column_bitmap_size);
        memcpy(child_table->write_set->bitmap,
          bulk_access_info_current->ins_child_bitmaps[1][roop_count],
          child_table->s->column_bitmap_size);
      }
    }
    cb_state = CB_INSERT;
  }

  memset(use_tables2, ~((uchar) 0), sizeof(uchar) * share->use_tables_size);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(share->same_columns, roop_count))
    {
      DBUG_PRINT("info",("vp child_table %d has same columns", roop_count));
      continue;
    }
    restore_record(part_tables[roop_count].table, s->default_values);
  }
  set_child_pt_bitmap();
  set_child_record_for_update(ptr_diff, 0, TRUE, FALSE);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
    set_child_record_for_insert(ptr_diff, roop_count);
  if (!child_binlog)
  {
#if MYSQL_VERSION_ID < 50500
    option_backup = thd->options;
    thd->options &= ~OPTION_BIN_LOG;
#else
    option_backup = thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_BIN_LOG;
#endif
  }

  if (
    table->next_number_field &&
    !(vp_bit_is_set(update_ignore, share->auto_increment_table))
  ) {
    first_insert = share->auto_increment_table;
  } else {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        first_insert = roop_count;
        break;
      }
    }
  }

  child_table = part_tables[first_insert].table;
  if (!suppress_autoinc)
  {
    child_table->next_number_field = child_table->found_next_number_field;
    if (table->next_number_field)
      child_table->auto_increment_field_not_null =
        table->auto_increment_field_not_null;
    else if (child_table->next_number_field)
      child_table->auto_increment_field_not_null =
        !child_table->next_number_field->is_null();
  }
  if ((error_num =
    child_table->file->ha_pre_write_row(child_table->record[0])))
  {
    child_table->next_number_field = NULL;
    child_table->auto_increment_field_not_null = FALSE;
    dup_table_idx = first_insert;
    if (!child_binlog)
    {
#if MYSQL_VERSION_ID < 50500
      thd->options = option_backup;
#else
      thd->variables.option_bits = option_backup;
#endif
    }
    goto error;
  }
  need_bulk_access_finish = FALSE;
  vp_set_bit(bulk_access_exec_bitmap, roop_count);
  child_table->next_number_field = NULL;
  child_table->auto_increment_field_not_null = FALSE;
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (roop_count == first_insert)
      continue;
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      child_table = part_tables[roop_count].table;
      if (!suppress_autoinc)
      {
        child_table->next_number_field =
          child_table->found_next_number_field;
        if (table->next_number_field)
          child_table->auto_increment_field_not_null =
          table->auto_increment_field_not_null;
        else if (child_table->next_number_field)
          child_table->auto_increment_field_not_null =
            !child_table->next_number_field->is_null();
      }
      if ((error_num =
        child_table->file->ha_pre_write_row(child_table->record[0])))
      {
        child_table->next_number_field = NULL;
        child_table->auto_increment_field_not_null = FALSE;
        dup_table_idx = roop_count;
        if (!child_binlog)
        {
#if MYSQL_VERSION_ID < 50500
          thd->options = option_backup;
#else
          thd->variables.option_bits = option_backup;
#endif
        }
        goto error;
      }
      child_table->next_number_field = NULL;
      child_table->auto_increment_field_not_null = FALSE;
    }
  }

  if (!child_binlog)
  {
#if MYSQL_VERSION_ID < 50500
    thd->options = option_backup;
#else
    thd->variables.option_bits = option_backup;
#endif
  }
  DBUG_RETURN(error_num);

error:
  DBUG_RETURN(error_num);
}
#endif

bool ha_vp::start_bulk_update()
{
  int roop_count;
  DBUG_ENTER("ha_vp::start_bulk_update");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (part_tables[roop_count].table->file->start_bulk_update())
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

int ha_vp::exec_bulk_update(
  ha_rows *dup_key_found
) {
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::exec_bulk_update");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num = part_tables[roop_count].table->file->exec_bulk_update(
      dup_key_found)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

#ifdef VP_END_BULK_UPDATE_RETURNS_INT
int ha_vp::end_bulk_update()
{
  int roop_count, error_num;
  DBUG_ENTER("ha_vp::end_bulk_update");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num = part_tables[roop_count].table->file->end_bulk_update()))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#else
void ha_vp::end_bulk_update()
{
  int roop_count;
  DBUG_ENTER("ha_vp::end_bulk_update");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
    part_tables[roop_count].table->file->end_bulk_update();
  DBUG_VOID_RETURN;
}
#endif


#ifdef VP_UPDATE_ROW_HAS_CONST_NEW_DATA
int ha_vp::bulk_update_row(
  const uchar *old_data,
  const uchar *new_data,
  ha_rows *dup_key_found
)
#else
int ha_vp::bulk_update_row(
  const uchar *old_data,
  uchar *new_data,
  ha_rows *dup_key_found
)
#endif
{
  int error_num, error_num2 = 0, roop_count;
  THD *thd = table->in_use;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(new_data, table->record[0]);
  my_ptrdiff_t ptr_diff2 = PTR_BYTE_DIFF(old_data, table->record[0]);
  int child_binlog = vp_param_child_binlog(thd, share->child_binlog);
  ulonglong option_backup = 0;
  VP_KEY_COPY vp_key_copy;
  TABLE *table2, *child_table;
  int bgu_mode;
  int bgi_mode;
  uchar *insert_table;
  bool rnd_state = (rnd_scan || cb_state == CB_SEL_RND);
  DBUG_ENTER("ha_vp::bulk_update_row");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp old_data=%p", old_data));
  DBUG_PRINT("info",("vp new_data=%p", new_data));
  dup_table_idx = share->table_count;
  if (!(insert_table =
    (uchar *) my_alloca(sizeof(uchar) * share->use_tables_size)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  memset(insert_table, 0, sizeof(uchar) * share->use_tables_size);
#ifndef WITHOUT_VP_BG_ACCESS
  bgu_mode = vp_param_bgu_mode(thd, share->bgu_mode);
  bgi_mode = vp_param_bgi_mode(thd, share->bgi_mode);
  VP_BG_BASE *base;
#endif
#ifndef VP_WITHOUT_HA_STATISTIC_INCREMENT
  ha_statistic_increment(&SSV::ha_update_count);
#endif
#ifdef VP_TABLE_HAS_TIMESTAMP_FIELD_TYPE
  if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
    table->timestamp_field->set_time();
#endif
  if (!init_upd_bitmap)
  {
    memset(use_tables2, 0, sizeof(uchar) * share->use_tables_size);
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        clear_child_bitmap(roop_count);
        if (set_child_bitmap(
          (uchar *) table->write_set->bitmap,
          roop_count, TRUE))
          vp_set_bit(use_tables2, roop_count);
        set_child_bitmap(
          (uchar *) table->read_set->bitmap,
          roop_count, FALSE);
        child_table = part_tables[roop_count].table;
        memcpy(upd_child_bitmaps[0][roop_count],
          child_table->read_set->bitmap,
          child_table->s->column_bitmap_size);
        memcpy(upd_child_bitmaps[1][roop_count],
          child_table->write_set->bitmap,
          child_table->s->column_bitmap_size);
      }
    }
    init_upd_bitmap = TRUE;
    cb_state = CB_UPDATE;
  } else if (cb_state != CB_UPDATE)
  {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        child_table = part_tables[roop_count].table;
        memcpy(child_table->read_set->bitmap,
          upd_child_bitmaps[0][roop_count],
          child_table->s->column_bitmap_size);
        memcpy(child_table->write_set->bitmap,
          upd_child_bitmaps[1][roop_count],
          child_table->s->column_bitmap_size);
      }
    }
    cb_state = CB_UPDATE;
  }

  if (!child_binlog)
  {
#if MYSQL_VERSION_ID < 50500
    option_backup = thd->options;
    thd->options &= ~OPTION_BIN_LOG;
#else
    option_backup = thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_BIN_LOG;
#endif
  }
  vp_key_copy.init = FALSE;
  vp_key_copy.mem_root_init = FALSE;
  vp_key_copy.ptr = NULL;

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    DBUG_PRINT("info",("vp table_count=%d", roop_count));
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      table2 = part_tables[roop_count].table;
/*
      DBUG_PRINT("info",("vp rnd_scan=%s", rnd_scan ? "TRUE" : "FALSE"));
      DBUG_PRINT("info",("vp use_tables[%d]=%s",
        roop_count,
        vp_bit_is_set(use_tables, roop_count) ? "TRUE" : "FALSE"));
      DBUG_ASSERT(init_sel_key_bitmap || rnd_scan);
      DBUG_PRINT("info",("vp sel_key_use_tables[%d]=%s",
        roop_count,
        vp_bit_is_set(sel_key_use_tables, roop_count) ? "TRUE" : "FALSE"));
      if (
        (rnd_scan && !vp_bit_is_set(use_tables, roop_count)) ||
        (!rnd_scan && !vp_bit_is_set(sel_key_use_tables, roop_count))
      )
*/
      DBUG_PRINT("info",("vp rnd_state=%s",
        rnd_state ? "TRUE" : "FALSE"));
      DBUG_ASSERT(init_sel_key_bitmap || rnd_state);
      if (
        (rnd_state && !vp_bit_is_set(use_tables, roop_count)) ||
        (!rnd_state && !vp_bit_is_set(sel_key_use_tables, roop_count))
      ) {
        DBUG_PRINT("info",("vp call search_by_pk"));
        if ((error_num = search_by_pk_for_update(roop_count, 1, &vp_key_copy,
          ptr_diff2, bgu_mode)))
        {
          if (
            !share->zero_record_update_mode ||
            !vp_bit_is_set(select_ignore_with_lock, roop_count) ||
            (error_num != HA_ERR_KEY_NOT_FOUND &&
              error_num != HA_ERR_END_OF_FILE)
          ) {
            dup_table_idx = roop_count;
            if (!child_binlog)
            {
#if MYSQL_VERSION_ID < 50500
              thd->options = option_backup;
#else
              thd->variables.option_bits = option_backup;
#endif
            }
            goto error;
          }
          vp_set_bit(insert_table, roop_count);
          error_num = 0;
        }
      } else {
        if (vp_bit_is_set(share->same_columns, roop_count))
        {
          DBUG_PRINT("info",("vp child_table %d has same columns",
            roop_count));
        } else
          store_record(table2, record[1]);
      }
    }
  }
#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      (
/*
        (rnd_scan && !vp_bit_is_set(use_tables, roop_count)) ||
        (!rnd_scan && !vp_bit_is_set(sel_key_use_tables, roop_count))
*/
        (rnd_state && !vp_bit_is_set(use_tables, roop_count)) ||
        (!rnd_state && !vp_bit_is_set(sel_key_use_tables, roop_count))
      ) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      table2 = part_tables[roop_count].table;
      if (
        (
          bgu_mode &&
          (table2->file->ha_table_flags() & VP_CAN_BG_UPDATE)
        ) ||
        (
          bgi_mode &&
          (table2->file->ha_table_flags() & VP_CAN_BG_INSERT)
        )
      ) {
        base = &bg_base[roop_count];
        bg_wait(base);
        if (base->bg_error)
        {
          dup_table_idx = roop_count;
          if (!child_binlog)
          {
#if MYSQL_VERSION_ID < 50500
            thd->options = option_backup;
#else
            thd->variables.option_bits = option_backup;
#endif
          }
          error_num = base->bg_error;
          goto error;
        }
      }
    }
  }
#endif

  error_num2 = HA_ERR_RECORD_IS_THE_SAME;
  set_child_pt_bitmap();
  set_child_record_for_update(ptr_diff, 0, TRUE, FALSE);
  set_child_record_for_update(ptr_diff2, 1, FALSE, FALSE);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    DBUG_PRINT("info",("vp table_count=%d", roop_count));
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      DBUG_PRINT("info",("vp call ha_update_row"));
      if (vp_bit_is_set(insert_table, roop_count))
      {
        if (!dup_key_found)
        {
          set_child_record_for_insert(ptr_diff, roop_count);
#ifndef WITHOUT_VP_BG_ACCESS
          table2 = part_tables[roop_count].table;
          if (
            bgi_mode &&
            (table2->file->ha_table_flags() & VP_CAN_BG_INSERT)
          ) {
            base = &bg_base[roop_count];
            if ((error_num = create_bg_thread(base)))
            {
              dup_table_idx = roop_count;
              if (!child_binlog)
              {
#if MYSQL_VERSION_ID < 50500
                thd->options = option_backup;
#else
                thd->variables.option_bits = option_backup;
#endif
              }
              DBUG_PRINT("info",("vp error_num=%d", error_num));
              goto error;
            }
            base->bg_command = VP_BG_COMMAND_INSERT;
            bg_kick(base);
            error_num2 = 0;
          } else {
#endif
            if (
              (error_num =
                part_tables[roop_count].table->file->ha_write_row(
                  part_tables[roop_count].table->record[0]))
            ) {
              dup_table_idx = roop_count;
              if (!child_binlog)
              {
#if MYSQL_VERSION_ID < 50500
                thd->options = option_backup;
#else
                thd->variables.option_bits = option_backup;
#endif
              }
              DBUG_PRINT("info",("vp error_num=%d", error_num));
              goto error;
            }
            error_num2 = 0;
#ifndef WITHOUT_VP_BG_ACCESS
          }
#endif
        }
      } else {
#ifndef WITHOUT_VP_BG_ACCESS
        table2 = part_tables[roop_count].table;
        if (
          bgu_mode &&
          (table2->file->ha_table_flags() & VP_CAN_BG_UPDATE)
        ) {
          base = &bg_base[roop_count];
          if ((error_num = create_bg_thread(base)))
          {
            dup_table_idx = roop_count;
            if (!child_binlog)
            {
#if MYSQL_VERSION_ID < 50500
              thd->options = option_backup;
#else
              thd->variables.option_bits = option_backup;
#endif
            }
            DBUG_PRINT("info",("vp error_num=%d", error_num));
            goto error;
          }
          base->bg_command = VP_BG_COMMAND_UPDATE;
          bg_kick(base);
          error_num2 = 0;
        } else {
#endif
          if (
            (
              (
                !dup_key_found &&
                (error_num =
                  part_tables[roop_count].table->file->ha_update_row(
                    part_tables[roop_count].table->record[1],
                    part_tables[roop_count].table->record[0]))
              ) ||
              (
                dup_key_found &&
                (error_num =
                  part_tables[roop_count].table->file->ha_bulk_update_row(
                    part_tables[roop_count].table->record[1],
                    part_tables[roop_count].table->record[0],
                    dup_key_found))
              )
            ) &&
            error_num != HA_ERR_RECORD_IS_THE_SAME
          ) {
            dup_table_idx = roop_count;
            if (!child_binlog)
            {
#if MYSQL_VERSION_ID < 50500
              thd->options = option_backup;
#else
              thd->variables.option_bits = option_backup;
#endif
            }
            DBUG_PRINT("info",("vp error_num=%d", error_num));
            goto error;
          }
          if (
            error_num2 == HA_ERR_RECORD_IS_THE_SAME &&
            error_num != HA_ERR_RECORD_IS_THE_SAME
          )
            error_num2 = 0;
#ifndef WITHOUT_VP_BG_ACCESS
        }
#endif
      }
    }
  }
  my_afree(insert_table);
#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      table2 = part_tables[roop_count].table;
      if (
        (
          bgu_mode &&
          (table2->file->ha_table_flags() & VP_CAN_BG_UPDATE)
        ) ||
        (
          bgi_mode &&
          (table2->file->ha_table_flags() & VP_CAN_BG_INSERT)
        )
      ) {
        base = &bg_base[roop_count];
        bg_wait(base);
        if (
          base->bg_error &&
          base->bg_error != HA_ERR_RECORD_IS_THE_SAME
        ) {
          dup_table_idx = roop_count;
          if (!child_binlog)
          {
#if MYSQL_VERSION_ID < 50500
            thd->options = option_backup;
#else
            thd->variables.option_bits = option_backup;
#endif
          }
          error_num = base->bg_error;
          goto error;
        }
        if (
          error_num2 == HA_ERR_RECORD_IS_THE_SAME &&
          base->bg_error != HA_ERR_RECORD_IS_THE_SAME
        )
          error_num2 = 0;
      }
    }
  }
#endif

  if (!child_binlog)
  {
#if MYSQL_VERSION_ID < 50500
    thd->options = option_backup;
#else
    thd->variables.option_bits = option_backup;
#endif
  }
  if (vp_key_copy.mem_root_init)
    free_root(&vp_key_copy.mem_root, MYF(0));
  if (vp_key_copy.ptr)
    vp_my_free(vp_key_copy.ptr, MYF(0));
  DBUG_RETURN(error_num2);

error:
#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      base = &bg_base[roop_count];
      if (base->bg_init)
        bg_wait(base);
    }
  }
#endif
  if (vp_key_copy.mem_root_init)
    free_root(&vp_key_copy.mem_root, MYF(0));
  if (vp_key_copy.ptr)
    vp_my_free(vp_key_copy.ptr, MYF(0));
#ifdef _MSC_VER
  vp_my_free(insert_table, MYF(MY_WME));
#endif
  DBUG_RETURN(error_num);
}

#ifdef VP_UPDATE_ROW_HAS_CONST_NEW_DATA
int ha_vp::update_row(
  const uchar *old_data,
  const uchar *new_data
)
#else
int ha_vp::update_row(
  const uchar *old_data,
  uchar *new_data
)
#endif
{
  DBUG_ENTER("ha_vp::update_row");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(bulk_update_row(old_data, new_data, NULL));
}

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
#ifdef VP_MDEV_16246
int ha_vp::direct_update_rows_init(
  List<Item> *update_fields,
  uint mode,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  uchar *new_data
)
#else
int ha_vp::direct_update_rows_init(
  uint mode,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  uchar *new_data
)
#endif
{
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int error_num, roop_count, child_table_idx_bak = 0;
  KEY_MULTI_RANGE *child_ranges = NULL;
#if defined(HAVE_HANDLERSOCKET)
  VP_CORRESPOND_KEY *correspond_key = NULL;
#endif
#endif
  DBUG_ENTER("ha_vp::direct_update_rows_init");
#ifndef HANDLER_HAS_TOP_TABLE_FIELDS
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
#else
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *explain_update = get_explain_upd_del();
  if (explain_update)
  {
    DBUG_PRINT("info",("vp join_type=%d", explain_update->jtype));
    if (
      explain_update->jtype == JT_CONST ||
      (explain_update->jtype == JT_RANGE && explain_update->rows == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_update"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
#else
/*
  JOIN *join = get_join();
  if (join && vp_join_table_count(join) == 1 && join->join_tab)
  {
    DBUG_PRINT("info",("vp join_type=%d", join->join_tab->type));
    if (
      join->join_tab->type == JT_CONST ||
      (join->join_tab->type == JT_RANGE && join->best_positions && join->best_positions[0].records_read == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_update"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
*/
#endif
/*
  if (
    !set_top_table_fields &&
    (error_num = set_top_table_and_fields(table, table->field,
      table_share->fields, TRUE))
  )
    DBUG_RETURN(error_num);

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num = part_tables[roop_count].table->file->
      set_top_table_and_fields(
      top_table,
      top_table_field_for_childs[roop_count],
      top_table_fields)))
      DBUG_RETURN(error_num);
  }
*/
  if (inited != NONE)
    child_table_idx_bak = child_table_idx;
  child_table_idx = share->table_count;
  memset(use_tables2, 0, sizeof(uchar) * share->use_tables_size);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      clear_child_bitmap(roop_count);
      if (set_child_bitmap(
        (uchar *) table->write_set->bitmap,
        roop_count, TRUE))
        vp_set_bit(use_tables2, roop_count);
      set_child_bitmap(
        (uchar *) table->read_set->bitmap,
        roop_count, FALSE);
    }
  }
  if (inited != NONE)
    child_table_idx = child_table_idx_bak;
  else {
    if (share->info_src_table)
      child_table_idx = share->info_src_table - 1;
    else
      child_table_idx = 0;
  }

#if defined(HAVE_HANDLERSOCKET)
  if (ranges)
  {
    /* handlersocket */
    correspond_key = share->keys[active_index].correspond_key;

    set_child_pt_bitmap();
    set_child_record_for_update(PTR_BYTE_DIFF(new_data, table->record[0]), 0,
      TRUE, FALSE);
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    DBUG_PRINT("info",("vp table_count=%d", roop_count));
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        while (correspond_key)
        {
          if (correspond_key->table_idx >= roop_count)
            break;
          correspond_key = correspond_key->next;
        }
        if (!correspond_key || correspond_key->table_idx > roop_count)
        {
          DBUG_PRINT("info",("vp correspond key is not found"));
          DBUG_RETURN(HA_ERR_WRONG_COMMAND);
        }
        child_multi_range[roop_count] = *ranges;
        child_multi_range[roop_count].start_key.key =  create_child_key(
          ranges->start_key.key, &child_key_buff[MAX_KEY_LENGTH * roop_count],
          ranges->start_key.keypart_map, ranges->start_key.length,
          &child_multi_range[roop_count].start_key.length
        );
        child_ranges = &child_multi_range[roop_count];
      }
#endif
#ifdef VP_MDEV_16246
      if ((error_num = part_tables[roop_count].table->file->
        ha_direct_update_rows_init(update_fields, mode, child_ranges,
        range_count, sorted,
        part_tables[roop_count].table->record[0])))
#else
      if ((error_num = part_tables[roop_count].table->file->
        ha_direct_update_rows_init(mode, child_ranges, range_count, sorted,
        part_tables[roop_count].table->record[0])))
#endif
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
#endif
}
#else
#ifdef VP_MDEV_16246
int ha_vp::direct_update_rows_init(
  List<Item> *update_fields
)
#else
int ha_vp::direct_update_rows_init()
#endif
{
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int error_num, roop_count, child_table_idx_bak = 0;
#endif
  DBUG_ENTER("ha_vp::direct_update_rows_init");
#ifndef HANDLER_HAS_TOP_TABLE_FIELDS
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
#else
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *explain_update = get_explain_upd_del();
  if (explain_update)
  {
    DBUG_PRINT("info",("vp join_type=%d", explain_update->jtype));
    if (
      explain_update->jtype == JT_CONST ||
      (explain_update->jtype == JT_RANGE && explain_update->rows == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_update"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
#else
/*
  JOIN *join = get_join();
  if (join && vp_join_table_count(join) == 1 && join->join_tab)
  {
    DBUG_PRINT("info",("vp join_type=%d", join->join_tab->type));
    if (
      join->join_tab->type == JT_CONST ||
      (join->join_tab->type == JT_RANGE && join->best_positions && join->best_positions[0].records_read == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_update"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
*/
#endif
  if (inited != NONE)
    child_table_idx_bak = child_table_idx;
  child_table_idx = share->table_count;
  memset(use_tables2, 0, sizeof(uchar) * share->use_tables_size);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      clear_child_bitmap(roop_count);
      if (set_child_bitmap(
        (uchar *) table->write_set->bitmap,
        roop_count, TRUE))
        vp_set_bit(use_tables2, roop_count);
      set_child_bitmap(
        (uchar *) table->read_set->bitmap,
        roop_count, FALSE);
    }
  }
  if (inited != NONE)
    child_table_idx = child_table_idx_bak;
  else {
    if (share->info_src_table)
      child_table_idx = share->info_src_table - 1;
    else
      child_table_idx = 0;
  }

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    DBUG_PRINT("info",("vp table_count=%d", roop_count));
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
#ifdef VP_MDEV_16246
      if ((error_num = part_tables[roop_count].table->file->
        direct_update_rows_init(update_fields)))
#else
      if ((error_num = part_tables[roop_count].table->file->
        direct_update_rows_init()))
#endif
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
#endif
}
#endif

#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int ha_vp::pre_direct_update_rows_init(
  uint mode,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  uchar *new_data
) {
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int error_num, roop_count, child_table_idx_bak = 0;
  KEY_MULTI_RANGE *child_ranges = NULL;
#if defined(HAVE_HANDLERSOCKET)
  VP_CORRESPOND_KEY *correspond_key = NULL;
#endif
#endif
  DBUG_ENTER("ha_vp::pre_direct_update_rows_init");
#ifndef HANDLER_HAS_TOP_TABLE_FIELDS
  need_bulk_access_finish = TRUE;
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
#else
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *explain_update = get_explain_upd_del();
  if (explain_update)
  {
    DBUG_PRINT("info",("vp join_type=%d", explain_update->jtype));
    if (
      explain_update->jtype == JT_CONST ||
      (explain_update->jtype == JT_RANGE && explain_update->rows == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_update"));
      need_bulk_access_finish = TRUE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
#else
/*
  JOIN *join = get_join();
  if (join && vp_join_table_count(join) == 1 && join->join_tab)
  {
    DBUG_PRINT("info",("vp join_type=%d", join->join_tab->type));
    if (
      join->join_tab->type == JT_CONST ||
      (join->join_tab->type == JT_RANGE && join->best_positions && join->best_positions[0].records_read == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_update"));
      need_bulk_access_finish = TRUE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
*/
#endif
  if (pre_inited != NONE)
    child_table_idx_bak = child_table_idx;
  child_table_idx = share->table_count;
  memset(use_tables2, 0, sizeof(uchar) * share->use_tables_size);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      clear_child_bitmap(roop_count);
      if (set_child_bitmap(
        (uchar *) table->write_set->bitmap,
        roop_count, TRUE))
        vp_set_bit(use_tables2, roop_count);
      set_child_bitmap(
        (uchar *) table->read_set->bitmap,
        roop_count, FALSE);
    }
  }
  if (pre_inited != NONE)
    child_table_idx = child_table_idx_bak;
  else {
    if (share->info_src_table)
      child_table_idx = share->info_src_table - 1;
    else
      child_table_idx = 0;
  }

#if defined(HAVE_HANDLERSOCKET)
  if (ranges)
  {
    /* handlersocket */
    correspond_key = share->keys[active_index].correspond_key;

    set_child_pt_bitmap();
    set_child_record_for_update(PTR_BYTE_DIFF(new_data, table->record[0]), 0,
      TRUE, FALSE);
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    DBUG_PRINT("info",("vp table_count=%d", roop_count));
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      handler *file;
      file = part_tables[roop_count].table->file;
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        while (correspond_key)
        {
          if (correspond_key->table_idx >= roop_count)
            break;
          correspond_key = correspond_key->next;
        }
        if (!correspond_key || correspond_key->table_idx > roop_count)
        {
          DBUG_PRINT("info",("vp correspond key is not found"));
          need_bulk_access_finish = TRUE;
          DBUG_RETURN(HA_ERR_WRONG_COMMAND);
        }
        child_multi_range[roop_count] = *ranges;
        child_multi_range[roop_count].start_key.key =  create_child_key(
          ranges->start_key.key, &child_key_buff[MAX_KEY_LENGTH * roop_count],
          ranges->start_key.keypart_map, ranges->start_key.length,
          &child_multi_range[roop_count].start_key.length
        );
        child_ranges = &child_multi_range[roop_count];
      }
#endif
      if ((error_num = file->
        pre_direct_update_rows_init(mode, child_ranges, range_count, sorted,
        part_tables[roop_count].table->record[0])))
      {
        if (error_num == HA_ERR_WRONG_COMMAND)
          need_bulk_access_finish = TRUE;
        DBUG_RETURN(error_num);
      }
    }
  }
  need_bulk_access_finish = FALSE;
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
  DBUG_RETURN(0);
#endif
}
#else
int ha_vp::pre_direct_update_rows_init()
{
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int error_num, roop_count, child_table_idx_bak = 0;
#endif
  DBUG_ENTER("ha_vp::pre_direct_update_rows_init");
#ifndef HANDLER_HAS_TOP_TABLE_FIELDS
  need_bulk_access_finish = TRUE;
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
#else
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *explain_update = get_explain_upd_del();
  if (explain_update)
  {
    DBUG_PRINT("info",("vp join_type=%d", explain_update->jtype));
    if (
      explain_update->jtype == JT_CONST ||
      (explain_update->jtype == JT_RANGE && explain_update->rows == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_update"));
      need_bulk_access_finish = TRUE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
#else
/*
  JOIN *join = get_join();
  if (join && vp_join_table_count(join) == 1 && join->join_tab)
  {
    DBUG_PRINT("info",("vp join_type=%d", join->join_tab->type));
    if (
      join->join_tab->type == JT_CONST ||
      (join->join_tab->type == JT_RANGE && join->best_positions && join->best_positions[0].records_read == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_update"));
      need_bulk_access_finish = TRUE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
*/
#endif
  if (pre_inited != NONE)
    child_table_idx_bak = child_table_idx;
  child_table_idx = share->table_count;
  memset(use_tables2, 0, sizeof(uchar) * share->use_tables_size);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      clear_child_bitmap(roop_count);
      if (set_child_bitmap(
        (uchar *) table->write_set->bitmap,
        roop_count, TRUE))
        vp_set_bit(use_tables2, roop_count);
      set_child_bitmap(
        (uchar *) table->read_set->bitmap,
        roop_count, FALSE);
    }
  }
  if (pre_inited != NONE)
    child_table_idx = child_table_idx_bak;
  else {
    if (share->info_src_table)
      child_table_idx = share->info_src_table - 1;
    else
      child_table_idx = 0;
  }

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    DBUG_PRINT("info",("vp table_count=%d", roop_count));
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      handler *file;
      file = part_tables[roop_count].table->file;
      if ((error_num = file->
        pre_direct_update_rows_init()))
      {
        if (error_num == HA_ERR_WRONG_COMMAND)
          need_bulk_access_finish = TRUE;
        DBUG_RETURN(error_num);
      }
    }
  }
  need_bulk_access_finish = FALSE;
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
  DBUG_RETURN(0);
#endif
}
#endif
#endif

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int ha_vp::direct_update_rows(
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  uchar *new_data,
  ha_rows *update_rows
) {
  int error_num, error_num2 = 0, roop_count;
  KEY_MULTI_RANGE *child_ranges = NULL;
#if defined(HAVE_HANDLERSOCKET)
  VP_CORRESPOND_KEY *correspond_key = NULL;
#endif
  handler *file;
  bool do_init;
  DBUG_ENTER("ha_vp::direct_update_rows");
#if defined(HAVE_HANDLERSOCKET)
  if (ranges)
  {
    /* handlersocket */
    correspond_key = share->keys[active_index].correspond_key;
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      file = part_tables[roop_count].table->file;
      if (file->inited == NONE)
        do_init = TRUE;
      else
        do_init = FALSE;
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        while (correspond_key)
        {
          if (correspond_key->table_idx >= roop_count)
            break;
          correspond_key = correspond_key->next;
        }
        if (do_init)
        {
          DBUG_PRINT("info",("vp call child[%d] ha_index_init",
            roop_count));
          if (
            (error_num = file->ha_index_init(correspond_key->key_idx, FALSE))
          )
            DBUG_RETURN(error_num);
        }

        child_ranges = &child_multi_range[roop_count];
      } else {
#endif
        if (do_init && (error_num = file->ha_rnd_init(TRUE)))
          DBUG_RETURN(error_num);
#if defined(HAVE_HANDLERSOCKET)
      }
#endif
      error_num = file->
        ha_direct_update_rows(child_ranges, range_count, sorted,
        part_tables[roop_count].table->record[0], update_rows);
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        if (do_init)
          error_num2 = file->ha_index_end();
      } else {
#endif
        if (do_init)
          error_num2 = file->ha_rnd_end();
#if defined(HAVE_HANDLERSOCKET)
      }
#endif
      if (!error_num)
        error_num = error_num2;
      if (error_num)
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#else
int ha_vp::direct_update_rows(
  ha_rows *update_rows
) {
  int error_num, error_num2 = 0, roop_count;
  handler *file;
  bool do_init;
  DBUG_ENTER("ha_vp::direct_update_rows");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      file = part_tables[roop_count].table->file;
      if (file->inited == NONE)
        do_init = TRUE;
      else
        do_init = FALSE;
      if (do_init && (error_num = file->ha_rnd_init(TRUE)))
        DBUG_RETURN(error_num);
      error_num = file->
        ha_direct_update_rows(update_rows);
      if (do_init)
        error_num2 = file->ha_rnd_end();
      if (!error_num)
        error_num = error_num2;
      if (error_num)
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#endif

#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int ha_vp::pre_direct_update_rows(
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  uchar *new_data,
  uint *update_rows
) {
  int error_num, error_num2 = 0, roop_count;
  KEY_MULTI_RANGE *child_ranges = NULL;
#if defined(HAVE_HANDLERSOCKET)
  VP_CORRESPOND_KEY *correspond_key = NULL;
#endif
  handler *file;
  bool do_init;
  DBUG_ENTER("ha_vp::pre_direct_update_rows");
#if defined(HAVE_HANDLERSOCKET)
  if (ranges)
  {
    /* handlersocket */
    correspond_key = share->keys[active_index].correspond_key;
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      file = part_tables[roop_count].table->file;
      if (file->pre_inited == NONE)
        do_init = TRUE;
      else
        do_init = FALSE;
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        while (correspond_key)
        {
          if (correspond_key->table_idx >= roop_count)
            break;
          correspond_key = correspond_key->next;
        }
        if (
          do_init &&
          (error_num = file->ha_pre_index_init(correspond_key->key_idx, FALSE))
        )
          DBUG_RETURN(error_num);

        child_ranges = &child_multi_range[roop_count];
      } else {
#endif
        if (do_init && (error_num = file->ha_pre_rnd_init(TRUE)))
          DBUG_RETURN(error_num);
#if defined(HAVE_HANDLERSOCKET)
      }
#endif
      error_num = file->
        ha_pre_direct_update_rows(child_ranges, range_count, sorted,
        part_tables[roop_count].table->record[0], update_rows);
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        if (do_init)
          error_num2 = file->ha_pre_index_end();
      } else {
#endif
        if (do_init)
          error_num2 = file->ha_pre_rnd_end();
#if defined(HAVE_HANDLERSOCKET)
      }
#endif
      if (!error_num)
      {
        vp_set_bit(bulk_access_exec_bitmap, roop_count);
        error_num = error_num2;
      }
      if (error_num)
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#else
int ha_vp::pre_direct_update_rows()
{
  int error_num, error_num2 = 0, roop_count;
  handler *file;
  bool do_init;
  DBUG_ENTER("ha_vp::pre_direct_update_rows");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      vp_bit_is_set(use_tables2, roop_count) &&
      !(vp_bit_is_set(update_ignore, roop_count))
    ) {
      file = part_tables[roop_count].table->file;
      if (file->pre_inited == NONE)
        do_init = TRUE;
      else
        do_init = FALSE;
      if (do_init && (error_num = file->ha_pre_rnd_init(TRUE)))
        DBUG_RETURN(error_num);
      error_num = file->ha_pre_direct_update_rows();
      if (do_init)
        error_num2 = file->ha_pre_rnd_end();
      if (!error_num)
      {
        vp_set_bit(bulk_access_exec_bitmap, roop_count);
        error_num = error_num2;
      }
      if (error_num)
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#endif
#endif
#endif

bool ha_vp::start_bulk_delete()
{
  int roop_count;
  DBUG_ENTER("ha_vp::start_bulk_delete");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (part_tables[roop_count].table->file->start_bulk_delete())
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

int ha_vp::end_bulk_delete()
{
  int error_num = 0, tmp, roop_count;
  DBUG_ENTER("ha_vp::end_bulk_delete");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((tmp = part_tables[roop_count].table->file->end_bulk_delete()))
      error_num = tmp;
  }
  DBUG_RETURN(error_num);
}

int ha_vp::delete_row(
  const uchar *buf
) {
  int error_num, roop_count;
  THD *thd = table->in_use;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  int child_binlog = vp_param_child_binlog(thd, share->child_binlog);
  ulonglong option_backup = 0;
  VP_KEY_COPY vp_key_copy;
  TABLE *table2, *child_table;
  int bgu_mode;
  bool do_delete;
  bool rnd_state = (rnd_scan || cb_state == CB_SEL_RND);
  DBUG_ENTER("ha_vp::delete_row");
  DBUG_PRINT("info",("vp this=%p", this));
#ifndef WITHOUT_VP_BG_ACCESS
  bgu_mode = vp_param_bgu_mode(thd, share->bgu_mode);
  VP_BG_BASE *base;
#endif
#ifndef VP_WITHOUT_HA_STATISTIC_INCREMENT
  ha_statistic_increment(&SSV::ha_delete_count);
#endif
  if (!init_del_bitmap)
  {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        clear_child_bitmap(roop_count);
        set_child_bitmap(
          (uchar *) table->read_set->bitmap,
          roop_count, FALSE);
        child_table = part_tables[roop_count].table;
        memcpy(del_child_bitmaps[0][roop_count], child_table->read_set->bitmap,
          child_table->s->column_bitmap_size);
      }
    }
    init_del_bitmap = TRUE;
    cb_state = CB_DELETE;
  } else if (cb_state != CB_DELETE)
  {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (!(vp_bit_is_set(update_ignore, roop_count)))
      {
        child_table = part_tables[roop_count].table;
        memcpy(child_table->read_set->bitmap,
          del_child_bitmaps[0][roop_count],
          child_table->s->column_bitmap_size);
        memcpy(child_table->write_set->bitmap,
          del_child_bitmaps[1][roop_count],
          child_table->s->column_bitmap_size);
      }
    }
    cb_state = CB_DELETE;
  }

  memset(use_tables2, ~((uchar) 0), sizeof(uchar) * share->use_tables_size);
  set_child_pt_bitmap();
  set_child_record_for_update(ptr_diff, 0, FALSE, FALSE);
  if (!child_binlog)
  {
#if MYSQL_VERSION_ID < 50500
    option_backup = thd->options;
    thd->options &= ~OPTION_BIN_LOG;
#else
    option_backup = thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_BIN_LOG;
#endif
  }
  vp_key_copy.init = FALSE;
  vp_key_copy.mem_root_init = FALSE;
  vp_key_copy.ptr = NULL;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      do_delete = TRUE;
      if (
/*
        (rnd_scan && !vp_bit_is_set(use_tables, roop_count)) ||
        (!rnd_scan && !vp_bit_is_set(sel_key_use_tables, roop_count))
*/
        (rnd_state && !vp_bit_is_set(use_tables, roop_count)) ||
        (!rnd_state && !vp_bit_is_set(sel_key_use_tables, roop_count))
      ) {
        if ((error_num = search_by_pk_for_update(roop_count, 0, &vp_key_copy,
          ptr_diff, bgu_mode)))
        {
          if (
            !share->zero_record_update_mode ||
            !vp_bit_is_set(select_ignore_with_lock, roop_count) ||
            (error_num != HA_ERR_KEY_NOT_FOUND &&
              error_num != HA_ERR_END_OF_FILE)
          ) {
            if (!child_binlog)
            {
#if MYSQL_VERSION_ID < 50500
              thd->options = option_backup;
#else
              thd->variables.option_bits = option_backup;
#endif
            }
            goto error;
          }
          do_delete = FALSE;
        }
      }
      if (do_delete)
      {
        table2 = part_tables[roop_count].table;
#ifndef WITHOUT_VP_BG_ACCESS
        if (
          bgu_mode &&
          (table2->file->ha_table_flags() & VP_CAN_BG_UPDATE)
        ) {
          if (
/*
            (rnd_scan && !vp_bit_is_set(use_tables, roop_count)) ||
            (!rnd_scan && !vp_bit_is_set(sel_key_use_tables, roop_count))
*/
            (rnd_state && !vp_bit_is_set(use_tables, roop_count)) ||
            (!rnd_state && !vp_bit_is_set(sel_key_use_tables, roop_count))
          ) {
            base = &bg_base[roop_count];
            if ((error_num = create_bg_thread(base)))
            {
              if (!child_binlog)
              {
#if MYSQL_VERSION_ID < 50500
                thd->options = option_backup;
#else
                thd->variables.option_bits = option_backup;
#endif
              }
              goto error;
            }
            base->bg_command = VP_BG_COMMAND_DELETE;
            bg_kick(base);
          }
        } else {
#endif
          if ((error_num = table2->file->ha_delete_row(table2->record[0])))
          {
            if (!child_binlog)
            {
#if MYSQL_VERSION_ID < 50500
              thd->options = option_backup;
#else
              thd->variables.option_bits = option_backup;
#endif
            }
            goto error;
          }
#ifndef WITHOUT_VP_BG_ACCESS
        }
#endif
      }
    }
  }
#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      table2 = part_tables[roop_count].table;
      if (
        bgu_mode &&
        (table2->file->ha_table_flags() & VP_CAN_BG_UPDATE)
      ) {
        base = &bg_base[roop_count];
        bg_wait(base);
        if (base->bg_error)
        {
          if (!child_binlog)
          {
#if MYSQL_VERSION_ID < 50500
            thd->options = option_backup;
#else
            thd->variables.option_bits = option_backup;
#endif
          }
          error_num = base->bg_error;
          goto error;
        }
      }
    }
  }
#endif

  if (!child_binlog)
  {
#if MYSQL_VERSION_ID < 50500
    thd->options = option_backup;
#else
    thd->variables.option_bits = option_backup;
#endif
  }
  if (vp_key_copy.mem_root_init)
    free_root(&vp_key_copy.mem_root, MYF(0));
  if (vp_key_copy.ptr)
    vp_my_free(vp_key_copy.ptr, MYF(0));
  DBUG_RETURN(0);

error:
#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      base = &bg_base[roop_count];
      if (base->bg_init)
        bg_wait(base);
    }
  }
#endif
  if (vp_key_copy.mem_root_init)
    free_root(&vp_key_copy.mem_root, MYF(0));
  if (vp_key_copy.ptr)
    vp_my_free(vp_key_copy.ptr, MYF(0));
  DBUG_RETURN(error_num);
}

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int ha_vp::direct_delete_rows_init(
  uint mode,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted
) {
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int error_num, roop_count, child_table_idx_bak = 0;
  KEY_MULTI_RANGE *child_ranges = NULL;
#if defined(HAVE_HANDLERSOCKET)
  VP_CORRESPOND_KEY *correspond_key = NULL;
#endif
#endif
  DBUG_ENTER("ha_vp::direct_delete_rows_init");
#ifndef HANDLER_HAS_TOP_TABLE_FIELDS
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
#else
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *explain_update = get_explain_upd_del();
  if (explain_update)
  {
    DBUG_PRINT("info",("vp join_type=%d", explain_update->jtype));
    if (
      explain_update->jtype == JT_CONST ||
      (explain_update->jtype == JT_RANGE && explain_update->rows == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_delete"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
#else
/*
  JOIN *join = get_join();
  if (join && vp_join_table_count(join) == 1 && join->join_tab)
  {
    DBUG_PRINT("info",("vp join_type=%d", join->join_tab->type));
    if (
      join->join_tab->type == JT_CONST ||
      (join->join_tab->type == JT_RANGE && join->best_positions && join->best_positions[0].records_read == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_delete"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
*/
#endif
/*
  if (
    !set_top_table_fields &&
    (error_num = set_top_table_and_fields(table, table->field,
      table_share->fields, TRUE))
  )
    DBUG_RETURN(error_num);

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num = part_tables[roop_count].table->file->
      set_top_table_and_fields(
      top_table,
      top_table_field_for_childs[roop_count],
      top_table_fields)))
      DBUG_RETURN(error_num);
  }
*/
  if (inited != NONE)
    child_table_idx_bak = child_table_idx;
  child_table_idx = share->table_count;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      clear_child_bitmap(roop_count);
      set_child_bitmap(
        (uchar *) table->read_set->bitmap,
        roop_count, FALSE);
    }
  }
  if (inited != NONE)
    child_table_idx = child_table_idx_bak;
  else {
    if (share->info_src_table)
      child_table_idx = share->info_src_table - 1;
    else
      child_table_idx = 0;
  }

#if defined(HAVE_HANDLERSOCKET)
  if (ranges)
  {
    /* handlersocket */
    correspond_key = share->keys[active_index].correspond_key;
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        while (correspond_key)
        {
          if (correspond_key->table_idx >= roop_count)
            break;
          correspond_key = correspond_key->next;
        }
        if (!correspond_key || correspond_key->table_idx > roop_count)
        {
          DBUG_PRINT("info",("vp correspond key is not found"));
          DBUG_RETURN(HA_ERR_WRONG_COMMAND);
        }
        child_multi_range[roop_count] = *ranges;
        child_multi_range[roop_count].start_key.key =  create_child_key(
          ranges->start_key.key, &child_key_buff[MAX_KEY_LENGTH * roop_count],
          ranges->start_key.keypart_map, ranges->start_key.length,
          &child_multi_range[roop_count].start_key.length);
        child_ranges = &child_multi_range[roop_count];
      }
#endif
      if ((error_num = part_tables[roop_count].table->file->
        ha_direct_delete_rows_init(mode, child_ranges, range_count, sorted)))
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
#endif
}
#else
int ha_vp::direct_delete_rows_init()
{
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int error_num, roop_count, child_table_idx_bak = 0;
#endif
  DBUG_ENTER("ha_vp::direct_delete_rows_init");
#ifndef HANDLER_HAS_TOP_TABLE_FIELDS
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
#else
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *explain_update = get_explain_upd_del();
  if (explain_update)
  {
    DBUG_PRINT("info",("vp join_type=%d", explain_update->jtype));
    if (
      explain_update->jtype == JT_CONST ||
      (explain_update->jtype == JT_RANGE && explain_update->rows == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_delete"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
#else
/*
  JOIN *join = get_join();
  if (join && vp_join_table_count(join) == 1 && join->join_tab)
  {
    DBUG_PRINT("info",("vp join_type=%d", join->join_tab->type));
    if (
      join->join_tab->type == JT_CONST ||
      (join->join_tab->type == JT_RANGE && join->best_positions && join->best_positions[0].records_read == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_delete"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
*/
#endif
  if (inited != NONE)
    child_table_idx_bak = child_table_idx;
  child_table_idx = share->table_count;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      clear_child_bitmap(roop_count);
      set_child_bitmap(
        (uchar *) table->read_set->bitmap,
        roop_count, FALSE);
    }
  }
  if (inited != NONE)
    child_table_idx = child_table_idx_bak;
  else {
    if (share->info_src_table)
      child_table_idx = share->info_src_table - 1;
    else
      child_table_idx = 0;
  }

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      if ((error_num = part_tables[roop_count].table->file->
        direct_delete_rows_init()))
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
#endif
}
#endif

#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int ha_vp::pre_direct_delete_rows_init(
  uint mode,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted
) {
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int error_num, roop_count, child_table_idx_bak = 0;
  KEY_MULTI_RANGE *child_ranges = NULL;
#if defined(HAVE_HANDLERSOCKET)
  VP_CORRESPOND_KEY *correspond_key = NULL;
#endif
#endif
  DBUG_ENTER("ha_vp::pre_direct_delete_rows_init");
#ifndef HANDLER_HAS_TOP_TABLE_FIELDS
  need_bulk_access_finish = TRUE;
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
#else
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *explain_update = get_explain_upd_del();
  if (explain_update)
  {
    DBUG_PRINT("info",("vp join_type=%d", explain_update->jtype));
    if (
      explain_update->jtype == JT_CONST ||
      (explain_update->jtype == JT_RANGE && explain_update->rows == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_delete"));
      need_bulk_access_finish = TRUE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
#else
/*
  JOIN *join = get_join();
  if (join && vp_join_table_count(join) == 1 && join->join_tab)
  {
    DBUG_PRINT("info",("vp join_type=%d", join->join_tab->type));
    if (
      join->join_tab->type == JT_CONST ||
      (join->join_tab->type == JT_RANGE && join->best_positions && join->best_positions[0].records_read == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_delete"));
      need_bulk_access_finish = TRUE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
*/
#endif
  if (pre_inited != NONE)
    child_table_idx_bak = child_table_idx;
  child_table_idx = share->table_count;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      clear_child_bitmap(roop_count);
      set_child_bitmap(
        (uchar *) table->read_set->bitmap,
        roop_count, FALSE);
    }
  }
  if (pre_inited != NONE)
    child_table_idx = child_table_idx_bak;
  else {
    if (share->info_src_table)
      child_table_idx = share->info_src_table - 1;
    else
      child_table_idx = 0;
  }

#if defined(HAVE_HANDLERSOCKET)
  if (ranges)
  {
    /* handlersocket */
    correspond_key = share->keys[active_index].correspond_key;
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      handler *file;
      file = part_tables[roop_count].table->file;
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        while (correspond_key)
        {
          if (correspond_key->table_idx >= roop_count)
            break;
          correspond_key = correspond_key->next;
        }
        if (!correspond_key || correspond_key->table_idx > roop_count)
        {
          DBUG_PRINT("info",("vp correspond key is not found"));
          need_bulk_access_finish = TRUE;
          DBUG_RETURN(HA_ERR_WRONG_COMMAND);
        }
        child_multi_range[roop_count] = *ranges;
        child_multi_range[roop_count].start_key.key =  create_child_key(
          ranges->start_key.key, &child_key_buff[MAX_KEY_LENGTH * roop_count],
          ranges->start_key.keypart_map, ranges->start_key.length,
          &child_multi_range[roop_count].start_key.length);
        child_ranges = &child_multi_range[roop_count];
      }
#endif
      if ((error_num = file->
        ha_pre_direct_delete_rows_init(mode, child_ranges, range_count,
          sorted)))
      {
        if (error_num == HA_ERR_WRONG_COMMAND)
          need_bulk_access_finish = TRUE;
        DBUG_RETURN(error_num);
      }
    }
  }
  need_bulk_access_finish = FALSE;
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
  DBUG_RETURN(0);
#endif
}
#else
int ha_vp::pre_direct_delete_rows_init()
{
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int error_num, roop_count, child_table_idx_bak = 0;
#endif
  DBUG_ENTER("ha_vp::pre_direct_delete_rows_init");
#ifndef HANDLER_HAS_TOP_TABLE_FIELDS
  need_bulk_access_finish = TRUE;
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
#else
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *explain_update = get_explain_upd_del();
  if (explain_update)
  {
    DBUG_PRINT("info",("vp join_type=%d", explain_update->jtype));
    if (
      explain_update->jtype == JT_CONST ||
      (explain_update->jtype == JT_RANGE && explain_update->rows == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_delete"));
      need_bulk_access_finish = TRUE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
#else
/*
  JOIN *join = get_join();
  if (join && vp_join_table_count(join) == 1 && join->join_tab)
  {
    DBUG_PRINT("info",("vp join_type=%d", join->join_tab->type));
    if (
      join->join_tab->type == JT_CONST ||
      (join->join_tab->type == JT_RANGE && join->best_positions && join->best_positions[0].records_read == 1)
    ) {
      DBUG_PRINT("info",("vp const does not need direct_delete"));
      need_bulk_access_finish = TRUE;
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
*/
#endif
  if (pre_inited != NONE)
    child_table_idx_bak = child_table_idx;
  child_table_idx = share->table_count;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      clear_child_bitmap(roop_count);
      set_child_bitmap(
        (uchar *) table->read_set->bitmap,
        roop_count, FALSE);
    }
  }
  if (pre_inited != NONE)
    child_table_idx = child_table_idx_bak;
  else {
    if (share->info_src_table)
      child_table_idx = share->info_src_table - 1;
    else
      child_table_idx = 0;
  }

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      handler *file;
      file = part_tables[roop_count].table->file;
      if ((error_num = file->ha_pre_direct_delete_rows_init()))
      {
        if (error_num == HA_ERR_WRONG_COMMAND)
          need_bulk_access_finish = TRUE;
        DBUG_RETURN(error_num);
      }
    }
  }
  need_bulk_access_finish = FALSE;
  if (bulk_access_started)
    bulk_access_info_current->called = TRUE;
  DBUG_RETURN(0);
#endif
}
#endif
#endif

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int ha_vp::direct_delete_rows(
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  ha_rows *delete_rows
) {
  int error_num, error_num2 = 0, roop_count;
  KEY_MULTI_RANGE *child_ranges = NULL;
#if defined(HAVE_HANDLERSOCKET)
  VP_CORRESPOND_KEY *correspond_key = NULL;
#endif
  handler *file;
  bool do_init;
  DBUG_ENTER("ha_vp::direct_delete_rows");
#if defined(HAVE_HANDLERSOCKET)
  if (ranges)
  {
    /* handlersocket */
    correspond_key = share->keys[active_index].correspond_key;
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      file = part_tables[roop_count].table->file;
      if (file->inited == NONE)
        do_init = TRUE;
      else
        do_init = FALSE;
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        while (correspond_key)
        {
          if (correspond_key->table_idx >= roop_count)
            break;
          correspond_key = correspond_key->next;
        }
        if (do_init)
        {
          DBUG_PRINT("info",("vp call child[%d] ha_index_init",
            roop_count));
          if (
            (error_num = file->ha_index_init(correspond_key->key_idx, FALSE))
          )
            DBUG_RETURN(error_num);
        }

        child_ranges = &child_multi_range[roop_count];
      } else {
#endif
        if (do_init && (error_num = file->ha_rnd_init(TRUE)))
          DBUG_RETURN(error_num);
#if defined(HAVE_HANDLERSOCKET)
      }
#endif
      error_num = file->
        ha_direct_delete_rows(child_ranges, range_count, sorted, delete_rows);
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        if (do_init)
          error_num2 = file->ha_index_end();
      } else {
#endif
        if (do_init)
          error_num2 = file->ha_rnd_end();
#if defined(HAVE_HANDLERSOCKET)
      }
#endif
      if (!error_num)
        error_num = error_num2;
      if (error_num)
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#else
int ha_vp::direct_delete_rows(
  ha_rows *delete_rows
) {
  int error_num, error_num2 = 0, roop_count;
  handler *file;
  bool do_init;
  DBUG_ENTER("ha_vp::direct_delete_rows");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      file = part_tables[roop_count].table->file;
      if (file->inited == NONE)
        do_init = TRUE;
      else
        do_init = FALSE;
      if (do_init && (error_num = file->ha_rnd_init(TRUE)))
        DBUG_RETURN(error_num);
      error_num = file->
        ha_direct_delete_rows(delete_rows);
      if (do_init)
        error_num2 = file->ha_rnd_end();
      if (!error_num)
        error_num = error_num2;
      if (error_num)
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#endif

#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int ha_vp::pre_direct_delete_rows(
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  bool sorted,
  uint *delete_rows
) {
  int error_num, error_num2 = 0, roop_count;
  KEY_MULTI_RANGE *child_ranges = NULL;
#if defined(HAVE_HANDLERSOCKET)
  VP_CORRESPOND_KEY *correspond_key = NULL;
#endif
  handler *file;
  bool do_init;
  DBUG_ENTER("ha_vp::pre_direct_delete_rows");
#if defined(HAVE_HANDLERSOCKET)
  if (ranges)
  {
    /* handlersocket */
    correspond_key = share->keys[active_index].correspond_key;
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      file = part_tables[roop_count].table->file;
      if (file->pre_inited == NONE)
        do_init = TRUE;
      else
        do_init = FALSE;
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        while (correspond_key)
        {
          if (correspond_key->table_idx >= roop_count)
            break;
          correspond_key = correspond_key->next;
        }
        if (
          do_init &&
          (error_num = file->ha_pre_index_init(correspond_key->key_idx, FALSE))
        )
          DBUG_RETURN(error_num);

        child_ranges = &child_multi_range[roop_count];
      } else {
#endif
        if (do_init && (error_num = file->ha_pre_rnd_init(TRUE)))
          DBUG_RETURN(error_num);
#if defined(HAVE_HANDLERSOCKET)
      }
#endif
      error_num = file->
        ha_pre_direct_delete_rows(child_ranges, range_count, sorted,
          delete_rows);
#if defined(HAVE_HANDLERSOCKET)
      if (ranges)
      {
        /* handlersocket */
        if (do_init)
          error_num2 = file->ha_pre_index_end();
      } else {
#endif
        if (do_init)
          error_num2 = file->ha_pre_rnd_end();
#if defined(HAVE_HANDLERSOCKET)
      }
#endif
      if (!error_num)
      {
        vp_set_bit(bulk_access_exec_bitmap, roop_count);
        error_num = error_num2;
      }
      if (error_num)
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#else
int ha_vp::pre_direct_delete_rows()
{
  int error_num, error_num2 = 0, roop_count;
  handler *file;
  bool do_init;
  DBUG_ENTER("ha_vp::pre_direct_delete_rows");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      file = part_tables[roop_count].table->file;
      if (file->pre_inited == NONE)
        do_init = TRUE;
      else
        do_init = FALSE;
      if (do_init && (error_num = file->ha_pre_rnd_init(TRUE)))
        DBUG_RETURN(error_num);
      error_num = file->
        ha_pre_direct_delete_rows();
      if (do_init)
        error_num2 = file->ha_pre_rnd_end();
      if (!error_num)
      {
        vp_set_bit(bulk_access_exec_bitmap, roop_count);
        error_num = error_num2;
      }
      if (error_num)
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}
#endif
#endif
#endif

int ha_vp::delete_all_rows()
{
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::delete_all_rows");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      if ((error_num =
        part_tables[roop_count].table->file->ha_delete_all_rows()))
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_vp::truncate()
{
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::truncate");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!(vp_bit_is_set(update_ignore, roop_count)))
    {
      if ((error_num =
        part_tables[roop_count].table->file->ha_truncate()))
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

double ha_vp::scan_time()
{
  DBUG_ENTER("ha_vp::scan_time");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp part_tables[0].table=%p", part_tables[0].table));
  DBUG_RETURN(part_tables[child_table_idx].table->file->scan_time());
}

double ha_vp::read_time(
  uint index,
  uint ranges,
  ha_rows rows
) {
  VP_CORRESPOND_KEY *tgt_correspond_key;
  int tgt_table_idx, tgt_key_idx;
  DBUG_ENTER("ha_vp::read_time");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp part_tables[0].table=%p", part_tables[0].table));
  if (index < MAX_KEY)
  {
    tgt_correspond_key = share->keys[index].shortest_correspond_key;
    tgt_table_idx = tgt_correspond_key->table_idx;
    tgt_key_idx = tgt_correspond_key->key_idx;
    DBUG_RETURN(part_tables[tgt_table_idx].table->file->read_time(
      tgt_key_idx, ranges, rows));
  } else {
    DBUG_RETURN(part_tables[child_table_idx].table->file->read_time(
      index, ranges, rows));
  }
}

const key_map *ha_vp::keys_to_use_for_scanning()
{
  DBUG_ENTER("ha_vp::keys_to_use_for_scanning");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(&key_map_full);
}

ha_rows ha_vp::estimate_rows_upper_bound()
{
  DBUG_ENTER("ha_vp::estimate_rows_upper_bound");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(
    part_tables[child_table_idx].table->file->estimate_rows_upper_bound());
}

bool ha_vp::get_error_message(
  int error,
  String *buf
) {
  DBUG_ENTER("ha_vp::get_error_message");
  DBUG_PRINT("info",("vp this=%p", this));
  if (buf->reserve(ER_VP_UNKNOWN_LEN))
    DBUG_RETURN(TRUE);
  buf->q_append(ER_VP_UNKNOWN_STR, ER_VP_UNKNOWN_LEN);
  DBUG_RETURN(FALSE);
}

int ha_vp::create(
  const char *name,
  TABLE *form,
  HA_CREATE_INFO *info
) {
  int error_num;
  VP_SHARE tmp_share;
  DBUG_ENTER("ha_vp::create");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp name=%s", name));
  DBUG_PRINT("info",
    ("vp form->s->connect_string=%s", form->s->connect_string.str));
  DBUG_PRINT("info",
    ("vp info->connect_string=%s", info->connect_string.str));
  memset(&tmp_share, 0, sizeof(VP_SHARE));
  tmp_share.table_name = (char*) name;
  tmp_share.table_name_length = strlen(name);
  if ((error_num = vp_parse_table_info(&tmp_share, form, 1)))
    goto error;

  vp_free_share_alloc(&tmp_share);
  DBUG_RETURN(0);

error:
  vp_free_share_alloc(&tmp_share);
  DBUG_RETURN(error_num);
}

void ha_vp::update_create_info(
  HA_CREATE_INFO* create_info
) {
  DBUG_ENTER("ha_vp::update_create_info");
  DBUG_PRINT("info",("vp this=%p", this));
  if (!create_info->connect_string.str)
  {
    create_info->connect_string.str = table->s->connect_string.str;
    create_info->connect_string.length = table->s->connect_string.length;
  }
  DBUG_PRINT("info",
    ("vp create_info->connect_string=%s",
    create_info->connect_string.str));
  DBUG_VOID_RETURN;
}

int ha_vp::rename_table(
  const char *from,
  const char *to
) {
  DBUG_ENTER("ha_vp::rename_table");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp from=%s", from));
  DBUG_PRINT("info",("vp to=%s", to));
  DBUG_RETURN(0);
}

int ha_vp::delete_table(
  const char *name
) {
  DBUG_ENTER("ha_vp::delete_table");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp name=%s", name));
  DBUG_RETURN(0);
}

bool ha_vp::is_crashed() const
{
  int roop_count;
  DBUG_ENTER("ha_vp::is_crashed");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (part_tables[roop_count].table->file->is_crashed())
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

#ifdef VP_HANDLER_AUTO_REPAIR_HAS_ERROR
bool ha_vp::auto_repair(int error) const
#else
bool ha_vp::auto_repair() const
#endif
{
  DBUG_ENTER("ha_vp::auto_repair");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(FALSE);
}

int ha_vp::disable_indexes(
  uint mode
) {
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::disable_indexes");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num =
      part_tables[roop_count].table->file->ha_disable_indexes(mode)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int ha_vp::enable_indexes(
  uint mode
) {
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::enable_indexes");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num =
      part_tables[roop_count].table->file->ha_enable_indexes(mode)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}


int ha_vp::check(
  THD* thd,
  HA_CHECK_OPT* check_opt
) {
  int error_num = 0, roop_count;
  DBUG_ENTER("ha_vp::check");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      (error_num =
        part_tables[roop_count].table->file->ha_check(thd, check_opt)) &&
      error_num != HA_ADMIN_ALREADY_DONE
    )
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(error_num);
}

int ha_vp::repair(
  THD* thd,
  HA_CHECK_OPT* check_opt
) {
  int error_num = 0, roop_count;
  DBUG_ENTER("ha_vp::repair");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      (error_num =
        part_tables[roop_count].table->file->ha_repair(thd, check_opt)) &&
      error_num != HA_ADMIN_ALREADY_DONE
    )
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(error_num);
}

bool ha_vp::check_and_repair(
  THD *thd
) {
  int roop_count;
  DBUG_ENTER("ha_vp::check_and_repair");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (part_tables[roop_count].table->file->ha_check_and_repair(thd))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

int ha_vp::analyze(
  THD* thd,
  HA_CHECK_OPT* check_opt
) {
  int error_num = 0, roop_count;
  DBUG_ENTER("ha_vp::analyze");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      (error_num =
        part_tables[roop_count].table->file->ha_analyze(thd, check_opt)) &&
      error_num != HA_ADMIN_ALREADY_DONE
    )
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(error_num);
}

int ha_vp::optimize(
  THD* thd,
  HA_CHECK_OPT* check_opt
) {
  int error_num = 0, roop_count;
  DBUG_ENTER("ha_vp::optimize");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      (error_num =
        part_tables[roop_count].table->file->ha_optimize(thd, check_opt)) &&
      error_num != HA_ADMIN_ALREADY_DONE
    )
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(error_num);
}

#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
int ha_vp::set_top_table_and_fields(
  TABLE *top_table,
  Field **top_table_field,
  uint top_table_fields,
  bool self
) {
  int roop_count, roop_count2;
  uint field_index, field_index2;
  DBUG_ENTER("ha_vp::set_top_table_fields");
  DBUG_PRINT("info",("vp this=%p", this));
  if (!set_top_table_fields || self != top_table_self)
  {
    if (top_table_fields > allocated_top_table_fields)
    {
      if (allocated_top_table_fields)
        vp_my_free(top_table_field_for_childs[0], MYF(0));

      if (!(top_table_field_for_childs[0] =
        (Field **) my_malloc(sizeof(Field *) * (top_table_fields + 1) *
          share->table_count, MYF(MY_WME))))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);

      for (roop_count = 0; roop_count < share->table_count - 1; roop_count++)
        top_table_field_for_childs[roop_count + 1] =
          top_table_field_for_childs[roop_count] + top_table_fields + 1;

      allocated_top_table_fields = top_table_fields;
    }

    for (roop_count = 0; roop_count < (int) top_table_fields + 1; roop_count++)
    {
      if (top_table_field[roop_count])
      {
        field_index = top_table_field[roop_count]->field_index;
        for (roop_count2 = 0; roop_count2 < share->table_count; roop_count2++)
        {
          field_index2 = share->correspond_columns_p[table_share->fields *
            roop_count2 + field_index];
          if (field_index2 < MAX_FIELDS)
            (top_table_field_for_childs[roop_count2])[roop_count] =
              part_tables[roop_count2].table->field[field_index2];
          else
            (top_table_field_for_childs[roop_count2])[roop_count] = NULL;
        }
      } else {
        for (roop_count2 = 0; roop_count2 < share->table_count; roop_count2++)
          (top_table_field_for_childs[roop_count2])[roop_count] = NULL;
      }
    }

    set_top_table_fields = TRUE;
    this->top_table = top_table;
    this->top_table_field = top_table_field;
    this->top_table_fields = top_table_fields;
    top_table_self = self;
  }
  DBUG_RETURN(0);
}

int ha_vp::set_top_table_and_fields(
  TABLE *top_table,
  Field **top_table_field,
  uint top_table_fields
) {
  DBUG_ENTER("ha_vp::set_top_table_fields");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(set_top_table_and_fields(top_table, top_table_field,
    top_table_fields, FALSE));
}

#ifdef HANDLER_HAS_PRUNE_PARTITIONS_FOR_CHILD
bool ha_vp::prune_partitions_for_child(
  THD *thd,
  Item *pprune_cond
) {
  bool res = TRUE;
  int roop_count;
  DBUG_ENTER("ha_vp::prune_partitions_for_child");
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!prune_partitions(thd, part_tables[roop_count].table, pprune_cond))
      res = FALSE;
  }
  DBUG_RETURN(res);
}
#endif

#ifdef HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
TABLE_LIST *ha_vp::get_next_global_for_child()
{
  int roop_count;
  DBUG_ENTER("ha_vp::get_next_global_for_child");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_PRINT("info",("vp part_tables=%p", part_tables));
  handler_close = TRUE;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    part_tables[roop_count].parent_l = table->pos_in_table_list;
  }
  DBUG_RETURN(part_tables);
}
#endif

const COND *ha_vp::cond_push(const COND *cond)
{
  int roop_count;
  COND *res_cond = NULL;
  DBUG_ENTER("ha_vp::cond_push");
  DBUG_PRINT("info",("vp this=%p", this));

  if (cond)
  {
    if (
      !set_top_table_fields &&
      set_top_table_and_fields(table, table->field, table_share->fields, TRUE)
    )
      DBUG_RETURN(cond);

    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (part_tables[roop_count].table->file->set_top_table_and_fields(
        top_table,
        top_table_field_for_childs[roop_count],
        top_table_fields))
        DBUG_RETURN(cond);
    }

    VP_CONDITION *tmp_cond;
    if (!(tmp_cond = (VP_CONDITION *)
      my_malloc(sizeof(*tmp_cond), MYF(MY_WME | MY_ZEROFILL)))
    )
      DBUG_RETURN(cond);
    tmp_cond->cond = (COND *) cond;
    tmp_cond->next = condition;
    condition = tmp_cond;

    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (part_tables[roop_count].table->file->pushed_cond != cond)
      {
        if (part_tables[roop_count].table->file->cond_push(cond))
          res_cond = (COND *) cond;
        else
          part_tables[roop_count].table->file->pushed_cond = cond;
      }
    }
  }
  DBUG_RETURN(res_cond);
}

void ha_vp::cond_pop()
{
  int roop_count;
  DBUG_ENTER("ha_vp::cond_pop");
  if (condition)
  {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
      part_tables[roop_count].table->file->cond_pop();
    VP_CONDITION *tmp_cond = condition->next;
    vp_my_free(condition, MYF(0));
    condition = tmp_cond;
  }
  DBUG_VOID_RETURN;
}
#endif

int ha_vp::info_push(
  uint info_type,
  void *info
) {
  int error_num = 0, tmp, roop_count;
  DBUG_ENTER("ha_vp::info_push");
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  if (
    info_type == 1 /* INFO_KIND_HS_RET_FIELDS */
/*
    info_type == INFO_KIND_HS_APPEND_STRING_REF ||
    info_type == INFO_KIND_UPDATE_FIELDS
*/
  ) {
    if (
      !set_top_table_fields &&
      (error_num = set_top_table_and_fields(table, table->field,
        table_share->fields, TRUE))
    )
      DBUG_RETURN(error_num);

    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if ((error_num = part_tables[roop_count].table->file->
        set_top_table_and_fields(
        top_table,
        top_table_field_for_childs[roop_count],
        top_table_fields)))
        DBUG_RETURN(error_num);
    }
  }
#ifdef HA_CAN_BULK_ACCESS
  switch (info_type)
  {
    case INFO_KIND_BULK_ACCESS_BEGIN:
      DBUG_PRINT("info",("vp INFO_KIND_BULK_ACCESS_BEGIN"));
      if (bulk_access_started)
      {
        if (!bulk_access_info_current->next)
        {
          if (!(bulk_access_info_current->next = create_bulk_access_info()))
          {
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
          bulk_access_info_current->next->sequence_num =
            bulk_access_info_current->sequence_num + 1;
        }
        bulk_access_info_current = bulk_access_info_current->next;
      } else {
        if (!bulk_access_info_first)
        {
          if (!(bulk_access_info_first = create_bulk_access_info()))
          {
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
          bulk_access_info_first->sequence_num = 0;
        }
        bulk_access_info_current = bulk_access_info_first;
        bulk_access_started = TRUE;
        bulk_access_executing = FALSE;
      }
      bulk_access_info_current->used = TRUE;
      bulk_access_info_current->called = FALSE;
      *((void **) info) = bulk_access_info_current;
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if ((tmp = part_tables[roop_count].table->file->info_push(info_type,
          &bulk_access_info_current->info[roop_count])))
          error_num = tmp;
      }
      DBUG_RETURN(error_num);
    case INFO_KIND_BULK_ACCESS_CURRENT:
      DBUG_PRINT("info",("vp INFO_KIND_BULK_ACCESS_CURRENT"));
      bulk_access_executing = TRUE;
      bulk_access_info_exec_tgt = (VP_BULK_ACCESS_INFO *) info;
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if ((tmp = part_tables[roop_count].table->file->info_push(info_type,
          bulk_access_info_exec_tgt->info[roop_count])))
          error_num = tmp;
      }
      DBUG_RETURN(error_num);
    case INFO_KIND_BULK_ACCESS_END:
      DBUG_PRINT("info",("vp INFO_KIND_BULK_ACCESS_END"));
      bulk_access_started = FALSE;
      break;
  }
#endif
#else
  switch (info_type)
  {
    case INFO_KIND_HS_RET_FIELDS:
    case INFO_KIND_HS_APPEND_STRING_REF:
    case INFO_KIND_HS_CLEAR_STRING_REF:
    case INFO_KIND_HS_INCREMENT_BEGIN:
    case INFO_KIND_HS_INCREMENT_END:
    case INFO_KIND_HS_DECREMENT_BEGIN:
    case INFO_KIND_HS_DECREMENT_END:
    case INFO_KIND_UPDATE_FIELDS:
    case INFO_KIND_UPDATE_VALUES:
#ifdef HA_CAN_BULK_ACCESS
    case INFO_KIND_BULK_ACCESS_BEGIN:
    case INFO_KIND_BULK_ACCESS_CURRENT:
    case INFO_KIND_BULK_ACCESS_END:
#endif
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    default:
      break;
  }
#endif
#endif
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((tmp = part_tables[roop_count].table->file->info_push(info_type,
      info)))
      error_num = tmp;
  }
  DBUG_RETURN(error_num);
}

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
void ha_vp::return_record_by_parent()
{
  int roop_count;
  DBUG_ENTER("ha_vp::return_record_by_parent");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (part_tables[roop_count].table->file->inited != NONE)
      part_tables[roop_count].table->file->return_record_by_parent();
  }
  DBUG_VOID_RETURN;
}
#endif

int ha_vp::start_stmt(
  THD *thd,
  thr_lock_type lock_type
) {
  int error_num, roop_count;
  DBUG_ENTER("ha_vp::start_stmt");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if ((error_num =
      part_tables[roop_count].table->file->start_stmt(thd, lock_type)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

bool ha_vp::is_fatal_error(
  int error_num,
  uint flags
) {
  bool res;
  DBUG_ENTER("ha_vp::is_fatal_error");
  if (dup_table_idx < share->table_count)
  {
    res = part_tables[dup_table_idx].table->file->is_fatal_error(error_num,
      flags);
  } else {
    res = handler::is_fatal_error(error_num, flags);
  }
  DBUG_RETURN(res);
}

bool ha_vp::check_if_incompatible_data(
  HA_CREATE_INFO *create_info,
  uint table_changes
) {
  DBUG_ENTER("ha_vp::check_if_incompatible_data");
  DBUG_RETURN(handler::check_if_incompatible_data(create_info, table_changes));
}

bool ha_vp::primary_key_is_clustered()
{
  DBUG_ENTER("ha_vp::primary_key_is_clustered");
  DBUG_RETURN(handler::primary_key_is_clustered());
}

bool ha_vp::can_switch_engines()
{
  DBUG_ENTER("ha_vp::can_switch_engines");
  DBUG_RETURN(handler::can_switch_engines());
}

VP_alter_table_operations ha_vp::alter_table_flags(
  VP_alter_table_operations flags
) {
  DBUG_ENTER("ha_vp::alter_table_flags");
  DBUG_RETURN(handler::alter_table_flags(flags));
}

#ifdef VP_HANDLER_HAS_ADD_INDEX
#if MYSQL_VERSION_ID < 50500
int ha_vp::add_index(
  TABLE *table_arg,
  KEY *key_info,
  uint num_of_keys
) {
  DBUG_ENTER("ha_vp::add_index");
  DBUG_RETURN(handler::add_index(table_arg, key_info, num_of_keys));
}
#else
int ha_vp::add_index(
  TABLE *table_arg,
  KEY *key_info,
  uint num_of_keys,
  handler_add_index **add
) {
  DBUG_ENTER("ha_vp::add_index");
  DBUG_RETURN(handler::add_index(table_arg, key_info, num_of_keys, add));
}

int ha_vp::final_add_index(
  handler_add_index *add,
  bool commit
) {
  DBUG_ENTER("ha_vp::final_add_index");
  DBUG_RETURN(handler::final_add_index(add, commit));
}
#endif
#endif

#ifdef VP_HANDLER_HAS_DROP_INDEX
int ha_vp::prepare_drop_index(
  TABLE *table_arg,
  uint *key_num,
  uint num_of_keys
) {
  DBUG_ENTER("ha_vp::prepare_drop_index");
  DBUG_RETURN(handler::prepare_drop_index(table_arg, key_num, num_of_keys));
}

int ha_vp::final_drop_index(
  TABLE *table_arg
) {
  DBUG_ENTER("ha_vp::final_drop_index");
  DBUG_RETURN(handler::final_drop_index(table_arg));
}
#endif

/*
int ha_vp::check_for_upgrade(
  HA_CHECK_OPT *check_opt
) {
  DBUG_ENTER("ha_vp::check_for_upgrade");
  DBUG_RETURN(handler::check_for_upgrade(check_opt));
}
*/

bool ha_vp::was_semi_consistent_read()
{
  int roop_count;
  DBUG_ENTER("ha_vp::was_semi_consistent_read");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!part_tables[roop_count].table->file->was_semi_consistent_read())
      DBUG_RETURN(FALSE);
  }
  DBUG_RETURN(TRUE);
}

void ha_vp::try_semi_consistent_read(
  bool yes
) {
  int roop_count;
  DBUG_ENTER("ha_vp::try_semi_consistent_read");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    part_tables[roop_count].table->file->try_semi_consistent_read(yes);
  }
  DBUG_VOID_RETURN;
}

void ha_vp::unlock_row()
{
  int roop_count;
  DBUG_ENTER("ha_vp::unlock_row");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    part_tables[roop_count].table->file->unlock_row();
  }
  DBUG_VOID_RETURN;
}

void ha_vp::init_table_handle_for_HANDLER()
{
  int roop_count;
  DBUG_ENTER("ha_vp::init_table_handle_for_HANDLER");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    part_tables[roop_count].table->file->init_table_handle_for_HANDLER();
  }
  DBUG_VOID_RETURN;
}

void ha_vp::change_table_ptr(
  TABLE *table_arg,
  TABLE_SHARE *share_arg
) {
  DBUG_ENTER("ha_vp::change_table_ptr");
  handler::change_table_ptr(table_arg, share_arg);
  DBUG_VOID_RETURN;
}

#if MYSQL_VERSION_ID < 50600
char *ha_vp::get_tablespace_name(
  THD *thd,
  char *name,
  uint name_len
) {
  DBUG_ENTER("ha_vp::get_tablespace_name");
  DBUG_RETURN(handler::get_tablespace_name(thd, name, name_len));
}
#endif

bool ha_vp::is_fk_defined_on_table_or_index(
  uint index
) {
  DBUG_ENTER("ha_vp::is_fk_defined_on_table_or_index");
  DBUG_RETURN(handler::is_fk_defined_on_table_or_index(index));
}

char *ha_vp::get_foreign_key_create_info()
{
  DBUG_ENTER("ha_vp::get_foreign_key_create_info");
  DBUG_RETURN(handler::get_foreign_key_create_info());
}

int ha_vp::get_foreign_key_list(
  THD *thd,
  List<FOREIGN_KEY_INFO> *f_key_list
) {
  DBUG_ENTER("ha_vp::get_foreign_key_list");
  DBUG_RETURN(handler::get_foreign_key_list(thd, f_key_list));
}

#if MYSQL_VERSION_ID < 50500
#else
int ha_vp::get_parent_foreign_key_list(
  THD *thd,
  List<FOREIGN_KEY_INFO> *f_key_list
) {
  DBUG_ENTER("ha_vp::get_parent_foreign_key_list");
  DBUG_RETURN(handler::get_parent_foreign_key_list(thd, f_key_list));
}
#endif

uint ha_vp::referenced_by_foreign_key()
{
  DBUG_ENTER("ha_vp::referenced_by_foreign_key");
  DBUG_RETURN(handler::referenced_by_foreign_key());
}

void ha_vp::free_foreign_key_create_info(
  char* str
) {
  DBUG_ENTER("ha_vp::free_foreign_key_create_info");
  handler::free_foreign_key_create_info(str);
  DBUG_VOID_RETURN;
}

#ifdef VP_HANDLER_HAS_COUNT_QUERY_CACHE_DEPENDANT_TABLES
#ifdef VP_REGISTER_QUERY_CACHE_TABLE_HAS_CONST_TABLE_KEY
my_bool ha_vp::register_query_cache_table(
  THD *thd,
  const char *table_key,
  uint key_length,
  qc_engine_callback *engine_callback,
  ulonglong *engine_data
)
#else
my_bool ha_vp::register_query_cache_table(
  THD *thd,
  char *table_key,
  uint key_length,
  qc_engine_callback *engine_callback,
  ulonglong *engine_data
)
#endif
{
  DBUG_ENTER("ha_vp::register_query_cache_table");
  DBUG_PRINT("info",("vp this=%p", this));
  DBUG_RETURN(handler::register_query_cache_table(
    thd,
    table_key,
    key_length,
    engine_callback,
    engine_data
  ));
}

uint ha_vp::count_query_cache_dependant_tables(
  uint8 *tables_type
) {
  int roop_count;
  uint table_count = 0;
  DBUG_ENTER("ha_vp::register_query_cache_table");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    table_count += part_tables[roop_count].table->file->
      count_query_cache_dependant_tables(tables_type);
  }
  DBUG_RETURN(table_count);
}

my_bool ha_vp::register_query_cache_dependant_tables(
  THD *thd,
  Query_cache *cache,
  Query_cache_block_table **block,
  uint *n
) {
  int roop_count;
  DBUG_ENTER("ha_vp::register_query_cache_dependant_tables");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    TABLE *table2 = part_tables[roop_count].table;
    (++(*block))->n = ++(*n);
#ifdef VP_QCACHE_INSERT_TABLE_REQUIRES_THDPTR
    if (!cache->insert_table(
      thd,
      table2->s->table_cache_key.length,
      table2->s->table_cache_key.str,
      (*block),
      table2->s->db.length,
      0,
      table_cache_type(),
      0,
      0,
      TRUE
    ))
      DBUG_RETURN(TRUE);
#else
    if (!cache->insert_table(
      table2->s->table_cache_key.length,
      table2->s->table_cache_key.str,
      (*block),
      table2->s->db.length,
      0,
      table_cache_type(),
      0,
      0,
      TRUE
    ))
      DBUG_RETURN(TRUE);
#endif

    if (table2->file->register_query_cache_dependant_tables(
      thd,
      cache,
      block,
      n
    ))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}
#else
#ifdef HTON_CAN_MERGE
int ha_vp::qcache_insert(
  Query_cache *qcache,
  Query_cache_block_table *block_table,
  TABLE_COUNTER_TYPE &n
) {
  int roop_count;
  DBUG_ENTER("ha_vp::qcache_insert");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (!part_tables[roop_count].table->file->qcache_insert(qcache,
      block_table, n))
      DBUG_RETURN(0);
  }
  DBUG_RETURN(1);
}

TABLE_COUNTER_TYPE ha_vp::qcache_table_count()
{
  int roop_count;
  TABLE_COUNTER_TYPE table_count = 1;
  DBUG_ENTER("ha_vp::qcache_table_count");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    table_count += part_tables[roop_count].table->file->qcache_table_count();
  }
  DBUG_RETURN(table_count);
}
#endif
#endif

int ha_vp::choose_child_index(
  uint idx,
  uchar *read_set,
  uchar *write_set,
  int *table_idx,
  int *key_idx
) {
  int error_num, roop_count, correspond_count, correspond_count2,
    choose_table_mode;
  uint field_count, correspond_cond_count;
  uchar *tmp_columns_bit, *pk_columns_bit, *tmp_select_ignore;
  VP_CORRESPOND_KEY *tmp_correspond_key, *tmp_correspond_key2;
  bool correspond_flag, correspond_flag2;
  DBUG_ENTER("ha_vp::choose_child_index");

  if (lock_mode > 0 || lock_type_ext == F_WRLCK)
  {
    choose_table_mode =
      vp_param_choose_table_mode_for_lock(ha_thd(),
        share->choose_table_mode_for_lock);
    DBUG_PRINT("info",("vp choose_table_mode_for_lock=%d",
      choose_table_mode));
    tmp_select_ignore = select_ignore_with_lock;
  } else {
    choose_table_mode =
      vp_param_choose_table_mode(ha_thd(), share->choose_table_mode);
    DBUG_PRINT("info",("vp choose_table_mode=%d",
      choose_table_mode));
    tmp_select_ignore = select_ignore;
  }

  /* choose a index phase 1 */
  correspond_flag = TRUE;
  tmp_columns_bit = share->keys[idx].columns_bit;
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    if (
      (read_set[roop_count] & tmp_columns_bit[roop_count]) !=
        read_set[roop_count] ||
      (write_set[roop_count] & tmp_columns_bit[roop_count]) !=
        write_set[roop_count]
    ) {
      correspond_flag = FALSE;
      break;
    }
  }
  if (correspond_flag)
  {
    if (choose_table_mode == 0)
    {
      tmp_correspond_key = share->keys[idx].shortest_correspond_key;
      while (vp_bit_is_set(tmp_select_ignore, tmp_correspond_key->table_idx))
      {
        if (!(tmp_correspond_key = tmp_correspond_key->next_shortest))
        {
          my_printf_error(ER_VP_IGNORED_CORRESPOND_KEY_NUM,
            ER_VP_IGNORED_CORRESPOND_KEY_STR, MYF(0), idx);
          DBUG_RETURN(ER_VP_IGNORED_CORRESPOND_KEY_NUM);
        }
      }
    } else {
      tmp_correspond_key = share->keys[idx].correspond_key;
      while (vp_bit_is_set(tmp_select_ignore, tmp_correspond_key->table_idx))
      {
        if (!(tmp_correspond_key = tmp_correspond_key->next))
        {
          my_printf_error(ER_VP_IGNORED_CORRESPOND_KEY_NUM,
            ER_VP_IGNORED_CORRESPOND_KEY_STR, MYF(0), idx);
          DBUG_RETURN(ER_VP_IGNORED_CORRESPOND_KEY_NUM);
        }
      }
    }
    *table_idx = tmp_correspond_key->table_idx;
    *key_idx = tmp_correspond_key->key_idx;
    child_keyread = TRUE;
    single_table = TRUE;
    set_used_table = TRUE;
    clear_child_bitmap(*table_idx);
    set_child_bitmap(read_set, *table_idx, FALSE);
    set_child_bitmap(write_set, *table_idx, TRUE);
    vp_set_bit(use_tables, *table_idx);
    memset(read_set, 0, sizeof(uchar) * share->bitmap_size);
    memset(write_set, 0, sizeof(uchar) * share->bitmap_size);
    DBUG_RETURN(0);
  }

  /* choose a index phase 2 */
  if (choose_table_mode == 0)
  {
    memset(child_cond_count, 0, sizeof(uint) * share->table_count);
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if ((error_num = count_condition(roop_count)))
        DBUG_RETURN(error_num);
    }

    bool first = TRUE;
    correspond_cond_count = 0;
    correspond_count = 0;
    field_count = 0;
    tmp_correspond_key = NULL;
    tmp_correspond_key2 = share->keys[idx].correspond_key;
    while (vp_bit_is_set(tmp_select_ignore, tmp_correspond_key2->table_idx))
    {
      if (!(tmp_correspond_key2 = tmp_correspond_key2->next))
      {
        my_printf_error(ER_VP_IGNORED_CORRESPOND_KEY_NUM,
          ER_VP_IGNORED_CORRESPOND_KEY_STR, MYF(0), idx);
        DBUG_RETURN(ER_VP_IGNORED_CORRESPOND_KEY_NUM);
      }
    }
    while (tmp_correspond_key2)
    {
      correspond_count2 = 0;
      tmp_columns_bit = tmp_correspond_key2->columns_bit;
      for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        correspond_count2 += vp_bit_count(
          (read_set[roop_count] | write_set[roop_count]) &
          tmp_columns_bit[roop_count]);
      if (
        first ||
        correspond_cond_count <
          child_cond_count[tmp_correspond_key2->table_idx]
      ) {
        first = FALSE;
        tmp_correspond_key = tmp_correspond_key2;
        correspond_count = correspond_count2;
        field_count =
          part_tables[tmp_correspond_key->table_idx].table->s->fields;
        correspond_cond_count =
          child_cond_count[tmp_correspond_key->table_idx];
      } else if (correspond_cond_count ==
        child_cond_count[tmp_correspond_key2->table_idx])
      {
        if (correspond_count < correspond_count2)
        {
          tmp_correspond_key = tmp_correspond_key2;
          correspond_count = correspond_count2;
          field_count =
            part_tables[tmp_correspond_key->table_idx].table->s->fields;
        } else if (
          correspond_count == correspond_count2 &&
          field_count >
            part_tables[tmp_correspond_key2->table_idx].table->s->fields
        ) {
          tmp_correspond_key = tmp_correspond_key2;
          field_count =
            part_tables[tmp_correspond_key->table_idx].table->s->fields;
        }
      }
      tmp_correspond_key2 = tmp_correspond_key2->next;
      if (tmp_correspond_key2)
      {
        while (vp_bit_is_set(tmp_select_ignore,
          tmp_correspond_key2->table_idx))
        {
          if (!(tmp_correspond_key2 = tmp_correspond_key2->next))
            break;
        }
      }
    }
  } else {
    tmp_correspond_key = share->keys[idx].correspond_key;
    while (vp_bit_is_set(tmp_select_ignore, tmp_correspond_key->table_idx))
    {
      if (!(tmp_correspond_key = tmp_correspond_key->next))
      {
        my_printf_error(ER_VP_IGNORED_CORRESPOND_KEY_NUM,
          ER_VP_IGNORED_CORRESPOND_KEY_STR, MYF(0), idx);
        DBUG_RETURN(ER_VP_IGNORED_CORRESPOND_KEY_NUM);
      }
    }
  }

  /* choose a index phase 3 */
  correspond_flag = TRUE;
  tmp_columns_bit = tmp_correspond_key->columns_bit;
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    work_bitmap[roop_count] =
      read_set[roop_count] & tmp_columns_bit[roop_count];
    DBUG_PRINT("info",("vp work_bitmap[%d]=%d",
      roop_count, work_bitmap[roop_count]));
    read_set[roop_count] &= ~work_bitmap[roop_count];
    DBUG_PRINT("info",("vp read_set[%d]=%d",
      roop_count, read_set[roop_count]));

    work_bitmap2[roop_count] =
      write_set[roop_count] & tmp_columns_bit[roop_count];
    DBUG_PRINT("info",("vp work_bitmap2[%d]=%d",
      roop_count, work_bitmap2[roop_count]));
    write_set[roop_count] &= ~work_bitmap2[roop_count];
    DBUG_PRINT("info",("vp write_set[%d]=%d",
      roop_count, write_set[roop_count]));

    if (read_set[roop_count] || write_set[roop_count])
      correspond_flag = FALSE;
  }
  *table_idx = tmp_correspond_key->table_idx;
  *key_idx = tmp_correspond_key->key_idx;
  if (correspond_flag)
  {
    child_keyread = TRUE;
    single_table = TRUE;
  } else {
    /* with table scanning */
    correspond_flag = TRUE;
    correspond_flag2 = TRUE;
    tmp_columns_bit =
      &share->correspond_columns_bit[*table_idx * share->bitmap_size];
    pk_columns_bit =
      share->keys[table_share->primary_key].columns_bit;
    for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
    {
      if (
        correspond_flag2 &&
        (
          ((read_set[roop_count] | write_set[roop_count]) &
            tmp_columns_bit[roop_count]) ||
          ((work_bitmap[roop_count] | work_bitmap2[roop_count]) &
            pk_columns_bit[roop_count]) != pk_columns_bit[roop_count]
        )
      ) {
        correspond_flag2 = FALSE;
      }
      work_bitmap[roop_count] |=
        read_set[roop_count] & tmp_columns_bit[roop_count];
      DBUG_PRINT("info",("vp work_bitmap[%d]=%d",
        roop_count, work_bitmap[roop_count]));
      read_set[roop_count] &= ~work_bitmap[roop_count];
      DBUG_PRINT("info",("vp read_set[%d]=%d",
        roop_count, read_set[roop_count]));

      work_bitmap2[roop_count] |=
        write_set[roop_count] & tmp_columns_bit[roop_count];
      DBUG_PRINT("info",("vp work_bitmap2[%d]=%d",
        roop_count, work_bitmap2[roop_count]));
      write_set[roop_count] &= ~work_bitmap2[roop_count];
      DBUG_PRINT("info",("vp write_set[%d]=%d",
        roop_count, write_set[roop_count]));

      if (read_set[roop_count] || write_set[roop_count])
        correspond_flag = FALSE;
    }
    if (correspond_flag)
      single_table = TRUE;
    else if (correspond_flag2)
      child_keyread = TRUE;
  }
  set_used_table = TRUE;
  clear_child_bitmap(*table_idx);
  set_child_bitmap(work_bitmap, *table_idx, FALSE);
  set_child_bitmap(work_bitmap2, *table_idx, TRUE);
  vp_set_bit(use_tables, *table_idx);

  DBUG_RETURN(0);
}

int ha_vp::choose_child_ft_tables(
  uchar *read_set,
  uchar *write_set
) {
  int roop_count, table_idx;
  uchar *tmp_columns_bit, *tmp_columns_bit2;
  bool correspond_flag;
  DBUG_ENTER("ha_vp::choose_child_ft_tables");
  ft_correspond_flag = FALSE;
  if (ft_current)
  {
    st_vp_ft_info  *ft_info = ft_first;
    while (TRUE)
    {
      table_idx = ft_info->target->table_idx;
      if (!(vp_bit_is_set(use_tables, table_idx)))
      {
        correspond_flag = TRUE;
        tmp_columns_bit =
          &share->correspond_columns_bit[table_idx * share->bitmap_size];
        tmp_columns_bit2 = share->keys[ft_info->inx].columns_bit;
        for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        {
          work_bitmap[roop_count] =
            read_set[roop_count] & tmp_columns_bit[roop_count];
          read_set[roop_count] &= ~work_bitmap[roop_count];
          work_bitmap[roop_count] |= tmp_columns_bit2[roop_count];

          work_bitmap2[roop_count] =
            write_set[roop_count] & tmp_columns_bit[roop_count];
          write_set[roop_count] &= ~work_bitmap2[roop_count];

          if (read_set[roop_count] || write_set[roop_count])
            correspond_flag = FALSE;
        }
        if (!set_used_table)
        {
          child_table_idx = table_idx;
          if (correspond_flag)
          {
            single_table = TRUE;
          }
        } else if (single_table && child_table_idx != table_idx)
          single_table = FALSE;
        set_used_table = TRUE;
        clear_child_bitmap(table_idx);
        set_child_bitmap(work_bitmap, table_idx, FALSE);
        set_child_bitmap(work_bitmap2, table_idx, TRUE);
        vp_set_bit(use_tables, table_idx);
        if (correspond_flag)
          ft_correspond_flag = TRUE;
      }

      if (ft_info == ft_current)
        break;
      ft_info = ft_info->next;
    }
  }
  DBUG_RETURN(0);
}

int ha_vp::choose_child_tables(
  uchar *read_set,
  uchar *write_set
) {
  int error_num, roop_count, roop_count2, correspond_count, correspond_count2,
    table_idx, table_idx2, choose_table_mode;
  uint field_count, correspond_cond_count;
  uchar *tmp_columns_bit, *tmp_select_ignore, *pk_bitmap = NULL;
  bool correspond_flag, has_non_pk_columns = FALSE;
  DBUG_ENTER("ha_vp::choose_child_tables");

  if (lock_mode > 0 || lock_type_ext == F_WRLCK)
  {
    choose_table_mode =
      vp_param_choose_table_mode_for_lock(ha_thd(),
        share->choose_table_mode_for_lock);
    DBUG_PRINT("info",("vp choose_table_mode_for_lock=%d",
      choose_table_mode));
    tmp_select_ignore = select_ignore_with_lock;
  } else {
    choose_table_mode =
      vp_param_choose_table_mode(ha_thd(), share->choose_table_mode);
    DBUG_PRINT("info",("vp choose_table_mode=%d",
      choose_table_mode));
    tmp_select_ignore = select_ignore;
  }
  if (choose_table_mode == 0)
  {
    memset(child_cond_count, 0, sizeof(uint) * share->table_count);
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if ((error_num = count_condition(roop_count)))
        DBUG_RETURN(error_num);
    }
  } else {
    has_non_pk_columns = FALSE;
    pk_bitmap = share->keys[table_share->primary_key].columns_bit;
    for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
    {
      if ((read_set[roop_count] | write_set[roop_count] |
        pk_bitmap[roop_count]) != pk_bitmap[roop_count])
      {
        DBUG_PRINT("info",("vp bitmap has non pk columns"));
        has_non_pk_columns = TRUE;
        break;
      }
    }
  }
  table_idx2 = 0;
  do {
    table_idx = share->table_count;
    if (choose_table_mode == 0)
    {
      correspond_count = 0;
      field_count = 0;
      correspond_cond_count = 0;
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (
          !(vp_bit_is_set(use_tables, roop_count)) &&
          !(vp_bit_is_set(tmp_select_ignore, roop_count))
        ) {
          correspond_count2 = 0;
          tmp_columns_bit =
            &share->correspond_columns_bit[roop_count * share->bitmap_size];
          for (roop_count2 = 0; roop_count2 < share->bitmap_size;
            roop_count2++)
            correspond_count2 += vp_bit_count(
              (read_set[roop_count2] | write_set[roop_count2]) &
              tmp_columns_bit[roop_count2]);
          if (correspond_cond_count < child_cond_count[roop_count])
          {
            table_idx = roop_count;
            correspond_count = correspond_count2;
            field_count =
              part_tables[roop_count].table->s->fields;
            correspond_cond_count = child_cond_count[roop_count];
          } else if (correspond_cond_count == child_cond_count[roop_count])
          {
            if (correspond_count < correspond_count2)
            {
              table_idx = roop_count;
              correspond_count = correspond_count2;
              field_count =
                part_tables[roop_count].table->s->fields;
            } else if (
              correspond_count == correspond_count2 &&
              field_count >
                part_tables[roop_count].table->s->fields
            ) {
              table_idx = roop_count;
              field_count =
                part_tables[roop_count].table->s->fields;
            }
          }
        }
      }
    } else {
      for (roop_count = table_idx2; roop_count < share->table_count;
        roop_count++)
      {
        if (
          !(vp_bit_is_set(use_tables, roop_count)) &&
          !(vp_bit_is_set(tmp_select_ignore, roop_count))
        ) {
          tmp_columns_bit =
            &share->correspond_columns_bit[roop_count * share->bitmap_size];
          if (has_non_pk_columns)
          {
            for (roop_count2 = 0; roop_count2 < share->bitmap_size;
              roop_count2++)
            {
              if (vp_bit_count(
                (read_set[roop_count2] | write_set[roop_count2]) &
                tmp_columns_bit[roop_count2] & (~pk_bitmap[roop_count2])))
                break;
            }
          } else {
            for (roop_count2 = 0; roop_count2 < share->bitmap_size;
              roop_count2++)
            {
              if (vp_bit_count(
                (read_set[roop_count2] | write_set[roop_count2]) &
                tmp_columns_bit[roop_count2]))
                break;
            }
          }
          if (roop_count2 < share->bitmap_size)
          {
            table_idx2 = table_idx = roop_count;
            break;
          }
        }
      }
    }

    if (table_idx < share->table_count)
    {
      correspond_flag = TRUE;
      tmp_columns_bit =
        &share->correspond_columns_bit[table_idx * share->bitmap_size];
      for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
      {
        work_bitmap[roop_count] =
          read_set[roop_count] & tmp_columns_bit[roop_count];
        read_set[roop_count] &= ~work_bitmap[roop_count];

        work_bitmap2[roop_count] =
          write_set[roop_count] & tmp_columns_bit[roop_count];
        write_set[roop_count] &= ~work_bitmap2[roop_count];

        if (read_set[roop_count] || write_set[roop_count])
          correspond_flag = FALSE;
      }
      if (!set_used_table)
      {
        child_table_idx = table_idx;
        if (correspond_flag)
        {
          single_table = TRUE;
        }
      }
      set_used_table = TRUE;
      clear_child_bitmap(table_idx);
      set_child_bitmap(work_bitmap, table_idx, FALSE);
      set_child_bitmap(work_bitmap2, table_idx, TRUE);
      vp_set_bit(use_tables, table_idx);
      if (correspond_flag)
        break;
    }
  } while (table_idx < share->table_count);
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    if (vp_bit_count((read_set[roop_count] | write_set[roop_count]) &
      share->all_columns_bit[roop_count]))
    {
      my_printf_error(ER_VP_IGNORED_CORRESPOND_COLUMN_NUM,
        ER_VP_IGNORED_CORRESPOND_COLUMN_STR, MYF(0));
      DBUG_RETURN(ER_VP_IGNORED_CORRESPOND_COLUMN_NUM);
    }
  }
  DBUG_RETURN(0);
}

void ha_vp::clear_child_bitmap(
  int table_idx
) {
  TABLE *child_table = part_tables[table_idx].table;
  int bitmap_size = (child_table->s->fields + 7) / 8;
  DBUG_ENTER("ha_vp::clear_child_bitmap");
  memset((uchar *) child_table->write_set->bitmap, 0,
    sizeof(uchar) * bitmap_size);
  memset((uchar *) child_table->read_set->bitmap, 0,
    sizeof(uchar) * bitmap_size);
  DBUG_VOID_RETURN;
}

uchar *ha_vp::create_child_key(
  const uchar *key_same,
  uchar *key_different,
  key_part_map keypart_map,
  uint key_length_same,
  uint *key_length
) {
  int roop_count, length;
  uint store_length;
  uchar *ptr;
  TABLE *table2;
  Field *field, *field2;
  KEY *key_info, *key_info2;
  KEY_PART_INFO *key_part, *key_part2;
  char buff[MAX_FIELD_WIDTH];
  String str(buff, sizeof(buff), &my_charset_bin), str2;
  key_part_map tmp_key_part_map;
  DBUG_ENTER("ha_vp::create_child_key");
  if (vp_bit_is_set(share->need_converting, child_table_idx))
  {
#ifndef DBUG_OFF
    int roop_count2;
    key_info = &table->key_info[active_index];
    key_part = key_info->key_part;
    tmp_key_part_map =
      make_prev_keypart_map(vp_user_defined_key_parts(key_info));
    tmp_key_part_map &= keypart_map;
    for (
      roop_count = 0, ptr = (uchar *) key_same;
      tmp_key_part_map > 0;
      ptr += store_length, roop_count++, tmp_key_part_map >>= 1, key_part++
    ) {
      store_length = key_part->store_length;
      for (roop_count2 = 0; roop_count2 < (int) store_length; roop_count2++)
      {
        DBUG_PRINT("info",("vp key[%d][%d]=%x",
          roop_count, roop_count2, ptr[roop_count2]));
      }
    }
#endif
    table2 = part_tables[child_table_idx].table;
    key_info = &table->key_info[active_index];
    key_part = key_info->key_part;
    key_info2 = &table2->key_info[child_key_idx];
    key_part2 = key_info2->key_part;
    tmp_key_part_map =
      make_prev_keypart_map(vp_user_defined_key_parts(key_info));
    tmp_key_part_map &= keypart_map;
#ifndef DBUG_OFF
    my_bitmap_map *tmp_map_r = dbug_tmp_use_all_columns(table,
      table->read_set);
    my_bitmap_map *tmp_map_w2 = dbug_tmp_use_all_columns(table2,
      table2->write_set);
#endif
    *key_length = 0;
    for (
      roop_count = 0, ptr = (uchar *) key_same,
      store_length = key_part->store_length;
      tmp_key_part_map > 0;
      ptr += store_length, roop_count++, tmp_key_part_map >>= 1,
      store_length = key_part->store_length
    ) {
      field = key_part[roop_count].field;
      field2 = key_part2[roop_count].field;
      if (key_part[roop_count].null_bit && *ptr++)
      {
        field2->set_null();
        field2->reset();
      } else {
        if (
          field->type() == MYSQL_TYPE_BLOB ||
          field->real_type() == MYSQL_TYPE_VARCHAR ||
          field->type() == MYSQL_TYPE_GEOMETRY
        ) {
          length = uint2korr(ptr);
          str2.set_quick((char *) ptr + HA_KEY_BLOB_LENGTH, length,
            &my_charset_bin);
          field2->set_notnull();
          field2->store(
            length ? str2.ptr() : NullS, length,
            field->charset());
        } else {
          field->val_str(&str, ptr);
          length = str.length();
          field2->set_notnull();
          field2->store(
            length ? str.ptr() : NullS, length,
            field->charset());
        }
      }
      *key_length += store_length;
    }
#ifndef DBUG_OFF
    dbug_tmp_restore_column_map(table->read_set, tmp_map_r);
    dbug_tmp_restore_column_map(table2->write_set, tmp_map_w2);
#endif
    key_copy(
      key_different,
      table2->record[0],
      key_info2,
      *key_length);
    DBUG_PRINT("info",("vp use key_different"));
    DBUG_RETURN(key_different);
  } else {
    *key_length = key_length_same;
  }
  DBUG_PRINT("info",("vp use key_same"));
  DBUG_RETURN((uchar *) key_same);
}

int ha_vp::get_child_record_by_idx(
  int table_idx,
  my_ptrdiff_t ptr_diff
) {
  int roop_count, length;
  TABLE *table2;
  uchar *tmp_bitmap, *tmp_bitmap2, *pk_bitmap;
  Field **field_ptr, **field_ptr2, *field, *field2;
  int *correspond_columns_c, column_idx;
  char buff[MAX_FIELD_WIDTH];
  String str(buff, sizeof(buff), &my_charset_bin);
  DBUG_ENTER("ha_vp::get_child_record_by_idx");
  if (vp_bit_is_set(share->same_columns, table_idx))
  {
    DBUG_PRINT("info",("vp table_idx %d has same columns", table_idx));
    DBUG_RETURN(0);
  }

  DBUG_PRINT("info",("vp table_idx=%d", table_idx));
  DBUG_PRINT("info",("vp child_table_idx=%d", child_table_idx));
  if (table_idx == child_table_idx)
    pk_bitmap = NULL;
  else
    pk_bitmap = share->keys[table_share->primary_key].columns_bit;
  table2 = part_tables[table_idx].table;
  tmp_bitmap = (uchar *) table2->read_set->bitmap;
  tmp_bitmap2 = (uchar *) table2->write_set->bitmap;
  field_ptr = table->field;
  field_ptr2 = table2->field;
  correspond_columns_c = share->correspond_columns_c_ptr[table_idx];
  DBUG_PRINT("info",("vp child[%d] fields=%u", table_idx, table2->s->fields));
#ifndef DBUG_OFF
  my_bitmap_map *tmp_map_w = dbug_tmp_use_all_columns(table, table->write_set);
#endif
  for (roop_count = 0; roop_count < (int) table2->s->fields; roop_count++)
  {
    if (
      vp_bit_is_set(tmp_bitmap, roop_count) ||
      vp_bit_is_set(tmp_bitmap2, roop_count)
    ) {
      DBUG_PRINT("info",("vp check field %d-%d",
        child_table_idx, roop_count));
#ifndef DBUG_OFF
      my_bitmap_map *tmp_map_r2 = dbug_tmp_use_all_columns(table2,
        table2->read_set);
#endif
      column_idx = correspond_columns_c[roop_count];
      DBUG_PRINT("info",("vp column_idx=%d", column_idx));
      if (!pk_bitmap || !(vp_bit_is_set(pk_bitmap, column_idx)))
      {
        DBUG_PRINT("info",("vp set field %d-%d to %d",
          child_table_idx, roop_count, column_idx));
        field2 = field_ptr2[roop_count];
        field = field_ptr[column_idx];
        field->move_field_offset(ptr_diff);
        if (field2->is_null())
        {
          DBUG_PRINT("info", ("vp null"));
          field->set_null();
          field->reset();
        } else {
          DBUG_PRINT("info", ("vp not null"));
          field->set_notnull();
          if (field->flags & BLOB_FLAG)
          {
            DBUG_PRINT("info", ("vp blob field"));
            if (
              (field2->flags & BLOB_FLAG) &&
              (
                field->charset() == &my_charset_bin ||
                field->charset()->cset == field2->charset()->cset
              )
            ) {
              uchar *tmp_char;
#ifdef VP_FIELD_BLOB_GET_PTR_RETURNS_UCHAR_PTR
              tmp_char = ((Field_blob *)field2)->get_ptr();
#else
              ((Field_blob *)field2)->get_ptr(&tmp_char);
#endif
              ((Field_blob *)field)->set_ptr(
                ((Field_blob *)field2)->get_length(), tmp_char);
              DBUG_PRINT("info", ("vp ((Field_blob *)field2)->get_length()=%u",
                ((Field_blob *)field2)->get_length()));
#ifndef DBUG_OFF
              if (field2->type() == MYSQL_TYPE_GEOMETRY)
              {
                Field_geom *g1 = (Field_geom *) field;
                Field_geom *g2 = (Field_geom *) field2;
                DBUG_PRINT("info", ("vp geometry_type is g1:%u g2:%u",
                  g1->geom_type, g2->geom_type));
                DBUG_PRINT("info", ("vp srid is g1:%u g2:%u",
                  g1->srid, g2->srid));
                DBUG_PRINT("info", ("vp precision is g1:%u g2:%u",
                  g1->precision, g2->precision));
                DBUG_PRINT("info", ("vp storage_type is g1:%u g2:%u",
                  g1->storage, g2->storage));

                Geometry_buffer buffer;
                Geometry *geom;
                if ((geom = Geometry::construct(&buffer, (char *) tmp_char,
                  g2->get_length())))
                {
                  str.length(0);
                  str.set_charset(&my_charset_latin1);
                  const char *dummy;
                  if (!(geom->as_wkt(&str, &dummy)))
                  {
                    DBUG_PRINT("info", ("vp geom child is %s",
                      str.c_ptr_safe()));
                  }
                }
#ifdef VP_FIELD_BLOB_GET_PTR_RETURNS_UCHAR_PTR
                tmp_char = g1->get_ptr();
#else
                g1->get_ptr(&tmp_char);
#endif
                if ((geom = Geometry::construct(&buffer, (char *) tmp_char,
                  g1->get_length())))
                {
                  str.length(0);
                  str.set_charset(&my_charset_latin1);
                  const char *dummy;
                  if (!(geom->as_wkt(&str, &dummy)))
                  {
                    DBUG_PRINT("info", ("vp geom parent is %s",
                      str.c_ptr_safe()));
                  }
                }
              }
#endif
            } else {
              DBUG_PRINT("info", ("vp blob convert"));
              String *str2 = &blob_buff[field->field_index];
              str2->length(0);
              field2->val_str(&str);
              if (str2->append(str.ptr(), str.length(), field2->charset()))
              {
#ifndef DBUG_OFF
                dbug_tmp_restore_column_map(table->write_set, tmp_map_w);
                dbug_tmp_restore_column_map(table2->read_set, tmp_map_r2);
#endif
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              }
              ((Field_blob *)field)->set_ptr(str2->length(),
                (uchar *) str2->ptr());
            }
          } else {
            field2->val_str(&str);
            length = str.length();
            field->store(
              length ? str.ptr() : NullS, length,
              field2->charset());
            DBUG_PRINT("info", ("vp length = %d", length));
          }
        }
        field->move_field_offset(-ptr_diff);
      }
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(table2->read_set, tmp_map_r2);
#endif
    }
  }
#ifndef DBUG_OFF
  dbug_tmp_restore_column_map(table->write_set, tmp_map_w);
#endif
  DBUG_RETURN(0);
}

int ha_vp::get_child_record_by_pk(
  my_ptrdiff_t ptr_diff
) {
  int error_num = 0, roop_count;
  uchar *table_key;
  uchar table_key_different[MAX_KEY_LENGTH];
  TABLE *table2;
  VP_KEY_COPY vp_key_copy;
#ifndef WITHOUT_VP_BG_ACCESS
  int bgs_mode = vp_param_bgs_mode(table->in_use, share->bgs_mode);
  VP_BG_BASE *base;
#endif
  DBUG_ENTER("ha_vp::get_child_record_by_pk");
  vp_key_copy.init = FALSE;
  vp_key_copy.mem_root_init = FALSE;
  vp_key_copy.ptr = NULL;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      child_table_idx != roop_count &&
      vp_bit_is_set(use_tables, roop_count)
    ) {
      table2 = part_tables[roop_count].table;
#ifndef WITHOUT_VP_BG_ACCESS
      if (
        bgs_mode &&
        (table2->file->ha_table_flags() & VP_CAN_BG_SEARCH)
      ) {
        base = &bg_base[roop_count];
        vp_key_copy.table_key_different = base->table_key_different;
        if (
          (error_num = create_bg_thread(base)) ||
          (error_num = search_by_pk(roop_count, 0, &vp_key_copy, 0,
            (uchar **) &base->table_key))
        )
          goto error;
        base->tgt_key_part_map = vp_key_copy.tgt_key_part_map;
        base->bg_command = VP_BG_COMMAND_SELECT;
        bg_kick(base);
      } else {
#endif
        vp_key_copy.table_key_different = table_key_different;
        if (
          (error_num = search_by_pk(roop_count, 0, &vp_key_copy, ptr_diff,
            &table_key)) ||
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
          (error_num = table2->file->ha_index_read_map(
            table2->record[0], table_key, vp_key_copy.tgt_key_part_map,
            HA_READ_KEY_EXACT))
#else
          (error_num = table2->file->index_read_map(
            table2->record[0], table_key, vp_key_copy.tgt_key_part_map,
            HA_READ_KEY_EXACT))
#endif
        ) {
          table->status = table2->status;
          goto error;
        }
#ifndef WITHOUT_VP_BG_ACCESS
      }
#endif
    }
  }
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (
      child_table_idx != roop_count &&
      vp_bit_is_set(use_tables, roop_count)
    ) {
#ifndef WITHOUT_VP_BG_ACCESS
      table2 = part_tables[roop_count].table;
      if (
        bgs_mode &&
        (table2->file->ha_table_flags() & VP_CAN_BG_SEARCH)
      ) {
        base = &bg_base[roop_count];
        bg_wait(base);
        if (base->bg_error)
        {
          error_num = base->bg_error;
          table->status = table2->status;
          goto error;
        }
      }
#endif
      if ((error_num = get_child_record_by_idx(roop_count, ptr_diff)))
      {
        table->status = table2->status;
        goto error;
      }
    }
  }

  if (vp_key_copy.mem_root_init)
    free_root(&vp_key_copy.mem_root, MYF(0));
  if (vp_key_copy.ptr)
    vp_my_free(vp_key_copy.ptr, MYF(0));
  DBUG_RETURN(error_num);

error:
#ifndef WITHOUT_VP_BG_ACCESS
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    base = &bg_base[roop_count];
    if (base->bg_init)
      bg_wait(base);
  }
#endif
  if (vp_key_copy.mem_root_init)
    free_root(&vp_key_copy.mem_root, MYF(0));
  if (vp_key_copy.ptr)
    vp_my_free(vp_key_copy.ptr, MYF(0));
  DBUG_RETURN(error_num);
}

bool ha_vp::set_child_bitmap(
  uchar *bitmap,
  int table_idx,
  bool write_flg
) {
  bool ret_flag = FALSE;
  int roop_count, field_idx;
  uchar *tmp_bitmap, *pk_bitmap;
  int *correspond_columns_p =
    &share->correspond_columns_p[table_idx * table_share->fields];
  TABLE *child_table = part_tables[table_idx].table;
  bool use_full_column = FALSE;
  DBUG_ENTER("ha_vp::set_child_bitmap");
  DBUG_PRINT("info",("vp table_idx=%d", table_idx));
  DBUG_PRINT("info",("vp write_flg=%d", write_flg));
  DBUG_PRINT("info",("vp update_request=%s",
    update_request ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp need_full_col_for_update=%s",
    vp_bit_is_set(share->need_full_col_for_update, table_idx) ? "TRUE" : "FALSE"));
#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
    DBUG_PRINT("info",("vp source bitmap is %d-%u", roop_count,
      bitmap[roop_count]));
#endif

  if (write_flg)
    tmp_bitmap = (uchar *) child_table->write_set->bitmap;
  else
    tmp_bitmap = (uchar *) child_table->read_set->bitmap;

  if (
    !write_flg &&
    update_request &&
    vp_bit_is_set(share->need_full_col_for_update, table_idx)
  ) {
    use_full_column = TRUE;
    child_keyread = FALSE;
  }

  if (
    !use_full_column &&
    !write_flg &&
    table_idx == child_table_idx &&
    (!single_table || update_request || extra_use_cmp_ref)
  ) {
    pk_bitmap = share->keys[table_share->primary_key].columns_bit;
    if (child_keyread)
    {
      VP_CORRESPOND_KEY *correspond_key =
        share->correspond_keys_p_ptr[child_table_idx];
      uchar *key_bitmap = correspond_key[child_key_idx].columns_bit;
      for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
      {
        if ((key_bitmap[roop_count] & pk_bitmap[roop_count]) !=
          pk_bitmap[roop_count])
        {
          DBUG_PRINT("info",("vp cancel keyread"));
          child_keyread = FALSE;
          break;
        }
      }
    }
  } else
    pk_bitmap = NULL;

  for (roop_count = 0; roop_count < (int) table_share->fields; roop_count++)
  {
    if (
      use_full_column ||
      vp_bit_is_set(bitmap, roop_count) ||
      (pk_bitmap && vp_bit_is_set(pk_bitmap, roop_count))
    ) {
      if ((field_idx = correspond_columns_p[roop_count]) < MAX_FIELDS)
      {
        vp_set_bit(tmp_bitmap, field_idx);
        ret_flag = TRUE;
      }
    }
  }
#ifndef DBUG_OFF
  int bitmap_size = (child_table->s->fields + 7) / 8;
  for (roop_count = 0; roop_count < bitmap_size; roop_count++)
    DBUG_PRINT("info",("vp bitmap is %d-%u", roop_count,
      tmp_bitmap[roop_count]));
#endif
  DBUG_RETURN(ret_flag);
}

bool ha_vp::add_pk_bitmap_to_child()
{
  bool ret_flag = FALSE;
  int roop_count, field_idx;
  uchar *tmp_bitmap, *pk_bitmap;
  int *correspond_columns_p =
    &share->correspond_columns_p[child_table_idx * table_share->fields];
  TABLE *child_table = part_tables[child_table_idx].table;
  DBUG_ENTER("ha_vp::add_pk_bitmap_to_child");
  DBUG_PRINT("info",("vp update_request=%s",
    update_request ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp need_full_col_for_update=%s",
    vp_bit_is_set(share->need_full_col_for_update, child_table_idx) ? "TRUE" : "FALSE"));

  tmp_bitmap = (uchar *) child_table->read_set->bitmap;

  if (
    update_request &&
    vp_bit_is_set(share->need_full_col_for_update, child_table_idx)
  ) {
    DBUG_RETURN(FALSE);
  }

  if (extra_use_cmp_ref || !single_table || update_request)
  {
    pk_bitmap = share->keys[table_share->primary_key].columns_bit;
    if (child_keyread)
    {
      VP_CORRESPOND_KEY *correspond_key =
        share->correspond_keys_p_ptr[child_table_idx];
      uchar *key_bitmap = correspond_key[child_key_idx].columns_bit;
      for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
      {
        if ((key_bitmap[roop_count] & pk_bitmap[roop_count]) !=
          pk_bitmap[roop_count])
        {
          DBUG_PRINT("info",("vp cancel keyread"));
          child_keyread = FALSE;
          break;
        }
      }
    }
  } else
    DBUG_RETURN(FALSE);

  for (roop_count = 0; roop_count < (int) table_share->fields; roop_count++)
  {
    if (vp_bit_is_set(pk_bitmap, roop_count))
    {
      if ((field_idx = correspond_columns_p[roop_count]) < MAX_FIELDS)
      {
        vp_set_bit(tmp_bitmap, field_idx);
        ret_flag = TRUE;
      }
    }
  }
#ifndef DBUG_OFF
  int bitmap_size = (child_table->s->fields + 7) / 8;
  for (roop_count = 0; roop_count < bitmap_size; roop_count++)
    DBUG_PRINT("info",("vp bitmap is %d-%u", roop_count,
      tmp_bitmap[roop_count]));
#endif
  DBUG_RETURN(ret_flag);
}

void ha_vp::set_child_pt_bitmap()
{
  int roop_count, roop_count2, roop_count3;
  int *correspond_pt_columns_p;
  int *uncorrespond_pt_columns_c;
  int field_idx;
  TABLE *child_table;
  DBUG_ENTER("ha_vp::set_child_pt_bitmap");

  for (roop_count = 0; roop_count < share->table_count; ++roop_count)
  {
    if (vp_bit_is_set(use_tables, roop_count))
    {
      correspond_pt_columns_p =
        &share->correspond_pt_columns_p[roop_count * table_share->fields];
      for (roop_count2 = 0;
        correspond_pt_columns_p[roop_count2] < MAX_FIELDS; ++roop_count2)
      {
        field_idx = correspond_pt_columns_p[roop_count2];
        int *correspond_columns_p =
          &share->correspond_columns_p[child_table_idx * table_share->fields];
        if (correspond_columns_p[field_idx] < MAX_FIELDS)
        {
          child_table = part_tables[child_table_idx].table;
          vp_set_bit(child_table->read_set->bitmap,
            correspond_columns_p[field_idx]);
          DBUG_PRINT("info",("vp child bitmap %d-%d is set",
            child_table_idx, correspond_columns_p[field_idx]));
          continue;
        }

        for (roop_count3 = 0; roop_count3 < share->table_count; ++roop_count3)
        {
          if (
            child_table_idx != roop_count3 &&
            vp_bit_is_set(use_tables, roop_count3)
          ) {
            correspond_columns_p =
              &share->correspond_columns_p[roop_count3 * table_share->fields];
            if (correspond_columns_p[field_idx] < MAX_FIELDS)
            {
              child_table = part_tables[roop_count3].table;
              vp_set_bit(child_table->read_set->bitmap,
                correspond_columns_p[field_idx]);
              DBUG_PRINT("info",("vp child bitmap %d-%d is set",
                roop_count3, correspond_columns_p[field_idx]));
              break;
            }
          }
        }
      }

      uncorrespond_pt_columns_c =
        share->uncorrespond_pt_columns_c_ptr[roop_count];
      for (roop_count2 = 0; uncorrespond_pt_columns_c[roop_count2] < MAX_FIELDS;
        ++roop_count2)
      {
        child_table = part_tables[roop_count2].table;
        vp_set_bit(child_table->read_set->bitmap,
          uncorrespond_pt_columns_c[roop_count2]);
        DBUG_PRINT("info",("vp child bitmap %d-%d for uncorrespond is set",
          roop_count, uncorrespond_pt_columns_c[roop_count2]));
      }
    }
  }
  DBUG_VOID_RETURN;
}

void ha_vp::set_child_record_for_update(
  my_ptrdiff_t ptr_diff,
  int record_idx,
  bool write_flg,
  bool use_table_chk
) {
  int roop_count, roop_count2, length,
    *correspond_columns_c, column_idx;
  my_ptrdiff_t ptr_diff2;
  TABLE *table2;
  Field **field_ptr, **field_ptr2, *field, *field2;
  MY_BITMAP *my_bitmap;
  char buff[MAX_FIELD_WIDTH];
  String str(buff, sizeof(buff), &my_charset_bin);
  DBUG_ENTER("ha_vp::set_child_record_for_update");
  field_ptr = table->field;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(share->same_columns, roop_count))
    {
      DBUG_PRINT("info",("vp child_table %d has same columns", roop_count));
      continue;
    }
    if (
      use_table_chk ||
      vp_bit_is_set(use_tables2, roop_count)
    ) {
      table2 = part_tables[roop_count].table;
      field_ptr2 = table2->field;
      correspond_columns_c = share->correspond_columns_c_ptr[roop_count];
      if (write_flg)
        my_bitmap = table2->write_set;
      else
        my_bitmap = table2->read_set;
#ifndef DBUG_OFF
      my_bitmap_map *tmp_map_r = dbug_tmp_use_all_columns(table,
        table->read_set);
      my_bitmap_map *tmp_map_w2 = NULL;
      if (!write_flg)
        tmp_map_w2 = dbug_tmp_use_all_columns(table2,
          table2->write_set);
#endif
      ptr_diff2 = PTR_BYTE_DIFF(table2->record[record_idx], table2->record[0]);
      for (roop_count2 = 0; roop_count2 < (int) table2->s->fields;
        roop_count2++)
      {
        column_idx = correspond_columns_c[roop_count2];
        field2 = field_ptr2[roop_count2];
        if (
          bitmap_is_set(my_bitmap, roop_count2)
        ) {
          DBUG_PRINT("info",("vp set field %d to %d-%d",
            column_idx, roop_count, roop_count2));
          field = field_ptr[column_idx];
          field->move_field_offset(ptr_diff);
          field2->move_field_offset(ptr_diff2);
          if (field->is_null())
          {
            field2->set_null();
            field2->reset();
          } else {
            field->val_str(&str);
            length = str.length();
            field2->set_notnull();
            field2->store(
              length ? str.ptr() : NullS, length,
              field->charset());
          }
          field2->move_field_offset(-ptr_diff2);
          field->move_field_offset(-ptr_diff);
          if (use_table_chk)
            vp_set_bit(use_tables2, roop_count);
        }
      }
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(table->read_set, tmp_map_r);
      if (!write_flg)
        dbug_tmp_restore_column_map(table2->write_set, tmp_map_w2);
#endif
    }
  }

  DBUG_VOID_RETURN;
}

void ha_vp::set_child_record_for_insert(
  my_ptrdiff_t ptr_diff,
  int table_idx
) {
  int roop_count2, length,
    *correspond_columns_c, column_idx;
  TABLE *table2;
  Field **field_ptr, **field_ptr2, *field, *field2;
  MY_BITMAP *my_bitmap;
  char buff[MAX_FIELD_WIDTH];
  String str(buff, sizeof(buff), &my_charset_bin);
  DBUG_ENTER("ha_vp::set_child_record_for_insert");
  if (vp_bit_is_set(share->same_columns, table_idx))
  {
    DBUG_PRINT("info",("vp child_table %d has same columns", table_idx));
    DBUG_VOID_RETURN;
  }
  field_ptr = table->field;
  if (
    vp_bit_is_set(use_tables2, table_idx)
  ) {
    table2 = part_tables[table_idx].table;
    field_ptr2 = table2->field;
    correspond_columns_c = share->correspond_columns_c_ptr[table_idx];
    my_bitmap = table2->write_set;
#ifndef DBUG_OFF
    my_bitmap_map *tmp_map_r = dbug_tmp_use_all_columns(table,
      table->read_set);
#endif
    for (roop_count2 = 0; roop_count2 < (int) table2->s->fields; roop_count2++)
    {
      column_idx = correspond_columns_c[roop_count2];
      field2 = field_ptr2[roop_count2];
      if (!bitmap_is_set(my_bitmap, roop_count2))
      {
        vp_set_bit(my_bitmap->bitmap, roop_count2);
        if (column_idx < MAX_FIELDS)
        {
          DBUG_PRINT("info",("vp set field %d to %d-%d",
            column_idx, table_idx, roop_count2));
          field = field_ptr[column_idx];
          field->move_field_offset(ptr_diff);
          if (field->is_null())
          {
            field2->set_null();
            field2->reset();
          } else {
            field->val_str(&str);
            length = str.length();
            field2->set_notnull();
            field2->store(
              length ? str.ptr() : NullS, length,
              field->charset());
          }
          field->move_field_offset(-ptr_diff);
        } else {
          DBUG_PRINT("info",("vp set field DEFAULT to %d-%d",
            table_idx, roop_count2));
          field2->set_default();
        }
      }
    }
#ifndef DBUG_OFF
    dbug_tmp_restore_column_map(table->read_set, tmp_map_r);
#endif
  }

  DBUG_VOID_RETURN;
}

int ha_vp::search_by_pk(
  int table_idx,
  int record_idx,
  VP_KEY_COPY *vp_key_copy,
  my_ptrdiff_t ptr_diff,
  uchar **table_key
) {
  int roop_count, key_idx, error_num;
  TABLE *table2;
  KEY *key_info, *key_info2;
  Field *field, *field2;
  KEY_PART_INFO *key_part, *key_part2;
  char buff[MAX_FIELD_WIDTH];
  String str(buff, sizeof(buff), &my_charset_bin);
  DBUG_ENTER("ha_vp::search_by_pk");
  table2 = part_tables[table_idx].table;
  key_idx = share->correspond_pk[table_idx]->key_idx;
/*
  my_ptrdiff_t ptr_diff2 =
    PTR_BYTE_DIFF(table2->record[record_idx], table2->record[0]);
*/
  key_info = &table->key_info[table_share->primary_key];
  key_part = key_info->key_part;

  if (!vp_key_copy->init)
  {
    vp_key_copy->init = TRUE;
    for (roop_count = 0;
      roop_count < (int) vp_user_defined_key_parts(key_info); roop_count++)
    {
      field = key_part[roop_count].field;
      field->move_field_offset(ptr_diff);
    }
    key_copy(
      vp_key_copy->table_key_same,
      table->record[record_idx],
      key_info,
      key_info->key_length);
    for (roop_count = 0;
      roop_count < (int) vp_user_defined_key_parts(key_info); roop_count++)
    {
      field = key_part[roop_count].field;
      field->move_field_offset(-ptr_diff);
    }
    vp_key_copy->tgt_key_part_map =
      make_prev_keypart_map(vp_user_defined_key_parts(key_info));
  }

  if (vp_bit_is_set(share->need_converting, table_idx))
  {
#ifndef DBUG_OFF
    my_bitmap_map *tmp_map_r = dbug_tmp_use_all_columns(table,
      table->read_set);
    my_bitmap_map *tmp_map_w2 = dbug_tmp_use_all_columns(table2,
      table2->write_set);
#endif
    if (!vp_key_copy->mem_root_init)
    {
      vp_key_copy->mem_root_init = TRUE;
      VP_INIT_ALLOC_ROOT(&vp_key_copy->mem_root, 1024, 0, MYF(MY_WME));
      if (
        !(vp_key_copy->ptr = (char **) my_multi_malloc(MYF(MY_WME),
          &vp_key_copy->ptr,
            sizeof(char *) * vp_user_defined_key_parts(key_info),
          &vp_key_copy->len, sizeof(int) * vp_user_defined_key_parts(key_info),
          &vp_key_copy->null_flg,
            sizeof(uchar) * ((vp_user_defined_key_parts(key_info) + 7) / 8),
          NullS))
      ) {
#ifndef DBUG_OFF
        dbug_tmp_restore_column_map(table->read_set, tmp_map_r);
        dbug_tmp_restore_column_map(table2->write_set, tmp_map_w2);
#endif
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      memset((uchar *) vp_key_copy->null_flg, 0,
        sizeof(uchar) * ((vp_user_defined_key_parts(key_info) + 7) / 8));
      for (roop_count = 0;
        roop_count < (int) vp_user_defined_key_parts(key_info);
        roop_count++)
      {
        field = key_part[roop_count].field;
        field->move_field_offset(ptr_diff);
        if (field->is_null())
        {
          vp_set_bit(vp_key_copy->null_flg, roop_count);
        } else {
          field->val_str(&str);
          vp_key_copy->len[roop_count] = str.length();
          if (!vp_key_copy->len[roop_count])
            vp_key_copy->ptr[roop_count] = NullS;
          else if ((vp_key_copy->ptr[roop_count] =
            (char *) alloc_root(&vp_key_copy->mem_root,
              vp_key_copy->len[roop_count])))
          {
            memcpy(vp_key_copy->ptr[roop_count], str.ptr(),
              vp_key_copy->len[roop_count]);
          } else {
#ifndef DBUG_OFF
            dbug_tmp_restore_column_map(table->read_set, tmp_map_r);
            dbug_tmp_restore_column_map(table2->write_set, tmp_map_w2);
#endif
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
        }
        field->move_field_offset(-ptr_diff);
      }
    }

    key_info2 = &table2->key_info[key_idx];
    key_part2 = key_info2->key_part;
    for (roop_count = 0;
      roop_count < (int) vp_user_defined_key_parts(key_info); roop_count++)
    {
      field2 = key_part2[roop_count].field;
/*
      field2->move_field_offset(ptr_diff2);
*/
      if (vp_bit_is_set(vp_key_copy->null_flg, roop_count))
      {
        field2->set_null();
        field2->reset();
      } else {
        field2->set_notnull();
        field2->store(
          vp_key_copy->ptr[roop_count], vp_key_copy->len[roop_count],
          key_part[roop_count].field->charset());
      }
    }
#ifndef DBUG_OFF
    dbug_tmp_restore_column_map(table->read_set, tmp_map_r);
    dbug_tmp_restore_column_map(table2->write_set, tmp_map_w2);
#endif

    *table_key = vp_key_copy->table_key_different;
    key_copy(
      vp_key_copy->table_key_different,
      table2->record[0],
      key_info2,
      key_info2->key_length);

/*
    for (roop_count = 0; roop_count < key_info->key_parts; roop_count++)
    {
      field2 = key_part2[roop_count].field;
      field2->move_field_offset(-ptr_diff2);
    }
*/
  } else
    *table_key = vp_key_copy->table_key_same;

  /* check part column is available for partition pruning */
  bool part_column_available = TRUE;
  int *correspond_columns_p;
  int *correspond_pt_columns_p;
  int field_idx;
  TABLE *child_table;
  correspond_columns_p =
    &share->correspond_columns_p[child_table_idx * table_share->fields];
  correspond_pt_columns_p =
    &share->correspond_pt_columns_p[table_idx * table_share->fields];
  child_table = part_tables[child_table_idx].table;
  for (roop_count = 0;
    correspond_pt_columns_p[roop_count] < MAX_FIELDS; ++roop_count)
  {
    field_idx = correspond_columns_p[correspond_pt_columns_p[roop_count]];
    DBUG_PRINT("info",("vp field_idx=%d", field_idx));
    if (
      field_idx == MAX_FIELDS ||
      (
        !vp_bit_is_set(child_table->read_set->bitmap, field_idx) &&
        !(
          update_request &&
          vp_bit_is_set(share->need_full_col_for_update, child_table_idx)
        )
      )
    ) {
      DBUG_PRINT("info",("vp did not get this column by child_table_idx"));
      part_column_available = FALSE;
      break;
    }
  }
  DBUG_PRINT("info",("vp part_column_available=%s",
    part_column_available ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("vp ptr_diff %s",
    ptr_diff ? "is not 0" : "is 0"));
  if (part_column_available)
  {
    if (!vp_bit_is_set(share->same_columns, table_idx))
    {
      DBUG_PRINT("info",("vp table_idx=%d", table_idx));
      /* set partition key value to child table */
      correspond_columns_p =
        &share->correspond_columns_p[table_idx * table_share->fields];
      for (roop_count = 0;
        correspond_pt_columns_p[roop_count] < MAX_FIELDS; ++roop_count)
      {
        int parent_field_idx = correspond_pt_columns_p[roop_count];
        DBUG_PRINT("info",("vp parent_field_idx=%d", parent_field_idx));
        int child_field_idx = correspond_columns_p[parent_field_idx];
        DBUG_PRINT("info",("vp child_field_idx=%d", child_field_idx));
        field = table->field[parent_field_idx];
        field2 = table2->field[child_field_idx];
        field->move_field_offset(ptr_diff);
        if (field->is_null())
        {
          DBUG_PRINT("info", ("vp null"));
          field2->set_null();
          field2->reset();
        } else {
          DBUG_PRINT("info", ("vp not null"));
          field2->set_notnull();
          field->val_str(&str);
          uint length = str.length();
          field2->store(
            length ? str.ptr() : NullS, length,
            field->charset());
          DBUG_PRINT("info", ("vp length = %d", length));
#ifndef DBUG_OFF
          char *value = (char *) my_alloca(length + 1);
          memcpy(value, str.ptr(), length);
          value[length] = '\0';
          DBUG_PRINT("info", ("vp value = %s", value));
          my_afree(value);
#endif
        }
        field->move_field_offset(-ptr_diff);
        vp_set_bit(table2->read_set->bitmap, child_field_idx);
      }
      if ((error_num =
        table2->file->choose_partition_from_column_value(table2->record[0])))
        DBUG_RETURN(error_num);
    } else {
      DBUG_PRINT("info",("vp same column"));
      DBUG_PRINT("info",("vp table_idx=%d", table_idx));
      DBUG_PRINT("info",("vp record[0]=%p", table->record[0]));
/*
      if ((error_num =
        table2->file->choose_partition_from_column_value(ADD_TO_PTR(table2->record[0], ptr_diff, uchar *))))
        DBUG_RETURN(error_num);
*/
      if ((error_num =
        table2->file->choose_partition_from_column_value(ADD_TO_PTR(table->record[0], ptr_diff, uchar *))))
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_vp::search_by_pk_for_update(
  int table_idx,
  int record_idx,
  VP_KEY_COPY *vp_key_copy,
  my_ptrdiff_t ptr_diff,
  int bgu_mode
) {
  int error_num, error_num2, key_idx;
  TABLE *table2;
  uchar *table_key;
  uchar table_key_different[MAX_KEY_LENGTH];
#ifndef WITHOUT_VP_BG_ACCESS
  VP_BG_BASE *base;
#endif
  DBUG_ENTER("ha_vp::search_by_pk_for_update");
  table2 = part_tables[table_idx].table;
  key_idx = share->correspond_pk[table_idx]->key_idx;

#ifndef WITHOUT_VP_BG_ACCESS
  if (
    bgu_mode &&
    (table2->file->ha_table_flags() & VP_CAN_BG_UPDATE)
  ) {
    base = &bg_base[table_idx];
    vp_key_copy->table_key_different = base->table_key_different;
    if (
      (error_num = create_bg_thread(base)) ||
      (error_num = search_by_pk(table_idx, record_idx, vp_key_copy, ptr_diff,
        (uchar **) &base->table_key))
    )
      DBUG_RETURN(error_num);
    base->tgt_key_part_map = vp_key_copy->tgt_key_part_map;
    base->key_idx = key_idx;
    base->record_idx = record_idx;
    base->bg_command = VP_BG_COMMAND_UPDATE_SELECT;
    bg_kick(base);
  } else {
#endif
    vp_key_copy->table_key_different = table_key_different;
    if ((error_num = search_by_pk(table_idx, record_idx, vp_key_copy, ptr_diff,
      &table_key)))
      DBUG_RETURN(error_num);

    if (inited == INDEX)
    {
      if (!vp_bit_is_set(key_inited_tables, table_idx))
      {
        DBUG_PRINT("info",("vp call child[%d] ha_index_init",
          table_idx));
        DBUG_PRINT("info",("vp INDEX child_table=%p", table2));
        vp_set_bit(key_inited_tables, table_idx);
        if ((error_num =
          table2->file->ha_index_init(key_idx, TRUE)))
          DBUG_RETURN(error_num);
      }
    } else if (inited == RND) {
      if (!vp_bit_is_set(rnd_inited_tables, table_idx))
      {
        DBUG_PRINT("info",("vp call child[%d] ha_index_init",
          table_idx));
        DBUG_PRINT("info",("vp RND child_table=%p", table2));
        vp_set_bit(rnd_inited_tables, table_idx);
        if ((error_num =
          table2->file->ha_index_init(key_idx, TRUE)))
          DBUG_RETURN(error_num);
      }
    } else {
      DBUG_PRINT("info",("vp call child[%d] ha_index_init",
        table_idx));
      DBUG_PRINT("info",("vp NONE child_table=%p", table2));
      if ((error_num =
        table2->file->ha_index_init(key_idx, TRUE)))
      DBUG_RETURN(error_num);
    }
    if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
      !(error_num = table2->file->ha_index_read_map(
        table2->record[0], table_key,
        vp_key_copy->tgt_key_part_map, HA_READ_KEY_EXACT)) &&
#else
      !(error_num = table2->file->index_read_map(
        table2->record[0], table_key,
        vp_key_copy->tgt_key_part_map, HA_READ_KEY_EXACT)) &&
#endif
      record_idx
    ) {
      store_record(table2, record[1]);
    }
    if (inited == NONE)
    {
      DBUG_PRINT("info",("vp NONE ha_index_end"));
      if ((error_num2 = table2->file->ha_index_end()))
        DBUG_RETURN(error_num2);
    }
#ifndef WITHOUT_VP_BG_ACCESS
  }
#endif
  DBUG_RETURN(error_num);
}

int ha_vp::create_child_bitmap_buff(
) {
  int roop_count;
  uchar *child_column_bitmap;
  TABLE_SHARE *part_table_share;
  DBUG_ENTER("ha_vp::create_child_bitmap_buff");
  child_column_bitmap_size = 0;
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    part_table_share = part_tables[roop_count].table->s;
    child_column_bitmap_size += part_table_share->column_bitmap_size;
  }
  if (!(ins_child_bitmaps[0] = (uchar **)
    my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
      &ins_child_bitmaps[0],
      sizeof(uchar *) * share->table_count,
      &ins_child_bitmaps[1],
      sizeof(uchar *) * share->table_count,
      &upd_child_bitmaps[0],
      sizeof(uchar *) * share->table_count,
      &upd_child_bitmaps[1],
      sizeof(uchar *) * share->table_count,
      &del_child_bitmaps[0],
      sizeof(uchar *) * share->table_count,
      &del_child_bitmaps[1],
      sizeof(uchar *) * share->table_count,
      &add_from_child_bitmaps[0],
      sizeof(uchar *) * share->table_count,
      &add_from_child_bitmaps[1],
      sizeof(uchar *) * share->table_count,
      &sel_key_init_child_bitmaps[0],
      sizeof(uchar *) * share->table_count,
      &sel_key_init_child_bitmaps[1],
      sizeof(uchar *) * share->table_count,
      &sel_key_child_bitmaps[0],
      sizeof(uchar *) * share->table_count,
      &sel_key_child_bitmaps[1],
      sizeof(uchar *) * share->table_count,
      &sel_rnd_child_bitmaps[0],
      sizeof(uchar *) * share->table_count,
      &sel_rnd_child_bitmaps[1],
      sizeof(uchar *) * share->table_count,
      &child_column_bitmap,
      sizeof(uchar) * child_column_bitmap_size * 14,
      NullS))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    part_table_share = part_tables[roop_count].table->s;
    ins_child_bitmaps[0][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    ins_child_bitmaps[1][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    upd_child_bitmaps[0][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    upd_child_bitmaps[1][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    del_child_bitmaps[0][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    del_child_bitmaps[1][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    add_from_child_bitmaps[0][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    add_from_child_bitmaps[1][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    sel_key_init_child_bitmaps[0][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    sel_key_init_child_bitmaps[1][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    sel_key_child_bitmaps[0][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    sel_key_child_bitmaps[1][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    sel_rnd_child_bitmaps[0][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
    sel_rnd_child_bitmaps[1][roop_count] = child_column_bitmap;
    child_column_bitmap += part_table_share->column_bitmap_size;
  }
  DBUG_RETURN(0);
}

void ha_vp::free_child_bitmap_buff(
) {
  DBUG_ENTER("ha_vp::free_child_bitmap_buff");
  if (ins_child_bitmaps[0])
  {
    vp_my_free(ins_child_bitmaps[0], MYF(0));
    ins_child_bitmaps[0] = NULL;
  }
  DBUG_VOID_RETURN;
}

bool ha_vp::get_added_bitmap(
  uchar *added_bitmap,
  const uchar *current_bitmap,
  const uchar *pre_bitmap
) {
  int roop_count;
  bool added = FALSE;
  DBUG_ENTER("ha_vp::get_added_bitmap");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp current_bitmap[%d]=%u", roop_count,
      current_bitmap[roop_count]));
    DBUG_PRINT("info",("vp pre_bitmap[%d]=%u", roop_count,
      pre_bitmap[roop_count]));
    added_bitmap[roop_count] = current_bitmap[roop_count] &
      (current_bitmap[roop_count] ^ pre_bitmap[roop_count]);
    if (added_bitmap[roop_count])
      added = TRUE;
    DBUG_PRINT("info",("vp added_bitmap[%d]=%u", roop_count,
      added_bitmap[roop_count]));
  }
  DBUG_RETURN(added);
}

void ha_vp::add_child_bitmap(
  int table_idx,
  uchar *bitmap
) {
  int roop_count, field_idx;
  uchar *tmp_r_bitmap;
  int *correspond_columns_p =
    &share->correspond_columns_p[table_idx * table_share->fields];
  TABLE *child_table = part_tables[table_idx].table;
  DBUG_ENTER("ha_vp::prune_child_bitmap");
  DBUG_PRINT("info",("vp table_idx=%d", table_idx));
  tmp_r_bitmap = (uchar *) child_table->read_set->bitmap;

#ifndef DBUG_OFF
  int bitmap_size = (child_table->s->fields + 7) / 8;
  for (roop_count = 0; roop_count < bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp r_bitmap is %d-%u", roop_count,
      tmp_r_bitmap[roop_count]));
  }
#endif
  for (roop_count = 0; roop_count < (int) table_share->fields; roop_count++)
  {
    if (vp_bit_is_set(bitmap, roop_count))
    {
      if ((field_idx = correspond_columns_p[roop_count]) < MAX_FIELDS)
      {
        vp_set_bit(tmp_r_bitmap, field_idx);
        vp_clear_bit(bitmap, roop_count);
      }
    }
  }
#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp r_bitmap is %d-%u", roop_count,
      tmp_r_bitmap[roop_count]));
  }
#endif
  DBUG_VOID_RETURN;
}

void ha_vp::prune_child_bitmap(
  int table_idx
) {
  int roop_count, field_idx;
  uchar *tmp_r_bitmap, *tmp_w_bitmap, *pk_bitmap, *r_bitmap, *w_bitmap,
    *idx_bitmap;
  int *correspond_columns_c =
    share->correspond_columns_c_ptr[table_idx];
  bool correspond_flag = FALSE;
  TABLE *child_table = part_tables[table_idx].table;
  DBUG_ENTER("ha_vp::prune_child_bitmap");
  DBUG_PRINT("info",("vp table_idx=%d", table_idx));
  w_bitmap = (uchar *) idx_write_bitmap;
  tmp_w_bitmap = (uchar *) child_table->write_set->bitmap;
  r_bitmap = (uchar *) idx_read_bitmap;
  tmp_r_bitmap = (uchar *) child_table->read_set->bitmap;

  if (
    update_request &&
    vp_bit_is_set(share->need_full_col_for_update, table_idx)
  ) {
    /* use_full_column = TRUE; */
    DBUG_VOID_RETURN;
  }

  if (
    table_idx == child_table_idx &&
    (!single_table || update_request || extra_use_cmp_ref || is_clone)
  )
    pk_bitmap = share->keys[table_share->primary_key].columns_bit;
  else
    pk_bitmap = NULL;

  if (active_index < MAX_KEY)
    idx_bitmap = share->keys[active_index].columns_bit;
  else
    idx_bitmap = NULL;

#ifndef DBUG_OFF
  int bitmap_size = (child_table->s->fields + 7) / 8;
  for (roop_count = 0; roop_count < bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp w_bitmap is %d-%u", roop_count,
      tmp_w_bitmap[roop_count]));
    DBUG_PRINT("info",("vp r_bitmap is %d-%u", roop_count,
      tmp_r_bitmap[roop_count]));
  }
#endif
  for (roop_count = 0; roop_count < (int) child_table->s->fields; roop_count++)
  {
    if (vp_bit_is_set(tmp_w_bitmap, roop_count))
    {
      field_idx = correspond_columns_c[roop_count];
      if (!vp_bit_is_set(w_bitmap, field_idx))
      {
        DBUG_PRINT("info",("vp clear tmp_w_bitmap %d-%d", table_idx, roop_count));
        vp_clear_bit(tmp_w_bitmap, roop_count);
      } else
        correspond_flag = TRUE;
    }
    if (vp_bit_is_set(tmp_r_bitmap, roop_count))
    {
      field_idx = correspond_columns_c[roop_count];
      if (!vp_bit_is_set(r_bitmap, field_idx))
      {
        if (
          (!idx_bitmap || !vp_bit_is_set(idx_bitmap, field_idx)) &&
          (!pk_bitmap || !vp_bit_is_set(pk_bitmap, field_idx))
        ) {
          DBUG_PRINT("info",("vp clear tmp_r_bitmap %d-%d", table_idx, roop_count));
          vp_clear_bit(tmp_r_bitmap, roop_count);
        }
      } else
        correspond_flag = TRUE;
    }
  }

  if (!correspond_flag && table_idx != child_table_idx)
  {
    DBUG_PRINT("info",("vp clear use_tables flag for %d", table_idx));
    vp_clear_bit(use_tables, table_idx);
    vp_set_bit(pruned_tables, table_idx);
    pruned = TRUE;
  }
#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp w_bitmap is %d-%u", roop_count,
      tmp_w_bitmap[roop_count]));
    DBUG_PRINT("info",("vp r_bitmap is %d-%u", roop_count,
      tmp_r_bitmap[roop_count]));
  }
#endif
  DBUG_VOID_RETURN;
}

void ha_vp::prune_child()
{
  int roop_count;
  TABLE *child_table;
  DBUG_ENTER("ha_vp::prune_child");
  DBUG_PRINT("info",("vp init_sel_key_bitmap=%s",
    init_sel_key_bitmap ? "TRUE" : "FALSE"));

#ifdef HA_CAN_BULK_ACCESS
  if (
    bulk_access_started ||
    (bulk_access_executing && bulk_access_info_exec_tgt->called)
  ) {
    VP_BULK_ACCESS_INFO *bulk_access_info;
    if (bulk_access_pre_called)
      bulk_access_info = bulk_access_info_current;
    else
      bulk_access_info = bulk_access_info_exec_tgt;

    if (bulk_access_info->init_sel_key_bitmap)
    {
      memcpy(use_tables, bulk_access_info->sel_key_use_tables,
        sizeof(uchar) * share->use_tables_size);
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (vp_bit_is_set(use_tables, roop_count))
        {
          child_table = part_tables[roop_count].table;
          memcpy(child_table->read_set->bitmap,
            bulk_access_info->sel_key_child_bitmaps[0][roop_count],
            child_table->s->column_bitmap_size);
          memcpy(child_table->write_set->bitmap,
            bulk_access_info->sel_key_child_bitmaps[1][roop_count],
            child_table->s->column_bitmap_size);
        }
      }
    } else {
      if (get_added_bitmap(work_bitmap3,
        (const uchar *) table->read_set->bitmap,
        (const uchar *) idx_init_read_bitmap))
      {
        for (roop_count = 0; roop_count < share->table_count; roop_count++)
        {
          if (vp_bit_is_set(use_tables, roop_count))
          {
            add_child_bitmap(roop_count, work_bitmap3);
          }
        }
#ifndef DBUG_OFF
        for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        {
          DBUG_PRINT("info",("vp added_bitmap[%d]=%u", roop_count,
            work_bitmap3[roop_count]));
        }
#endif
      }
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (vp_bit_is_set(use_tables, roop_count))
        {
          prune_child_bitmap(roop_count);
          if (vp_bit_is_set(use_tables, roop_count))
          {
            child_table = part_tables[roop_count].table;
            memcpy(bulk_access_info->sel_key_child_bitmaps[0][roop_count],
              child_table->read_set->bitmap,
              child_table->s->column_bitmap_size);
            memcpy(bulk_access_info->sel_key_child_bitmaps[1][roop_count],
              child_table->write_set->bitmap,
              child_table->s->column_bitmap_size);
          }
        }
      }
      memcpy(bulk_access_info->sel_key_use_tables, use_tables,
        sizeof(uchar) * share->use_tables_size);
      bulk_access_info->init_sel_key_bitmap = TRUE;
    }
  } else {
#endif
    if (init_sel_key_bitmap)
    {
      memcpy(use_tables, sel_key_use_tables,
        sizeof(uchar) * share->use_tables_size);
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (vp_bit_is_set(use_tables, roop_count))
        {
          child_table = part_tables[roop_count].table;
          memcpy(child_table->read_set->bitmap,
            sel_key_child_bitmaps[0][roop_count],
            child_table->s->column_bitmap_size);
          memcpy(child_table->write_set->bitmap,
            sel_key_child_bitmaps[1][roop_count],
            child_table->s->column_bitmap_size);
        }
      }
    } else {
      if (get_added_bitmap(work_bitmap3,
        (const uchar *) table->read_set->bitmap,
        (const uchar *) idx_init_read_bitmap))
      {
        for (roop_count = 0; roop_count < share->table_count; roop_count++)
        {
          if (vp_bit_is_set(use_tables, roop_count))
          {
            add_child_bitmap(roop_count, work_bitmap3);
          }
        }
#ifndef DBUG_OFF
        for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        {
          DBUG_PRINT("info",("vp added_bitmap[%d]=%u", roop_count,
            work_bitmap3[roop_count]));
        }
#endif
      }
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (vp_bit_is_set(use_tables, roop_count))
        {
          prune_child_bitmap(roop_count);
          if (vp_bit_is_set(use_tables, roop_count))
          {
            child_table = part_tables[roop_count].table;
            memcpy(sel_key_child_bitmaps[0][roop_count],
              child_table->read_set->bitmap,
              child_table->s->column_bitmap_size);
            memcpy(sel_key_child_bitmaps[1][roop_count],
              child_table->write_set->bitmap,
              child_table->s->column_bitmap_size);
          }
        }
      }
      memcpy(sel_key_use_tables, use_tables,
        sizeof(uchar) * share->use_tables_size);
      init_sel_key_bitmap = TRUE;
    }
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  set_child_pt_bitmap();
  DBUG_VOID_RETURN;
}

int ha_vp::set_rnd_bitmap()
{
  int error_num, roop_count;
  TABLE *child_table;
#ifdef HA_CAN_BULK_ACCESS
  VP_BULK_ACCESS_INFO *bulk_access_info = NULL;
#endif
  DBUG_ENTER("ha_vp::set_rnd_bitmap");
#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp read_bitmap is %d-%u", roop_count,
      table->read_set->bitmap[roop_count]));
    DBUG_PRINT("info",("vp write_bitmap is %d-%u", roop_count,
      table->write_set->bitmap[roop_count]));
  }
#endif

#ifdef HA_CAN_BULK_ACCESS
  if (
    bulk_access_started ||
    (bulk_access_executing && bulk_access_info_exec_tgt->called)
  ) {
    if (bulk_access_pre_called)
      bulk_access_info = bulk_access_info_current;
    else
      bulk_access_info = bulk_access_info_exec_tgt;
  }
#endif

  if (rnd_scan)
  {
    if (
#ifdef HA_CAN_BULK_ACCESS
      (
        !bulk_access_started &&
        !(bulk_access_executing && bulk_access_info_exec_tgt->called) &&
#endif
        !init_sel_rnd_bitmap
#ifdef HA_CAN_BULK_ACCESS
      ) ||
      (
        bulk_access_started && bulk_access_pre_called &&
        !bulk_access_info->init_sel_rnd_bitmap
      )
#endif
    ) {
      memset(use_tables, 0, sizeof(uchar) * share->use_tables_size);
      child_keyread = FALSE;
      single_table = FALSE;
      set_used_table = FALSE;
      if (
        share->zero_record_update_mode &&
        (lock_mode > 0 || lock_type_ext == F_WRLCK) &&
        (sql_command == SQLCOM_UPDATE || sql_command == SQLCOM_UPDATE_MULTI)
      ) {
        for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
        {
          rnd_init_read_bitmap[roop_count] |=
            share->cpy_clm_bitmap[roop_count];
          rnd_init_write_bitmap[roop_count] |=
            share->cpy_clm_bitmap[roop_count];
        }
      }
      if (
        (sql_command == SQLCOM_DELETE || sql_command == SQLCOM_DELETE_MULTI) &&
        memcmp(rnd_init_read_bitmap, rnd_read_bitmap,
          sizeof(uchar) * share->bitmap_size) < 0
      ) {
        memcpy(work_bitmap3, rnd_read_bitmap,
          sizeof(uchar) * share->bitmap_size);
        memcpy(work_bitmap4, rnd_write_bitmap,
          sizeof(uchar) * share->bitmap_size);
      } else {
        memcpy(work_bitmap3, rnd_init_read_bitmap,
          sizeof(uchar) * share->bitmap_size);
        memcpy(work_bitmap4, rnd_init_write_bitmap,
          sizeof(uchar) * share->bitmap_size);
      }

      if (
        (error_num = choose_child_ft_tables(work_bitmap3, work_bitmap4)) ||
        (
          !ft_correspond_flag &&
          (error_num = choose_child_tables(work_bitmap3, work_bitmap4))
        )
      ) {
        DBUG_RETURN(error_num);
      }
      set_child_pt_bitmap();
    }
  }

#ifdef HA_CAN_BULK_ACCESS
  if (
    bulk_access_started ||
    (bulk_access_executing && bulk_access_info_exec_tgt->called)
  ) {
    if (
      bulk_access_pre_called &&
      !bulk_access_info->init_sel_rnd_bitmap
    ) {
      if (rnd_scan)
        memcpy(bulk_access_info->sel_rnd_use_tables, use_tables,
          sizeof(uchar) * share->use_tables_size);
      else {
        memcpy(bulk_access_info->sel_rnd_use_tables,
          bulk_access_info->sel_key_init_use_tables,
          sizeof(uchar) * share->use_tables_size);
        memcpy(use_tables, bulk_access_info->sel_rnd_use_tables,
          sizeof(uchar) * share->use_tables_size);
      }
    } else if (cb_state != CB_SEL_RND)
      memcpy(use_tables, bulk_access_info->sel_rnd_use_tables,
        sizeof(uchar) * share->use_tables_size);
  } else {
#endif
    if (!init_sel_rnd_bitmap)
    {
      if (rnd_scan)
        memcpy(sel_rnd_use_tables, use_tables,
          sizeof(uchar) * share->use_tables_size);
      else {
        memcpy(sel_rnd_use_tables, sel_key_init_use_tables,
          sizeof(uchar) * share->use_tables_size);
        memcpy(use_tables, sel_rnd_use_tables,
          sizeof(uchar) * share->use_tables_size);
      }
    } else if (cb_state != CB_SEL_RND)
      memcpy(use_tables, sel_rnd_use_tables,
        sizeof(uchar) * share->use_tables_size);
#ifdef HA_CAN_BULK_ACCESS
  }
#endif

#ifdef HA_CAN_BULK_ACCESS
  if (
    bulk_access_started ||
    (bulk_access_executing && bulk_access_info_exec_tgt->called)
  ) {
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (vp_bit_is_set(use_tables, roop_count))
      {
        DBUG_PRINT("info",("vp table_count=%d", roop_count));
        if (bulk_access_info->init_sel_rnd_bitmap)
        {
          if (cb_state != CB_SEL_RND)
          {
            child_table = part_tables[roop_count].table;
            memcpy(child_table->read_set->bitmap,
              bulk_access_info->sel_rnd_child_bitmaps[0][roop_count],
              child_table->s->column_bitmap_size);
            memcpy(child_table->write_set->bitmap,
              bulk_access_info->sel_rnd_child_bitmaps[1][roop_count],
              child_table->s->column_bitmap_size);
          }
        } else {
          child_table = part_tables[roop_count].table;
          if (rnd_scan)
          {
            memcpy(bulk_access_info->sel_rnd_child_bitmaps[0][roop_count],
              child_table->read_set->bitmap,
              child_table->s->column_bitmap_size);
            memcpy(bulk_access_info->sel_rnd_child_bitmaps[1][roop_count],
              child_table->write_set->bitmap,
              child_table->s->column_bitmap_size);
          } else {
            memcpy(bulk_access_info->sel_rnd_child_bitmaps[0][roop_count],
              bulk_access_info->sel_key_init_child_bitmaps[0][roop_count],
              child_table->s->column_bitmap_size);
            memcpy(bulk_access_info->sel_rnd_child_bitmaps[1][roop_count],
              bulk_access_info->sel_key_init_child_bitmaps[1][roop_count],
              child_table->s->column_bitmap_size);
            memcpy(child_table->read_set->bitmap,
              bulk_access_info->sel_rnd_child_bitmaps[0][roop_count],
              child_table->s->column_bitmap_size);
            memcpy(child_table->write_set->bitmap,
              bulk_access_info->sel_rnd_child_bitmaps[1][roop_count],
              child_table->s->column_bitmap_size);
          }
        }
        if (
          (!rnd_scan && !vp_bit_is_set(pruned_tables, roop_count)) ||
          roop_count == child_table_idx
        ) {
          if (!ft_inited || !vp_bit_is_set(ft_inited_tables, roop_count))
          {
            DBUG_PRINT("info",("vp child_table=%p",
              part_tables[roop_count].table));
            vp_set_bit(rnd_inited_tables, roop_count);
            if (bulk_access_pre_called)
            {
              if ((error_num =
                part_tables[roop_count].table->file->
                  ha_pre_rnd_init(rnd_scan)))
                DBUG_RETURN(error_num);
            } else {
              if ((error_num =
                part_tables[roop_count].table->file->ha_rnd_init(rnd_scan)))
                DBUG_RETURN(error_num);
            }
          } else {
            DBUG_PRINT("info",("vp no rnd init"));
          }
        } else if (!bulk_access_pre_called || update_request) {
          DBUG_PRINT("info",("vp child_table=%p",
            part_tables[roop_count].table));
          vp_set_bit(rnd_inited_tables, roop_count);
          if (bulk_access_pre_called)
          {
            if ((error_num =
              part_tables[roop_count].table->file->ha_pre_index_init(
                share->correspond_pk[roop_count]->key_idx, FALSE)))
              DBUG_RETURN(error_num);
          } else {
            DBUG_PRINT("info",("vp call child[%d] ha_index_init",
              roop_count));
            if ((error_num =
              part_tables[roop_count].table->file->ha_index_init(
                share->correspond_pk[roop_count]->key_idx, FALSE)))
              DBUG_RETURN(error_num);
          }
        }
      }
    }
    bulk_access_info->init_sel_rnd_bitmap = TRUE;
  } else {
#endif
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (vp_bit_is_set(use_tables, roop_count))
      {
        DBUG_PRINT("info",("vp table_count=%d", roop_count));
        if (init_sel_rnd_bitmap)
        {
          if (cb_state != CB_SEL_RND)
          {
            child_table = part_tables[roop_count].table;
            memcpy(child_table->read_set->bitmap,
              sel_rnd_child_bitmaps[0][roop_count],
              child_table->s->column_bitmap_size);
            memcpy(child_table->write_set->bitmap,
              sel_rnd_child_bitmaps[1][roop_count],
              child_table->s->column_bitmap_size);
          }
        } else {
          child_table = part_tables[roop_count].table;
          if (rnd_scan)
          {
            memcpy(sel_rnd_child_bitmaps[0][roop_count],
              child_table->read_set->bitmap,
              child_table->s->column_bitmap_size);
            memcpy(sel_rnd_child_bitmaps[1][roop_count],
              child_table->write_set->bitmap,
              child_table->s->column_bitmap_size);
          } else {
            memcpy(sel_rnd_child_bitmaps[0][roop_count],
              sel_key_init_child_bitmaps[0][roop_count],
              child_table->s->column_bitmap_size);
            memcpy(sel_rnd_child_bitmaps[1][roop_count],
              sel_key_init_child_bitmaps[1][roop_count],
              child_table->s->column_bitmap_size);
            memcpy(child_table->read_set->bitmap,
              sel_rnd_child_bitmaps[0][roop_count],
              child_table->s->column_bitmap_size);
            memcpy(child_table->write_set->bitmap,
              sel_rnd_child_bitmaps[1][roop_count],
              child_table->s->column_bitmap_size);
          }
        }
        if (
          (!rnd_scan && !vp_bit_is_set(pruned_tables, roop_count)) ||
          roop_count == child_table_idx
        ) {
          if (!ft_inited || !vp_bit_is_set(ft_inited_tables, roop_count))
          {
            DBUG_PRINT("info",("vp child_table=%p",
              part_tables[roop_count].table));
            vp_set_bit(rnd_inited_tables, roop_count);
            if ((error_num =
              part_tables[roop_count].table->file->ha_rnd_init(rnd_scan)))
              DBUG_RETURN(error_num);
          } else {
            DBUG_PRINT("info",("vp no rnd init"));
          }
        } else {
          DBUG_PRINT("info",("vp call child[%d] ha_index_init",
            roop_count));
          DBUG_PRINT("info",("vp child_table=%p",
            part_tables[roop_count].table));
          vp_set_bit(rnd_inited_tables, roop_count);
          if ((error_num =
            part_tables[roop_count].table->file->ha_index_init(
              share->correspond_pk[roop_count]->key_idx, FALSE)))
            DBUG_RETURN(error_num);
        }
      }
    }
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  init_sel_rnd_bitmap = TRUE;
  DBUG_RETURN(0);
}

void ha_vp::reset_rnd_bitmap()
{
  int roop_count;
  TABLE *child_table;
#ifdef HA_CAN_BULK_ACCESS
  VP_BULK_ACCESS_INFO *bulk_access_info;
#endif
  DBUG_ENTER("ha_vp::reset_rnd_bitmap");
#ifdef HA_CAN_BULK_ACCESS
  if (
    bulk_access_started ||
    (bulk_access_executing && bulk_access_info_exec_tgt->called)
  ) {
    if (bulk_access_pre_called)
      bulk_access_info = bulk_access_info_current;
    else
      bulk_access_info = bulk_access_info_exec_tgt;

    memcpy(use_tables, bulk_access_info->sel_rnd_use_tables,
      sizeof(uchar) * share->use_tables_size);
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (vp_bit_is_set(use_tables, roop_count))
      {
        child_table = part_tables[roop_count].table;
        memcpy(child_table->read_set->bitmap,
          bulk_access_info->sel_rnd_child_bitmaps[0][roop_count],
          child_table->s->column_bitmap_size);
        memcpy(child_table->write_set->bitmap,
          bulk_access_info->sel_rnd_child_bitmaps[1][roop_count],
          child_table->s->column_bitmap_size);
      }
    }
  } else {
#endif
    memcpy(use_tables, sel_rnd_use_tables,
      sizeof(uchar) * share->use_tables_size);
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (vp_bit_is_set(use_tables, roop_count))
      {
        child_table = part_tables[roop_count].table;
        memcpy(child_table->read_set->bitmap,
          sel_rnd_child_bitmaps[0][roop_count],
          child_table->s->column_bitmap_size);
        memcpy(child_table->write_set->bitmap,
          sel_rnd_child_bitmaps[1][roop_count],
          child_table->s->column_bitmap_size);
      }
    }
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  DBUG_VOID_RETURN;
}

int ha_vp::set_rnd_bitmap_from_another(
  ha_vp *another_vp
) {
  int error_num, roop_count;
  TABLE *child_table;
  DBUG_ENTER("ha_vp::set_rnd_bitmap_from_another");
  if (inited == NONE)
    memset(rnd_inited_tables, 0, sizeof(uchar) * share->use_tables_size);

  memcpy(use_tables, another_vp->sel_key_init_use_tables,
    sizeof(uchar) * share->use_tables_size);
  child_table_idx = another_vp->child_table_idx;

  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(use_tables, roop_count))
    {
      child_table = part_tables[roop_count].table;
      memcpy(child_table->read_set->bitmap,
        another_vp->sel_key_init_child_bitmaps[0][roop_count],
        child_table->s->column_bitmap_size);
      memcpy(child_table->write_set->bitmap,
        another_vp->sel_key_init_child_bitmaps[1][roop_count],
        child_table->s->column_bitmap_size);
      if (!vp_bit_is_set(rnd_inited_tables, roop_count))
      {
        DBUG_PRINT("info",("vp table_count=%d", roop_count));
        DBUG_PRINT("info",("vp child_table=%p", child_table));
        vp_set_bit(rnd_inited_tables, roop_count);
        if ((error_num =
          child_table->file->ha_rnd_init(rnd_scan)))
          DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int ha_vp::open_item_type(
  Item *item,
  int table_idx
) {
  DBUG_ENTER("ha_vp::open_item_type");
  DBUG_PRINT("info",("vp COND type=%d", item->type()));
  switch (item->type())
  {
    case Item::FUNC_ITEM:
      DBUG_RETURN(open_item_func((Item_func *) item, table_idx));
    case Item::COND_ITEM:
      DBUG_RETURN(open_item_cond((Item_cond *) item, table_idx));
    case Item::FIELD_ITEM:
      DBUG_RETURN(open_item_field((Item_field *) item, table_idx));
    case Item::REF_ITEM:
      DBUG_RETURN(open_item_ref((Item_ref *) item, table_idx));
    case Item::ROW_ITEM:
      DBUG_RETURN(open_item_row((Item_row *) item, table_idx));
    case Item::SUBSELECT_ITEM:
    case Item::TRIGGER_FIELD_ITEM:
      DBUG_PRINT("info",("vp return = %d", ER_VP_COND_SKIP_NUM));
      DBUG_RETURN(ER_VP_COND_SKIP_NUM);
    default:
      break;
  }
  DBUG_RETURN(0);
}

int ha_vp::open_item_cond(
  Item_cond *item_cond,
  int table_idx
) {
  int error_num = 0;
  List_iterator_fast<Item> lif(*(item_cond->argument_list()));
  Item *item;
  uint restart_pos;
  DBUG_ENTER("ha_vp::open_item_cond");

restart_first:
  if ((item = lif++))
  {
    restart_pos = child_cond_count[table_idx];
    if ((error_num = open_item_type(item, table_idx)))
    {
      if (error_num == ER_VP_COND_SKIP_NUM)
      {
        DBUG_PRINT("info",("vp COND skip"));
        child_cond_count[table_idx] = restart_pos;
        goto restart_first;
      }
      DBUG_RETURN(error_num);
    }
  }
  if (error_num)
    DBUG_RETURN(error_num);
  while ((item = lif++))
  {
    restart_pos = child_cond_count[table_idx];
    if ((error_num = open_item_type(item, table_idx)))
    {
      if (error_num == ER_VP_COND_SKIP_NUM)
      {
        DBUG_PRINT("info",("vp COND skip"));
        child_cond_count[table_idx] = restart_pos;
      } else
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_vp::open_item_func(
  Item_func *item_func,
  int table_idx
) {
  int error_num;
  Item *item, **item_list = item_func->arguments();
  uint roop_count, item_count = item_func->argument_count();
  DBUG_ENTER("ha_vp::open_item_func");
  DBUG_PRINT("info",("vp functype = %d", item_func->functype()));
  switch (item_func->functype())
  {
#ifdef VP_ITEM_FUNC_HAS_XOR_FUNC
#else
    case Item_func::COND_XOR_FUNC:
      DBUG_RETURN(
        open_item_cond((Item_cond *) item_func, table_idx));
#endif
    case Item_func::TRIG_COND_FUNC:
      DBUG_PRINT("info",("vp return = %d", ER_VP_COND_SKIP_NUM));
      DBUG_RETURN(ER_VP_COND_SKIP_NUM);
/*  memo
    case Item_func::ISNULL_FUNC:
    case Item_func::ISNOTNULL_FUNC:
    case Item_func::UNKNOWN_FUNC:
    case Item_func::NOW_FUNC:
    case Item_func::NOT_FUNC:
    case Item_func::NEG_FUNC:
    case Item_func::IN_FUNC:
    case Item_func::BETWEEN:
    case Item_func::UDF_FUNC:
    case Item_func::EQ_FUNC:
    case Item_func::EQUAL_FUNC:
    case Item_func::NE_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GE_FUNC:
    case Item_func::GT_FUNC:
#ifdef VP_ITEM_FUNC_HAS_XOR_FUNC
    case Item_func::XOR_FUNC:
#endif
*/
    default:
      break;
  }
  if (item_count)
  {
    for (roop_count = 0; roop_count < item_count; roop_count++)
    {
      item = item_list[roop_count];
      if ((error_num = open_item_type(item, table_idx)))
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int ha_vp::open_item_ident(
  Item_ident *item_ident,
  int table_idx
) {
  DBUG_ENTER("ha_vp::open_item_ident");
  DBUG_RETURN(0);
}

int ha_vp::open_item_field(
  Item_field *item_field,
  int table_idx
) {
  Field *field = item_field->field;
  TABLE *child_table = part_tables[table_idx].table;
  handler *file = child_table->file;
  DBUG_ENTER("ha_vp::open_item_field");
  if (field)
  {
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
    if (file->set_top_table_fields)
    {
      if (field->table != file->top_table)
      {
        DBUG_PRINT("info",("vp return = %d", ER_VP_COND_SKIP_NUM));
        DBUG_RETURN(ER_VP_COND_SKIP_NUM);
      }
      if (!(field = file->top_table_field[field->field_index]))
      {
        DBUG_PRINT("info",("vp return = %d", ER_VP_COND_SKIP_NUM));
        DBUG_RETURN(ER_VP_COND_SKIP_NUM);
      }
    } else {
#endif
      if (field->table != child_table)
      {
        DBUG_PRINT("info",("vp return = %d", ER_VP_COND_SKIP_NUM));
        DBUG_RETURN(ER_VP_COND_SKIP_NUM);
      }
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
    }
#endif
    child_cond_count[table_idx]++;
    DBUG_PRINT("info",("vp child_cond_count[%d] = %u", table_idx,
      child_cond_count[table_idx]));
    DBUG_RETURN(0);
  }
  DBUG_RETURN(open_item_ident(
    (Item_ident *) item_field, table_idx));
}

int ha_vp::open_item_ref(
  Item_ref *item_ref,
  int table_idx
) {
  DBUG_ENTER("ha_vp::open_item_ref");
  if (item_ref->ref)
  {
    if (
      (*(item_ref->ref))->type() != Item::CACHE_ITEM &&
      item_ref->ref_type() != Item_ref::VIEW_REF &&
      !item_ref->table_name &&
      VP_item_name_str(item_ref) &&
      item_ref->alias_name_used
    )
      DBUG_RETURN(0);
    DBUG_RETURN(open_item_type(*(item_ref->ref), table_idx));
  }
  DBUG_RETURN(open_item_ident((Item_ident *) item_ref, table_idx));
}

int ha_vp::open_item_row(
  Item_row *item_row,
  int table_idx
) {
  int error_num;
  uint roop_count, cols = item_row->cols();
  Item *item;
  DBUG_ENTER("ha_vp::open_item_row");
  for (roop_count = 0; roop_count < cols; roop_count++)
  {
    item = item_row->element_index(roop_count);
    if ((error_num = open_item_type(item, table_idx)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int ha_vp::count_condition(
  int table_idx
) {
  int error_num;
  uint restart_pos;
  VP_CONDITION *tmp_cond = condition;
  DBUG_ENTER("ha_vp::count_condition");
  while (tmp_cond)
  {
    restart_pos = child_cond_count[table_idx];
    if ((error_num = open_item_type((Item *) tmp_cond->cond, table_idx)))
    {
      if (error_num == ER_VP_COND_SKIP_NUM)
      {
        DBUG_PRINT("info",("vp COND skip"));
        child_cond_count[table_idx] = restart_pos;
      } else
        DBUG_RETURN(error_num);
    }
    tmp_cond = tmp_cond->next;
  }
  DBUG_RETURN(0);
}

int ha_vp::create_bg_thread(
  VP_BG_BASE *base
) {
  int error_num;
  DBUG_ENTER("ha_vp::create_bg_thread");
  if (!base->bg_init)
  {
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&base->bg_sync_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(vp_key_mutex_bg_sync,
      &base->bg_sync_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_mutex_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&base->bg_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(vp_key_mutex_bg,
      &base->bg_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_mutex_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&base->bg_sync_cond, NULL))
#else
    if (mysql_cond_init(vp_key_cond_bg_sync,
      &base->bg_sync_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&base->bg_cond, NULL))
#else
    if (mysql_cond_init(vp_key_cond_bg,
      &base->bg_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
    pthread_mutex_lock(&base->bg_mutex);
#if MYSQL_VERSION_ID < 50500
    if (pthread_create(&base->bg_thread, &vp_pt_attr,
      vp_bg_action, (void *) base)
    )
#else
    if (mysql_thread_create(vp_key_thd_bg, &base->bg_thread,
      &vp_pt_attr, vp_bg_action, (void *) base)
    )
#endif
    {
      pthread_mutex_unlock(&base->bg_mutex);
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
    pthread_mutex_lock(&base->bg_sync_mutex);
    pthread_cond_signal(&base->bg_cond);
    pthread_mutex_unlock(&base->bg_mutex);
    pthread_cond_wait(&base->bg_sync_cond, &base->bg_sync_mutex);
    pthread_mutex_unlock(&base->bg_sync_mutex);
    if (!base->bg_init)
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&base->bg_cond);
error_cond_init:
  pthread_cond_destroy(&base->bg_sync_cond);
error_sync_cond_init:
  pthread_mutex_destroy(&base->bg_mutex);
error_mutex_init:
  pthread_mutex_destroy(&base->bg_sync_mutex);
error_sync_mutex_init:
  DBUG_RETURN(error_num);
}

void ha_vp::free_bg_thread(
  VP_BG_BASE *base
) {
  DBUG_ENTER("ha_vp::free_bg_thread");
  if (base->bg_init)
  {
    pthread_mutex_lock(&base->bg_mutex);
    base->bg_command = VP_BG_COMMAND_KILL;
    pthread_mutex_lock(&base->bg_sync_mutex);
    pthread_cond_signal(&base->bg_cond);
    pthread_mutex_unlock(&base->bg_mutex);
    pthread_cond_wait(&base->bg_sync_cond, &base->bg_sync_mutex);
    pthread_mutex_unlock(&base->bg_sync_mutex);
    pthread_cond_destroy(&base->bg_cond);
    pthread_cond_destroy(&base->bg_sync_cond);
    pthread_mutex_destroy(&base->bg_mutex);
    pthread_mutex_destroy(&base->bg_sync_mutex);
    base->bg_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void ha_vp::bg_kick(
  VP_BG_BASE *base
) {
  DBUG_ENTER("ha_vp::bg_kick");
  pthread_mutex_lock(&base->bg_mutex);
  base->bg_caller_sync_wait = TRUE;
  pthread_mutex_lock(&base->bg_sync_mutex);
  pthread_cond_signal(&base->bg_cond);
  pthread_mutex_unlock(&base->bg_mutex);
  pthread_cond_wait(&base->bg_sync_cond, &base->bg_sync_mutex);
  pthread_mutex_unlock(&base->bg_sync_mutex);
  base->bg_caller_sync_wait = FALSE;
  DBUG_VOID_RETURN;
}

void ha_vp::bg_wait(
  VP_BG_BASE *base
) {
  DBUG_ENTER("ha_vp::bg_wait");
  pthread_mutex_lock(&base->bg_mutex);
  pthread_mutex_unlock(&base->bg_mutex);
  DBUG_VOID_RETURN;
}

void ha_vp::init_select_column(bool rnd)
{
  DBUG_ENTER("ha_vp::init_select_column");
#ifndef DBUG_OFF
  int roop_count;
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp read_bitmap is %d-%u", roop_count,
      ((uchar *) table->read_set->bitmap)[roop_count]));
    DBUG_PRINT("info",("vp write_bitmap is %d-%u", roop_count,
      ((uchar *) table->write_set->bitmap)[roop_count]));
  }
#endif
#ifdef HA_CAN_BULK_ACCESS
  if (
    bulk_access_started ||
    (bulk_access_executing && bulk_access_info_exec_tgt->called)
  ) {
    VP_BULK_ACCESS_INFO *bulk_access_info;
    if (bulk_access_pre_called)
      bulk_access_info = bulk_access_info_current;
    else
      bulk_access_info = bulk_access_info_exec_tgt;
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (partition_handler_share)
    {
      if (!rnd)
      {
        if (!bulk_access_info->partition_handler_share->idx_init_flg)
        {
          memcpy(bulk_access_info->partition_handler_share->
            idx_init_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(bulk_access_info->partition_handler_share->
            idx_init_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          bulk_access_info->partition_handler_share->idx_init_flg = TRUE;
          DBUG_PRINT("info",("vp set idx_init_bitmap"));
        }
        if (!bulk_access_info->idx_bitmap_init_flg)
        {
          memcpy(bulk_access_info->idx_init_read_bitmap,
            bulk_access_info->partition_handler_share->idx_init_read_bitmap,
            bitmap_map_size);
          memcpy(bulk_access_info->idx_init_write_bitmap,
            bulk_access_info->partition_handler_share->idx_init_write_bitmap,
            bitmap_map_size);
          bulk_access_info->idx_bitmap_init_flg = TRUE;
          DBUG_PRINT("info",("vp copy idx_init_bitmap"));
        }
      } else {
        if (!bulk_access_info->partition_handler_share->rnd_init_flg)
        {
          memcpy(bulk_access_info->partition_handler_share->
            rnd_init_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(bulk_access_info->partition_handler_share->
            rnd_init_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          bulk_access_info->partition_handler_share->rnd_init_flg = TRUE;
          DBUG_PRINT("info",("vp set rnd_init_bitmap"));
        }
        if (!bulk_access_info->rnd_bitmap_init_flg)
        {
          memcpy(bulk_access_info->rnd_init_read_bitmap,
            bulk_access_info->partition_handler_share->rnd_init_read_bitmap,
            bitmap_map_size);
          memcpy(bulk_access_info->rnd_init_write_bitmap,
            bulk_access_info->partition_handler_share->rnd_init_write_bitmap,
            bitmap_map_size);
          bulk_access_info->rnd_bitmap_init_flg = TRUE;
          DBUG_PRINT("info",("vp copy rnd_init_bitmap"));
        }
      }
    } else {
#endif
      if (!rnd)
      {
        if (!bulk_access_info->idx_bitmap_init_flg)
        {
          memcpy(bulk_access_info->idx_init_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(bulk_access_info->idx_init_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          bulk_access_info->idx_bitmap_init_flg = TRUE;
          DBUG_PRINT("info",("vp set ha idx_init_bitmap"));
        }
      } else {
        if (!bulk_access_info->rnd_bitmap_init_flg)
        {
          memcpy(bulk_access_info->rnd_init_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(bulk_access_info->rnd_init_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          bulk_access_info->rnd_bitmap_init_flg = TRUE;
          DBUG_PRINT("info",("vp set ha rnd_init_bitmap"));
        }
      }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    }
#endif
  } else {
#endif
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (partition_handler_share)
    {
      if (!rnd)
      {
        if (!partition_handler_share->idx_init_flg)
        {
          memcpy(partition_handler_share->idx_init_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(partition_handler_share->idx_init_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          partition_handler_share->idx_init_flg = TRUE;
          DBUG_PRINT("info",("vp set idx_init_bitmap"));
        }
        if (!idx_bitmap_init_flg)
        {
          memcpy(idx_init_read_bitmap,
            partition_handler_share->idx_init_read_bitmap, bitmap_map_size);
          memcpy(idx_init_write_bitmap,
            partition_handler_share->idx_init_write_bitmap, bitmap_map_size);
          idx_bitmap_init_flg = TRUE;
          DBUG_PRINT("info",("vp copy idx_init_bitmap"));
        }
      } else {
        if (!partition_handler_share->rnd_init_flg)
        {
          memcpy(partition_handler_share->rnd_init_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(partition_handler_share->rnd_init_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          partition_handler_share->rnd_init_flg = TRUE;
          DBUG_PRINT("info",("vp set rnd_init_bitmap"));
        }
        if (!rnd_bitmap_init_flg)
        {
          memcpy(rnd_init_read_bitmap,
            partition_handler_share->rnd_init_read_bitmap, bitmap_map_size);
          memcpy(rnd_init_write_bitmap,
            partition_handler_share->rnd_init_write_bitmap, bitmap_map_size);
          rnd_bitmap_init_flg = TRUE;
          DBUG_PRINT("info",("vp copy rnd_init_bitmap"));
        }
      }
    } else {
#endif
      if (!rnd)
      {
        if (!idx_bitmap_init_flg)
        {
          memcpy(idx_init_read_bitmap, table->read_set->bitmap,
            bitmap_map_size);
          memcpy(idx_init_write_bitmap, table->write_set->bitmap,
            bitmap_map_size);
          idx_bitmap_init_flg = TRUE;
          DBUG_PRINT("info",("vp set ha idx_init_bitmap"));
        }
      } else {
        if (!rnd_bitmap_init_flg)
        {
          memcpy(rnd_init_read_bitmap, table->read_set->bitmap,
            bitmap_map_size);
          memcpy(rnd_init_write_bitmap, table->write_set->bitmap,
            bitmap_map_size);
          rnd_bitmap_init_flg = TRUE;
          DBUG_PRINT("info",("vp set ha rnd_init_bitmap"));
        }
      }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    }
#endif
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  DBUG_VOID_RETURN;
}

void ha_vp::check_select_column(bool rnd)
{
  int roop_count;
  DBUG_ENTER("ha_vp::check_select_column");
#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp read_bitmap is %d-%u", roop_count,
      ((uchar *) table->read_set->bitmap)[roop_count]));
    DBUG_PRINT("info",("vp write_bitmap is %d-%u", roop_count,
      ((uchar *) table->write_set->bitmap)[roop_count]));
  }
#endif
#ifdef HA_CAN_BULK_ACCESS
  if (
    bulk_access_started ||
    (bulk_access_executing && bulk_access_info_exec_tgt->called)
  ) {
    VP_BULK_ACCESS_INFO *bulk_access_info;
    if (bulk_access_pre_called)
    {
      DBUG_PRINT("info",("vp bulk_access_pre_called=TRUE"));
      bulk_access_info = bulk_access_info_current;
    } else {
      DBUG_PRINT("info",("vp bulk_access_pre_called=FALSE"));
      bulk_access_info = bulk_access_info_exec_tgt;
    }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (bulk_access_info->partition_handler_share)
    {
      if (!rnd)
      {
        if (is_clone)
        {
          if (!bulk_access_info->clone_partition_handler_share->
            idx_bitmap_is_set)
          {
            memcpy(bulk_access_info->clone_partition_handler_share->
              idx_read_bitmap,
              table->read_set->bitmap, bitmap_map_size);
            memcpy(bulk_access_info->clone_partition_handler_share->
              idx_write_bitmap,
              table->write_set->bitmap, bitmap_map_size);
            bulk_access_info->clone_partition_handler_share->
              idx_bitmap_is_set = TRUE;
            DBUG_PRINT("info",("vp set clone idx_bitmap"));
          }
          if (!bulk_access_info->idx_bitmap_is_set)
          {
            memcpy(bulk_access_info->idx_read_bitmap,
              bulk_access_info->clone_partition_handler_share->
              idx_read_bitmap, bitmap_map_size);
            memcpy(bulk_access_info->idx_write_bitmap,
              bulk_access_info->clone_partition_handler_share->
              idx_write_bitmap,
              bitmap_map_size);
            bulk_access_info->idx_bitmap_is_set = TRUE;
            DBUG_PRINT("info",("vp copy clone idx_bitmap"));
          }
          TABLE *table2;
          for (roop_count = 0; roop_count < share->table_count; roop_count++)
          {
            if (vp_bit_is_set(use_tables, roop_count))
            {
              table2 = part_tables[roop_count].table;
              memcpy(table2->read_set->bitmap,
                bulk_access_info->sel_key_init_child_bitmaps[0][roop_count],
                table2->s->column_bitmap_size);
              memcpy(table2->write_set->bitmap,
                bulk_access_info->sel_key_init_child_bitmaps[1][roop_count],
                table2->s->column_bitmap_size);
#ifndef DBUG_OFF
              int roop_count2;
              for (roop_count2 = 0; roop_count2 <
                (int) table2->s->column_bitmap_size; roop_count2++)
              {
                DBUG_PRINT("info",("vp child read_bitmap is %d-%d-%u",
                  roop_count, roop_count2,
                  ((uchar *) table2->read_set->bitmap)[roop_count2]));
                DBUG_PRINT("info",("vp child write_bitmap is %d-%d-%u",
                  roop_count, roop_count2,
                  ((uchar *) table2->write_set->bitmap)[roop_count2]));
              }
#endif
            }
          }
        } else {
          if (!bulk_access_info->partition_handler_share->idx_bitmap_is_set)
          {
            memcpy(bulk_access_info->partition_handler_share->idx_read_bitmap,
              table->read_set->bitmap, bitmap_map_size);
            memcpy(bulk_access_info->partition_handler_share->idx_write_bitmap,
              table->write_set->bitmap, bitmap_map_size);
            bulk_access_info->partition_handler_share->
              idx_bitmap_is_set = TRUE;
            DBUG_PRINT("info",("vp set idx_bitmap"));
          }
          if (!bulk_access_info->idx_bitmap_is_set)
          {
            memcpy(bulk_access_info->idx_read_bitmap,
              bulk_access_info->partition_handler_share->idx_read_bitmap,
              bitmap_map_size);
            memcpy(bulk_access_info->idx_write_bitmap,
              bulk_access_info->partition_handler_share->idx_write_bitmap,
              bitmap_map_size);
            bulk_access_info->idx_bitmap_is_set = TRUE;
            DBUG_PRINT("info",("vp copy idx_bitmap"));
          }
        }
      } else {
        if (!bulk_access_info->partition_handler_share->rnd_bitmap_is_set)
        {
          memcpy(bulk_access_info->partition_handler_share->rnd_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(bulk_access_info->partition_handler_share->rnd_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          bulk_access_info->partition_handler_share->rnd_bitmap_is_set = TRUE;
          DBUG_PRINT("info",("vp set rnd_bitmap"));
        }
        if (!bulk_access_info->rnd_bitmap_is_set)
        {
          memcpy(bulk_access_info->rnd_read_bitmap,
            bulk_access_info->partition_handler_share->rnd_read_bitmap,
            bitmap_map_size);
          memcpy(bulk_access_info->rnd_write_bitmap,
            bulk_access_info->partition_handler_share->rnd_write_bitmap,
            bitmap_map_size);
          bulk_access_info->rnd_bitmap_is_set = TRUE;
          DBUG_PRINT("info",("vp copy rnd_bitmap"));
        }
      }
    } else {
#endif
      if (!rnd)
      {
        if (!bulk_access_info->idx_bitmap_is_set)
        {
          memcpy(bulk_access_info->idx_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(bulk_access_info->idx_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          bulk_access_info->idx_bitmap_is_set = TRUE;
          DBUG_PRINT("info",("vp set ha idx_bitmap"));
        }
      } else {
        if (!bulk_access_info->rnd_bitmap_is_set)
        {
          memcpy(bulk_access_info->rnd_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(bulk_access_info->rnd_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          bulk_access_info->rnd_bitmap_is_set = TRUE;
          DBUG_PRINT("info",("vp set ha rnd_bitmap"));
        }
      }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    }
#endif
  } else {
#endif
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (partition_handler_share)
    {
      if (!rnd)
      {
        if (is_clone)
        {
          if (!clone_partition_handler_share->idx_bitmap_is_set)
          {
            memcpy(clone_partition_handler_share->idx_read_bitmap,
              table->read_set->bitmap, bitmap_map_size);
            memcpy(clone_partition_handler_share->idx_write_bitmap,
              table->write_set->bitmap, bitmap_map_size);
            clone_partition_handler_share->idx_bitmap_is_set = TRUE;
            DBUG_PRINT("info",("vp set clone idx_bitmap"));
          }
          if (!idx_bitmap_is_set)
          {
            memcpy(idx_read_bitmap,
              clone_partition_handler_share->idx_read_bitmap, bitmap_map_size);
            memcpy(idx_write_bitmap,
              clone_partition_handler_share->idx_write_bitmap,
              bitmap_map_size);
            idx_bitmap_is_set = TRUE;
            DBUG_PRINT("info",("vp copy clone idx_bitmap"));
          }
          TABLE *table2;
          for (roop_count = 0; roop_count < share->table_count; roop_count++)
          {
            if (vp_bit_is_set(use_tables, roop_count))
            {
              table2 = part_tables[roop_count].table;
              memcpy(table2->read_set->bitmap,
                sel_key_init_child_bitmaps[0][roop_count],
                table2->s->column_bitmap_size);
              memcpy(table2->write_set->bitmap,
                sel_key_init_child_bitmaps[1][roop_count],
                table2->s->column_bitmap_size);
#ifndef DBUG_OFF
              int roop_count2;
              for (roop_count2 = 0; roop_count2 <
                (int) table2->s->column_bitmap_size; roop_count2++)
              {
                DBUG_PRINT("info",("vp child read_bitmap is %d-%d-%u",
                  roop_count, roop_count2,
                  ((uchar *) table2->read_set->bitmap)[roop_count2]));
                DBUG_PRINT("info",("vp child write_bitmap is %d-%d-%u",
                  roop_count, roop_count2,
                  ((uchar *) table2->write_set->bitmap)[roop_count2]));
              }
#endif
            }
          }
        } else {
          if (!partition_handler_share->idx_bitmap_is_set)
          {
            memcpy(partition_handler_share->idx_read_bitmap,
              table->read_set->bitmap, bitmap_map_size);
            memcpy(partition_handler_share->idx_write_bitmap,
              table->write_set->bitmap, bitmap_map_size);
            partition_handler_share->idx_bitmap_is_set = TRUE;
            DBUG_PRINT("info",("vp set idx_bitmap"));
          }
          if (!idx_bitmap_is_set)
          {
            memcpy(idx_read_bitmap,
              partition_handler_share->idx_read_bitmap, bitmap_map_size);
            memcpy(idx_write_bitmap,
              partition_handler_share->idx_write_bitmap, bitmap_map_size);
            idx_bitmap_is_set = TRUE;
            DBUG_PRINT("info",("vp copy idx_bitmap"));
          }
        }
      } else {
        if (!partition_handler_share->rnd_bitmap_is_set)
        {
          memcpy(partition_handler_share->rnd_read_bitmap,
            table->read_set->bitmap, bitmap_map_size);
          memcpy(partition_handler_share->rnd_write_bitmap,
            table->write_set->bitmap, bitmap_map_size);
          partition_handler_share->rnd_bitmap_is_set = TRUE;
          DBUG_PRINT("info",("vp set rnd_bitmap"));
        }
        if (!rnd_bitmap_is_set)
        {
          memcpy(rnd_read_bitmap,
            partition_handler_share->rnd_read_bitmap, bitmap_map_size);
          memcpy(rnd_write_bitmap,
            partition_handler_share->rnd_write_bitmap, bitmap_map_size);
          rnd_bitmap_is_set = TRUE;
          DBUG_PRINT("info",("vp copy rnd_bitmap"));
        }
      }
    } else {
#endif
      if (!rnd)
      {
        if (!idx_bitmap_is_set)
        {
          memcpy(idx_read_bitmap, table->read_set->bitmap, bitmap_map_size);
          memcpy(idx_write_bitmap, table->write_set->bitmap, bitmap_map_size);
          idx_bitmap_is_set = TRUE;
          DBUG_PRINT("info",("vp set ha idx_bitmap"));
        }
      } else {
        if (!rnd_bitmap_is_set)
        {
          memcpy(rnd_read_bitmap, table->read_set->bitmap, bitmap_map_size);
          memcpy(rnd_write_bitmap, table->write_set->bitmap, bitmap_map_size);
          rnd_bitmap_is_set = TRUE;
          DBUG_PRINT("info",("vp set ha rnd_bitmap"));
        }
      }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    }
#endif
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  DBUG_VOID_RETURN;
}

void ha_vp::clone_init_select_column()
{
  DBUG_ENTER("ha_vp::clone_init_select_column");
#ifndef DBUG_OFF
  int roop_count;
  for (roop_count = 0; roop_count < share->bitmap_size; roop_count++)
  {
    DBUG_PRINT("info",("vp read_bitmap is %d-%u", roop_count,
      ((uchar *) pt_clone_source_handler->idx_init_read_bitmap)[roop_count]));
    DBUG_PRINT("info",("vp write_bitmap is %d-%u", roop_count,
      ((uchar *) pt_clone_source_handler->idx_init_write_bitmap)[roop_count]));
  }
#endif
  memcpy(idx_init_read_bitmap,
    pt_clone_source_handler->idx_init_read_bitmap, bitmap_map_size);
  memcpy(idx_init_write_bitmap,
    pt_clone_source_handler->idx_init_write_bitmap, bitmap_map_size);
  idx_bitmap_init_flg = TRUE;
  DBUG_VOID_RETURN;
}

uint ha_vp::check_partitioned()
{
  uint part_num;
  DBUG_ENTER("ha_vp::check_partitioned");
  DBUG_PRINT("info",("vp this=%p", this));
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

#ifdef HA_CAN_BULK_ACCESS
VP_BULK_ACCESS_INFO *ha_vp::create_bulk_access_info()
{
  int roop_count;
  TABLE_SHARE *part_table_share;
  VP_BULK_ACCESS_INFO *bulk_access_info;
  my_bitmap_map *tmp_idx_init_read_bitmap;
  my_bitmap_map *tmp_idx_init_write_bitmap;
  my_bitmap_map *tmp_rnd_init_read_bitmap;
  my_bitmap_map *tmp_rnd_init_write_bitmap;
  my_bitmap_map *tmp_idx_read_bitmap;
  my_bitmap_map *tmp_idx_write_bitmap;
  my_bitmap_map *tmp_rnd_read_bitmap;
  my_bitmap_map *tmp_rnd_write_bitmap;
  uchar **tmp_sel_key_init_child_bitmaps[2];
  uchar **tmp_sel_key_child_bitmaps[2];
  uchar **tmp_sel_rnd_child_bitmaps[2];
  uchar **tmp_ins_child_bitmaps[2];
  uchar *tmp_child_column_bitmap;
  uchar *tmp_sel_key_init_use_tables;
  uchar *tmp_sel_key_use_tables;
  uchar *tmp_sel_rnd_use_tables;
  void **tmp_info;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  VP_PARTITION_HANDLER_SHARE *tmp_partition_handler_share;
  my_bitmap_map *tmp_idx_init_read_bitmap2;
  my_bitmap_map *tmp_idx_init_write_bitmap2;
  my_bitmap_map *tmp_rnd_init_read_bitmap2;
  my_bitmap_map *tmp_rnd_init_write_bitmap2;
  my_bitmap_map *tmp_idx_read_bitmap2;
  my_bitmap_map *tmp_idx_write_bitmap2;
  my_bitmap_map *tmp_rnd_read_bitmap2;
  my_bitmap_map *tmp_rnd_write_bitmap2;
  VP_CLONE_PARTITION_HANDLER_SHARE *tmp_clone_partition_handler_share;
  my_bitmap_map *tmp_idx_read_bitmap3;
  my_bitmap_map *tmp_idx_write_bitmap3;
#endif
  DBUG_ENTER("ha_vp::create_bulk_access_info");
  DBUG_PRINT("info",("vp this=%p", this));
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (partition_handler_share && partition_handler_share->creator == this)
  {
    if (!(bulk_access_info = (VP_BULK_ACCESS_INFO *)
      my_multi_malloc(MYF(MY_WME),
      &bulk_access_info, sizeof(VP_BULK_ACCESS_INFO),
      &tmp_idx_init_read_bitmap, bitmap_map_size,
      &tmp_idx_init_write_bitmap, bitmap_map_size,
      &tmp_rnd_init_read_bitmap, bitmap_map_size,
      &tmp_rnd_init_write_bitmap, bitmap_map_size,
      &tmp_idx_read_bitmap, bitmap_map_size,
      &tmp_idx_write_bitmap, bitmap_map_size,
      &tmp_rnd_read_bitmap, bitmap_map_size,
      &tmp_rnd_write_bitmap, bitmap_map_size,
      &tmp_ins_child_bitmaps[0], sizeof(uchar *) * share->table_count,
      &tmp_ins_child_bitmaps[1], sizeof(uchar *) * share->table_count,
      &tmp_sel_key_init_child_bitmaps[0], sizeof(uchar *) * share->table_count,
      &tmp_sel_key_init_child_bitmaps[1], sizeof(uchar *) * share->table_count,
      &tmp_sel_key_child_bitmaps[0], sizeof(uchar *) * share->table_count,
      &tmp_sel_key_child_bitmaps[1], sizeof(uchar *) * share->table_count,
      &tmp_sel_rnd_child_bitmaps[0], sizeof(uchar *) * share->table_count,
      &tmp_sel_rnd_child_bitmaps[1], sizeof(uchar *) * share->table_count,
      &tmp_child_column_bitmap, sizeof(uchar) * child_column_bitmap_size * 8,
      &tmp_sel_key_init_use_tables, sizeof(uchar) * share->use_tables_size,
      &tmp_sel_key_use_tables, sizeof(uchar) * share->use_tables_size,
      &tmp_sel_rnd_use_tables, sizeof(uchar) * share->use_tables_size,
      &tmp_info, sizeof(void *) * share->table_count,
      &tmp_partition_handler_share, sizeof(VP_PARTITION_HANDLER_SHARE),
      &tmp_idx_init_read_bitmap2, bitmap_map_size,
      &tmp_idx_init_write_bitmap2, bitmap_map_size,
      &tmp_rnd_init_read_bitmap2, bitmap_map_size,
      &tmp_rnd_init_write_bitmap2, bitmap_map_size,
      &tmp_idx_read_bitmap2, bitmap_map_size,
      &tmp_idx_write_bitmap2, bitmap_map_size,
      &tmp_rnd_read_bitmap2, bitmap_map_size,
      &tmp_rnd_write_bitmap2, bitmap_map_size,
      &tmp_clone_partition_handler_share,
        sizeof(VP_CLONE_PARTITION_HANDLER_SHARE),
      &tmp_idx_read_bitmap3, bitmap_map_size,
      &tmp_idx_write_bitmap3, bitmap_map_size,
      NullS))
    ) {
      goto error_bulk_malloc;
    }
  } else {
#endif
    if (!(bulk_access_info = (VP_BULK_ACCESS_INFO *)
      my_multi_malloc(MYF(MY_WME),
      &bulk_access_info, sizeof(VP_BULK_ACCESS_INFO),
      &tmp_idx_init_read_bitmap, bitmap_map_size,
      &tmp_idx_init_write_bitmap, bitmap_map_size,
      &tmp_rnd_init_read_bitmap, bitmap_map_size,
      &tmp_rnd_init_write_bitmap, bitmap_map_size,
      &tmp_idx_read_bitmap, bitmap_map_size,
      &tmp_idx_write_bitmap, bitmap_map_size,
      &tmp_rnd_read_bitmap, bitmap_map_size,
      &tmp_rnd_write_bitmap, bitmap_map_size,
      &tmp_ins_child_bitmaps[0], sizeof(uchar *) * share->table_count,
      &tmp_ins_child_bitmaps[1], sizeof(uchar *) * share->table_count,
      &tmp_sel_key_init_child_bitmaps[0], sizeof(uchar *) * share->table_count,
      &tmp_sel_key_init_child_bitmaps[1], sizeof(uchar *) * share->table_count,
      &tmp_sel_key_child_bitmaps[0], sizeof(uchar *) * share->table_count,
      &tmp_sel_key_child_bitmaps[1], sizeof(uchar *) * share->table_count,
      &tmp_sel_rnd_child_bitmaps[0], sizeof(uchar *) * share->table_count,
      &tmp_sel_rnd_child_bitmaps[1], sizeof(uchar *) * share->table_count,
      &tmp_child_column_bitmap, sizeof(uchar) * child_column_bitmap_size * 8,
      &tmp_sel_key_init_use_tables, sizeof(uchar) * share->use_tables_size,
      &tmp_sel_key_use_tables, sizeof(uchar) * share->use_tables_size,
      &tmp_sel_rnd_use_tables, sizeof(uchar) * share->use_tables_size,
      &tmp_info, sizeof(void *) * share->table_count,
      NullS))
    ) {
      goto error_bulk_malloc;
    }
#ifdef WITH_PARTITION_STORAGE_ENGINE
  }
#endif
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    part_table_share = part_tables[roop_count].table->s;
    tmp_ins_child_bitmaps[0][roop_count] = tmp_child_column_bitmap;
    tmp_child_column_bitmap += part_table_share->column_bitmap_size;
    tmp_ins_child_bitmaps[1][roop_count] = tmp_child_column_bitmap;
    tmp_child_column_bitmap += part_table_share->column_bitmap_size;
    tmp_sel_key_init_child_bitmaps[0][roop_count] = tmp_child_column_bitmap;
    tmp_child_column_bitmap += part_table_share->column_bitmap_size;
    tmp_sel_key_init_child_bitmaps[1][roop_count] = tmp_child_column_bitmap;
    tmp_child_column_bitmap += part_table_share->column_bitmap_size;
    tmp_sel_key_child_bitmaps[0][roop_count] = tmp_child_column_bitmap;
    tmp_child_column_bitmap += part_table_share->column_bitmap_size;
    tmp_sel_key_child_bitmaps[1][roop_count] = tmp_child_column_bitmap;
    tmp_child_column_bitmap += part_table_share->column_bitmap_size;
    tmp_sel_rnd_child_bitmaps[0][roop_count] = tmp_child_column_bitmap;
    tmp_child_column_bitmap += part_table_share->column_bitmap_size;
    tmp_sel_rnd_child_bitmaps[1][roop_count] = tmp_child_column_bitmap;
    tmp_child_column_bitmap += part_table_share->column_bitmap_size;
  }
  bulk_access_info->idx_init_read_bitmap = tmp_idx_init_read_bitmap;
  bulk_access_info->idx_init_write_bitmap = tmp_idx_init_write_bitmap;
  bulk_access_info->rnd_init_read_bitmap = tmp_rnd_init_read_bitmap;
  bulk_access_info->rnd_init_write_bitmap = tmp_rnd_init_write_bitmap;
  bulk_access_info->idx_read_bitmap = tmp_idx_read_bitmap;
  bulk_access_info->idx_write_bitmap = tmp_idx_write_bitmap;
  bulk_access_info->rnd_read_bitmap = tmp_rnd_read_bitmap;
  bulk_access_info->rnd_write_bitmap = tmp_rnd_write_bitmap;
  bulk_access_info->ins_child_bitmaps[0] = tmp_ins_child_bitmaps[0];
  bulk_access_info->ins_child_bitmaps[1] = tmp_ins_child_bitmaps[1];
  bulk_access_info->sel_key_init_child_bitmaps[0] =
    tmp_sel_key_init_child_bitmaps[0];
  bulk_access_info->sel_key_init_child_bitmaps[1] =
    tmp_sel_key_init_child_bitmaps[1];
  bulk_access_info->sel_key_child_bitmaps[0] = tmp_sel_key_child_bitmaps[0];
  bulk_access_info->sel_key_child_bitmaps[1] = tmp_sel_key_child_bitmaps[1];
  bulk_access_info->sel_rnd_child_bitmaps[0] = tmp_sel_rnd_child_bitmaps[0];
  bulk_access_info->sel_rnd_child_bitmaps[1] = tmp_sel_rnd_child_bitmaps[1];
  bulk_access_info->sel_key_init_use_tables = tmp_sel_key_init_use_tables;
  bulk_access_info->sel_key_use_tables = tmp_sel_key_use_tables;
  bulk_access_info->sel_rnd_use_tables = tmp_sel_rnd_use_tables;
  bulk_access_info->info = tmp_info;
  bulk_access_info->next = NULL;
  bulk_access_info->idx_bitmap_init_flg = FALSE;
  bulk_access_info->rnd_bitmap_init_flg = FALSE;
  bulk_access_info->idx_bitmap_is_set = FALSE;
  bulk_access_info->rnd_bitmap_is_set = FALSE;
  bulk_access_info->child_keyread = FALSE;
  bulk_access_info->single_table = FALSE;
  bulk_access_info->set_used_table = FALSE;
  bulk_access_info->init_sel_key_init_bitmap = FALSE;
  bulk_access_info->init_sel_key_bitmap = FALSE;
  bulk_access_info->init_sel_rnd_bitmap = FALSE;
  bulk_access_info->init_ins_bitmap = FALSE;
  bulk_access_info->used = FALSE;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (partition_handler_share && partition_handler_share->creator == this)
  {
    tmp_partition_handler_share->idx_init_read_bitmap =
      tmp_idx_init_read_bitmap2;
    tmp_partition_handler_share->idx_init_write_bitmap =
      tmp_idx_init_write_bitmap2;
    tmp_partition_handler_share->rnd_init_read_bitmap =
      tmp_rnd_init_read_bitmap2;
    tmp_partition_handler_share->rnd_init_write_bitmap =
      tmp_rnd_init_write_bitmap2;
    tmp_partition_handler_share->idx_read_bitmap =
      tmp_idx_read_bitmap2;
    tmp_partition_handler_share->idx_write_bitmap =
      tmp_idx_write_bitmap2;
    tmp_partition_handler_share->rnd_read_bitmap =
      tmp_rnd_read_bitmap2;
    tmp_partition_handler_share->rnd_write_bitmap =
      tmp_rnd_write_bitmap2;
    tmp_partition_handler_share->idx_init_flg = FALSE;
    tmp_partition_handler_share->rnd_init_flg = FALSE;
    tmp_partition_handler_share->idx_bitmap_is_set = FALSE;
    tmp_partition_handler_share->rnd_bitmap_is_set = FALSE;
    tmp_clone_partition_handler_share->idx_read_bitmap =
      tmp_idx_read_bitmap3;
    tmp_clone_partition_handler_share->idx_write_bitmap =
      tmp_idx_write_bitmap3;
    tmp_clone_partition_handler_share->idx_bitmap_is_set = FALSE;
    bulk_access_info->partition_handler_share =
      tmp_partition_handler_share;
    bulk_access_info->clone_partition_handler_share =
      tmp_clone_partition_handler_share;
    tmp_partition_handler_share->clone_partition_handler_share =
      tmp_clone_partition_handler_share;
    partition_handler_share->current_bulk_access_info = bulk_access_info;
  } else if (partition_handler_share)
  {
    VP_BULK_ACCESS_INFO *bulk_access_info2 =
      partition_handler_share->current_bulk_access_info;
    bulk_access_info->partition_handler_share =
      bulk_access_info2->partition_handler_share;
    bulk_access_info->clone_partition_handler_share =
      bulk_access_info2->clone_partition_handler_share;
  } else {
    bulk_access_info->partition_handler_share = NULL;
    bulk_access_info->clone_partition_handler_share = NULL;
  }
#endif
  DBUG_RETURN(bulk_access_info);

error_bulk_malloc:
  DBUG_RETURN(NULL);
}

void ha_vp::delete_bulk_access_info(
  VP_BULK_ACCESS_INFO *bulk_access_info
) {
  DBUG_ENTER("ha_vp::delete_bulk_access_info");
  DBUG_PRINT("info",("vp this=%p", this));
  vp_my_free(bulk_access_info, MYF(0));
  DBUG_VOID_RETURN;
}
#endif

void ha_vp::overwrite_index_bits()
{
  uint roop_count, roop_count2;
  DBUG_ENTER("ha_vp::overwrite_index_bits");
  DBUG_PRINT("info",("vp this=%p", this));
  table_share->keys_for_keyread.clear_all();
  for (roop_count = 0; roop_count < table_share->fields; roop_count++)
  {
    Field *field = table_share->field[roop_count];
    field->part_of_key.clear_all();
    field->part_of_key_not_clustered.clear_all();
    field->part_of_sortkey.clear_all();
  }
  for (roop_count = 0; roop_count < table_share->keys; roop_count++)
  {
    KEY *key_info = &table->s->key_info[roop_count];
    KEY_PART_INFO *key_part = key_info->key_part;
    VP_CORRESPOND_KEY *correspond_key = share->keys[roop_count].correspond_key;
    for (roop_count2 = 0 ; roop_count2 < vp_user_defined_key_parts(key_info);
      key_part++, roop_count2++)
    {
      Field *field = key_part->field;
      VP_CORRESPOND_KEY *tmp_ck;
      if (field->key_length() == key_part->length &&
          !(field->flags & BLOB_FLAG))
      {
        for (tmp_ck = correspond_key; tmp_ck; tmp_ck = tmp_ck->next)
        {
          if (!(part_tables[tmp_ck->table_idx].table->file->index_flags(
            tmp_ck->key_idx, roop_count2, 0) & HA_KEYREAD_ONLY))
            break;
        }
        if (!tmp_ck)
        {
          table_share->keys_for_keyread.set_bit(roop_count);
          field->part_of_key.set_bit(roop_count);
          field->part_of_key_not_clustered.set_bit(roop_count);
        }
        for (tmp_ck = correspond_key; tmp_ck; tmp_ck = tmp_ck->next)
        {
          if (!(part_tables[tmp_ck->table_idx].table->file->index_flags(
            tmp_ck->key_idx, roop_count2, 1) & HA_READ_ORDER))
            break;
        }
        if (!tmp_ck)
          field->part_of_sortkey.set_bit(roop_count);
      }
      if (roop_count == table_share->primary_key)
      {
        for (tmp_ck = correspond_key; tmp_ck; tmp_ck = tmp_ck->next)
        {
          if (
            (uint) tmp_ck->key_idx !=
              part_tables[tmp_ck->table_idx].table->s->primary_key ||
            !(part_tables[tmp_ck->table_idx].table->file->ha_table_flags() &
              HA_PRIMARY_KEY_IN_READ_INDEX)
          )
            break;
        }
        if (!tmp_ck)
        {
          if (field->key_length() == key_part->length &&
              !(field->flags & BLOB_FLAG))
            field->part_of_key = table_share->keys_in_use;
          if (field->part_of_sortkey.is_set(roop_count))
            field->part_of_sortkey = table_share->keys_in_use;
        }
      }
    }
  }
  DBUG_VOID_RETURN;
}

#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
void ha_vp::check_and_set_bitmap_for_update(
  bool rnd
) {
  DBUG_ENTER("ha_vp::check_and_set_bitmap_for_update");
  DBUG_PRINT("info",("vp this=%p", this));
  uchar *w_bitmap = (uchar *) table->write_set->bitmap;
  memset(upd_target_tables, 0, sizeof(uchar) * share->use_tables_size);
  for (int roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    int *correspond_columns_p =
      &share->correspond_columns_p[roop_count * table_share->fields];
    int *correspond_columns_c =
      share->correspond_columns_c_ptr[roop_count];
    for (uint roop_count2 = 0; roop_count2 < table_share->fields;
      roop_count2++)
    {
      DBUG_PRINT("info",("vp field num=%u", roop_count2));
      if (vp_bit_is_set(w_bitmap, roop_count2))
      {
        DBUG_PRINT("info",("vp correspond_columns_p=%d",
          correspond_columns_p[roop_count2]));
        if (correspond_columns_p[roop_count2] < MAX_FIELDS)
        {
          TABLE *child_table = part_tables[roop_count].table;
          vp_set_bit(upd_target_tables, roop_count);
          clear_child_bitmap(roop_count);
          uchar *cw_bitmap = (uchar *) child_table->write_set->bitmap;
          uchar *cr_bitmap = (uchar *) child_table->read_set->bitmap;
          uchar *add_child_bitmap = add_from_child_bitmaps[0][roop_count];
          vp_set_bit(cw_bitmap, correspond_columns_p[roop_count2]);
          for (roop_count2++; roop_count2 < table_share->fields;
            roop_count2++)
          {
            DBUG_PRINT("info",("vp field num=%u", roop_count2));
            DBUG_PRINT("info",("vp correspond_columns_p=%d",
              correspond_columns_p[roop_count2]));
            if (
              vp_bit_is_set(w_bitmap, roop_count2) &&
              correspond_columns_p[roop_count2] < MAX_FIELDS
            ) {
              vp_set_bit(cw_bitmap, correspond_columns_p[roop_count2]);
            }
          }
          child_table->file->check_and_set_bitmap_for_update(rnd);
          uint child_col_num = 0;
          DBUG_PRINT("info",("vp child fields=%u", child_table->s->fields));
          DBUG_PRINT("info",("vp child column_bitmap_size=%u",
            child_table->s->column_bitmap_size));
          for (uint roop_count3 = 0;
            roop_count3 < child_table->s->column_bitmap_size;
            roop_count3++)
          {
            if (cr_bitmap[roop_count3])
            {
              for (uint roop_count4 = 0; roop_count4 < 8; roop_count4++)
              {
                DBUG_PRINT("info",("vp child column=%u", child_col_num));
                if (vp_bit_is_set(&cr_bitmap[roop_count3], roop_count4))
                {
                  uint correspond_column =
                    correspond_columns_c[child_col_num];
                  DBUG_PRINT("info",("vp add bitmap=%u",
                    correspond_column));
                  if (correspond_column < MAX_FIELDS)
                  {
                    vp_set_bit(((uchar *) table->read_set->bitmap),
                      correspond_column);
                  }
                }
                child_col_num++;
                if (child_col_num >= child_table->s->fields)
                  break;
              }
            }
            add_child_bitmap[roop_count3] = cr_bitmap[roop_count3];
            if (child_col_num >= child_table->s->fields)
              break;
          }
          break;
        }
      }
    }
  }
#ifndef DBUG_OFF
  for (int roop_count = 0; roop_count < share->bitmap_size; roop_count++)
    DBUG_PRINT("info",("vp source bitmap is %d-%u", roop_count,
      ((uchar *) table->read_set->bitmap)[roop_count]));
#endif
  DBUG_VOID_RETURN;
}
#endif

#ifdef HA_CAN_BULK_ACCESS
void ha_vp::bulk_req_exec()
{
  int roop_count;
  DBUG_ENTER("ha_vp::bulk_req_exec");
  DBUG_PRINT("info",("vp this=%p", this));
  for (roop_count = 0; roop_count < share->table_count; roop_count++)
  {
    if (vp_bit_is_set(bulk_access_exec_bitmap, roop_count))
    {
      part_tables[roop_count].table->file->bulk_req_exec();
    }
  }
  memset(bulk_access_exec_bitmap, 0, sizeof(uchar) * share->use_tables_size);
  DBUG_VOID_RETURN;
}
#endif

TABLE_LIST *ha_vp::get_parent_table_list()
{
  TABLE_LIST *table_list = table->pos_in_table_list;
  DBUG_ENTER("ha_vp::get_parent_table_list");
  if (table_list)
  {
    while (table_list->parent_l)
      table_list = table_list->parent_l;
    DBUG_RETURN(table_list);
  }
  DBUG_RETURN(NULL);
}

st_select_lex *ha_vp::get_select_lex()
{
  TABLE_LIST *table_list = get_parent_table_list();
  DBUG_ENTER("ha_vp::get_select_lex");
  if (table_list)
  {
    DBUG_PRINT("info",("vp select_lex=%p", table_list->select_lex));
    DBUG_RETURN(table_list->select_lex);
  }
  DBUG_RETURN(NULL);
}

JOIN *ha_vp::get_join()
{
  st_select_lex *select_lex = get_select_lex();
  DBUG_ENTER("ha_vp::get_join");
  if (select_lex)
  {
    DBUG_PRINT("info",("vp join=%p", select_lex->join));
    DBUG_RETURN(select_lex->join);
  }
  DBUG_RETURN(NULL);
}

#ifdef VP_HAS_EXPLAIN_QUERY
Explain_select *ha_vp::get_explain_select()
{
  DBUG_ENTER("ha_vp::get_explain_select");
  Explain_query *explain = current_thd->lex->explain;
  if (explain)
  {
    st_select_lex *select_lex = get_select_lex();
    if (select_lex)
    {
      DBUG_PRINT("info",("vp select_lex=%p", select_lex));
      DBUG_PRINT("info",("vp select_number=%u",
        select_lex->select_number));
      Explain_select *explain_select =
        explain->get_select(select_lex->select_number);
      DBUG_PRINT("info",("vp explain_select=%p", explain_select));
      DBUG_RETURN(explain_select);
    }
  }
  DBUG_RETURN(NULL);
}

#ifdef EXPLAIN_HAS_GET_UPD_DEL
Explain_update *ha_vp::get_explain_upd_del()
{
  DBUG_ENTER("ha_vp::get_explain_upd_del");
  Explain_query *explain = current_thd->lex->explain;
  if (explain)
  {
    Explain_update *explain_update = explain->get_upd_del();
    DBUG_PRINT("info",("vp explain_update=%p", explain_update));
    DBUG_RETURN(explain_update);
  }
  DBUG_RETURN(NULL);
}
#endif
#endif
