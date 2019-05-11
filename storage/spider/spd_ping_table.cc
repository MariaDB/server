/* Copyright (C) 2009-2015 Kentoku Shiba

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

#define MYSQL_SERVER 1
#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "sql_acl.h"
#endif
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "ha_spider.h"
#include "spd_db_conn.h"
#include "spd_trx.h"
#include "spd_conn.h"
#include "spd_sys_table.h"
#include "spd_table.h"
#include "spd_ping_table.h"
#include "spd_direct_sql.h"
#include "spd_udf.h"
#include "spd_malloc.h"

extern bool volatile *spd_abort_loop;

extern handlerton *spider_hton_ptr;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key spd_key_mutex_mon_list_caller;
extern PSI_mutex_key spd_key_mutex_mon_list_receptor;
extern PSI_mutex_key spd_key_mutex_mon_list_monitor;
extern PSI_mutex_key spd_key_mutex_mon_list_update_status;
extern PSI_mutex_key spd_key_mutex_mon_table_cache;
#endif

HASH *spider_udf_table_mon_list_hash;
uint spider_udf_table_mon_list_hash_id;
const char *spider_udf_table_mon_list_hash_func_name;
const char *spider_udf_table_mon_list_hash_file_name;
ulong spider_udf_table_mon_list_hash_line_no;
pthread_mutex_t *spider_udf_table_mon_mutexes;
pthread_cond_t *spider_udf_table_mon_conds;

pthread_mutex_t spider_mon_table_cache_mutex;
DYNAMIC_ARRAY spider_mon_table_cache;
uint spider_mon_table_cache_id;
const char *spider_mon_table_cache_func_name;
const char *spider_mon_table_cache_file_name;
ulong spider_mon_table_cache_line_no;
volatile ulonglong spider_mon_table_cache_version = 0;
volatile ulonglong spider_mon_table_cache_version_req = 1;

SPIDER_TABLE_MON_LIST *spider_get_ping_table_mon_list(
  SPIDER_TRX *trx,
  THD *thd,
  spider_string *str,
  uint conv_name_length,
  int link_idx,
  uint32 server_id,
  bool need_lock,
  int *error_num
) {
  uint mutex_hash;
  SPIDER_TABLE_MON_LIST *table_mon_list;
  MEM_ROOT mem_root;
  ulonglong mon_table_cache_version;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value;
#endif
  DBUG_ENTER("spider_get_ping_table_mon_list");
  if (spider_mon_table_cache_version != spider_mon_table_cache_version_req)
  {
    SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
    if ((*error_num = spider_init_ping_table_mon_cache(thd, &mem_root,
      need_lock)))
    {
      free_root(&mem_root, MYF(0));
      goto error;
    }
    free_root(&mem_root, MYF(0));
  }

  mutex_hash = spider_udf_calc_hash(str->c_ptr(),
    spider_param_udf_table_mon_mutex_count());
  DBUG_PRINT("info",("spider hash key=%s", str->c_ptr()));
  DBUG_PRINT("info",("spider hash key length=%u", str->length()));
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  hash_value = my_calc_hash(
    &spider_udf_table_mon_list_hash[mutex_hash],
    (uchar*) str->c_ptr(), str->length());
#endif
  pthread_mutex_lock(&spider_udf_table_mon_mutexes[mutex_hash]);
  mon_table_cache_version = (ulonglong) spider_mon_table_cache_version;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(table_mon_list = (SPIDER_TABLE_MON_LIST *)
    my_hash_search_using_hash_value(
      &spider_udf_table_mon_list_hash[mutex_hash], hash_value,
      (uchar*) str->c_ptr(), str->length())) ||
      table_mon_list->mon_table_cache_version != mon_table_cache_version
  )
#else
  if (!(table_mon_list = (SPIDER_TABLE_MON_LIST *) my_hash_search(
    &spider_udf_table_mon_list_hash[mutex_hash],
    (uchar*) str->c_ptr(), str->length())) ||
    table_mon_list->mon_table_cache_version != mon_table_cache_version
  )
#endif
  {
    if (
      table_mon_list &&
      table_mon_list->mon_table_cache_version != mon_table_cache_version
    )
      spider_release_ping_table_mon_list_loop(mutex_hash, table_mon_list);

    if (!(table_mon_list = spider_get_ping_table_tgt(thd, str->c_ptr(),
      conv_name_length, link_idx, server_id, str, need_lock, error_num)))
    {
      pthread_mutex_unlock(&spider_udf_table_mon_mutexes[mutex_hash]);
      goto error;
    }
    table_mon_list->mutex_hash = mutex_hash;
    table_mon_list->mon_table_cache_version = mon_table_cache_version;
    uint old_elements =
      spider_udf_table_mon_list_hash[mutex_hash].array.max_element;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    table_mon_list->key_hash_value = hash_value;
#endif
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    if (my_hash_insert_with_hash_value(
      &spider_udf_table_mon_list_hash[mutex_hash],
      hash_value, (uchar*) table_mon_list))
#else
    if (my_hash_insert(&spider_udf_table_mon_list_hash[mutex_hash],
      (uchar*) table_mon_list))
#endif
    {
      spider_ping_table_free_mon_list(table_mon_list);
      *error_num = HA_ERR_OUT_OF_MEM;
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      pthread_mutex_unlock(&spider_udf_table_mon_mutexes[mutex_hash]);
      goto error;
    }
    if (spider_udf_table_mon_list_hash[mutex_hash].array.max_element >
      old_elements)
    {
      spider_alloc_calc_mem(spider_current_trx,
        spider_udf_table_mon_list_hash,
        (spider_udf_table_mon_list_hash[mutex_hash].array.max_element -
        old_elements) *
        spider_udf_table_mon_list_hash[mutex_hash].array.size_of_element);
    }
  }
  table_mon_list->use_count++;
  DBUG_PRINT("info",("spider table_mon_list->use_count=%d",
    table_mon_list->use_count));
  pthread_mutex_unlock(&spider_udf_table_mon_mutexes[mutex_hash]);
  DBUG_RETURN(table_mon_list);

error:
  DBUG_RETURN(NULL);
}

void spider_free_ping_table_mon_list(
  SPIDER_TABLE_MON_LIST *table_mon_list
) {
  DBUG_ENTER("spider_free_ping_table_mon_list");
  pthread_mutex_lock(&spider_udf_table_mon_mutexes[
    table_mon_list->mutex_hash]);
  table_mon_list->use_count--;
  DBUG_PRINT("info",("spider table_mon_list->use_count=%d", table_mon_list->use_count));
  if (!table_mon_list->use_count)
    pthread_cond_broadcast(&spider_udf_table_mon_conds[
      table_mon_list->mutex_hash]);
  pthread_mutex_unlock(&spider_udf_table_mon_mutexes[
    table_mon_list->mutex_hash]);
  DBUG_VOID_RETURN;
}

void spider_release_ping_table_mon_list_loop(
  uint mutex_hash,
  SPIDER_TABLE_MON_LIST *table_mon_list
) {
  DBUG_ENTER("spider_release_ping_table_mon_list_loop");
#ifdef HASH_UPDATE_WITH_HASH_VALUE
  my_hash_delete_with_hash_value(&spider_udf_table_mon_list_hash[mutex_hash],
    table_mon_list->key_hash_value, (uchar*) table_mon_list);
#else
  my_hash_delete(&spider_udf_table_mon_list_hash[mutex_hash],
    (uchar*) table_mon_list);
#endif
  while (TRUE)
  {
    if (table_mon_list->use_count)
      pthread_cond_wait(&spider_udf_table_mon_conds[mutex_hash],
        &spider_udf_table_mon_mutexes[mutex_hash]);
    else {
      spider_ping_table_free_mon_list(table_mon_list);
      break;
    }
  }
  DBUG_VOID_RETURN;
}

void spider_release_ping_table_mon_list(
  const char *conv_name,
  uint conv_name_length,
  int link_idx
) {
  uint mutex_hash;
  SPIDER_TABLE_MON_LIST *table_mon_list;
  char link_idx_str[SPIDER_SQL_INT_LEN];
  int link_idx_str_length;
  DBUG_ENTER("spider_release_ping_table_mon_list");
  DBUG_PRINT("info", ("spider conv_name=%s", conv_name));
  DBUG_PRINT("info", ("spider conv_name_length=%u", conv_name_length));
  DBUG_PRINT("info", ("spider link_idx=%d", link_idx));
  link_idx_str_length = my_sprintf(link_idx_str, (link_idx_str, "%010d",
    link_idx));
#if defined(_MSC_VER) || defined(__SUNPRO_CC)
  spider_string conv_name_str(conv_name_length + link_idx_str_length + 1);
  conv_name_str.set_charset(system_charset_info);
#else
  char buf[conv_name_length + link_idx_str_length + 1];
  spider_string conv_name_str(buf, conv_name_length + link_idx_str_length + 1,
    system_charset_info);
#endif
  conv_name_str.init_calc_mem(134);
  conv_name_str.length(0);
  conv_name_str.q_append(conv_name, conv_name_length);
  conv_name_str.q_append(link_idx_str, link_idx_str_length);

  mutex_hash = spider_udf_calc_hash(conv_name_str.c_ptr_safe(),
    spider_param_udf_table_mon_mutex_count());
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value = my_calc_hash(
    &spider_udf_table_mon_list_hash[mutex_hash],
    (uchar*) conv_name_str.c_ptr(), conv_name_str.length());
#endif
  pthread_mutex_lock(&spider_udf_table_mon_mutexes[mutex_hash]);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if ((table_mon_list = (SPIDER_TABLE_MON_LIST *)
    my_hash_search_using_hash_value(
      &spider_udf_table_mon_list_hash[mutex_hash], hash_value,
      (uchar*) conv_name_str.c_ptr(), conv_name_str.length())))
#else
  if ((table_mon_list = (SPIDER_TABLE_MON_LIST *) my_hash_search(
    &spider_udf_table_mon_list_hash[mutex_hash],
    (uchar*) conv_name_str.c_ptr(), conv_name_str.length())))
#endif
    spider_release_ping_table_mon_list_loop(mutex_hash, table_mon_list);
  pthread_mutex_unlock(&spider_udf_table_mon_mutexes[mutex_hash]);
  DBUG_VOID_RETURN;
}

int spider_get_ping_table_mon(
  THD *thd,
  SPIDER_TABLE_MON_LIST *table_mon_list,
  char *name,
  uint name_length,
  int link_idx,
  uint32 server_id,
  MEM_ROOT *mem_root,
  bool need_lock
) {
  int error_num;
  TABLE *table_link_mon = NULL;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  char table_key[MAX_KEY_LENGTH];
  SPIDER_TABLE_MON *table_mon, *table_mon_prev = NULL;
  SPIDER_SHARE *tmp_share;
  char **tmp_connect_info, *tmp_ptr;
  uint *tmp_connect_info_length;
  long *tmp_long;
  longlong *tmp_longlong;
  int list_size = 0;
  DBUG_ENTER("spider_get_ping_table_mon");

  if (
    !(table_link_mon = spider_open_sys_table(
      thd, SPIDER_SYS_LINK_MON_TABLE_NAME_STR,
      SPIDER_SYS_LINK_MON_TABLE_NAME_LEN, FALSE, &open_tables_backup,
      need_lock, &error_num))
  ) {
    my_error(error_num, MYF(0));
    goto error;
  }
  spider_store_tables_name(table_link_mon, name, name_length);
  spider_store_tables_link_idx(table_link_mon, link_idx);
  if (!(error_num = spider_ping_table_cache_compare(table_link_mon, mem_root)))
    goto create_table_mon;
  if (error_num == HA_ERR_OUT_OF_MEM)
    goto error;
  if ((tmp_ptr = strstr(name, "#P#")))
  {
    *tmp_ptr = '\0';
    spider_store_tables_name(table_link_mon, name, strlen(name));
    *tmp_ptr = '#';
    if (!(error_num = spider_ping_table_cache_compare(table_link_mon,
      mem_root)))
      goto create_table_mon;
    if (error_num == HA_ERR_OUT_OF_MEM)
      goto error;
  }
  error_num = HA_ERR_KEY_NOT_FOUND;
  table_link_mon->file->print_error(error_num, MYF(0));
  goto error;

create_table_mon:
  if ((error_num = spider_get_sys_table_by_idx(table_link_mon, table_key,
    table_link_mon->s->primary_key, 3)))
  {
    table_link_mon->file->print_error(error_num, MYF(0));
    goto error;
  }

  do {
    if (!(table_mon = (SPIDER_TABLE_MON *)
      spider_bulk_malloc(spider_current_trx, 35, MYF(MY_WME | MY_ZEROFILL),
        &table_mon, sizeof(SPIDER_TABLE_MON),
        &tmp_share, sizeof(SPIDER_SHARE),
        &tmp_connect_info, sizeof(char *) * SPIDER_TMP_SHARE_CHAR_PTR_COUNT,
        &tmp_connect_info_length, sizeof(uint) * SPIDER_TMP_SHARE_UINT_COUNT,
        &tmp_long, sizeof(long) * SPIDER_TMP_SHARE_LONG_COUNT,
        &tmp_longlong, sizeof(longlong) * SPIDER_TMP_SHARE_LONGLONG_COUNT,
        NullS))
    ) {
      spider_sys_index_end(table_link_mon);
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      goto error;
    }
    spider_set_tmp_share_pointer(tmp_share, tmp_connect_info,
      tmp_connect_info_length, tmp_long, tmp_longlong);
    tmp_share->link_statuses[0] = -1;
    table_mon->share = tmp_share;
    if (table_mon_prev)
      table_mon_prev->next = table_mon;
    else
      table_mon_list->first = table_mon;
    table_mon_prev = table_mon;
    if (
      (error_num = spider_get_sys_link_mon_server_id(
        table_link_mon, &table_mon->server_id, mem_root)) ||
      (error_num = spider_get_sys_link_mon_connect_info(
        table_link_mon, tmp_share, 0, mem_root))
    ) {
      table_link_mon->file->print_error(error_num, MYF(0));
      spider_sys_index_end(table_link_mon);
      goto error;
    }
    if (
      (error_num = spider_set_connect_info_default(
        tmp_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
        NULL,
        NULL,
#endif
        NULL
      )) ||
      (error_num = spider_set_connect_info_default_dbtable(
        tmp_share, name, name_length
      )) ||
      (error_num = spider_create_conn_keys(tmp_share))
    ) {
      spider_sys_index_end(table_link_mon);
      goto error;
    }
    DBUG_PRINT("info",("spider table_mon->server_id=%u",
      table_mon->server_id));
    DBUG_PRINT("info",("spider server_id=%u", server_id));
    if (table_mon->server_id == server_id)
      table_mon_list->current = table_mon;
    list_size++;
    error_num = spider_sys_index_next_same(table_link_mon, table_key);
  } while (error_num == 0);
  spider_sys_index_end(table_link_mon);
  spider_close_sys_table(thd, table_link_mon,
    &open_tables_backup, need_lock);
  table_link_mon = NULL;
  table_mon_list->list_size = list_size;

  if (!table_mon_list->current)
  {
    error_num = ER_SPIDER_UDF_PING_TABLE_NO_SERVER_ID_NUM;
    my_printf_error(ER_SPIDER_UDF_PING_TABLE_NO_SERVER_ID_NUM,
      ER_SPIDER_UDF_PING_TABLE_NO_SERVER_ID_STR, MYF(0));
    goto error;
  }

  DBUG_RETURN(0);

error:
  if (table_link_mon)
    spider_close_sys_table(thd, table_link_mon,
      &open_tables_backup, need_lock);
  table_mon = table_mon_list->first;
  table_mon_list->first = NULL;
  table_mon_list->current = NULL;
  while (table_mon)
  {
    spider_free_tmp_share_alloc(table_mon->share);
    table_mon_prev = table_mon->next;
    spider_free(spider_current_trx, table_mon, MYF(0));
    table_mon = table_mon_prev;
  }
  DBUG_RETURN(error_num);
}

SPIDER_TABLE_MON_LIST *spider_get_ping_table_tgt(
  THD *thd,
  char *name,
  uint name_length,
  int link_idx,
  uint32 server_id,
  spider_string *str,
  bool need_lock,
  int *error_num
) {
  TABLE *table_tables = NULL;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  char table_key[MAX_KEY_LENGTH];

  SPIDER_TABLE_MON_LIST *table_mon_list = NULL;
  SPIDER_SHARE *tmp_share;
  char **tmp_connect_info;
  uint *tmp_connect_info_length;
  long *tmp_long;
  longlong *tmp_longlong;
  char *key_str;
  MEM_ROOT mem_root;
  DBUG_ENTER("spider_get_ping_table_tgt");

  SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
  if (!(table_mon_list = (SPIDER_TABLE_MON_LIST *)
    spider_bulk_malloc(spider_current_trx, 36, MYF(MY_WME | MY_ZEROFILL),
      &table_mon_list, sizeof(SPIDER_TABLE_MON_LIST),
      &tmp_share, sizeof(SPIDER_SHARE),
      &tmp_connect_info, sizeof(char *) * SPIDER_TMP_SHARE_CHAR_PTR_COUNT,
      &tmp_connect_info_length, sizeof(uint) * SPIDER_TMP_SHARE_UINT_COUNT,
      &tmp_long, sizeof(long) * SPIDER_TMP_SHARE_LONG_COUNT,
      &tmp_longlong, sizeof(longlong) * SPIDER_TMP_SHARE_LONGLONG_COUNT,
      &key_str, str->length() + 1,
      NullS))
  ) {
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    goto error;
  }
  spider_set_tmp_share_pointer(tmp_share, tmp_connect_info,
    tmp_connect_info_length, tmp_long, tmp_longlong);
  table_mon_list->share = tmp_share;
  table_mon_list->key = key_str;
  table_mon_list->key_length = str->length();
  memcpy(key_str, str->ptr(), table_mon_list->key_length);
  tmp_share->access_charset = thd->variables.character_set_client;

  if (
    !(table_tables = spider_open_sys_table(
      thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
      SPIDER_SYS_TABLES_TABLE_NAME_LEN, FALSE, &open_tables_backup, need_lock,
      error_num))
  ) {
    my_error(*error_num, MYF(0));
    goto error;
  }
  spider_store_tables_name(table_tables, name, name_length);
  spider_store_tables_link_idx(table_tables, link_idx);
  if (
    (*error_num = spider_check_sys_table(table_tables, table_key)) ||
    (*error_num = spider_get_sys_tables_connect_info(
      table_tables, tmp_share, 0, &mem_root)) ||
    (*error_num = spider_get_sys_tables_link_status(
      table_tables, tmp_share, 0, &mem_root))
  ) {
    table_tables->file->print_error(*error_num, MYF(0));
    goto error;
  }
  spider_close_sys_table(thd, table_tables,
    &open_tables_backup, need_lock);
  table_tables = NULL;

  if (
    (*error_num = spider_set_connect_info_default(
      tmp_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
      NULL,
      NULL,
#endif
      NULL
    )) ||
    (*error_num = spider_set_connect_info_default_dbtable(
      tmp_share, name, name_length
    )) ||
    (*error_num = spider_create_conn_keys(tmp_share)) ||
/*
    (*error_num = spider_db_create_table_names_str(tmp_share)) ||
*/
    (*error_num = spider_get_ping_table_mon(
      thd, table_mon_list, name, name_length, link_idx, server_id, &mem_root,
      need_lock))
  )
    goto error;

  if (tmp_share->link_statuses[0] == SPIDER_LINK_STATUS_NG)
    table_mon_list->mon_status = SPIDER_LINK_MON_NG;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&table_mon_list->caller_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_mon_list_caller,
    &table_mon_list->caller_mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_caller_mutex_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&table_mon_list->receptor_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_mon_list_receptor,
    &table_mon_list->receptor_mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_receptor_mutex_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&table_mon_list->monitor_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_mon_list_monitor,
    &table_mon_list->monitor_mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_monitor_mutex_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&table_mon_list->update_status_mutex,
    MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_mon_list_update_status,
    &table_mon_list->update_status_mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_update_status_mutex_init;
  }

  free_root(&mem_root, MYF(0));
  DBUG_RETURN(table_mon_list);

