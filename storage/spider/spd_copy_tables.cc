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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_base.h"
#include "sql_partition.h"
#include "transaction.h"
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
#include "spd_copy_tables.h"
#include "spd_udf.h"
#include "spd_malloc.h"

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];

int spider_udf_set_copy_tables_param_default(
  SPIDER_COPY_TABLES *copy_tables
) {
  DBUG_ENTER("spider_udf_set_copy_tables_param_default");

  if (!copy_tables->database)
  {
    DBUG_PRINT("info",("spider create default database"));
    copy_tables->database_length = SPIDER_THD_db_length(copy_tables->trx->thd);
    if (
      !(copy_tables->database = spider_create_string(
        SPIDER_THD_db_str(copy_tables->trx->thd),
        copy_tables->database_length))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (copy_tables->bulk_insert_interval == -1)
    copy_tables->bulk_insert_interval = 10;
  if (copy_tables->bulk_insert_rows == -1)
    copy_tables->bulk_insert_rows = 100;
  if (copy_tables->use_table_charset == -1)
    copy_tables->use_table_charset = 1;
  if (copy_tables->use_transaction == -1)
    copy_tables->use_transaction = 1;
  if (copy_tables->bg_mode == -1)
    copy_tables->bg_mode = 0;
  DBUG_RETURN(0);
}

#define SPIDER_PARAM_STR_LEN(name) name ## _length
#define SPIDER_PARAM_STR(title_name, param_name) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (!copy_tables->param_name) \
    { \
      if ((copy_tables->param_name = spider_get_string_between_quote( \
        start_ptr, TRUE, &param_string_parse))) \
        copy_tables->SPIDER_PARAM_STR_LEN(param_name) = \
          strlen(copy_tables->param_name); \
      else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%s", copy_tables->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_HINT_WITH_MAX(title_name, param_name, check_length, max_size, min_val, max_val) \
  if (!strncasecmp(tmp_ptr, title_name, check_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    DBUG_PRINT("info",("spider max_size=%d", max_size)); \
    int hint_num = atoi(tmp_ptr + check_length) - 1; \
    DBUG_PRINT("info",("spider hint_num=%d", hint_num)); \
    DBUG_PRINT("info",("spider copy_tables->param_name=%x", \
      copy_tables->param_name)); \
    if (copy_tables->param_name) \
    { \
      if (hint_num < 0 || hint_num >= max_size) \
      { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } else if (copy_tables->param_name[hint_num] != -1) \
        break; \
      char *hint_str = spider_get_string_between_quote(start_ptr, FALSE); \
      if (hint_str) \
      { \
        copy_tables->param_name[hint_num] = atoi(hint_str); \
        if (copy_tables->param_name[hint_num] < min_val) \
          copy_tables->param_name[hint_num] = min_val; \
        else if (copy_tables->param_name[hint_num] > max_val) \
          copy_tables->param_name[hint_num] = max_val; \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "[%d]=%d", hint_num, \
        copy_tables->param_name[hint_num])); \
    } else { \
      error_num = param_string_parse.print_param_error(); \
      goto error; \
    } \
    break; \
  }
#define SPIDER_PARAM_INT_WITH_MAX(title_name, param_name, min_val, max_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (copy_tables->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        copy_tables->param_name = atoi(tmp_ptr2); \
        if (copy_tables->param_name < min_val) \
          copy_tables->param_name = min_val; \
        else if (copy_tables->param_name > max_val) \
          copy_tables->param_name = max_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                           tmp_ptr2 + \
                                             strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%d", copy_tables->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_INT(title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (copy_tables->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        copy_tables->param_name = atoi(tmp_ptr2); \
        if (copy_tables->param_name < min_val) \
          copy_tables->param_name = min_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                           tmp_ptr2 + \
                                             strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%d", copy_tables->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_LONGLONG(title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (copy_tables->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        copy_tables->param_name = \
          my_strtoll10(tmp_ptr2, (char**) NULL, &error_num); \
        if (copy_tables->param_name < min_val) \
          copy_tables->param_name = min_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                           tmp_ptr2 + \
                                             strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%lld", \
        copy_tables->param_name)); \
    } \
    break; \
  }

int spider_udf_parse_copy_tables_param(
  SPIDER_COPY_TABLES *copy_tables,
  char *param,
  int param_length
) {
  int error_num = 0;
  char *param_string = NULL;
  char *sprit_ptr;
  char *tmp_ptr, *tmp_ptr2, *start_ptr;
  int title_length;
  SPIDER_PARAM_STRING_PARSE param_string_parse;
  DBUG_ENTER("spider_udf_parse_copy_tables_param");
  copy_tables->bulk_insert_interval = -1;
  copy_tables->bulk_insert_rows = -1;
  copy_tables->use_table_charset = -1;
  copy_tables->use_transaction = -1;
  copy_tables->bg_mode = -1;

  if (param_length == 0)
    goto set_default;
  DBUG_PRINT("info",("spider create param_string string"));
  if (
    !(param_string = spider_create_string(
      param,
      param_length))
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error_alloc_param_string;
  }
  DBUG_PRINT("info",("spider param_string=%s", param_string));

  sprit_ptr = param_string;
  param_string_parse.init(param_string, ER_SPIDER_INVALID_UDF_PARAM_NUM);
  while (sprit_ptr)
  {
    tmp_ptr = sprit_ptr;
    while (*tmp_ptr == ' ' || *tmp_ptr == '\r' ||
      *tmp_ptr == '\n' || *tmp_ptr == '\t')
      tmp_ptr++;

    if (*tmp_ptr == '\0')
      break;

    title_length = 0;
    start_ptr = tmp_ptr;
    while (*start_ptr != ' ' && *start_ptr != '\'' &&
      *start_ptr != '"' && *start_ptr != '\0' &&
      *start_ptr != '\r' && *start_ptr != '\n' &&
      *start_ptr != '\t')
    {
      title_length++;
      start_ptr++;
    }
    param_string_parse.set_param_title(tmp_ptr, tmp_ptr + title_length);
    if ((error_num = param_string_parse.get_next_parameter_head(
      start_ptr, &sprit_ptr)))
    {
      goto error;
    }

    switch (title_length)
    {
      case 0:
        error_num = param_string_parse.print_param_error();
        if (error_num)
          goto error;
        continue;
      case 3:
        SPIDER_PARAM_INT_WITH_MAX("bgm", bg_mode, 0, 1);
        SPIDER_PARAM_INT("bii", bulk_insert_interval, 0);
        SPIDER_PARAM_LONGLONG("bir", bulk_insert_rows, 1);
        SPIDER_PARAM_STR("dtb", database);
        SPIDER_PARAM_INT_WITH_MAX("utc", use_table_charset, 0, 1);
        SPIDER_PARAM_INT_WITH_MAX("utr", use_transaction, 0, 1);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 7:
        SPIDER_PARAM_INT_WITH_MAX("bg_mode", bg_mode, 0, 1);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 8:
        SPIDER_PARAM_STR("database", database);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 15:
        SPIDER_PARAM_INT_WITH_MAX("use_transaction", use_transaction, 0, 1);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 16:
        SPIDER_PARAM_LONGLONG("bulk_insert_rows", bulk_insert_rows, 1);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 17:
        SPIDER_PARAM_INT_WITH_MAX(
          "use_table_charset", use_table_charset, 0, 1);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 20:
        SPIDER_PARAM_INT("bulk_insert_interval", bulk_insert_interval, 0);
        error_num = param_string_parse.print_param_error();
        goto error;
      default:
        error_num = param_string_parse.print_param_error();
        goto error;
    }

    /* Verify that the remainder of the parameter value is whitespace */
    if ((error_num = param_string_parse.has_extra_parameter_values()))
      goto error;
  }

set_default:
  if ((error_num = spider_udf_set_copy_tables_param_default(
    copy_tables
  )))
    goto error;

  if (param_string)
    spider_free(spider_current_trx, param_string, MYF(0));
  DBUG_RETURN(0);

error:
  if (param_string)
    spider_free(spider_current_trx, param_string, MYF(0));
error_alloc_param_string:
  DBUG_RETURN(error_num);
}

int spider_udf_get_copy_tgt_tables(
  THD *thd,
  SPIDER_COPY_TABLES *copy_tables,
  MEM_ROOT *mem_root,
  bool need_lock
) {
  int error_num, roop_count;
  TABLE *table_tables = NULL;
  SPIDER_Open_tables_backup open_tables_backup;
  char table_key[MAX_KEY_LENGTH];
  SPIDER_COPY_TABLE_CONN *table_conn = NULL, *src_table_conn_prev = NULL,
    *dst_table_conn_prev = NULL;
  SPIDER_SHARE *tmp_share;
  char **tmp_connect_info;
  uint *tmp_connect_info_length;
  long *tmp_long;
  longlong *tmp_longlong;
  DBUG_ENTER("spider_udf_get_copy_tgt_tables");

  if (
    !(table_tables = spider_open_sys_table(
      thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
      SPIDER_SYS_TABLES_TABLE_NAME_LEN, FALSE, &open_tables_backup,
      need_lock, &error_num))
  ) {
    my_error(error_num, MYF(0));
    goto error;
  }
  spider_store_db_and_table_name(table_tables,
    copy_tables->spider_db_name, copy_tables->spider_db_name_length,
    copy_tables->spider_table_name, copy_tables->spider_table_name_length
  );
  if ((error_num = spider_get_sys_table_by_idx(table_tables, table_key,
    table_tables->s->primary_key, 2)))
  {
    table_tables->file->print_error(error_num, MYF(0));
    goto error;
  }
  do {
    if (!(table_conn = (SPIDER_COPY_TABLE_CONN *)
      spider_bulk_malloc(spider_current_trx, 25, MYF(MY_WME | MY_ZEROFILL),
        &table_conn, (uint) (sizeof(SPIDER_COPY_TABLE_CONN)),
        &tmp_share, (uint) (sizeof(SPIDER_SHARE)),
        &tmp_connect_info,
          (uint) (sizeof(char *) * SPIDER_TMP_SHARE_CHAR_PTR_COUNT),
        &tmp_connect_info_length,
          (uint) (sizeof(uint) * SPIDER_TMP_SHARE_UINT_COUNT),
        &tmp_long, (uint) (sizeof(long) * SPIDER_TMP_SHARE_LONG_COUNT),
        &tmp_longlong,
          (uint) (sizeof(longlong) * SPIDER_TMP_SHARE_LONGLONG_COUNT),
        NullS))
    ) {
      spider_sys_index_end(table_tables);
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      goto error;
    }
    spider_set_tmp_share_pointer(tmp_share, tmp_connect_info,
      tmp_connect_info_length, tmp_long, tmp_longlong);
    tmp_share->link_statuses[0] = -1;
    table_conn->share = tmp_share;

    if (
      (error_num = spider_get_sys_tables_connect_info(
        table_tables, tmp_share, 0, mem_root)) ||
      (error_num = spider_get_sys_tables_link_status(
        table_tables, tmp_share, 0, mem_root)) ||
      (error_num = spider_get_sys_tables_link_idx(
        table_tables, &table_conn->link_idx, mem_root))
    ) {
      table_tables->file->print_error(error_num, MYF(0));
      spider_sys_index_end(table_tables);
      goto error;
    }
    if (
      (error_num = spider_set_connect_info_default(
        tmp_share,
        NULL,
        NULL,
        NULL
      )) ||
      (error_num = spider_set_connect_info_default_db_table(
        tmp_share,
        copy_tables->spider_db_name, copy_tables->spider_db_name_length,
        copy_tables->spider_table_name, copy_tables->spider_table_name_length
      )) ||
      (error_num = spider_create_conn_keys(tmp_share)) ||
      (error_num = spider_create_tmp_dbton_share(tmp_share))
    ) {
      spider_sys_index_end(table_tables);
      goto error;
    }

/*
    if (spider_db_create_table_names_str(tmp_share))
    {
      spider_sys_index_end(table_tables);
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      goto error;
    }
*/

    for (roop_count = 0; roop_count < (int) tmp_share->use_dbton_count;
      roop_count++)
    {
      uint dbton_id = tmp_share->use_dbton_ids[roop_count];

      if (!spider_dbton[dbton_id].create_db_copy_table)
        continue;

      if (!(table_conn->copy_table =
        spider_dbton[dbton_id].create_db_copy_table(
        tmp_share->dbton_share[dbton_id])))
      {
        spider_sys_index_end(table_tables);
        error_num = HA_ERR_OUT_OF_MEM;
        my_error(HA_ERR_OUT_OF_MEM, MYF(0));
        goto error;
      }
      if ((error_num = table_conn->copy_table->init()))
        goto error;
      break;
    }

    if (
      !copy_tables->use_auto_mode[0]
    ) {
      for (roop_count = 0; roop_count < copy_tables->link_idx_count[0];
        roop_count++)
      {
        if (table_conn->link_idx == copy_tables->link_idxs[0][roop_count])
        {
          if (tmp_share->link_statuses[0] == SPIDER_LINK_STATUS_NG)
          {
            spider_sys_index_end(table_tables);
            error_num = ER_SPIDER_UDF_COPY_TABLE_SRC_NG_STATUS_NUM;
            my_printf_error(ER_SPIDER_UDF_COPY_TABLE_SRC_NG_STATUS_NUM,
              ER_SPIDER_UDF_COPY_TABLE_SRC_NG_STATUS_STR, MYF(0));
            goto error;
          }
          if (src_table_conn_prev)
            src_table_conn_prev->next = table_conn;
          else
            copy_tables->table_conn[0] = table_conn;
          src_table_conn_prev = table_conn;
          table_conn = NULL;
          break;
        }
      }
    }
    if (table_conn && !copy_tables->use_auto_mode[1])
    {
      for (roop_count = 0; roop_count < copy_tables->link_idx_count[1];
        roop_count++)
      {
        if (table_conn->link_idx == copy_tables->link_idxs[1][roop_count])
        {
          if (tmp_share->link_statuses[0] == SPIDER_LINK_STATUS_NG)
          {
            spider_sys_index_end(table_tables);
            error_num = ER_SPIDER_UDF_COPY_TABLE_SRC_NG_STATUS_NUM;
            my_printf_error(ER_SPIDER_UDF_COPY_TABLE_SRC_NG_STATUS_NUM,
              ER_SPIDER_UDF_COPY_TABLE_SRC_NG_STATUS_STR, MYF(0));
            goto error;
          }
          if (dst_table_conn_prev)
            dst_table_conn_prev->next = table_conn;
          else
            copy_tables->table_conn[1] = table_conn;
          dst_table_conn_prev = table_conn;
          table_conn = NULL;
          break;
        }
      }
    }
    if (table_conn && copy_tables->use_auto_mode[0] &&
      tmp_share->link_statuses[0] == SPIDER_LINK_STATUS_OK)
    {
      if (src_table_conn_prev)
        src_table_conn_prev->next = table_conn;
      else
        copy_tables->table_conn[0] = table_conn;
      src_table_conn_prev = table_conn;
      copy_tables->link_idx_count[0]++;
      table_conn = NULL;
    }
    if (table_conn && copy_tables->use_auto_mode[1] &&
      tmp_share->link_statuses[0] == SPIDER_LINK_STATUS_RECOVERY)
    {
      if (dst_table_conn_prev)
        dst_table_conn_prev->next = table_conn;
      else
        copy_tables->table_conn[1] = table_conn;
      dst_table_conn_prev = table_conn;
      copy_tables->link_idx_count[1]++;
      table_conn = NULL;
    }
    if (table_conn)
    {
      spider_free_tmp_dbton_share(tmp_share);
      spider_free_tmp_share_alloc(tmp_share);
      if (table_conn->copy_table)
        delete table_conn->copy_table;
      spider_free(spider_current_trx, table_conn, MYF(0));
      table_conn = NULL;
    }

    error_num = spider_sys_index_next_same(table_tables, table_key);
  } while (error_num == 0);
  spider_sys_index_end(table_tables);
  spider_close_sys_table(thd, table_tables,
    &open_tables_backup, need_lock);
  table_tables = NULL;

  if (!copy_tables->table_conn[0])
  {
    error_num = ER_SPIDER_UDF_COPY_TABLE_SRC_NOT_FOUND_NUM;
    my_printf_error(ER_SPIDER_UDF_COPY_TABLE_SRC_NOT_FOUND_NUM,
      ER_SPIDER_UDF_COPY_TABLE_SRC_NOT_FOUND_STR, MYF(0));
    goto error;
  }
  if (!copy_tables->table_conn[1])
  {
    error_num = ER_SPIDER_UDF_COPY_TABLE_DST_NOT_FOUND_NUM;
    my_printf_error(ER_SPIDER_UDF_COPY_TABLE_DST_NOT_FOUND_NUM,
      ER_SPIDER_UDF_COPY_TABLE_DST_NOT_FOUND_STR, MYF(0));
    goto error;
  }

  DBUG_RETURN(0);

error:
  if (table_tables)
    spider_close_sys_table(thd, table_tables,
      &open_tables_backup, need_lock);
  if (table_conn)
  {
    spider_free_tmp_dbton_share(tmp_share);
    spider_free_tmp_share_alloc(tmp_share);
    if (table_conn->copy_table)
      delete table_conn->copy_table;
    spider_free(spider_current_trx, table_conn, MYF(0));
  }
  DBUG_RETURN(error_num);
}

int spider_udf_get_copy_tgt_conns(
  SPIDER_COPY_TABLES *copy_tables
) {
  int error_num, roop_count;
  SPIDER_TRX *trx = copy_tables->trx;
  SPIDER_SHARE *share;
  SPIDER_COPY_TABLE_CONN *table_conn;
  DBUG_ENTER("spider_udf_get_copy_tgt_conns");
  for (roop_count = 0; roop_count < 2; roop_count++)
  {
    table_conn = copy_tables->table_conn[roop_count];
    while (table_conn)
    {
      share = table_conn->share;
      if (
        !(table_conn->conn = spider_get_conn(
          share, 0, share->conn_keys[0], trx, NULL, FALSE, FALSE,
          SPIDER_CONN_KIND_MYSQL, &error_num))
      ) {
        my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), share->server_names[0]);
        DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);
      }
      table_conn->conn->error_mode = 0;
      table_conn = table_conn->next;
    }
  }
  DBUG_RETURN(0);
}

void spider_udf_free_copy_tables_alloc(
  SPIDER_COPY_TABLES *copy_tables
) {
  int roop_count;
  SPIDER_COPY_TABLE_CONN *table_conn, *table_conn_next;
  DBUG_ENTER("spider_udf_free_copy_tables_alloc");
  for (roop_count = 0; roop_count < 2; roop_count++)
  {
    table_conn = copy_tables->table_conn[roop_count];
    while (table_conn)
    {
      table_conn_next = table_conn->next;
      spider_free_tmp_dbton_share(table_conn->share);
      spider_free_tmp_share_alloc(table_conn->share);
      if (table_conn->copy_table)
        delete table_conn->copy_table;
      spider_free(spider_current_trx, table_conn, MYF(0));
      table_conn = table_conn_next;
    }
  }
  if (copy_tables->link_idxs[0])
    spider_free(spider_current_trx, copy_tables->link_idxs[0], MYF(0));
  if (copy_tables->database)
    spider_free(spider_current_trx, copy_tables->database, MYF(0));
  spider_free(spider_current_trx, copy_tables, MYF(0));
  DBUG_VOID_RETURN;
}

int spider_udf_copy_tables_create_table_list(
  SPIDER_COPY_TABLES *copy_tables,
  char *spider_table_name,
  uint spider_table_name_length,
  char *src_link_idx_list,
  uint src_link_idx_list_length,
  char *dst_link_idx_list,
  uint dst_link_idx_list_length
) {
  int roop_count, roop_count2, length;
  char *tmp_ptr, *tmp_ptr2, *tmp_ptr3, *tmp_name_ptr;
  DBUG_ENTER("spider_udf_copy_tables_create_table_list");

  if (!spider_table_name_length)
  {
    my_printf_error(ER_SPIDER_BLANK_UDF_ARGUMENT_NUM,
      ER_SPIDER_BLANK_UDF_ARGUMENT_STR, MYF(0), 1);
    DBUG_RETURN(ER_SPIDER_BLANK_UDF_ARGUMENT_NUM);
  }

  for (roop_count2 = 0; roop_count2 < 2; roop_count2++)
  {
    if (roop_count2 == 0)
      tmp_ptr = src_link_idx_list;
    else
      tmp_ptr = dst_link_idx_list;

    while (*tmp_ptr == ' ')
      tmp_ptr++;
    if (*tmp_ptr)
      copy_tables->link_idx_count[roop_count2] = 1;
    else {
      /* use auto detect */
      copy_tables->use_auto_mode[roop_count2] = TRUE;
      copy_tables->link_idx_count[roop_count2] = 0;
      continue;
    }

    while (TRUE)
    {
      if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
      {
        copy_tables->link_idx_count[roop_count2]++;
        tmp_ptr = tmp_ptr2 + 1;
        while (*tmp_ptr == ' ')
          tmp_ptr++;
      } else
        break;
    }
  }

  if (!(copy_tables->link_idxs[0] = (int *)
    spider_bulk_malloc(spider_current_trx, 26, MYF(MY_WME | MY_ZEROFILL),
      &copy_tables->link_idxs[0],
        (uint) (sizeof(int) * copy_tables->link_idx_count[0]),
      &copy_tables->link_idxs[1],
        (uint) (sizeof(int) * copy_tables->link_idx_count[1]),
      &tmp_name_ptr, (uint) (sizeof(char) * (
        spider_table_name_length * 2 + copy_tables->database_length + 3
      )),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  copy_tables->spider_db_name = tmp_name_ptr;
  if ((tmp_ptr3 = strchr(spider_table_name, '.')))
  {
    /* exist database name */
    *tmp_ptr3 = '\0';
    length = strlen(spider_table_name);
    memcpy(tmp_name_ptr, spider_table_name, length + 1);
    copy_tables->spider_db_name_length = length;
    tmp_name_ptr += length + 1;
    tmp_ptr3++;
  } else {
    memcpy(tmp_name_ptr, copy_tables->database,
      copy_tables->database_length + 1);
    copy_tables->spider_db_name_length = copy_tables->database_length;
    tmp_name_ptr += copy_tables->database_length + 1;
    tmp_ptr3 = spider_table_name;
    length = -1;
  }
  copy_tables->spider_table_name = tmp_name_ptr;
  length = spider_table_name_length - length - 1;
  memcpy(tmp_name_ptr, tmp_ptr3, length + 1);
  copy_tables->spider_table_name_length = length;
  tmp_name_ptr += length + 1;
  memcpy(tmp_name_ptr, tmp_ptr3, length + 1);
  copy_tables->spider_real_table_name = tmp_name_ptr;
  if ((tmp_ptr2 = strstr(tmp_name_ptr, "#P#")))
  {
    *tmp_ptr2 = '\0';
    copy_tables->spider_real_table_name_length = strlen(tmp_name_ptr);
  } else
    copy_tables->spider_real_table_name_length = length;

  DBUG_PRINT("info",("spider spider_db=%s", copy_tables->spider_db_name));
  DBUG_PRINT("info",("spider spider_table_name=%s",
    copy_tables->spider_table_name));
  DBUG_PRINT("info",("spider spider_real_table_name=%s",
    copy_tables->spider_real_table_name));

  for (roop_count2 = 0; roop_count2 < 2; roop_count2++)
  {
    if (roop_count2 == 0)
      tmp_ptr = src_link_idx_list;
    else
      tmp_ptr = dst_link_idx_list;

    while (*tmp_ptr == ' ')
      tmp_ptr++;
    roop_count = 0;
    while (*tmp_ptr)
    {
      if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
        *tmp_ptr2 = '\0';

      copy_tables->link_idxs[roop_count2][roop_count] = atoi(tmp_ptr);

      DBUG_PRINT("info",("spider link_idx[%d][%d]=%d",
        roop_count2, roop_count,
        copy_tables->link_idxs[roop_count2][roop_count]));
      if (!tmp_ptr2)
        break;

      tmp_ptr = tmp_ptr2 + 1;
      while (*tmp_ptr == ' ')
        tmp_ptr++;
      roop_count++;
    }
  }
  DBUG_RETURN(0);
}

int spider_udf_bg_copy_exec_sql(
  SPIDER_COPY_TABLE_CONN *table_conn
) {
  int error_num;
  SPIDER_CONN *conn = table_conn->conn;
  ha_spider *spider = table_conn->spider;
  spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
  DBUG_ENTER("spider_udf_bg_copy_exec_sql");
  if ((error_num = spider_create_conn_thread(conn)))
    DBUG_RETURN(error_num);
  if ((error_num = dbton_hdl->set_sql_for_exec(table_conn->copy_table,
    SPIDER_SQL_TYPE_INSERT_SQL)))
    DBUG_RETURN(error_num);
  pthread_mutex_lock(&conn->bg_conn_mutex);
  conn->bg_target = spider;
  conn->bg_error_num = &table_conn->bg_error_num;
  conn->bg_sql_type = SPIDER_SQL_TYPE_INSERT_SQL;
  conn->link_idx = 0;
  conn->bg_exec_sql = TRUE;
  conn->bg_caller_sync_wait = TRUE;
  pthread_mutex_lock(&conn->bg_conn_sync_mutex);
  pthread_cond_signal(&conn->bg_conn_cond);
  pthread_mutex_unlock(&conn->bg_conn_mutex);
  pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
  pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
  conn->bg_caller_sync_wait = FALSE;
  DBUG_RETURN(0);
}

long long spider_copy_tables_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  int error_num, roop_count, all_link_cnt = 0, use_table_charset;
  SPIDER_COPY_TABLES *copy_tables = NULL;
  THD *thd = current_thd;
  TABLE_LIST *table_list = NULL;
  TABLE *table;
  TABLE_SHARE *table_share;
  KEY *key_info;
  ha_spider *spider = NULL, *tmp_spider;
  spider_string *tmp_sql = NULL;
  SPIDER_COPY_TABLE_CONN *table_conn, *src_tbl_conn, *dst_tbl_conn;
  SPIDER_CONN *tmp_conn;
  SPIDER_WIDE_HANDLER *wide_handler;
  spider_db_copy_table *select_ct, *insert_ct;
  MEM_ROOT mem_root;
  longlong bulk_insert_rows;
  Reprepare_observer *reprepare_observer_backup;
  uint tmp_conn_link_idx = 0;
  DBUG_ENTER("spider_copy_tables_body");
  if (
    thd->open_tables != 0 ||
    thd->handler_tables_hash.records != 0 ||
    thd->derived_tables != 0 ||
    thd->lock != 0 ||
    thd->locked_tables_list.locked_tables() ||
    thd->locked_tables_mode != LTM_NONE
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
    }
    goto error;
  }

  if (!(copy_tables = (SPIDER_COPY_TABLES *)
    spider_bulk_malloc(spider_current_trx, 27, MYF(MY_WME | MY_ZEROFILL),
      &copy_tables, (uint) (sizeof(SPIDER_COPY_TABLES)),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  if (!(copy_tables->trx = spider_get_trx(thd, TRUE, &error_num)))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }

  if (args->arg_count == 4)
  {
    if (spider_udf_parse_copy_tables_param(
      copy_tables,
      args->args[3] ? args->args[3] : (char *) "",
      args->args[3] ? args->lengths[3] : 0
    ))
      goto error;
  } else {
    if (spider_udf_parse_copy_tables_param(
      copy_tables,
      (char *) "",
      0
    ))
      goto error;
  }
  if (
    spider_udf_copy_tables_create_table_list(
      copy_tables,
      args->args[0],
      args->lengths[0],
      args->args[1] ? args->args[1] : (char *) "",
      args->args[1] ? args->lengths[1] : 0,
      args->args[2] ? args->args[2] : (char *) "",
      args->args[2] ? args->lengths[2] : 0
    )
  )
    goto error;

  SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
  if (
    spider_udf_get_copy_tgt_tables(
      thd,
      copy_tables,
      &mem_root,
      TRUE
    )
  ) {
    free_root(&mem_root, MYF(0));
    goto error;
  }
  free_root(&mem_root, MYF(0));

  if (
    spider_udf_get_copy_tgt_conns(copy_tables)
  )
    goto error;

  table_list = &copy_tables->spider_table_list;
  SPIDER_TABLE_LIST_db_str(table_list) = copy_tables->spider_db_name;
  SPIDER_TABLE_LIST_db_length(table_list) = copy_tables->spider_db_name_length;
  SPIDER_TABLE_LIST_alias_str(table_list) =
    SPIDER_TABLE_LIST_table_name_str(table_list) =
    copy_tables->spider_real_table_name;
  SPIDER_TABLE_LIST_table_name_length(table_list) =
    copy_tables->spider_real_table_name_length;
#ifdef SPIDER_use_LEX_CSTRING_for_database_tablename_alias
  SPIDER_TABLE_LIST_alias_length(table_list) =
    SPIDER_TABLE_LIST_table_name_length(table_list);
#endif
  table_list->lock_type = TL_READ;

  DBUG_PRINT("info",("spider db=%s", SPIDER_TABLE_LIST_db_str(table_list)));
  DBUG_PRINT("info",("spider db_length=%zd", SPIDER_TABLE_LIST_db_length(table_list)));
  DBUG_PRINT("info",("spider table_name=%s",
    SPIDER_TABLE_LIST_table_name_str(table_list)));
  DBUG_PRINT("info",("spider table_name_length=%zd",
    SPIDER_TABLE_LIST_table_name_length(table_list)));
  reprepare_observer_backup = thd->m_reprepare_observer;
  thd->m_reprepare_observer = NULL;
  copy_tables->trx->trx_start = TRUE;
  copy_tables->trx->updated_in_this_trx = FALSE;
  DBUG_PRINT("info",("spider trx->updated_in_this_trx=FALSE"));

    MDL_REQUEST_INIT(&table_list->mdl_request,
    MDL_key::TABLE,
    SPIDER_TABLE_LIST_db_str(table_list),
    SPIDER_TABLE_LIST_table_name_str(table_list),
    MDL_SHARED_READ,
    MDL_TRANSACTION
  );
  if (open_and_lock_tables(thd, table_list, FALSE, 0))
  {
    thd->m_reprepare_observer = reprepare_observer_backup;
    copy_tables->trx->trx_start = FALSE;
    copy_tables->trx->updated_in_this_trx = FALSE;
    DBUG_PRINT("info",("spider trx->updated_in_this_trx=FALSE"));
    my_printf_error(ER_SPIDER_UDF_CANT_OPEN_TABLE_NUM,
      ER_SPIDER_UDF_CANT_OPEN_TABLE_STR, MYF(0),
      SPIDER_TABLE_LIST_db_str(table_list),
      SPIDER_TABLE_LIST_table_name_str(table_list));
    goto error;
  }
  thd->m_reprepare_observer = reprepare_observer_backup;
  copy_tables->trx->trx_start = FALSE;
  copy_tables->trx->updated_in_this_trx = FALSE;
  DBUG_PRINT("info",("spider trx->updated_in_this_trx=FALSE"));

  table = table_list->table;
  table_share = table->s;
  if (table_share->primary_key == MAX_KEY)
  {
    my_printf_error(ER_SPIDER_UDF_COPY_TABLE_NEED_PK_NUM,
      ER_SPIDER_UDF_COPY_TABLE_NEED_PK_STR, MYF(0),
      SPIDER_TABLE_LIST_db_str(table_list),
      SPIDER_TABLE_LIST_table_name_str(table_list));
    goto error;
  }
  key_info = &table->key_info[table_share->primary_key];

  use_table_charset = spider_param_use_table_charset(
    copy_tables->use_table_charset);
  if (use_table_charset)
    copy_tables->access_charset = table_share->table_charset;
  else
    copy_tables->access_charset = system_charset_info;

  bulk_insert_rows = spider_param_udf_ct_bulk_insert_rows(
    copy_tables->bulk_insert_rows);
  for (src_tbl_conn = copy_tables->table_conn[0]; src_tbl_conn;
    src_tbl_conn = src_tbl_conn->next)
  {
    select_ct = src_tbl_conn->copy_table;
    src_tbl_conn->share->access_charset = copy_tables->access_charset;
    select_ct->set_sql_charset(copy_tables->access_charset);
    if (
      select_ct->append_select_str() ||
      select_ct->append_table_columns(table_share)
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }

    if (
      select_ct->append_from_str() ||
      select_ct->append_table_name(0)
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }

    select_ct->set_sql_pos();

    if (
      select_ct->append_key_order_str(key_info, 0, FALSE) ||
      select_ct->append_limit(0, bulk_insert_rows)
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }

    if (
      copy_tables->use_transaction &&
      select_ct->append_select_lock_str(SPIDER_LOCK_MODE_SHARED)
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
  }

  for (dst_tbl_conn = copy_tables->table_conn[1]; dst_tbl_conn;
    dst_tbl_conn = dst_tbl_conn->next)
  {
    insert_ct = dst_tbl_conn->copy_table;
    dst_tbl_conn->share->access_charset = copy_tables->access_charset;
    insert_ct->set_sql_charset(copy_tables->access_charset);
    if (
      insert_ct->append_insert_str(SPIDER_DB_INSERT_IGNORE) ||
      insert_ct->append_into_str() ||
      insert_ct->append_table_name(0) ||
      insert_ct->append_open_paren_str() ||
      insert_ct->append_table_columns(table_share) ||
      insert_ct->append_values_str()
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }

    insert_ct->set_sql_pos();
  }

  all_link_cnt =
    copy_tables->link_idx_count[0] + copy_tables->link_idx_count[1];
  if (
    !(tmp_sql = new spider_string[all_link_cnt]) ||
    !(spider = new ha_spider[all_link_cnt])
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
  {
    spider[roop_count].conns = NULL;
    spider[roop_count].change_table_ptr(table, table_share);
  }
  for (roop_count = 0, table_conn = copy_tables->table_conn[0];
    table_conn; roop_count++, table_conn = table_conn->next)
  {
    tmp_spider = &spider[roop_count];
    if (!(tmp_spider->dbton_handler = (spider_db_handler **)
      spider_bulk_alloc_mem(spider_current_trx, 205,
        __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
        &tmp_spider->dbton_handler,
          sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE,
        &wide_handler, sizeof(SPIDER_WIDE_HANDLER),
        NullS))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    tmp_spider->share = table_conn->share;
    tmp_spider->wide_handler = wide_handler;
    wide_handler->trx = copy_tables->trx;
/*
    if (spider_db_append_set_names(table_conn->share))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error_append_set_names;
    }
*/
    tmp_spider->conns = &table_conn->conn;
    tmp_sql[roop_count].init_calc_mem(122);
    tmp_sql[roop_count].set_charset(copy_tables->access_charset);
    tmp_spider->result_list.sqls = &tmp_sql[roop_count];
    tmp_spider->need_mons = &table_conn->need_mon;
    tmp_spider->wide_handler->lock_type = TL_READ;
    tmp_spider->conn_link_idx = &tmp_conn_link_idx;
    uint dbton_id = tmp_spider->share->use_dbton_ids[0];
    if (!(tmp_spider->dbton_handler[dbton_id] =
      spider_dbton[dbton_id].create_db_handler(tmp_spider,
      tmp_spider->share->dbton_share[dbton_id])))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error_create_dbton_handler;
    }
    if ((error_num = tmp_spider->dbton_handler[dbton_id]->init()))
    {
      goto error_init_dbton_handler;
    }
    table_conn->spider = tmp_spider;
  }
  for (table_conn = copy_tables->table_conn[1];
    table_conn; roop_count++, table_conn = table_conn->next)
  {
    tmp_spider = &spider[roop_count];
    if (!(tmp_spider->dbton_handler = (spider_db_handler **)
      spider_bulk_alloc_mem(spider_current_trx, 206,
        __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
        &tmp_spider->dbton_handler,
          sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE,
        &wide_handler, sizeof(SPIDER_WIDE_HANDLER),
        NullS))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    tmp_spider->share = table_conn->share;
    tmp_spider->wide_handler = wide_handler;
    wide_handler->trx = copy_tables->trx;
/*
    if (spider_db_append_set_names(table_conn->share))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error_append_set_names;
    }
*/
    tmp_spider->conns = &table_conn->conn;
    tmp_sql[roop_count].init_calc_mem(201);
    tmp_sql[roop_count].set_charset(copy_tables->access_charset);
    tmp_spider->result_list.sqls = &tmp_sql[roop_count];
    tmp_spider->need_mons = &table_conn->need_mon;
    tmp_spider->wide_handler->lock_type = TL_WRITE;
    tmp_spider->conn_link_idx = &tmp_conn_link_idx;
    uint dbton_id = tmp_spider->share->use_dbton_ids[0];
    if (!(tmp_spider->dbton_handler[dbton_id] =
      spider_dbton[dbton_id].create_db_handler(tmp_spider,
      tmp_spider->share->dbton_share[dbton_id])))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error_create_dbton_handler;
    }
    if ((error_num = tmp_spider->dbton_handler[dbton_id]->init()))
    {
      goto error_init_dbton_handler;
    }
    table_conn->spider = tmp_spider;
  }

  if ((error_num = spider_db_udf_copy_tables(copy_tables, spider, table,
    bulk_insert_rows)))
    goto error_db_udf_copy_tables;

/*
  for (table_conn = copy_tables->table_conn[0];
    table_conn; table_conn = table_conn->next)
    spider_db_free_set_names(table_conn->share);
  for (table_conn = copy_tables->table_conn[1];
    table_conn; table_conn = table_conn->next)
    spider_db_free_set_names(table_conn->share);
*/
  if (table_list->table)
  {
    (thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd));
    close_thread_tables(thd);
  }
  if (spider)
  {
    for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
    {
      if (spider[roop_count].share && spider[roop_count].dbton_handler)
      {
        uint dbton_id = spider[roop_count].share->use_dbton_ids[0];
        if (spider[roop_count].dbton_handler[dbton_id])
          delete spider[roop_count].dbton_handler[dbton_id];
        spider_free(spider_current_trx, spider[roop_count].dbton_handler,
          MYF(0));
      }
    }
    delete [] spider;
  }
  if (tmp_sql)
    delete [] tmp_sql;
  spider_udf_free_copy_tables_alloc(copy_tables);

  DBUG_RETURN(1);

error_db_udf_copy_tables:
error_create_dbton_handler:
error_init_dbton_handler:
/*
error_append_set_names:
*/
/*
  for (table_conn = copy_tables->table_conn[0];
    table_conn; table_conn = table_conn->next)
    spider_db_free_set_names(table_conn->share);
  for (table_conn = copy_tables->table_conn[1];
    table_conn; table_conn = table_conn->next)
    spider_db_free_set_names(table_conn->share);
*/
error:
  if (spider)
  {
    for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
    {
      tmp_spider = &spider[roop_count];
      if (tmp_spider->conns)
      {
        tmp_conn = tmp_spider->conns[0];
        if (tmp_conn && tmp_conn->db_conn &&
          tmp_conn->db_conn->get_lock_table_hash_count()
        ) {
          tmp_conn->db_conn->reset_lock_table_hash();
          tmp_conn->table_lock = 0;
        }
      }
    }
  }
  if (table_list && table_list->table)
  {
    (thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd));
    close_thread_tables(thd);
  }
  if (spider)
  {
    for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
    {
      tmp_spider = &spider[roop_count];
      if (tmp_spider->share && spider[roop_count].dbton_handler)
      {
        uint dbton_id = tmp_spider->share->use_dbton_ids[0];
        if (tmp_spider->dbton_handler[dbton_id])
          delete tmp_spider->dbton_handler[dbton_id];
        spider_free(spider_current_trx, spider[roop_count].dbton_handler,
          MYF(0));
      }
    }
    delete [] spider;
  }
  if (tmp_sql)
  {
    delete [] tmp_sql;
  }
  if (copy_tables)
  {
    spider_udf_free_copy_tables_alloc(copy_tables);
  }
  *error = 1;
  DBUG_RETURN(0);
}

my_bool spider_copy_tables_init_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  DBUG_ENTER("spider_copy_tables_init_body");
  if (args->arg_count != 3 && args->arg_count != 4)
  {
    strcpy(message, "spider_copy_tables() requires 3 or 4 arguments");
    goto error;
  }
  if (
    args->arg_type[0] != STRING_RESULT ||
    args->arg_type[1] != STRING_RESULT ||
    args->arg_type[2] != STRING_RESULT ||
    (
      args->arg_count == 4 &&
      args->arg_type[3] != STRING_RESULT
    )
  ) {
    strcpy(message, "spider_copy_tables() requires string arguments");
    goto error;
  }
  DBUG_RETURN(FALSE);

error:
  DBUG_RETURN(TRUE);
}

void spider_copy_tables_deinit_body(
  UDF_INIT *initid
) {
  int error_num;
  THD *thd = current_thd;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_copy_tables_deinit_body");
  if (
    !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN) &&
    (trx = spider_get_trx(thd, TRUE, &error_num))
  )
    spider_copy_table_free_trx_conn(trx);
  DBUG_VOID_RETURN;
}
