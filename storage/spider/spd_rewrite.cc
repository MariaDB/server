/* Copyright (C) 2018-2019 Kentoku Shiba
   Copyright (C) 2018-2019 MariaDB corp

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

#define MYSQL_SERVER 1
#define MYSQL_LEX 1
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "sql_acl.h"
#include "spd_err.h"
#ifdef SPIDER_REWRITE_AVAILABLE
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_sys_table.h"
#include "spd_table.h"
#include "spd_malloc.h"
#include "spd_parse.h"
#include "spd_rewrite.h"

extern handlerton *spider_hton_ptr;

static pthread_key(spider_parse_sql *, SPIDER_PARSE_SQL);
static bool audit_rewrite_initialized = FALSE, rw_table_mem_root = TRUE,
  rewrite_cache_initialized = FALSE;
static MEM_ROOT rw_table_mem_root1, rw_table_mem_root2;

static DYNAMIC_ARRAY spider_rw_table_cache1;
static uint spider_rw_table_cache1_id;
static const char *spider_rw_table_cache1_func_name;
static const char *spider_rw_table_cache1_file_name;
static ulong spider_rw_table_cache1_line_no;
static DYNAMIC_ARRAY spider_rw_table_cache2;
static uint spider_rw_table_cache2_id;
static const char *spider_rw_table_cache2_func_name;
static const char *spider_rw_table_cache2_file_name;
static ulong spider_rw_table_cache2_line_no;
static DYNAMIC_ARRAY *spider_rw_table_cache;
static DYNAMIC_ARRAY *spider_rw_table_cache_tmp;

void spider_free_rewrite_table_subpartitions(
  SPIDER_RWTBLSPTT *info
) {
  DBUG_ENTER("spider_free_rewrite_table_subpartitions");
  while (info)
  {
    if (info->subpartition_name.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->subpartition_name.str, MYF(0));
    }
    if (info->subpartition_description.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->subpartition_description.str, MYF(0));
    }
    if (info->connection_str.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->connection_str.str, MYF(0));
    }
    if (info->comment_str.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->comment_str.str, MYF(0));
    }
    info = info->next;
  }
  DBUG_VOID_RETURN;
}

void spider_free_rewrite_table_partitions(
  SPIDER_RWTBLPTT *info
) {
  DBUG_ENTER("spider_free_rewrite_table_partitions");
  while (info)
  {
    if (info->partition_name.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->partition_name.str, MYF(0));
    }
    if (info->partition_description.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->partition_description.str, MYF(0));
    }
    if (info->connection_str.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->connection_str.str, MYF(0));
    }
    if (info->comment_str.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->comment_str.str, MYF(0));
    }
    spider_free_rewrite_table_subpartitions(info->ts);
    info = info->next;
  }
  DBUG_VOID_RETURN;
}

void spider_free_rewrite_table_tables(
  SPIDER_RWTBLTBL *info
) {
  DBUG_ENTER("spider_free_rewrite_table_tables");
  while (info)
  {
    if (info->partition_method.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->partition_method.str, MYF(0));
    }
    if (info->partition_expression.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->partition_expression.str, MYF(0));
    }
    if (info->subpartition_method.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->subpartition_method.str, MYF(0));
    }
    if (info->subpartition_expression.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->subpartition_expression.str, MYF(0));
    }
    if (info->connection_str.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->connection_str.str, MYF(0));
    }
    if (info->comment_str.str != NullS)
    {
      spider_free(spider_current_trx,
        (void *) info->comment_str.str, MYF(0));
    }
    spider_free_rewrite_table_partitions(info->tp);
    info = info->next;
  }
  DBUG_VOID_RETURN;
}

void spider_free_rewrite_tables(
  SPIDER_RWTBL *info
) {
  DBUG_ENTER("spider_free_rewrite_tables");
  if (!info)
  {
    DBUG_VOID_RETURN;
  }
  if (info->db_name.str != NullS)
  {
    spider_free(spider_current_trx,
      (void *) info->db_name.str, MYF(0));
  }
  if (info->table_name.str != NullS)
  {
    spider_free(spider_current_trx,
      (void *) info->table_name.str, MYF(0));
  }
  spider_free_rewrite_table_tables(info->tt);
  DBUG_VOID_RETURN;
}

void spider_free_rewrite_cache(
  DYNAMIC_ARRAY *rw_table_cache
) {
  uint roop_count;
  SPIDER_RWTBL *info;
  DBUG_ENTER("spider_free_rewrite_cache");
  for (roop_count = 0; roop_count < rw_table_cache->elements;
    ++roop_count)
  {
    info = dynamic_element(rw_table_cache, roop_count,
      SPIDER_RWTBL *);
    spider_free_rewrite_tables(info);
  }
  rw_table_cache->elements = 0;
  DBUG_VOID_RETURN;
}

bool spider_load_rewrite_table_subpartitions(
  THD *thd,
  MEM_ROOT *mem_root,
  TABLE_LIST *tables,
  SPIDER_RWTBLPTT *rwtblptt
) {
  int error_num;
  TABLE *table;
  char table_key[MAX_KEY_LENGTH];
  SPIDER_RWTBLSPTT *current = NULL;
  DBUG_ENTER("spider_load_rewrite_table_subpartitions");
  DBUG_PRINT("info",("spider table_name=%s",
    SPIDER_TABLE_LIST_table_name_str(tables)));
  table = tables->table;
  if ((error_num = spider_get_sys_table_by_idx(table, table_key,
    table->s->primary_key, 3)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      goto error_get_sys_table_by_idx;
    }
    rwtblptt->ts = NULL;
    DBUG_RETURN(FALSE);
  }
  while (!error_num)
  {
    SPIDER_RWTBLSPTT *info;
    if (!(info = (SPIDER_RWTBLSPTT *) alloc_root(mem_root, sizeof(SPIDER_RWTBLSPTT))))
    {
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      goto error_alloc_info;
    }
    if (!current)
    {
      rwtblptt->ts = info;
    } else {
      current->next = info;
    }
    current = info;
    if (spider_get_sys_rewrite_table_subpartitions(table, info, mem_root))
    {
      goto error_get_sys_rewrite_table_subpartitions;
    }

    if ((error_num = spider_sys_index_next_same(table, table_key)))
    {
      if (
        error_num != HA_ERR_KEY_NOT_FOUND &&
        error_num != HA_ERR_END_OF_FILE
      ) {
        table->file->print_error(error_num, MYF(0));
        goto error_sys_index_next_same;
      }
    }
  }
  spider_sys_index_end(table);
  if (current)
  {
    current->next = NULL;
  }
  DBUG_RETURN(FALSE);

error_sys_index_next_same:
error_get_sys_rewrite_table_subpartitions:
error_alloc_info:
  spider_sys_index_end(table);
error_get_sys_table_by_idx:
  DBUG_RETURN(TRUE);
}

bool spider_load_rewrite_table_partitions(
  THD *thd,
  MEM_ROOT *mem_root,
  TABLE_LIST *tables,
  SPIDER_RWTBLTBL *rwtbltbl
) {
  int error_num;
  TABLE *table;
  TABLE_LIST *tables_next = tables->next_global;
  char table_key[MAX_KEY_LENGTH];
  SPIDER_RWTBLPTT *current = NULL;
  DBUG_ENTER("spider_load_rewrite_table_partitions");
  DBUG_PRINT("info",("spider table_name=%s",
    SPIDER_TABLE_LIST_table_name_str(tables)));
  table = tables->table;
  if ((error_num = spider_get_sys_table_by_idx(table, table_key,
    table->s->primary_key, 2)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      goto error_get_sys_table_by_idx;
    }
    rwtbltbl->tp = NULL;
    DBUG_RETURN(FALSE);
  }
  while (!error_num)
  {
    SPIDER_RWTBLPTT *info;
    if (!(info = (SPIDER_RWTBLPTT *) alloc_root(mem_root, sizeof(SPIDER_RWTBLPTT))))
    {
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      goto error_alloc_info;
    }
    if (!current)
    {
      rwtbltbl->tp = info;
    } else {
      current->next = info;
    }
    current = info;
    if (spider_get_sys_rewrite_table_partitions(table, info, mem_root))
    {
      goto error_get_sys_rewrite_table_partitions;
    }
    if (
      spider_copy_sys_rewrite_columns(table, tables_next->table, 3) ||
      spider_load_rewrite_table_subpartitions(thd, mem_root, tables_next, info)
    ) {
      goto error_load_rewrite_table_subpartitions;
    }

    if ((error_num = spider_sys_index_next_same(table, table_key)))
    {
      if (
        error_num != HA_ERR_KEY_NOT_FOUND &&
        error_num != HA_ERR_END_OF_FILE
      ) {
        table->file->print_error(error_num, MYF(0));
        goto error_sys_index_next_same;
      }
    }
  }
  spider_sys_index_end(table);
  if (current)
  {
    current->next = NULL;
  }
  DBUG_RETURN(FALSE);

error_sys_index_next_same:
error_load_rewrite_table_subpartitions:
error_get_sys_rewrite_table_partitions:
error_alloc_info:
  spider_sys_index_end(table);
error_get_sys_table_by_idx:
  DBUG_RETURN(TRUE);
}

bool spider_load_rewrite_table_tables(
  THD *thd,
  MEM_ROOT *mem_root,
  TABLE_LIST *tables,
  SPIDER_RWTBL *rwtbl
) {
  int error_num;
  TABLE *table;
  TABLE_LIST *tables_next = tables->next_global;
  char table_key[MAX_KEY_LENGTH];
  SPIDER_RWTBLTBL *current = NULL;
  DBUG_ENTER("spider_load_rewrite_table_tables");
  DBUG_PRINT("info",("spider table_name=%s",
    SPIDER_TABLE_LIST_table_name_str(tables)));
  table = tables->table;
  if ((error_num = spider_get_sys_table_by_idx(table, table_key,
    table->s->primary_key, 1)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      goto error_get_sys_table_by_idx;
    }
    rwtbl->tt = NULL;
    DBUG_RETURN(FALSE);
  }
  while (!error_num)
  {
    SPIDER_RWTBLTBL *info;
    if (!(info = (SPIDER_RWTBLTBL *) alloc_root(mem_root, sizeof(SPIDER_RWTBLTBL))))
    {
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      goto error_alloc_info;
    }
    if (!current)
    {
      rwtbl->tt = info;
    } else {
      current->next = info;
    }
    current = info;
    if (spider_get_sys_rewrite_table_tables(table, info, mem_root))
    {
      goto error_get_sys_rewrite_table_tables;
    }
    if (
      spider_copy_sys_rewrite_columns(table, tables_next->table, 2) ||
      spider_load_rewrite_table_partitions(thd, mem_root, tables_next, info)
    ) {
      goto error_load_rewrite_table_partitions;
    }

    if ((error_num = spider_sys_index_next_same(table, table_key)))
    {
      if (
        error_num != HA_ERR_KEY_NOT_FOUND &&
        error_num != HA_ERR_END_OF_FILE
      ) {
        table->file->print_error(error_num, MYF(0));
        goto error_sys_index_next_same;
      }
    }
  }
  spider_sys_index_end(table);
  if (current)
  {
    current->next = NULL;
  }
  DBUG_RETURN(FALSE);

error_sys_index_next_same:
error_load_rewrite_table_partitions:
error_get_sys_rewrite_table_tables:
error_alloc_info:
  spider_sys_index_end(table);
error_get_sys_table_by_idx:
  DBUG_RETURN(TRUE);
}

bool spider_init_rewrite_cache(
  THD *thd
) {
  int error_num;
  TABLE_LIST rw_tbls, rw_tbl_tbls, rw_tbl_ptts, rw_tbl_sptts, *tables;
  TABLE *table;
  MEM_ROOT *mem_root;
  MDL_request mdl_request, mdl_request_cache;
  uint counter, roop_count;
  MYSQL_LOCK *lock;
  TABLE *tbl[4];
  Open_tables_backup open_tables_backup;
  bool no_record = FALSE;
  DBUG_ENTER("spider_init_rewrite_cache");
  mdl_request.init(MDL_key::USER_LOCK, "spider", "rw_table_mem_root",
    MDL_EXCLUSIVE, MDL_EXPLICIT);
  while (TRUE)
  {
    if (thd->mdl_context.acquire_lock(&mdl_request, 10))
    {
      if (spider_stmt_da_sql_errno(thd) == ER_LOCK_WAIT_TIMEOUT)
      {
        thd->clear_error();
        continue;
      } else {
        DBUG_RETURN(TRUE);
      }
    }
    break;
  }
  if (rw_table_mem_root)
  {
    mem_root = &rw_table_mem_root1;
    spider_rw_table_cache_tmp = &spider_rw_table_cache1;
  } else {
    mem_root = &rw_table_mem_root2;
    spider_rw_table_cache_tmp = &spider_rw_table_cache2;
  }
  /* reset */
  spider_free_rewrite_cache(spider_rw_table_cache_tmp);
  free_root(mem_root, MYF(MY_MARK_BLOCKS_FREE));
  tables = &rw_tbls;
  spider_sys_init_one_table(
    &rw_tbls,
    SPIDER_SYS_DB_NAME_STR, SPIDER_SYS_DB_NAME_LEN,
    SPIDER_SYS_RW_TBLS_TABLE_NAME_STR,
    SPIDER_SYS_RW_TBLS_TABLE_NAME_LEN,
    TL_READ
  );
  rw_tbls.next_global = &rw_tbl_tbls;
  spider_sys_init_one_table(
    &rw_tbl_tbls,
    SPIDER_SYS_DB_NAME_STR, SPIDER_SYS_DB_NAME_LEN,
    SPIDER_SYS_RW_TBL_TBLS_TABLE_NAME_STR,
    SPIDER_SYS_RW_TBL_TBLS_TABLE_NAME_LEN,
    TL_READ
  );
  rw_tbl_tbls.next_global = &rw_tbl_ptts;
  spider_sys_init_one_table(
    &rw_tbl_ptts,
    SPIDER_SYS_DB_NAME_STR, SPIDER_SYS_DB_NAME_LEN,
    SPIDER_SYS_RW_TBL_PTTS_TABLE_NAME_STR,
    SPIDER_SYS_RW_TBL_PTTS_TABLE_NAME_LEN,
    TL_READ
  );
  rw_tbl_ptts.next_global = &rw_tbl_sptts;
  spider_sys_init_one_table(
    &rw_tbl_sptts,
    SPIDER_SYS_DB_NAME_STR, SPIDER_SYS_DB_NAME_LEN,
    SPIDER_SYS_RW_TBL_SPTTS_TABLE_NAME_STR,
    SPIDER_SYS_RW_TBL_SPTTS_TABLE_NAME_LEN,
    TL_READ
  );
  if (spider_sys_open_tables(thd, &tables, &counter, &open_tables_backup))
  {
    goto error_open_tables;
  }
  DBUG_PRINT("info",("spider table_name_list_start"));
  roop_count = 0;
  while (tables)
  {
    DBUG_PRINT("info",("spider table_name=%s",
      SPIDER_TABLE_LIST_table_name_str(tables)));
    tbl[roop_count] = tables->table;
    DBUG_ASSERT(tables->table->reginfo.lock_type == TL_READ);
    tables = tables->next_global;
    ++roop_count;
  }
  DBUG_PRINT("info",("spider table_name_list_end"));
  DBUG_ASSERT(counter == roop_count);
  if (!(lock = spider_sys_lock_tables(thd, tbl, counter)))
  {
    goto error_lock_tables;
  }
  rw_tbls.table->use_all_columns();
  rw_tbl_tbls.table->use_all_columns();
  rw_tbl_ptts.table->use_all_columns();
  rw_tbl_sptts.table->use_all_columns();
  DBUG_PRINT("info",("spider table_name=%s",
    SPIDER_TABLE_LIST_table_name_str(&rw_tbls)));
  table = rw_tbls.table;
  if ((error_num = spider_sys_index_first(table, table->s->primary_key)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      goto error_sys_index_first;
    }
    no_record = TRUE;
  }
  while (!error_num)
  {
    SPIDER_RWTBL *info;
    if (!(info = (SPIDER_RWTBL *) alloc_root(mem_root, sizeof(SPIDER_RWTBL))))
    {
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      goto error_alloc_info;
    }
    if (spider_get_sys_rewrite_tables(table, info, mem_root))
    {
      goto error_get_sys_rewrite_tables;
    }
    if (
      spider_copy_sys_rewrite_columns(table, rw_tbl_tbls.table, 1) ||
      spider_load_rewrite_table_tables(thd, mem_root, &rw_tbl_tbls, info)
    ) {
      goto error_load_rewrite_table_tables;
    }
    info->sort = spider_calc_for_sort(2, info->db_name.str,
      info->table_name.str);
    if (push_dynamic(spider_rw_table_cache_tmp, (uchar *) info))
    {
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      goto error_push_dynamic;
    }

    if ((error_num = spider_sys_index_next(table)))
    {
      if (
        error_num != HA_ERR_KEY_NOT_FOUND &&
        error_num != HA_ERR_END_OF_FILE
      ) {
        table->file->print_error(error_num, MYF(0));
        goto error_sys_index_next;
      }
    }
  }
  if (!no_record)
  {
    spider_sys_index_end(table);
    my_qsort(
      (uchar *) dynamic_element(spider_rw_table_cache_tmp, 0, SPIDER_RWTBL *),
      spider_rw_table_cache_tmp->elements, sizeof(SPIDER_RWTBL),
      (qsort_cmp) spider_compare_for_sort);
    {
      uint old_elements = spider_rw_table_cache_tmp->max_element;
      freeze_size(spider_rw_table_cache_tmp);
      if (spider_rw_table_cache_tmp->max_element != old_elements)
      {
        if (rw_table_mem_root)
        {
          spider_free_mem_calc(spider_current_trx,
            spider_rw_table_cache1_id,
            old_elements *
            spider_rw_table_cache_tmp->size_of_element);
          spider_alloc_calc_mem(spider_current_trx,
            spider_rw_table_cache1,
            spider_rw_table_cache_tmp->max_element *
            spider_rw_table_cache_tmp->size_of_element);
        } else {
          spider_free_mem_calc(spider_current_trx,
            spider_rw_table_cache2_id,
            old_elements *
            spider_rw_table_cache_tmp->size_of_element);
          spider_alloc_calc_mem(spider_current_trx,
            spider_rw_table_cache2,
            spider_rw_table_cache_tmp->max_element *
            spider_rw_table_cache_tmp->size_of_element);
        }
      }
    }
  }

  spider_sys_unlock_tables(thd, lock);
  if ((error_num = ha_commit_trans(thd, FALSE)))
  {
    my_error(error_num, MYF(0));
    goto error_commit;
  }
  spider_sys_close_table(thd, &open_tables_backup);
  mdl_request_cache.init(MDL_key::USER_LOCK, "spider", "rw_table_cache",
    MDL_EXCLUSIVE, MDL_EXPLICIT);
  while (TRUE)
  {
    if (thd->mdl_context.acquire_lock(&mdl_request_cache, 10))
    {
      if (spider_stmt_da_sql_errno(thd) == ER_LOCK_WAIT_TIMEOUT)
      {
        thd->clear_error();
        continue;
      } else {
        thd->mdl_context.release_lock(mdl_request.ticket);
        DBUG_RETURN(TRUE);
      }
    }
    break;
  }
  spider_rw_table_cache = spider_rw_table_cache_tmp;
  thd->mdl_context.release_lock(mdl_request_cache.ticket);
  if (rw_table_mem_root)
  {
    rw_table_mem_root = FALSE;
  } else {
    rw_table_mem_root = TRUE;
  }
  rewrite_cache_initialized = TRUE;
  thd->mdl_context.release_lock(mdl_request.ticket);
  DBUG_RETURN(FALSE);