error_update_status_mutex_init:
  pthread_mutex_destroy(&table_mon_list->monitor_mutex);
error_monitor_mutex_init:
  pthread_mutex_destroy(&table_mon_list->receptor_mutex);
error_receptor_mutex_init:
  pthread_mutex_destroy(&table_mon_list->caller_mutex);
error_caller_mutex_init:
error:
  if (table_tables)
    spider_close_sys_table(thd, table_tables,
      &open_tables_backup, need_lock);
  free_root(&mem_root, MYF(0));
  if (table_mon_list)
  {
    spider_free_tmp_share_alloc(table_mon_list->share);
    spider_free(spider_current_trx, table_mon_list, MYF(0));
  }
  DBUG_RETURN(NULL);
}

SPIDER_CONN *spider_get_ping_table_tgt_conn(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  int *error_num
) {
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_get_ping_table_tgt_conn");
  if (
    !(conn = spider_get_conn(
      share, 0, share->conn_keys[0], trx, NULL, FALSE, FALSE,
      SPIDER_CONN_KIND_MYSQL, error_num))
  ) {
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
      share->server_names[0]);
    *error_num = ER_CONNECT_TO_FOREIGN_DATA_SOURCE;
    goto error;
  }
#ifndef DBUG_OFF
  DBUG_PRINT("info",("spider conn->thd=%p", conn->thd));
  if (conn->thd)
  {
    DBUG_PRINT("info",("spider query_id=%lld", conn->thd->query_id));
  }
#endif
  conn->error_mode = 0;
  DBUG_RETURN(conn);

error:
  DBUG_RETURN(NULL);
}

int spider_init_ping_table_mon_cache(
  THD *thd,
  MEM_ROOT *mem_root,
  bool need_lock
) {
  int error_num, same;
  TABLE *table_link_mon = NULL;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  SPIDER_MON_KEY mon_key;
  DBUG_ENTER("spider_init_ping_table_mon_cache");

  if (
    !(table_link_mon = spider_open_sys_table(
      thd, SPIDER_SYS_LINK_MON_TABLE_NAME_STR,
      SPIDER_SYS_LINK_MON_TABLE_NAME_LEN, FALSE, &open_tables_backup,
      need_lock, &error_num))
  ) {
    my_error(error_num, MYF(0));
    goto error_open_sys_table;
  }

  pthread_mutex_lock(&spider_mon_table_cache_mutex);
  if (spider_mon_table_cache_version != spider_mon_table_cache_version_req)
  {
    /* reset */
    spider_mon_table_cache.elements = 0;

    if ((error_num = spider_sys_index_first(table_link_mon,
      table_link_mon->s->primary_key)))
    {
      if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
      {
        table_link_mon->file->print_error(error_num, MYF(0));
        goto error_sys_index_first;
      }
    }

    if (!error_num)
    {
      mon_key.db_name_length = SPIDER_SYS_LINK_MON_TABLE_DB_NAME_SIZE + 1;
      mon_key.table_name_length = SPIDER_SYS_LINK_MON_TABLE_TABLE_NAME_SIZE + 1;
      mon_key.link_id_length = SPIDER_SYS_LINK_MON_TABLE_LINK_ID_SIZE + 1;
      do {
        if ((error_num = spider_get_sys_link_mon_key(table_link_mon, &mon_key,
          mem_root, &same)))
          goto error_get_sys_link_mon_key;

        if (!same)
        {
          mon_key.sort = spider_calc_for_sort(3, mon_key.db_name,
            mon_key.table_name, mon_key.link_id);
          if (push_dynamic(&spider_mon_table_cache, (uchar *) &mon_key))
          {
            error_num = HA_ERR_OUT_OF_MEM;
            goto error_push_dynamic;
          }
        }

        if ((error_num = spider_sys_index_next(table_link_mon)))
        {
          if (
            error_num != HA_ERR_KEY_NOT_FOUND &&
            error_num != HA_ERR_END_OF_FILE
          ) {
            table_link_mon->file->print_error(error_num, MYF(0));
            goto error_sys_index_next;
          }
        }
      } while (!error_num);
      spider_sys_index_end(table_link_mon);
    }
    my_qsort(
      (uchar *) dynamic_element(&spider_mon_table_cache, 0, SPIDER_MON_KEY *),
      spider_mon_table_cache.elements, sizeof(SPIDER_MON_KEY),
      (qsort_cmp) spider_compare_for_sort);
    uint old_elements = spider_mon_table_cache.max_element;
    freeze_size(&spider_mon_table_cache);
    if (spider_mon_table_cache.max_element < old_elements)
    {
      spider_free_mem_calc(spider_current_trx,
        spider_mon_table_cache_id,
        spider_mon_table_cache.max_element *
        spider_mon_table_cache.size_of_element);
    }
    spider_mon_table_cache_version = spider_mon_table_cache_version_req;
  }
  pthread_mutex_unlock(&spider_mon_table_cache_mutex);
  spider_close_sys_table(thd, table_link_mon, &open_tables_backup, need_lock);
  DBUG_RETURN(0);

error_push_dynamic:
error_get_sys_link_mon_key:
error_sys_index_next:
  spider_sys_index_end(table_link_mon);
error_sys_index_first:
  pthread_mutex_unlock(&spider_mon_table_cache_mutex);
  spider_close_sys_table(thd, table_link_mon, &open_tables_backup, need_lock);
error_open_sys_table:
  DBUG_RETURN(error_num);
}