error_push_dynamic:
error_load_rewrite_table_tables:
error_get_sys_rewrite_tables:
error_sys_index_next:
error_alloc_info:
  spider_sys_index_end(table);
error_sys_index_first:
  spider_sys_unlock_tables(thd, lock);
error_commit:
error_lock_tables:
  (void) ha_rollback_trans(thd, FALSE);
  spider_sys_close_table(thd, &open_tables_backup);
error_open_tables:
  thd->mdl_context.release_lock(mdl_request.ticket);
  DBUG_RETURN(TRUE);
}

long long spider_flush_rewrite_cache_body(
  char *error
) {
  DBUG_ENTER("spider_flush_rewrite_cache_body");
  if (!audit_rewrite_initialized)
  {
    /* nothing to do */
    DBUG_RETURN(0);
  }
  if (spider_init_rewrite_cache(current_thd))
  {
    *error = 1;
    DBUG_RETURN(0);
  }
  DBUG_RETURN(1);
}

SPIDER_RWTBL *spider_rewrite_table_cache_compare(
  const LEX_CSTRING *db_name,
  const LEX_CSTRING *table_name,
  const struct charset_info_st *cs
) {
  uint32 roop_count;
  SPIDER_RWTBL *info;
  char db_buf[MAX_FIELD_WIDTH];
  char table_buf[MAX_FIELD_WIDTH];
  const char *db, *table;
  DBUG_ENTER("spider_rewrite_table_cache_compare");
  spider_string db_str(db_buf, MAX_FIELD_WIDTH, system_charset_info);
  spider_string table_str(table_buf, MAX_FIELD_WIDTH, system_charset_info);
  db_str.init_calc_mem(260);
  table_str.init_calc_mem(261);
  db_str.length(0);
  table_str.length(0);
  if (
    db_str.append(db_name->str, db_name->length, cs) ||
    table_str.append(table_name->str, table_name->length, cs)
  ) {
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    DBUG_RETURN(NULL);
  }

  db = db_str.c_ptr_safe();
  table = table_str.c_ptr_safe();
  DBUG_PRINT("info", ("spider db_name=%s", db));
  DBUG_PRINT("info", ("spider table_name=%s", table));

  for (roop_count = 0; roop_count < spider_rw_table_cache->elements;
    ++roop_count)
  {
    info = dynamic_element(spider_rw_table_cache, roop_count,
      SPIDER_RWTBL *);
    DBUG_PRINT("info", ("spider roop_count=%d", roop_count));
    DBUG_PRINT("info", ("spider info.db_name=%s", info->db_name.str));
    DBUG_PRINT("info", ("spider info.table_name=%s", info->table_name.str));
    if (
      !wild_case_compare(system_charset_info, db, info->db_name.str) &&
      !wild_case_compare(system_charset_info, table, info->table_name.str)
    ) {
      DBUG_PRINT("info", ("spider found"));
      DBUG_RETURN(info);
    }
  }
  DBUG_PRINT("info", ("spider not found"));
  DBUG_RETURN(NULL);
}