int spider_ping_table_cache_compare(
  TABLE *table,
  MEM_ROOT *mem_root
) {
  uint32 roop_count;
  SPIDER_MON_KEY *mon_key;
  char *db_name, *table_name, *link_id;
  DBUG_ENTER("spider_ping_table_cache_compare");

  if (
    !(db_name = get_field(mem_root, table->field[0])) ||
    !(table_name = get_field(mem_root, table->field[1])) ||
    !(link_id = get_field(mem_root, table->field[2]))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  DBUG_PRINT("info", ("spider db_name=%s", db_name));
  DBUG_PRINT("info", ("spider table_name=%s", table_name));
  DBUG_PRINT("info", ("spider link_id=%s", link_id));

  pthread_mutex_lock(&spider_mon_table_cache_mutex);
  for (roop_count = 0; roop_count < spider_mon_table_cache.elements;
    roop_count++)
  {
    mon_key = dynamic_element(&spider_mon_table_cache, roop_count,
      SPIDER_MON_KEY *);
    DBUG_PRINT("info", ("spider roop_count=%d", roop_count));
    DBUG_PRINT("info", ("spider mon_key.db_name=%s", mon_key->db_name));
    DBUG_PRINT("info", ("spider mon_key.table_name=%s", mon_key->table_name));
    DBUG_PRINT("info", ("spider mon_key.link_id=%s", mon_key->link_id));
    if (
      !wild_case_compare(system_charset_info, db_name, mon_key->db_name) &&
      !wild_case_compare(system_charset_info, table_name,
        mon_key->table_name) &&
      !wild_case_compare(system_charset_info, link_id, mon_key->link_id)
    ) {
      spider_store_db_and_table_name(
        table,
        mon_key->db_name,
        mon_key->db_name_length,
        mon_key->table_name,
        mon_key->table_name_length
      );
      spider_store_tables_link_idx_str(
        table,
        mon_key->link_id,
        mon_key->link_id_length
      );
      pthread_mutex_unlock(&spider_mon_table_cache_mutex);
      DBUG_PRINT("info", ("spider found"));
      DBUG_RETURN(0);
    }
  }
  pthread_mutex_unlock(&spider_mon_table_cache_mutex);
  DBUG_PRINT("info", ("spider not found"));
  DBUG_RETURN(1);
}

long long spider_ping_table_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  int error_num = 0, link_idx, flags, full_mon_count, current_mon_count,
    success_count, fault_count, tmp_error_num = 0;
  uint32 first_sid;
  longlong limit, tmp_sid = -1;
  SPIDER_MON_TABLE_RESULT *mon_table_result =
    (SPIDER_MON_TABLE_RESULT *) initid->ptr;
  SPIDER_TRX *trx = mon_table_result->trx;
  THD *thd = trx->thd;
  SPIDER_CONN *ping_conn = NULL, *mon_conn;
  char *where_clause;
  SPIDER_TABLE_MON_LIST *table_mon_list;
  SPIDER_TABLE_MON *table_mon;

  char buf[MAX_FIELD_WIDTH];
  spider_string conv_name(buf, sizeof(buf), system_charset_info);
  int conv_name_length;
  char link_idx_str[SPIDER_SQL_INT_LEN];
  int link_idx_str_length;
  bool get_lock = FALSE;
  DBUG_ENTER("spider_ping_table_body");
  conv_name.init_calc_mem(135);
  conv_name.length(0);
  if (
    thd->open_tables != 0 ||
    thd->handler_tables_hash.records != 0 ||
    thd->derived_tables != 0 ||
    thd->lock != 0 ||
#if MYSQL_VERSION_ID < 50500
    thd->locked_tables != 0 ||
    thd->prelocked_mode != NON_PRELOCKED
#else
    thd->locked_tables_list.locked_tables() ||
    thd->locked_tables_mode != LTM_NONE
#endif
  ) {
    if (thd->open_tables != 0)
    {
      my_printf_error(ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
        ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_PTR, MYF(0),
        "thd->open_tables", thd->open_tables);
    } else if (thd->handler_tables_hash.records != 0)
    {
      my_printf_error(ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
        ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_NUM, MYF(0),
        "thd->handler_tables_hash.records",
        (longlong) thd->handler_tables_hash.records);
    } else if (thd->derived_tables != 0)
    {
      my_printf_error(ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
        ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_PTR, MYF(0),
        "thd->derived_tables", thd->derived_tables);
    } else if (thd->lock != 0)
    {
      my_printf_error(ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
        ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_PTR, MYF(0),
        "thd->lock", thd->lock);
#if MYSQL_VERSION_ID < 50500
    } else if (thd->locked_tables != 0)
    {
      my_printf_error(ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
        ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_PTR, MYF(0),
        "thd->locked_tables", thd->locked_tables);
    } else if (thd->prelocked_mode != NON_PRELOCKED)
    {
      my_printf_error(ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
        ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_NUM, MYF(0),
        "thd->prelocked_mode", (longlong) thd->prelocked_mode);
#else
    } else if (thd->locked_tables_list.locked_tables())
    {
      my_printf_error(ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
        ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_PTR, MYF(0),
        "thd->locked_tables_list.locked_tables()",
        thd->locked_tables_list.locked_tables());
    } else if (thd->locked_tables_mode != LTM_NONE)
    {
      my_printf_error(ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
        ER_SPIDER_UDF_CANT_USE_IF_OPEN_TABLE_STR_WITH_NUM, MYF(0),
        "thd->locked_tables_mode", (longlong) thd->locked_tables_mode);
#endif
    }
    goto error;
  }

  if (
    args->lengths[0] > SPIDER_CONNECT_INFO_MAX_LEN
  ) {
    my_printf_error(ER_SPIDER_UDF_PING_TABLE_PARAM_TOO_LONG_NUM,
      ER_SPIDER_UDF_PING_TABLE_PARAM_TOO_LONG_STR, MYF(0));
    goto error;
  }
  if (
    args->lengths[0] == 0
  ) {
    my_printf_error(ER_SPIDER_UDF_PING_TABLE_PARAM_REQIRED_NUM,
      ER_SPIDER_UDF_PING_TABLE_PARAM_REQIRED_STR, MYF(0));
    goto error;
  }

  link_idx = (int) (args->args[1] ? *((longlong *) args->args[1]) : 0);
  flags = (int) (args->args[2] ? *((longlong *) args->args[2]) : 0);
  limit = args->args[3] ? *((longlong *) args->args[3]) : 0;
  where_clause = args->args[4] ? args->args[4] : (char *) "";

  link_idx_str_length = my_sprintf(link_idx_str, (link_idx_str, "%010d",
    link_idx));

  if (conv_name.append(args->args[0], args->lengths[0],
    trx->thd->variables.character_set_client))
  {
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    goto error;
  }
  conv_name_length = conv_name.length();
  if (conv_name.reserve(link_idx_str_length + 1))
  {
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    goto error;
  }
  conv_name.q_append(link_idx_str, link_idx_str_length + 1);
  conv_name.length(conv_name.length() - 1);

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
  if (!(table_mon_list = spider_get_ping_table_mon_list(trx, trx->thd,
    &conv_name, conv_name_length, link_idx, global_system_variables.server_id,
    TRUE, &error_num)))
#else
  if (!(table_mon_list = spider_get_ping_table_mon_list(trx, trx->thd,
    &conv_name, conv_name_length, link_idx, thd->server_id, TRUE, &error_num)))
#endif
    goto error;

  if (table_mon_list->mon_status == SPIDER_LINK_MON_NG)
  {
    mon_table_result->result_status = SPIDER_LINK_MON_NG;
    DBUG_PRINT("info",
      ("spider mon_table_result->result_status=SPIDER_LINK_MON_NG 1"));
    goto end;
  }

  if (args->args[5])
    tmp_sid = *((longlong *) args->args[5]);

  if (tmp_sid >= 0)
  {
    first_sid = (uint32) tmp_sid;
    full_mon_count = (int) (args->args[6] ? *((longlong *) args->args[6]) : 0);
    current_mon_count =
      (int) (args->args[7] ? *((longlong *) args->args[7]) + 1 : 1);
    if (full_mon_count != table_mon_list->list_size)
    {
      my_printf_error(ER_SPIDER_UDF_PING_TABLE_DIFFERENT_MON_NUM,
        ER_SPIDER_UDF_PING_TABLE_DIFFERENT_MON_STR, MYF(0));
      goto error_with_free_table_mon_list;
    }
  } else {
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
    first_sid = global_system_variables.server_id;
#else
    first_sid = thd->server_id;
#endif
    full_mon_count = table_mon_list->list_size;
    current_mon_count = 1;
  }

  success_count = (int) (args->args[8] ? *((longlong *) args->args[8]) : 0);
  fault_count = (int) (args->args[9] ? *((longlong *) args->args[9]) : 0);
  if (
    table_mon_list->mon_status != SPIDER_LINK_MON_NG &&
    !(ping_conn = spider_get_ping_table_tgt_conn(trx,
      table_mon_list->share, &error_num))
  ) {
    if (error_num == HA_ERR_OUT_OF_MEM)
      goto error_with_free_table_mon_list;
    else
      thd->clear_error();
  }
  if (
    table_mon_list->mon_status == SPIDER_LINK_MON_NG ||
    error_num ||
    (tmp_error_num = spider_db_udf_ping_table(table_mon_list, table_mon_list->share, trx,
      ping_conn, where_clause, args->lengths[4],
      (flags & SPIDER_UDF_PING_TABLE_PING_ONLY),
      (flags & SPIDER_UDF_PING_TABLE_USE_WHERE),
      limit
    ))
  ) {
    DBUG_PRINT("info",("spider table_mon_list->mon_status == SPIDER_LINK_MON_NG:%s",
      table_mon_list->mon_status == SPIDER_LINK_MON_NG ? "TRUE" : "FALSE"));
    DBUG_PRINT("info",("spider error_num=%d", error_num));
    DBUG_PRINT("info",("spider tmp_error_num=%d", tmp_error_num));
    if (tmp_error_num == HA_ERR_OUT_OF_MEM)
      goto error_with_free_table_mon_list;
    else if(tmp_error_num)
      thd->clear_error();
    if (tmp_error_num != ER_CON_COUNT_ERROR)
    {
      fault_count++;
      error_num = 0;
      if (
        !(flags & SPIDER_UDF_PING_TABLE_USE_ALL_MONITORING_NODES) &&
        fault_count > full_mon_count / 2
      ) {
        mon_table_result->result_status = SPIDER_LINK_MON_NG;
        DBUG_PRINT("info",("spider mon_table_result->result_status=SPIDER_LINK_MON_NG 2"));
        if (table_mon_list->mon_status != SPIDER_LINK_MON_NG)
        {
/*
          pthread_mutex_lock(&table_mon_list->update_status_mutex);
*/
          pthread_mutex_lock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
          if (table_mon_list->mon_status != SPIDER_LINK_MON_NG)
          {
            table_mon_list->mon_status = SPIDER_LINK_MON_NG;
            table_mon_list->share->link_statuses[0] = SPIDER_LINK_STATUS_NG;
            spider_update_link_status_for_share(conv_name.c_ptr(),
              conv_name_length, link_idx, SPIDER_LINK_STATUS_NG);
            spider_sys_update_tables_link_status(trx->thd,
              conv_name.c_ptr(), conv_name_length, link_idx,
              SPIDER_LINK_STATUS_NG, TRUE);
            spider_sys_log_tables_link_failed(trx->thd,
              conv_name.c_ptr(), conv_name_length, link_idx, TRUE);
          }
/*
          pthread_mutex_unlock(&table_mon_list->update_status_mutex);
*/
          pthread_mutex_unlock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
        }
        goto end;
      }
    }
  } else {
    success_count++;
    if (
      !(flags & SPIDER_UDF_PING_TABLE_USE_ALL_MONITORING_NODES) &&
      success_count > full_mon_count / 2
    ) {
      mon_table_result->result_status = SPIDER_LINK_MON_OK;
      DBUG_PRINT("info",("spider mon_table_result->result_status=SPIDER_LINK_MON_OK 1"));
      goto end;
    }
  }

  if (tmp_sid < 0)
  {
    if (!pthread_mutex_trylock(&table_mon_list->receptor_mutex))
      get_lock = TRUE;
  }

  if (
    tmp_sid >= 0 ||
    get_lock
  ) {
    table_mon = table_mon_list->current->next;
    while (TRUE)
    {
      if (!table_mon)
        table_mon = table_mon_list->first;
      if (
        table_mon->server_id == first_sid ||
        current_mon_count > full_mon_count
      ) {
        if (
          (flags & SPIDER_UDF_PING_TABLE_USE_ALL_MONITORING_NODES) &&
          fault_count > full_mon_count / 2
        ) {
          mon_table_result->result_status = SPIDER_LINK_MON_NG;
          DBUG_PRINT("info",("spider mon_table_result->result_status=SPIDER_LINK_MON_NG 3"));
          if (table_mon_list->mon_status != SPIDER_LINK_MON_NG)
          {
/*
            pthread_mutex_lock(&table_mon_list->update_status_mutex);
*/
            pthread_mutex_lock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
            if (table_mon_list->mon_status != SPIDER_LINK_MON_NG)
            {
              table_mon_list->mon_status = SPIDER_LINK_MON_NG;
              table_mon_list->share->link_statuses[0] = SPIDER_LINK_STATUS_NG;
              spider_update_link_status_for_share(conv_name.c_ptr(),
                conv_name_length, link_idx, SPIDER_LINK_STATUS_NG);
              spider_sys_update_tables_link_status(trx->thd,
                conv_name.c_ptr(), conv_name_length, link_idx,
                SPIDER_LINK_STATUS_NG, TRUE);
              spider_sys_log_tables_link_failed(trx->thd,
                conv_name.c_ptr(), conv_name_length, link_idx, TRUE);
            }
/*
            pthread_mutex_unlock(&table_mon_list->update_status_mutex);
*/
            pthread_mutex_unlock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
          }
        } else if (
          (flags & SPIDER_UDF_PING_TABLE_USE_ALL_MONITORING_NODES) &&
          success_count > full_mon_count / 2
        ) {
          mon_table_result->result_status = SPIDER_LINK_MON_OK;
          DBUG_PRINT("info",("spider mon_table_result->result_status=SPIDER_LINK_MON_OK 2"));
        } else if (success_count + fault_count > full_mon_count / 2)
        {
          mon_table_result->result_status = SPIDER_LINK_MON_DRAW;
          DBUG_PRINT("info",(
            "spider mon_table_result->result_status=SPIDER_LINK_MON_DRAW 1"));
        } else {
          mon_table_result->result_status = SPIDER_LINK_MON_DRAW_FEW_MON;
          DBUG_PRINT("info",(
            "spider mon_table_result->result_status=SPIDER_LINK_MON_DRAW_FEW_MON 1"));
        }
        table_mon_list->last_receptor_result = mon_table_result->result_status;
        break;
      }
      if ((mon_conn = spider_get_ping_table_tgt_conn(trx,
        table_mon->share, &error_num))
      ) {
        if (!spider_db_udf_ping_table_mon_next(
          thd, table_mon, mon_conn, mon_table_result, args->args[0],
          args->lengths[0], link_idx,
          where_clause, args->lengths[4], first_sid, full_mon_count,
          current_mon_count, success_count, fault_count, flags, limit))
        {
          if (
            mon_table_result->result_status == SPIDER_LINK_MON_NG &&
            table_mon_list->mon_status != SPIDER_LINK_MON_NG
          ) {
/*
            pthread_mutex_lock(&table_mon_list->update_status_mutex);
*/
            pthread_mutex_lock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
            if (table_mon_list->mon_status != SPIDER_LINK_MON_NG)
            {
              table_mon_list->mon_status = SPIDER_LINK_MON_NG;
              table_mon_list->share->link_statuses[0] = SPIDER_LINK_STATUS_NG;
              spider_update_link_status_for_share(conv_name.c_ptr(),
                conv_name_length, link_idx, SPIDER_LINK_STATUS_NG);
              spider_sys_update_tables_link_status(trx->thd,
                conv_name.c_ptr(), conv_name_length, link_idx,
                SPIDER_LINK_STATUS_NG, TRUE);
              spider_sys_log_tables_link_failed(trx->thd,
                conv_name.c_ptr(), conv_name_length, link_idx, TRUE);
            }
/*
            pthread_mutex_unlock(&table_mon_list->update_status_mutex);
*/
            pthread_mutex_unlock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
          }
          table_mon_list->last_receptor_result =
            mon_table_result->result_status;
          break;
        }
      }
      thd->clear_error();
      table_mon = table_mon->next;
      current_mon_count++;
    }
    if (get_lock)
      pthread_mutex_unlock(&table_mon_list->receptor_mutex);
  } else {
    pthread_mutex_lock(&table_mon_list->receptor_mutex);
    mon_table_result->result_status = table_mon_list->last_receptor_result;
    DBUG_PRINT("info",("spider mon_table_result->result_status=%d 1",
      table_mon_list->last_receptor_result));
    pthread_mutex_unlock(&table_mon_list->receptor_mutex);
  }

end:
  spider_free_ping_table_mon_list(table_mon_list);
  DBUG_RETURN(mon_table_result->result_status);

error_with_free_table_mon_list:
  spider_free_ping_table_mon_list(table_mon_list);
error:
  *error = 1;
  DBUG_RETURN(0);
}

my_bool spider_ping_table_init_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  int error_num;
  THD *thd = current_thd;
  SPIDER_TRX *trx;
  SPIDER_MON_TABLE_RESULT *mon_table_result = NULL;
  DBUG_ENTER("spider_ping_table_init_body");
  if (args->arg_count != 10)
  {
    strcpy(message, "spider_ping_table() requires 10 arguments");
    goto error;
  }
  if (
    args->arg_type[0] != STRING_RESULT ||
    args->arg_type[4] != STRING_RESULT
  ) {
    strcpy(message, "spider_ping_table() requires string 1st "
      "and 5th arguments");
    goto error;
  }
  if (
    args->arg_type[1] != INT_RESULT ||
    args->arg_type[2] != INT_RESULT ||
    args->arg_type[3] != INT_RESULT ||
    args->arg_type[5] != INT_RESULT ||
    args->arg_type[6] != INT_RESULT ||
    args->arg_type[7] != INT_RESULT ||
    args->arg_type[8] != INT_RESULT ||
    args->arg_type[9] != INT_RESULT
  ) {
    strcpy(message, "spider_ping_table() requires integer 2nd, 3rd, 4,6,7,8,"
      "9th and 10th argument");
    goto error;
  }

  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
  {
    my_error(error_num, MYF(0));
    strcpy(message, spider_stmt_da_message(thd));
    goto error;
  }

  if (!(mon_table_result = (SPIDER_MON_TABLE_RESULT *)
    spider_malloc(spider_current_trx, 11, sizeof(SPIDER_MON_TABLE_RESULT),
      MYF(MY_WME | MY_ZEROFILL)))
  ) {
    strcpy(message, "spider_ping_table() out of memory");
    goto error;
  }
  mon_table_result->trx = trx;
  initid->ptr = (char *) mon_table_result;
  DBUG_RETURN(FALSE);