int spider_rewrite_insert_rewritten_tables(
  THD *thd,
  LEX_CSTRING *schema_name,
  LEX_CSTRING *table_name,
  const struct charset_info_st *cs,
  SPIDER_RWTBL *rwtbl
) {
  int error_num;
  Open_tables_backup open_tables_backup;
  TABLE *table;
  SPIDER_RWTBLTBL *info = rwtbl->tt;
  DBUG_ENTER("spider_rewrite_insert_rewritten_tables");
  if (
    !(table = spider_open_sys_table(
      thd, SPIDER_SYS_RWN_TBLS_TABLE_NAME_STR, SPIDER_SYS_RWN_TBLS_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  ) {
    DBUG_RETURN(error_num);
  }
  table->use_all_columns();
  empty_record(table);
  spider_store_rewritten_table_name(table, schema_name, table_name, cs);
  spider_store_rewritten_table_id(table, rwtbl);
  do {
    if ((error_num = spider_insert_rewritten_table(table, info)))
    {
      break;
    }
  } while ((info = info->next));
  spider_close_sys_table(thd, table, &open_tables_backup, TRUE);
  DBUG_RETURN(error_num);
}

int spider_rewrite_parse(
  THD *thd,
  mysql_event_query_rewrite *ev,
  spider_parse_sql **parse_sql_p
) {
  int error_num;
  spider_parse_sql *parse_sql;
  DBUG_ENTER("spider_rewrite_parse");
  if (ev->query_length > 0)
  {
    switch (ev->query[0])
    {
      case 'c':
      case 'C':
        if (spider_rw_table_cache->elements)
        {
          break;
        }
      default:
        *parse_sql_p = NULL;
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
    }
    parse_sql = *parse_sql_p;
    if (!parse_sql)
    {
      if (!(parse_sql = new spider_parse_sql()))
      {
        my_error(HA_ERR_OUT_OF_MEM, MYF(0));
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      if ((error_num = parse_sql->init(
        thd, ev->query, ev->query_length, ev->query_charset, ev->query_id)))
      {
        parse_sql->push_error(error_num);
        delete parse_sql;
        DBUG_RETURN(error_num);
      }
      my_pthread_setspecific_ptr(SPIDER_PARSE_SQL, parse_sql);
      *parse_sql_p = parse_sql;
    } else {
      parse_sql->reset(
        ev->query, ev->query_length, ev->query_charset, ev->query_id);
    }
    int retval;
    union YYSTYPE yylval;
    retval = parse_sql->get_next(&yylval);
    switch (retval)
    {
      case CREATE:
        if (
          (error_num = parse_sql->append_parsed_symbol(retval, &yylval))
        ) {
          parse_sql->push_error(error_num);
          DBUG_RETURN(error_num);
        }
        if (
          (error_num = spider_rewrite_parse_create(parse_sql))
        ) {
          DBUG_RETURN(error_num);
        }
        break;
      default:
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
    }
    DBUG_RETURN(0);
  }
  DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
}

int spider_rewrite_parse_create(
  spider_parse_sql *parse_sql
) {
  int retval, error_num;
  YYSTYPE yylval;
  DBUG_ENTER("spider_rewrite_parse_create");
  retval = parse_sql->get_next(&yylval);
  /* checking "or replace" */
  switch (retval)
  {
    case OR_SYM:
      retval = parse_sql->get_next(&yylval);
      if (retval != REPLACE)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if (unlikely((error_num = parse_sql->set_create_or_replace())))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      break;
    default:
      break;
  }
  /* checking "temporary" */
  switch (retval)
  {
    case TEMPORARY:
      if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      break;
    default:
      break;
  }
  switch (retval)
  {
    case TABLE_SYM:
      if (
        (error_num = parse_sql->append_create_or_replace_table()) ||
        (error_num = parse_sql->append_parsed_symbol(retval, &yylval))
      ) {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      if (
        (error_num = spider_rewrite_parse_create_table(parse_sql))
      ) {
        DBUG_RETURN(error_num);
      }
      break;
    default:
      DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
  }
  DBUG_RETURN(0);
}

int spider_rewrite_parse_create_table(
  spider_parse_sql *parse_sql
) {
  int retval, retval2, error_num;
  YYSTYPE yylval, yylval2;
  DBUG_ENTER("spider_rewrite_parse_create_table");
  retval = parse_sql->get_next(&yylval);
  /* checking "if not exists" */
  switch (retval)
  {
    case IF_SYM:
      retval = parse_sql->get_next(&yylval);
      if (retval != NOT_SYM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      retval = parse_sql->get_next(&yylval);
      if (retval != EXISTS)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num = parse_sql->append_if_not_exists()))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      break;
    default:
      break;
  }
  if (retval != IDENT_QUOTED && retval != IDENT && retval != ID_SYM)
  {
    parse_sql->push_syntax_error(yylval.simple_string);
    DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
  }
  retval2 = parse_sql->get_next(&yylval2);
  if (retval2 == (int) '.')
  {
    /* yylval is a database name */
    retval2 = parse_sql->get_next(&yylval2);
    if (retval2 != IDENT_QUOTED && retval2 != IDENT && retval2 != ID_SYM)
    {
      parse_sql->push_syntax_error(yylval.simple_string);
      DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
    }
    parse_sql->set_schema_name(yylval.lex_str);
    parse_sql->set_table_name(yylval2.lex_str);
    retval2 = parse_sql->get_next(&yylval2);
  } else {
    /* yylval is a table name */
    parse_sql->set_schema_name(parse_sql->thd->db);
    parse_sql->set_table_name(yylval.lex_str);
  }
  switch (retval2)
  {
    case '(':
      if ((error_num = parse_sql->append_parsed_symbol(retval2, &yylval2)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      break;
    case IGNORE_SYM:
    case REPLACE:
    case AS:
    case SELECT_SYM:
    case LIKE:
    default:
      my_printf_error(ER_SPIDER_NOT_SUPPORTED_NUM,
        ER_SPIDER_NOT_SUPPORTED_STR, MYF(0),
        "This SQL", "Spider Rewrite Plugin");
      DBUG_RETURN(ER_SPIDER_NOT_SUPPORTED_NUM);
  }
  retval = parse_sql->get_next(&yylval);
  switch (retval)
  {
    case IDENT:
    case IDENT_QUOTED:
    case ID_SYM:
    case PERIOD_SYM:
    case CHECK_SYM:
    case INDEX_SYM:
    case KEY_SYM:
    case FULLTEXT_SYM:
    case SPATIAL_SYM:
    case CONSTRAINT:
    case PRIMARY_SYM:
    case UNIQUE_SYM:
      break;
    case FOREIGN:
    case LIKE:
    case ')':
      my_printf_error(ER_SPIDER_NOT_SUPPORTED_NUM,
        ER_SPIDER_NOT_SUPPORTED_STR, MYF(0),
        "This SQL", "Spider Rewrite Plugin");
      DBUG_RETURN(ER_SPIDER_NOT_SUPPORTED_NUM);
    default:
      parse_sql->push_syntax_error(yylval.simple_string);
      DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
  }
  /* column definition */
  do {
    switch (retval)
    {
      case IDENT:
      case IDENT_QUOTED:
      case ID_SYM:
        if ((error_num = spider_rewrite_parse_column_definition(parse_sql, retval, yylval)))
        {
          DBUG_RETURN(error_num);
        }
        break;
      case PERIOD_SYM:
        if ((error_num = spider_rewrite_parse_period_definition(parse_sql, retval, yylval)))
        {
          DBUG_RETURN(error_num);
        }
        break;
      case CHECK_SYM:
        if ((error_num = spider_rewrite_parse_check_definition(parse_sql, retval, yylval)))
        {
          DBUG_RETURN(error_num);
        }
        break;
      case INDEX_SYM:
      case KEY_SYM:
      case FULLTEXT_SYM:
      case SPATIAL_SYM:
      case CONSTRAINT:
      case PRIMARY_SYM:
      case UNIQUE_SYM:
        if ((error_num = spider_rewrite_parse_index_definition(parse_sql, retval, yylval)))
        {
          DBUG_RETURN(error_num);
        }
        break;
      case FOREIGN:
        my_printf_error(ER_SPIDER_NOT_SUPPORTED_NUM,
          ER_SPIDER_NOT_SUPPORTED_STR, MYF(0),
          "This SQL", "Spider Rewrite Plugin");
        DBUG_RETURN(ER_SPIDER_NOT_SUPPORTED_NUM);
      default:
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
    }
    if (retval == ')')
    {
      break;
    }
    if (retval != ',')
    {
      parse_sql->push_syntax_error(yylval.simple_string);
      DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
    }
    retval = parse_sql->get_next(&yylval);
  } while (TRUE);
  retval = parse_sql->get_next(&yylval);
  do {
    switch (retval)
    {
      case IGNORE_SYM:
      case REPLACE:
      case AS:
      case SELECT_SYM:
        /* select statement */
        DBUG_RETURN(spider_rewrite_parse_create_table_select_statement(
          parse_sql, retval, yylval));
      case ';':
      case END_OF_INPUT:
        /* end of ddl */
        DBUG_RETURN(0);
      case PARTITION_SYM:
        if ((error_num = spider_rewrite_parse_create_table_partition(
          parse_sql, retval, yylval)))
        {
          DBUG_RETURN(error_num);
        }
        /* this function returns after get_next */
        break;
      default:
        if ((error_num = spider_rewrite_parse_create_table_table_option(
          parse_sql, retval, yylval)))
        {
          DBUG_RETURN(error_num);
        }
        retval = parse_sql->get_next(&yylval);
        break;
    }
  } while (TRUE);
}

int spider_rewrite_parse_nest_of_paren(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_nest_of_paren");
  do {
    if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
    {
      DBUG_RETURN(error_num);
    }
    retval = parse_sql->get_next(&yylval);
    if (retval == '(')
    {
      if ((error_num = spider_rewrite_parse_nest_of_paren(parse_sql, retval, yylval)))
      {
        DBUG_RETURN(error_num);
      }
    }
  } while (retval != ')');
  if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  retval = parse_sql->get_next(&yylval);
  DBUG_RETURN(0);
}

int spider_rewrite_parse_nest_of_paren_for_data_nodes(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_nest_of_paren_for_data_nodes");
  do {
    if ((error_num = parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
    {
      parse_sql->push_error(error_num);
      DBUG_RETURN(error_num);
    }
    retval = parse_sql->get_next(&yylval);
    if (retval == '(')
    {
      if ((error_num = spider_rewrite_parse_nest_of_paren_for_data_nodes(parse_sql, retval, yylval)))
      {
        DBUG_RETURN(error_num);
      }
    }
  } while (retval != ')');
  if ((error_num = parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  retval = parse_sql->get_next(&yylval);
  DBUG_RETURN(0);
}

int spider_rewrite_parse_column_definition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_column_definition");
  do {
    if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
    {
      parse_sql->push_error(error_num);
      DBUG_RETURN(error_num);
    }
    retval = parse_sql->get_next(&yylval);
    if (retval == '(')
    {
      if ((error_num = spider_rewrite_parse_nest_of_paren(parse_sql, retval, yylval)))
      {
        DBUG_RETURN(error_num);
      }
    }
  } while (retval != ',' && retval != ')');
  if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_rewrite_parse_index_definition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_index_definition");
  do {
    if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
    {
      parse_sql->push_error(error_num);
      DBUG_RETURN(error_num);
    }
    retval = parse_sql->get_next(&yylval);
    if (retval == FOREIGN)
    {
      my_printf_error(ER_SPIDER_NOT_SUPPORTED_NUM,
        ER_SPIDER_NOT_SUPPORTED_STR, MYF(0),
        "This SQL", "Spider Rewrite Plugin");
      DBUG_RETURN(ER_SPIDER_NOT_SUPPORTED_NUM);
    }
    if (retval == '(')
    {
      if ((error_num = spider_rewrite_parse_nest_of_paren(parse_sql, retval, yylval)))
      {
        DBUG_RETURN(error_num);
      }
    }
  } while (retval != ',' && retval != ')');
  if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_rewrite_parse_period_definition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_period_definition");
  do {
    if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
    {
      parse_sql->push_error(error_num);
      DBUG_RETURN(error_num);
    }
    retval = parse_sql->get_next(&yylval);
    if (retval == '(')
    {
      if ((error_num = spider_rewrite_parse_nest_of_paren(parse_sql, retval, yylval)))
      {
        DBUG_RETURN(error_num);
      }
    }
  } while (retval != ',' && retval != ')');
  if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_rewrite_parse_check_definition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_check_definition");
  do {
    if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
    {
      parse_sql->push_error(error_num);
      DBUG_RETURN(error_num);
    }
    retval = parse_sql->get_next(&yylval);
    if (retval == '(')
    {
      if ((error_num = spider_rewrite_parse_nest_of_paren(parse_sql, retval, yylval)))
      {
        DBUG_RETURN(error_num);
      }
    }
  } while (retval != ',' && retval != ')');
  if ((error_num = parse_sql->append_parsed_symbol(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_rewrite_parse_create_table_select_statement(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_create_table_select_statement");
  do {
    if ((error_num =
      parse_sql->append_parsed_symbol_for_spider_nodes_ex(retval, &yylval)))
    {
      parse_sql->push_error(error_num);
      DBUG_RETURN(error_num);
    }
    retval = parse_sql->get_next(&yylval);
  } while (retval != ';' && retval != END_OF_INPUT);
  DBUG_RETURN(0);
}

int spider_rewrite_parse_interval(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_interval");
  if ((error_num =
    parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  retval = parse_sql->get_next(&yylval);
  switch (retval)
  {
    case '+':
    case '-':
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      break;
    default:
      break;
  }
  switch (retval)
  {
    case NUM:
    case TEXT_STRING:
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      break;
    default:
      parse_sql->push_syntax_error(yylval.simple_string);
      DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
  }
  retval = parse_sql->get_next(&yylval);
  switch (retval)
  {
    case MICROSECOND_SYM:
    case SECOND_SYM:
    case MINUTE_SYM:
    case HOUR_SYM:
    case DAY_SYM:
    case WEEK_SYM:
    case MONTH_SYM:
    case QUARTER_SYM:
    case YEAR_SYM:
    case SECOND_MICROSECOND_SYM:
    case MINUTE_MICROSECOND_SYM:
    case MINUTE_SECOND_SYM:
    case HOUR_MICROSECOND_SYM:
    case HOUR_SECOND_SYM:
    case HOUR_MINUTE_SYM:
    case DAY_MICROSECOND_SYM:
    case DAY_SECOND_SYM:
    case DAY_MINUTE_SYM:
    case DAY_HOUR_SYM:
    case YEAR_MONTH_SYM:
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      break;
    default:
      parse_sql->push_syntax_error(yylval.simple_string);
      DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
  }
  DBUG_RETURN(0);
}

int spider_rewrite_parse_create_table_table_option(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  bool all_nodes = FALSE;
  DBUG_ENTER("spider_rewrite_parse_create_table_table_option");
  switch (retval)
  {
    case STORAGE_SYM:
      retval = parse_sql->get_next(&yylval);
      if (retval != ENGINE_SYM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_table_option_name_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      break;
    case DEFAULT:
      retval = parse_sql->get_next(&yylval);
      if (retval == CHAR_SYM)
      {
        retval = parse_sql->get_next(&yylval);
        if (retval != SET)
        {
          parse_sql->push_syntax_error(yylval.simple_string);
          DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
        }
        if ((error_num =
          parse_sql->append_table_option_character_set()))
        {
          parse_sql->push_error(error_num);
          DBUG_RETURN(error_num);
        }
        all_nodes = TRUE;
        break;
      }
      if (retval == CHARSET)
      {
        if ((error_num =
          parse_sql->append_table_option_character_set()))
        {
          parse_sql->push_error(error_num);
          DBUG_RETURN(error_num);
        }
        all_nodes = TRUE;
        break;
      }
      if (retval != COLLATE_SYM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_table_option_name(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      all_nodes = TRUE;
      break;
    case CHAR_SYM:
      retval = parse_sql->get_next(&yylval);
      if (retval != SET)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_table_option_character_set()))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      all_nodes = TRUE;
      break;
    case CHARSET:
      retval = parse_sql->get_next(&yylval);
      if ((error_num =
        parse_sql->append_table_option_character_set()))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      all_nodes = TRUE;
      break;
    case DATA_SYM:
      retval = parse_sql->get_next(&yylval);
      if (retval != DIRECTORY_SYM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_table_option_data_directory_for_data_nodes()))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      break;
    case INDEX_SYM:
      retval = parse_sql->get_next(&yylval);
      if (retval != DIRECTORY_SYM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_table_option_index_directory_for_data_nodes()))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      break;
    case WITH:
      retval = parse_sql->get_next(&yylval);
      if (retval != SYSTEM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      retval = parse_sql->get_next(&yylval);
      if (retval != VERSIONING_SYM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_table_option_with_system_versioning_for_data_nodes())
      ) {
        parse_sql->push_error(error_num);
      }
      DBUG_RETURN(error_num);
    default:
      if ((error_num =
        parse_sql->append_table_option_name_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      break;
  }
  retval = parse_sql->get_next(&yylval);
  if (retval == '=')
  {
    /* skip */
    retval = parse_sql->get_next(&yylval);
  }
  if (all_nodes)
  {
    if ((error_num = parse_sql->append_table_option_value(
      retval, &yylval))
    ) {
      parse_sql->push_error(error_num);
    }
  } else {
    if ((error_num = parse_sql->append_table_option_value_for_data_nodes(
      retval, &yylval))
    ) {
      parse_sql->push_error(error_num);
    }
  }
  DBUG_RETURN(error_num);
}

int spider_rewrite_parse_create_table_partition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
) {
  int error_num;
  DBUG_ENTER("spider_rewrite_parse_create_table_partition");
  if ((error_num =
    parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  retval = parse_sql->get_next(&yylval);
  if (retval != BY)
  {
    parse_sql->push_syntax_error(yylval.simple_string);
    DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
  }
  if ((error_num =
    parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
  {
    parse_sql->push_error(error_num);
    DBUG_RETURN(error_num);
  }
  retval = parse_sql->get_next(&yylval);
  switch (retval)
  {
    case LINEAR_SYM:
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      if (retval != HASH_SYM && retval != KEY_SYM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      /* fall through */
    case HASH_SYM:
    case KEY_SYM:
    case RANGE_SYM:
    case LIST_SYM:
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      if (retval != '(')
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num = spider_rewrite_parse_nest_of_paren_for_data_nodes(parse_sql, retval, yylval)))
      {
        DBUG_RETURN(error_num);
      }
      break;
    case SYSTEM_TIME_SYM:
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      if (retval == INTERVAL_SYM)
      {
        if ((error_num =
          spider_rewrite_parse_interval(parse_sql, retval, yylval)))
        {
          DBUG_RETURN(error_num);
        }
        retval = parse_sql->get_next(&yylval);
      }
      break;
    default:
      parse_sql->push_syntax_error(yylval.simple_string);
      DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
  }
  switch (retval)
  {
    case LIMIT:
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      if (retval != NUM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      break;
    default:
      break;
  }
  switch (retval)
  {
    case PARTITIONS_SYM:
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      if (retval != NUM)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      break;
    default:
      break;
  }
  switch (retval)
  {
    case SUBPARTITION_SYM:
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      if (retval != BY)
      {
        parse_sql->push_syntax_error(yylval.simple_string);
        DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      if ((error_num =
        parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
      {
        parse_sql->push_error(error_num);
        DBUG_RETURN(error_num);
      }
      retval = parse_sql->get_next(&yylval);
      switch (retval)
      {
        case LINEAR_SYM:
          if ((error_num =
            parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
          {
            parse_sql->push_error(error_num);
            DBUG_RETURN(error_num);
          }
          retval = parse_sql->get_next(&yylval);
          if (retval != HASH_SYM && retval != KEY_SYM)
          {
            parse_sql->push_syntax_error(yylval.simple_string);
            DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
          }
          /* fall through */
        case HASH_SYM:
        case KEY_SYM:
          if ((error_num =
            parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
          {
            parse_sql->push_error(error_num);
            DBUG_RETURN(error_num);
          }
          retval = parse_sql->get_next(&yylval);
          if (retval != '(')
          {
            parse_sql->push_syntax_error(yylval.simple_string);
            DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
          }
          if ((error_num = spider_rewrite_parse_nest_of_paren_for_data_nodes(
            parse_sql, retval, yylval)))
          {
            DBUG_RETURN(error_num);
          }
          retval = parse_sql->get_next(&yylval);
          break;
        default:
          parse_sql->push_syntax_error(yylval.simple_string);
          DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
      }
      switch (retval)
      {
        case SUBPARTITIONS_SYM:
          if ((error_num =
            parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
          {
            parse_sql->push_error(error_num);
            DBUG_RETURN(error_num);
          }
          retval = parse_sql->get_next(&yylval);
          if (retval != NUM)
          {
            parse_sql->push_syntax_error(yylval.simple_string);
            DBUG_RETURN(ER_SPIDER_SYNTAX_NUM);
          }
          if ((error_num =
            parse_sql->append_parsed_symbol_for_data_nodes(retval, &yylval)))
          {
            parse_sql->push_error(error_num);
            DBUG_RETURN(error_num);
          }
          retval = parse_sql->get_next(&yylval);
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
  if (retval == '(')
  {
    if ((error_num = spider_rewrite_parse_nest_of_paren_for_data_nodes(
      parse_sql, retval, yylval)))
    {
      DBUG_RETURN(error_num);
    }
    retval = parse_sql->get_next(&yylval);
  }
  /* remains the last parsed part */
  DBUG_RETURN(0);
}

static int spider_audit_rewrite_init(
  void *p
) {
  int error_num;
  THD *thd = current_thd;
  DBUG_ENTER("spider_audit_rewrite_init");
  if (pthread_key_create(&SPIDER_PARSE_SQL, NULL))
  {
    error_num = HA_ERR_OUT_OF_MEM;
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    goto error_pthread_key_create;
  }
  if(
    SPD_INIT_DYNAMIC_ARRAY2(&spider_rw_table_cache1, sizeof(SPIDER_RWTBL),
      NULL, 64, 64, MYF(MY_WME))
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    goto error_rw_table_cache1_array_init;
  }
  spider_alloc_calc_mem_init(spider_rw_table_cache1, 258);
  spider_alloc_calc_mem(NULL,
    spider_rw_table_cache1,
    spider_rw_table_cache1.max_element *
    spider_rw_table_cache1.size_of_element);
  if(
    SPD_INIT_DYNAMIC_ARRAY2(&spider_rw_table_cache2, sizeof(SPIDER_RWTBL),
      NULL, 64, 64, MYF(MY_WME))
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    goto error_rw_table_cache2_array_init;
  }
  spider_alloc_calc_mem_init(spider_rw_table_cache2, 259);
  spider_alloc_calc_mem(NULL,
    spider_rw_table_cache2,
    spider_rw_table_cache2.max_element *
    spider_rw_table_cache2.size_of_element);
  SPD_INIT_ALLOC_ROOT(&rw_table_mem_root1, 1024, 0, MYF(MY_WME));
  SPD_INIT_ALLOC_ROOT(&rw_table_mem_root2, 1024, 0, MYF(MY_WME));
  if (thd)
  {
    if (spider_init_rewrite_cache(thd))
    {
      error_num = spider_stmt_da_sql_errno(thd);
      goto error_init_rewrite_cache;
    }
  }
  audit_rewrite_initialized = TRUE;
  DBUG_RETURN(0);

error_init_rewrite_cache:
  free_root(&rw_table_mem_root2, MYF(0));
  free_root(&rw_table_mem_root1, MYF(0));
  spider_free_rewrite_cache(&spider_rw_table_cache2);
  spider_free_mem_calc(NULL,
    spider_rw_table_cache2_id,
    spider_rw_table_cache2.max_element *
    spider_rw_table_cache2.size_of_element);
  delete_dynamic(&spider_rw_table_cache2);
error_rw_table_cache2_array_init:
  spider_free_rewrite_cache(&spider_rw_table_cache1);
  spider_free_mem_calc(NULL,
    spider_rw_table_cache1_id,
    spider_rw_table_cache1.max_element *
    spider_rw_table_cache1.size_of_element);
  delete_dynamic(&spider_rw_table_cache1);
error_rw_table_cache1_array_init:
  pthread_key_delete(SPIDER_PARSE_SQL);
error_pthread_key_create:
  DBUG_RETURN(error_num);
}

static int spider_audit_rewrite_deinit(
  void *p
) {
  DBUG_ENTER("spider_audit_rewrite_deinit");
  if (audit_rewrite_initialized)
  {
    spider_free_rewrite_cache(&spider_rw_table_cache2);
    spider_free_mem_calc(NULL,
      spider_rw_table_cache2_id,
      spider_rw_table_cache2.max_element *
      spider_rw_table_cache2.size_of_element);
    delete_dynamic(&spider_rw_table_cache2);
    spider_free_rewrite_cache(&spider_rw_table_cache1);
    spider_free_mem_calc(NULL,
      spider_rw_table_cache1_id,
      spider_rw_table_cache1.max_element *
      spider_rw_table_cache1.size_of_element);
    delete_dynamic(&spider_rw_table_cache1);
    pthread_key_delete(SPIDER_PARSE_SQL);
    free_root(&rw_table_mem_root1, MYF(0));
    free_root(&rw_table_mem_root2, MYF(0));
    audit_rewrite_initialized = FALSE;
  }
  DBUG_RETURN(0);
}

static void spider_audit_rewrite_release_thd(
  MYSQL_THD thd
) {
  spider_parse_sql *parse_sql = my_pthread_getspecific_ptr(
    spider_parse_sql *, SPIDER_PARSE_SQL);
  DBUG_ENTER("spider_audit_rewrite_release_thd");
  if (parse_sql)
  {
    delete parse_sql;
    my_pthread_setspecific_ptr(SPIDER_PARSE_SQL, NULL);
  }
  DBUG_VOID_RETURN;
}

static void spider_audit_rewrite_event_notify(
  MYSQL_THD thd,
  unsigned int event_class,
  const void *event
) {
  int error_num;
  mysql_event_query_rewrite *ev = (mysql_event_query_rewrite *) event;
  spider_parse_sql *parse_sql = my_pthread_getspecific_ptr(
    spider_parse_sql *, SPIDER_PARSE_SQL);
  MDL_request mdl_request_cache;
  SPIDER_RWTBL *rwtbl;
  DBUG_ENTER("spider_audit_rewrite_event_notify");
  switch (ev->event_subclass)
  {
    case MYSQL_AUDIT_QUERY_REWRITE_QUERY:
      if (!rewrite_cache_initialized)
      {
        if (spider_init_rewrite_cache(thd))
        {
          DBUG_VOID_RETURN;
        }
      }
      error_num = spider_rewrite_parse(thd, ev, &parse_sql);
      if (likely(!parse_sql))
      {
        DBUG_VOID_RETURN;
      }
      parse_sql->end_parse();
      if (error_num)
      {
        DBUG_VOID_RETURN;
      }
      /* get lock */
      mdl_request_cache.init(MDL_key::USER_LOCK, "spider", "rw_table_cache",
        MDL_SHARED, MDL_EXPLICIT);
      while (TRUE)
      {
        if (thd->mdl_context.acquire_lock(&mdl_request_cache, 10))
        {
          if (spider_stmt_da_sql_errno(thd) == ER_LOCK_WAIT_TIMEOUT)
          {
            thd->clear_error();
            continue;
          } else {
            DBUG_VOID_RETURN;
          }
        }
        break;
      }
      if (!(rwtbl = spider_rewrite_table_cache_compare(
        &parse_sql->schema_name, &parse_sql->table_name, parse_sql->cs)))
      {
        thd->mdl_context.release_lock(mdl_request_cache.ticket);
        DBUG_VOID_RETURN;
      }
      if ((error_num = parse_sql->append_spider_table_for_spider_nodes(
        rwtbl->tt)))
      {
        parse_sql->push_error(error_num);
        thd->mdl_context.release_lock(mdl_request_cache.ticket);
        DBUG_VOID_RETURN;
      }
      if ((error_num = parse_sql->create_share_from_table(
        rwtbl->tt)))
      {
        parse_sql->push_error(error_num);
        thd->mdl_context.release_lock(mdl_request_cache.ticket);
        DBUG_VOID_RETURN;
      }
      if ((error_num = parse_sql->get_conn()))
      {
        parse_sql->push_error(error_num);
        thd->mdl_context.release_lock(mdl_request_cache.ticket);
        DBUG_VOID_RETURN;
      }
      if ((error_num = parse_sql->send_sql_to_data_nodes()))
      {
        parse_sql->push_error(error_num);
        thd->mdl_context.release_lock(mdl_request_cache.ticket);
        DBUG_VOID_RETURN;
      }
      if (spider_rewrite_insert_rewritten_tables(
        thd, &parse_sql->schema_name, &parse_sql->table_name, parse_sql->cs,
        rwtbl))
      {
        thd->mdl_context.release_lock(mdl_request_cache.ticket);
        DBUG_VOID_RETURN;
      }
      thd->mdl_context.release_lock(mdl_request_cache.ticket);
      ev->flags = MYSQL_AUDIT_QUERY_REWRITE_FOR_EXECUTE | MYSQL_AUDIT_QUERY_REWRITE_FOR_GENERAL_LOG;
      ev->rewritten_query = parse_sql->get_query_for_spider_node(
        &ev->rewritten_query_length);
      ev->found_semicolon = parse_sql->get_found_semicolon();
      break;
    case MYSQL_AUDIT_QUERY_REWRITE_SLOW:
      if (unlikely(parse_sql && parse_sql->get_query_id() == ev->query_id))
      {
        ev->flags = MYSQL_AUDIT_QUERY_REWRITE_FOR_SLOW_LOG;
        ev->rewritten_query = parse_sql->get_query_for_spider_node(
          &ev->rewritten_query_length);
      }
      break;
    case MYSQL_AUDIT_QUERY_REWRITE_BINLOG:
      /* nothing to do */
      break;
    default:
      /* unknown event class */
      DBUG_ASSERT(FALSE);
      break;
  }
  DBUG_VOID_RETURN;
}

static struct st_mysql_audit spider_audit_rewrite_descriptor =
{
  MYSQL_AUDIT_INTERFACE_VERSION,
  spider_audit_rewrite_release_thd,
  spider_audit_rewrite_event_notify,
  { MYSQL_AUDIT_QUERY_REWRITE_CLASSMASK }
};

struct st_mysql_plugin spider_audit_rewrite =
{
  MYSQL_AUDIT_PLUGIN,
  &spider_audit_rewrite_descriptor,
  "SPIDER_REWRITE",
  "Kentoku Shiba & MariaDB corp",
  "Spider query rewrite",
  PLUGIN_LICENSE_GPL,
  spider_audit_rewrite_init,
  spider_audit_rewrite_deinit,
  0x0001,
  NULL,
  NULL,
  NULL,
  0,
};

#ifdef MARIADB_BASE_VERSION
struct st_maria_plugin spider_audit_rewrite_maria =
{
  MYSQL_AUDIT_PLUGIN,
  &spider_audit_rewrite_descriptor,
  "SPIDER_REWRITE",
  "Kentoku Shiba & MariaDB corp",
  "Spider query rewrite",
  PLUGIN_LICENSE_GPL,
  spider_audit_rewrite_init,
  spider_audit_rewrite_deinit,
  0x0001,
  NULL,
  NULL,
  "0.1.1",
  MariaDB_PLUGIN_MATURITY_BETA,
};
#endif
#else
long long spider_flush_rewrite_cache_body(
  char *error
) {
  DBUG_ENTER("spider_flush_rewrite_cache_body");
  my_printf_error(ER_SPIDER_NOT_SUPPORTED_NUM,
    ER_SPIDER_NOT_SUPPORTED_STR, MYF(0),
    "This function", "this version");
  *error = 1;
  DBUG_RETURN(0);
}
#endif