error:
  if (mon_table_result)
  {
    spider_free(spider_current_trx, mon_table_result, MYF(0));
  }
  DBUG_RETURN(TRUE);
}

void spider_ping_table_deinit_body(
  UDF_INIT *initid
) {
  SPIDER_MON_TABLE_RESULT *mon_table_result =
    (SPIDER_MON_TABLE_RESULT *) initid->ptr;
  DBUG_ENTER("spider_ping_table_deinit_body");
  if (mon_table_result)
  {
    spider_free(spider_current_trx, mon_table_result, MYF(0));
  }
  DBUG_VOID_RETURN;
}

long long spider_flush_table_mon_cache_body()
{
  DBUG_ENTER("spider_flush_table_mon_cache_body");
  spider_mon_table_cache_version_req++;
  DBUG_RETURN(1);
}

void spider_ping_table_free_mon_list(
  SPIDER_TABLE_MON_LIST *table_mon_list
) {
  DBUG_ENTER("spider_ping_table_free_mon_list");
  if (table_mon_list)
  {
    spider_ping_table_free_mon(table_mon_list->first);
    spider_free_tmp_share_alloc(table_mon_list->share);
    pthread_mutex_destroy(&table_mon_list->update_status_mutex);
    pthread_mutex_destroy(&table_mon_list->monitor_mutex);
    pthread_mutex_destroy(&table_mon_list->receptor_mutex);
    pthread_mutex_destroy(&table_mon_list->caller_mutex);
    spider_free(spider_current_trx, table_mon_list, MYF(0));
  }
  DBUG_VOID_RETURN;
}

void spider_ping_table_free_mon(
  SPIDER_TABLE_MON *table_mon
) {
  SPIDER_TABLE_MON *table_mon_next;
  DBUG_ENTER("spider_ping_table_free_mon");
  while (table_mon)
  {
    spider_free_tmp_share_alloc(table_mon->share);
    table_mon_next = table_mon->next;
    spider_free(spider_current_trx, table_mon, MYF(0));
    table_mon = table_mon_next;
  }
  DBUG_VOID_RETURN;
}

int spider_ping_table_mon_from_table(
  SPIDER_TRX *trx,
  THD *thd,
  SPIDER_SHARE *share,
  uint32 server_id,
  char *conv_name,
  uint conv_name_length,
  int link_idx,
  char *where_clause,
  uint where_clause_length,
  long monitoring_kind,
  longlong monitoring_limit,
  long monitoring_flag,
  bool need_lock
) {
  int error_num = 0, current_mon_count, flags;
  uint32 first_sid;
/*
  THD *thd = trx->thd;
*/
  SPIDER_TABLE_MON_LIST *table_mon_list;
  SPIDER_TABLE_MON *table_mon;
  SPIDER_MON_TABLE_RESULT mon_table_result;
  SPIDER_CONN *mon_conn;
  TABLE_SHARE *table_share = share->table_share;
  char link_idx_str[SPIDER_SQL_INT_LEN];
  int link_idx_str_length;
  uint sql_command = thd_sql_command(thd);
  DBUG_ENTER("spider_ping_table_mon_from_table");
  if (table_share->tmp_table != NO_TMP_TABLE)
  {
    my_printf_error(ER_SPIDER_TMP_TABLE_MON_NUM,
      ER_SPIDER_TMP_TABLE_MON_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_TMP_TABLE_MON_NUM);
  }
  if (
    sql_command == SQLCOM_DROP_TABLE ||
    sql_command == SQLCOM_ALTER_TABLE
  ) {
    my_printf_error(ER_SPIDER_MON_AT_ALTER_TABLE_NUM,
      ER_SPIDER_MON_AT_ALTER_TABLE_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_MON_AT_ALTER_TABLE_NUM);
  }
  DBUG_PRINT("info",("spider thd->killed=%s",
    thd ? (thd->killed ? "TRUE" : "FALSE") : "NULL"));
  DBUG_PRINT("info",("spider abort_loop=%s",
    *spd_abort_loop ? "TRUE" : "FALSE"));
  if (
    (thd && thd->killed) ||
    *spd_abort_loop
  ) {
    DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
  }

  link_idx_str_length = my_sprintf(link_idx_str, (link_idx_str, "%010d",
    link_idx));
#if defined(_MSC_VER) || defined(__SUNPRO_CC)
  spider_string conv_name_str(conv_name_length + link_idx_str_length + 1);
  conv_name_str.set_charset(system_charset_info);
  *((char *)(conv_name_str.ptr() + conv_name_length + link_idx_str_length)) =
    '\0';
#else
  char buf[conv_name_length + link_idx_str_length + 1];
  buf[conv_name_length + link_idx_str_length] = '\0';
  spider_string conv_name_str(buf, conv_name_length + link_idx_str_length + 1,
    system_charset_info);
#endif
  conv_name_str.init_calc_mem(136);
  conv_name_str.length(0);
  conv_name_str.q_append(conv_name, conv_name_length);
  conv_name_str.q_append(link_idx_str, link_idx_str_length + 1);
  conv_name_str.length(conv_name_str.length() - 1);

  if (monitoring_kind == 1)
    flags = SPIDER_UDF_PING_TABLE_PING_ONLY;
  else if (monitoring_kind == 3)
    flags = SPIDER_UDF_PING_TABLE_USE_WHERE;
  else
    flags = 0;

  if (monitoring_flag & 1)
    flags |= SPIDER_UDF_PING_TABLE_USE_ALL_MONITORING_NODES;

  if (!(table_mon_list = spider_get_ping_table_mon_list(trx, thd,
    &conv_name_str, conv_name_length, link_idx, server_id, need_lock,
    &error_num)))
    goto end;

  if (table_mon_list->mon_status == SPIDER_LINK_MON_NG)
  {
    DBUG_PRINT("info",
      ("spider share->link_statuses[%d]=SPIDER_LINK_STATUS_NG", link_idx));
    pthread_mutex_lock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
    share->link_statuses[link_idx] = SPIDER_LINK_STATUS_NG;
    pthread_mutex_unlock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
    error_num = ER_SPIDER_LINK_MON_NG_NUM;
    my_printf_error(error_num,
      ER_SPIDER_LINK_MON_NG_STR, MYF(0),
      table_mon_list->share->tgt_dbs[0],
      table_mon_list->share->tgt_table_names[0]);
    goto end_with_free_table_mon_list;
  }

  if (!pthread_mutex_trylock(&table_mon_list->caller_mutex))
  {
    table_mon = table_mon_list->current;
    first_sid = table_mon->server_id;
    current_mon_count = 1;
    while (TRUE)
    {
      DBUG_PRINT("info",("spider thd->killed=%s",
        thd ? (thd->killed ? "TRUE" : "FALSE") : "NULL"));
      DBUG_PRINT("info",("spider abort_loop=%s",
        *spd_abort_loop ? "TRUE" : "FALSE"));
      if (
        (thd && thd->killed) ||
        *spd_abort_loop
      ) {
        error_num = ER_SPIDER_COND_SKIP_NUM;
        break;
      } else {
        if (!table_mon)
          table_mon = table_mon_list->first;
        if (
          current_mon_count > table_mon_list->list_size ||
          (current_mon_count > 1 && table_mon->server_id == first_sid)
        ) {
          table_mon_list->last_caller_result = SPIDER_LINK_MON_DRAW_FEW_MON;
          mon_table_result.result_status = SPIDER_LINK_MON_DRAW_FEW_MON;
          DBUG_PRINT("info",(
            "spider mon_table_result->result_status=SPIDER_LINK_MON_DRAW_FEW_MON 1"));
          error_num = ER_SPIDER_LINK_MON_DRAW_FEW_MON_NUM;
          my_printf_error(error_num,
            ER_SPIDER_LINK_MON_DRAW_FEW_MON_STR, MYF(0),
            table_mon_list->share->tgt_dbs[0],
            table_mon_list->share->tgt_table_names[0]);
          break;
        }
        int prev_error = 0;
        char prev_error_msg[MYSQL_ERRMSG_SIZE];
        if (thd->is_error())
        {
          prev_error = spider_stmt_da_sql_errno(thd);
          strmov(prev_error_msg, spider_stmt_da_message(thd));
          thd->clear_error();
        }
        if ((mon_conn = spider_get_ping_table_tgt_conn(trx,
          table_mon->share, &error_num))
        ) {
          if (!spider_db_udf_ping_table_mon_next(
            thd, table_mon, mon_conn, &mon_table_result, conv_name,
            conv_name_length, link_idx,
            where_clause, where_clause_length, -1, table_mon_list->list_size,
            0, 0, 0, flags, monitoring_limit))
          {
            if (
              mon_table_result.result_status == SPIDER_LINK_MON_NG &&
              table_mon_list->mon_status != SPIDER_LINK_MON_NG
            ) {
/*
              pthread_mutex_lock(&table_mon_list->update_status_mutex);
*/
              pthread_mutex_lock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
              if (table_mon_list->mon_status != SPIDER_LINK_MON_NG)
              {
                table_mon_list->mon_status = SPIDER_LINK_MON_NG;
                table_mon_list->share->link_statuses[0] = SPIDER_LINK_STATUS_NG;
                DBUG_PRINT("info", (
                  "spider share->link_statuses[%d]=SPIDER_LINK_STATUS_NG",
                  link_idx));
                share->link_statuses[link_idx] = SPIDER_LINK_STATUS_NG;
                spider_sys_update_tables_link_status(thd, conv_name,
                  conv_name_length, link_idx, SPIDER_LINK_STATUS_NG, need_lock);
                spider_sys_log_tables_link_failed(thd, conv_name,
                  conv_name_length, link_idx, need_lock);
              }
/*
              pthread_mutex_unlock(&table_mon_list->update_status_mutex);
*/
              pthread_mutex_unlock(&spider_udf_table_mon_mutexes[table_mon_list->mutex_hash]);
            }
            table_mon_list->last_caller_result = mon_table_result.result_status;
            if (mon_table_result.result_status == SPIDER_LINK_MON_OK)
            {
              if (prev_error)
                my_message(prev_error, prev_error_msg, MYF(0));
              error_num = ER_SPIDER_LINK_MON_OK_NUM;
              my_printf_error(error_num,
                ER_SPIDER_LINK_MON_OK_STR, MYF(0),
                table_mon_list->share->tgt_dbs[0],
                table_mon_list->share->tgt_table_names[0]);
              break;
            }
            if (mon_table_result.result_status == SPIDER_LINK_MON_NG)
            {
              error_num = ER_SPIDER_LINK_MON_NG_NUM;
              my_printf_error(error_num,
                ER_SPIDER_LINK_MON_NG_STR, MYF(0),
                table_mon_list->share->tgt_dbs[0],
                table_mon_list->share->tgt_table_names[0]);
              break;
            }
            if (mon_table_result.result_status ==
              SPIDER_LINK_MON_DRAW_FEW_MON)
            {
              error_num = ER_SPIDER_LINK_MON_DRAW_FEW_MON_NUM;
              my_printf_error(error_num,
                ER_SPIDER_LINK_MON_DRAW_FEW_MON_STR, MYF(0),
                table_mon_list->share->tgt_dbs[0],
                table_mon_list->share->tgt_table_names[0]);
              break;
            }
            error_num = ER_SPIDER_LINK_MON_DRAW_NUM;
            my_printf_error(error_num,
              ER_SPIDER_LINK_MON_DRAW_STR, MYF(0),
              table_mon_list->share->tgt_dbs[0],
              table_mon_list->share->tgt_table_names[0]);
            break;
          }
        }
        table_mon = table_mon->next;
        current_mon_count++;
      }
    }
    pthread_mutex_unlock(&table_mon_list->caller_mutex);
  } else {
    pthread_mutex_lock(&table_mon_list->caller_mutex);
    DBUG_PRINT("info",("spider thd->killed=%s",
      thd ? (thd->killed ? "TRUE" : "FALSE") : "NULL"));
    DBUG_PRINT("info",("spider abort_loop=%s",
      *spd_abort_loop ? "TRUE" : "FALSE"));
    if (
      (thd && thd->killed) ||
      *spd_abort_loop
    ) {
      error_num = ER_SPIDER_COND_SKIP_NUM;
    } else {
      switch (table_mon_list->last_caller_result)
      {
        case SPIDER_LINK_MON_OK:
          error_num = ER_SPIDER_LINK_MON_OK_NUM;
          my_printf_error(error_num,
            ER_SPIDER_LINK_MON_OK_STR, MYF(0),
            table_mon_list->share->tgt_dbs[0],
            table_mon_list->share->tgt_table_names[0]);
          break;
        case SPIDER_LINK_MON_NG:
          error_num = ER_SPIDER_LINK_MON_NG_NUM;
          my_printf_error(error_num,
            ER_SPIDER_LINK_MON_NG_STR, MYF(0),
            table_mon_list->share->tgt_dbs[0],
            table_mon_list->share->tgt_table_names[0]);
          break;
        case SPIDER_LINK_MON_DRAW_FEW_MON:
          error_num = ER_SPIDER_LINK_MON_DRAW_FEW_MON_NUM;
          my_printf_error(error_num,
            ER_SPIDER_LINK_MON_DRAW_FEW_MON_STR, MYF(0),
            table_mon_list->share->tgt_dbs[0],
            table_mon_list->share->tgt_table_names[0]);
          break;
        default:
          error_num = ER_SPIDER_LINK_MON_DRAW_NUM;
          my_printf_error(error_num,
            ER_SPIDER_LINK_MON_DRAW_STR, MYF(0),
            table_mon_list->share->tgt_dbs[0],
            table_mon_list->share->tgt_table_names[0]);
          break;
      }
    }
    pthread_mutex_unlock(&table_mon_list->caller_mutex);
  }

end_with_free_table_mon_list:
  spider_free_ping_table_mon_list(table_mon_list);
end:
  DBUG_RETURN(error_num);
}
