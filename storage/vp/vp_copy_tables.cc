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
#include "key.h"
#include "sql_base.h"
#include "transaction.h"
#include "sql_select.h"
#endif
#ifdef WITH_PARTITION_STORAGE_ENGINE
#include "ha_partition.h"
#endif
#include "vp_err.h"
#include "vp_param.h"
#include "vp_include.h"
#include "ha_vp.h"
#include "vp_table.h"
#include "vp_copy_tables.h"
#include "vp_udf.h"

extern handlerton *vp_hton_ptr;
extern handlerton *vp_partition_hton_ptr;

int vp_udf_copy_tables_create_table_list(
  VP_COPY_TABLES *copy_tables,
  char *vp_table_name,
  uint vp_table_name_length,
  char *src_table_name_list,
  uint src_table_name_list_length,
  char *dst_table_name_list,
  uint dst_table_name_list_length
) {
  int roop_count, roop_count2, length;
  char *tmp_ptr, *tmp_ptr2, *tmp_ptr3, *tmp_name_ptr;
  DBUG_ENTER("vp_udf_copy_tables_create_table_list");

  if (!vp_table_name_length)
  {
    my_printf_error(ER_VP_BLANK_UDF_ARGUMENT_NUM,
      ER_VP_BLANK_UDF_ARGUMENT_STR, MYF(0), 1);
    DBUG_RETURN(ER_VP_BLANK_UDF_ARGUMENT_NUM);
  }

  for (roop_count2 = 0; roop_count2 < 2; roop_count2++)
  {
    if (roop_count2 == 0)
      tmp_ptr = src_table_name_list;
    else
      tmp_ptr = dst_table_name_list;

    while (*tmp_ptr == ' ')
      tmp_ptr++;
    if (*tmp_ptr)
      copy_tables->table_count[roop_count2] = 1;
    else {
      copy_tables->table_count[roop_count2] = 0;
      my_printf_error(ER_VP_BLANK_UDF_ARGUMENT_NUM,
        ER_VP_BLANK_UDF_ARGUMENT_STR, MYF(0), roop_count2 + 2);
      DBUG_RETURN(ER_VP_BLANK_UDF_ARGUMENT_NUM);
    }

    while (TRUE)
    {
      if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
      {
        copy_tables->table_count[roop_count2]++;
        tmp_ptr = tmp_ptr2 + 1;
        while (*tmp_ptr == ' ')
          tmp_ptr++;
      } else
        break;
    }
  }

  if (!(copy_tables->db_names[0] = (char**)
    my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
      &copy_tables->db_names[0], sizeof(char*) * copy_tables->table_count[0],
      &copy_tables->db_names[1], sizeof(char*) * copy_tables->table_count[1],
      &copy_tables->db_names_length[0],
        sizeof(uint) * copy_tables->table_count[0],
      &copy_tables->db_names_length[1],
        sizeof(uint) * copy_tables->table_count[1],
      &copy_tables->table_names[0],
        sizeof(char*) * copy_tables->table_count[0],
      &copy_tables->table_names[1],
        sizeof(char*) * copy_tables->table_count[1],
      &copy_tables->table_names_length[0],
        sizeof(uint) * copy_tables->table_count[0],
      &copy_tables->table_names_length[1],
        sizeof(uint) * copy_tables->table_count[1],
      &copy_tables->table_idx[0],
        sizeof(uint) * copy_tables->table_count[0],
      &copy_tables->table_idx[1],
        sizeof(uint) * copy_tables->table_count[1],
      &tmp_name_ptr, sizeof(char) * (
        vp_table_name_length + copy_tables->default_database_length + 2 +
        src_table_name_list_length +
        copy_tables->default_database_length * copy_tables->table_count[0] +
        2 * copy_tables->table_count[0] +
        dst_table_name_list_length +
        copy_tables->default_database_length * copy_tables->table_count[1] +
        2 * copy_tables->table_count[1] +
        (copy_tables->table_name_prefix_length +
          copy_tables->table_name_suffix_length) *
          (copy_tables->table_count[0] + copy_tables->table_count[1])
      ),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  copy_tables->vp_db_name = tmp_name_ptr;
  if ((tmp_ptr3 = strchr(vp_table_name, '.')))
  {
    /* exist database name */
    *tmp_ptr3 = '\0';
    length = strlen(vp_table_name);
    memcpy(tmp_name_ptr, vp_table_name, length + 1);
    copy_tables->vp_db_name_length = length;
    tmp_name_ptr += length + 1;
    tmp_ptr3++;
  } else {
    memcpy(tmp_name_ptr, copy_tables->default_database,
      copy_tables->default_database_length + 1);
    copy_tables->vp_db_name_length = copy_tables->default_database_length;
    tmp_name_ptr += copy_tables->default_database_length + 1;
    tmp_ptr3 = vp_table_name;
    length = -1;
  }
  copy_tables->vp_table_name = tmp_name_ptr;
  length = vp_table_name_length - length - 1;
  memcpy(tmp_name_ptr, tmp_ptr3, length + 1);
  copy_tables->vp_table_name_length = length;
  tmp_name_ptr += length + 1;

  DBUG_PRINT("info",("vp vp_db=%s", copy_tables->vp_db_name));
  DBUG_PRINT("info",("vp vp_table_name=%s", copy_tables->vp_table_name));

  for (roop_count2 = 0; roop_count2 < 2; roop_count2++)
  {
    if (roop_count2 == 0)
      tmp_ptr = src_table_name_list;
    else
      tmp_ptr = dst_table_name_list;

    while (*tmp_ptr == ' ')
      tmp_ptr++;
    roop_count = 0;
    while (TRUE)
    {
      if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
        *tmp_ptr2 = '\0';

      copy_tables->db_names[roop_count2][roop_count] = tmp_name_ptr;

      if ((tmp_ptr3 = strchr(tmp_ptr, '.')))
      {
        /* exist database name */
        *tmp_ptr3 = '\0';
        length = strlen(tmp_ptr);
        memcpy(tmp_name_ptr, tmp_ptr, length + 1);
        copy_tables->db_names_length[roop_count2][roop_count] = length;
        tmp_name_ptr += length + 1;
        tmp_ptr = tmp_ptr3 + 1;
      } else {
        memcpy(tmp_name_ptr, copy_tables->default_database,
          copy_tables->default_database_length + 1);
        copy_tables->db_names_length[roop_count2][roop_count] =
          copy_tables->default_database_length;
        tmp_name_ptr += copy_tables->default_database_length + 1;
      }

      copy_tables->table_names[roop_count2][roop_count] = tmp_name_ptr;
      memcpy(tmp_name_ptr, copy_tables->table_name_prefix,
        copy_tables->table_name_prefix_length);
      tmp_name_ptr += copy_tables->table_name_prefix_length;
      length = strlen(tmp_ptr);
      memcpy(tmp_name_ptr, tmp_ptr, length);
      tmp_name_ptr += length;
      memcpy(tmp_name_ptr, copy_tables->table_name_suffix,
        copy_tables->table_name_suffix_length + 1);
      tmp_name_ptr += copy_tables->table_name_suffix_length + 1;
      copy_tables->table_names_length[roop_count2][roop_count] =
        copy_tables->table_name_prefix_length + length +
        copy_tables->table_name_suffix_length;

      DBUG_PRINT("info",("vp db=%s",
        copy_tables->db_names[roop_count2][roop_count]));
      DBUG_PRINT("info",("vp table_name=%s",
        copy_tables->table_names[roop_count2][roop_count]));

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

int vp_udf_parse_copy_tables_param(
  VP_COPY_TABLES *copy_tables,
  char *param,
  int param_length
) {
  int error_num = 0;
  char *param_string = NULL;
  char *sprit_ptr[2];
  char *tmp_ptr, *tmp_ptr2, *start_ptr;
  int title_length;
  VP_PARAM_STRING_PARSE param_string_parse;
  DBUG_ENTER("vp_udf_parse_copy_tables_param");
  copy_tables->bulk_insert_interval = -1;
  copy_tables->bulk_insert_rows = -1;
  copy_tables->suppress_autoinc = -1;

  if (param_length == 0)
    goto set_default;
  DBUG_PRINT("info",("vp create param_string string"));
  if (
    !(param_string = vp_create_string(
      param,
      param_length))
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error_alloc_param_string;
  }
  DBUG_PRINT("info",("vp param_string=%s", param_string));

  sprit_ptr[0] = param_string;
  param_string_parse.init(param_string, ER_VP_INVALID_UDF_PARAM_NUM);
  while (sprit_ptr[0])
  {
    if ((sprit_ptr[1] = strchr(sprit_ptr[0], ',')))
    {
      *sprit_ptr[1] = '\0';
      sprit_ptr[1]++;
    }
    tmp_ptr = sprit_ptr[0];
    sprit_ptr[0] = sprit_ptr[1];
    while (*tmp_ptr == ' ' || *tmp_ptr == '\r' ||
      *tmp_ptr == '\n' || *tmp_ptr == '\t')
      tmp_ptr++;

    if (*tmp_ptr == '\0')
      continue;

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

    switch (title_length)
    {
      case 0:
        error_num = param_string_parse.print_param_error();
        if (error_num)
          goto error;
        continue;
      case 3:
        VP_PARAM_INT(copy_tables, "bii", bulk_insert_interval, 0);
        VP_PARAM_LONGLONG(copy_tables, "bir", bulk_insert_rows, 1);
        VP_PARAM_STR(copy_tables, "ddb", default_database);
        VP_PARAM_INT_WITH_MAX(copy_tables, "sai", suppress_autoinc, 0, 1);
        VP_PARAM_STR(copy_tables, "tnp", table_name_prefix);
        VP_PARAM_STR(copy_tables, "tns", table_name_suffix);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 16:
        VP_PARAM_LONGLONG(copy_tables, "bulk_insert_rows", bulk_insert_rows,
          1);
        VP_PARAM_STR(copy_tables, "default_database", default_database);
        VP_PARAM_INT_WITH_MAX(copy_tables, "suppress_autoinc",
          suppress_autoinc, 0, 1);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 17:
        VP_PARAM_STR(copy_tables, "table_name_prefix", table_name_prefix);
        VP_PARAM_STR(copy_tables, "table_name_suffix", table_name_suffix);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 20:
        VP_PARAM_INT(copy_tables, "bulk_insert_interval", bulk_insert_interval,
          0);
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
  if ((error_num = vp_udf_set_copy_tables_param_default(
    copy_tables
  )))
    goto error;

  if (param_string)
    vp_my_free(param_string, MYF(0));
  DBUG_RETURN(0);

error:
  if (param_string)
    vp_my_free(param_string, MYF(0));
error_alloc_param_string:
  DBUG_RETURN(error_num);
}

int vp_udf_set_copy_tables_param_default(
  VP_COPY_TABLES *copy_tables
) {
  DBUG_ENTER("vp_udf_set_copy_tables_param_default");

  if (!copy_tables->default_database)
  {
    DBUG_PRINT("info",("vp create default default_database"));
    copy_tables->default_database_length = VP_THD_db_length(copy_tables->thd);
    if (
      !(copy_tables->default_database = vp_create_string(
        VP_THD_db_str(copy_tables->thd),
        copy_tables->default_database_length))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (!copy_tables->table_name_prefix)
  {
    DBUG_PRINT("info",("vp create default table_name_prefix"));
    copy_tables->table_name_prefix_length = 0;
    if (
      !(copy_tables->table_name_prefix = vp_create_string(
        "",
        copy_tables->table_name_prefix_length))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (!copy_tables->table_name_suffix)
  {
    DBUG_PRINT("info",("vp create default table_name_suffix"));
    copy_tables->table_name_suffix_length = 0;
    if (
      !(copy_tables->table_name_suffix = vp_create_string(
        "",
        copy_tables->table_name_suffix_length))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (copy_tables->bulk_insert_interval == -1)
    copy_tables->bulk_insert_interval = 10;
  if (copy_tables->bulk_insert_rows == -1)
    copy_tables->bulk_insert_rows = 100;
  if (copy_tables->suppress_autoinc == -1)
    copy_tables->suppress_autoinc = 0;
  DBUG_RETURN(0);
}

void vp_udf_free_copy_tables_alloc(
  VP_COPY_TABLES *copy_tables
) {
  DBUG_ENTER("vp_udf_free_copy_tables_alloc");
  if (copy_tables->db_names[0])
    vp_my_free(copy_tables->db_names[0], MYF(0));
  if (copy_tables->default_database)
    vp_my_free(copy_tables->default_database, MYF(0));
  if (copy_tables->table_name_prefix)
    vp_my_free(copy_tables->table_name_prefix, MYF(0));
  if (copy_tables->table_name_suffix)
    vp_my_free(copy_tables->table_name_suffix, MYF(0));
  vp_my_free(copy_tables, MYF(0));
  DBUG_VOID_RETURN;
}

long long vp_copy_tables_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  int error_num, roop_count, roop_count2, roop_count3;
  VP_COPY_TABLES *copy_tables = NULL;
  THD *thd = current_thd;
  TABLE_LIST *table_list;
  uint db_name_length, table_name_length;
  char *db_name, *table_name;
  int part_idx = -1, tmp_idx;
  ha_vp *vp_table = NULL;
  TABLE *table;
  VP_SHARE *share;
  TABLE_LIST *part_tables;
  uchar *src_bitmap = NULL, *dst_bitmap, *cpy_clm_bitmap,
    *select_ignore = NULL, *select_ignore_with_lock = NULL,
    *update_ignore = NULL;
  uchar start_key[MAX_KEY_LENGTH], end_key[MAX_KEY_LENGTH];
  key_range start_key_range, end_key_range;
  KEY *key_info, *saved_key_info;
  KEY_PART_INFO *key_part, *saved_key_part;
  longlong bulk_insert_rows;
  int bulk_insert_interval;
  ulong table_def_version;
  bool restart = FALSE;
#if MYSQL_VERSION_ID < 50500
#else
  uint flags = (
    MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
    MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
    MYSQL_OPEN_IGNORE_FLUSH |
    MYSQL_LOCK_IGNORE_TIMEOUT |
#ifdef VP_USE_OPEN_SKIP_TEMPORARY
    MYSQL_OPEN_SKIP_TEMPORARY |
#endif
    MYSQL_OPEN_GET_NEW_TABLE
  );
#endif
  Reprepare_observer *reprepare_observer_backup;

  DBUG_ENTER("vp_copy_tables_body");
  if (
    thd->open_tables != 0 ||
    thd->temporary_tables != 0 ||
    thd->handler_tables_hash.records != 0 ||
    thd->derived_tables != 0 ||
    thd->lock != 0 ||
#if MYSQL_VERSION_ID < 50500
    thd->locked_tables != 0 ||
    thd->prelocked_mode != NON_PRELOCKED ||
#else
    thd->locked_tables_list.locked_tables() ||
    thd->locked_tables_mode != LTM_NONE ||
#endif
    thd->m_reprepare_observer != NULL
  ) {
    my_printf_error(ER_VP_UDF_CANT_USE_IF_OPEN_TABLE_NUM,
      ER_VP_UDF_CANT_USE_IF_OPEN_TABLE_STR, MYF(0));
    goto error;
  }

  if (!(copy_tables = (VP_COPY_TABLES *)
    my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
      &copy_tables, sizeof(VP_COPY_TABLES),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  copy_tables->thd = thd;

  if (args->arg_count == 4)
  {
    if (vp_udf_parse_copy_tables_param(
      copy_tables,
      args->args[3] ? args->args[3] : (char *) "",
      args->args[3] ? args->lengths[3] : 0
    ))
      goto error;
  } else {
    if (vp_udf_parse_copy_tables_param(
      copy_tables,
      (char *) "",
      0
    ))
      goto error;
  }
  if (vp_udf_copy_tables_create_table_list(
    copy_tables,
    args->args[0],
    args->lengths[0],
    args->args[1] ? args->args[1] : (char *) "",
    args->args[1] ? args->lengths[1] : 0,
    args->args[2] ? args->args[2] : (char *) "",
    args->args[2] ? args->lengths[2] : 0
  ))
    goto error;

  table_list = &copy_tables->vp_table_list;
  VP_TABLE_LIST_db_str(table_list) = copy_tables->vp_db_name;
  VP_TABLE_LIST_db_length(table_list) = copy_tables->vp_db_name_length;
  VP_TABLE_LIST_alias_str(table_list) =
    VP_TABLE_LIST_table_name_str(table_list) = copy_tables->vp_table_name;
#ifdef VP_TABLE_LIST_ALIAS_HAS_LENGTH
  VP_TABLE_LIST_alias_length(table_list) =
#endif
    VP_TABLE_LIST_table_name_length(table_list) =
    copy_tables->vp_table_name_length;
  table_list->lock_type = TL_WRITE;

  reprepare_observer_backup = thd->m_reprepare_observer;
  thd->m_reprepare_observer = NULL;
#if MYSQL_VERSION_ID < 50500
  if (open_and_lock_tables(thd, table_list))
#else
  table_list->mdl_request.init(
    MDL_key::TABLE,
    VP_TABLE_LIST_db_str(table_list),
    VP_TABLE_LIST_table_name_str(table_list),
    MDL_SHARED_WRITE,
    MDL_TRANSACTION
  );
  if (open_and_lock_tables(thd, table_list, FALSE, flags))
#endif
  {
    thd->m_reprepare_observer = reprepare_observer_backup;
    my_printf_error(ER_VP_UDF_CANT_OPEN_TABLE_NUM,
      ER_VP_UDF_CANT_OPEN_TABLE_STR, MYF(0));
    goto error;
  }
  thd->m_reprepare_observer = reprepare_observer_backup;

change_table_version:
  if (!(
#ifdef WITH_PARTITION_STORAGE_ENGINE
#ifdef PARTITION_HAS_GET_CHILD_HANDLERS
    (table_list->table->file->ht == vp_partition_hton_ptr &&
      vp_get_default_part_db_type_from_partition(table_list->table->s) ==
        vp_hton_ptr) ||
#endif
#endif
    table_list->table->file->ht == vp_hton_ptr
  )) {
    my_printf_error(ER_VP_UDF_IS_NOT_VP_TABLE_NUM,
      ER_VP_UDF_IS_NOT_VP_TABLE_STR, MYF(0));
    goto error;
  }

  db_name_length = copy_tables->db_names_length[0][0];
  table_name_length = copy_tables->table_names_length[0][0];
  db_name = copy_tables->db_names[0][0];
  table_name = copy_tables->table_names[0][0];
#ifdef WITH_PARTITION_STORAGE_ENGINE
#ifdef PARTITION_HAS_GET_CHILD_HANDLERS
  if (table_list->table->file->ht == vp_partition_hton_ptr)
  {
    ha_vp **vp_table_ptr = (ha_vp **)
      ((ha_partition *) table_list->table->file)->get_child_handlers();
    for (roop_count = 0; vp_table_ptr[roop_count]; roop_count++)
    {
      vp_table = vp_table_ptr[roop_count];
      share = vp_table->share;
      part_tables = vp_table->part_tables;
      for (roop_count2 = 0; roop_count2 < share->table_count; roop_count2++)
      {
        if (
          VP_TABLE_LIST_db_length(&part_tables[roop_count2]) ==
            db_name_length &&
          VP_TABLE_LIST_table_name_length(&part_tables[roop_count2]) ==
            table_name_length &&
          !memcmp(VP_TABLE_LIST_db_str(&part_tables[roop_count2]), db_name,
            db_name_length) &&
          !memcmp(VP_TABLE_LIST_table_name_str(&part_tables[roop_count2]),
            table_name, table_name_length)
        ) {
          part_idx = roop_count;
          copy_tables->table_idx[0][0] = roop_count2;
          break;
        }
      }
      if (part_idx >= 0)
        break;
    }
    if (part_idx == -1)
    {
      my_printf_error(ER_VP_UDF_CANT_FIND_TABLE_NUM,
        ER_VP_UDF_CANT_FIND_TABLE_STR, MYF(0), db_name, table_name);
      goto error;
    }
  } else {
#endif
#endif
    table = table_list->table;
    vp_table = (ha_vp *) table->file;
    share = vp_table->share;
    part_tables = vp_table->part_tables;
    for (roop_count2 = 0; roop_count2 < share->table_count; roop_count2++)
    {
      if (
        VP_TABLE_LIST_db_length(&part_tables[roop_count2]) == db_name_length &&
        VP_TABLE_LIST_table_name_length(&part_tables[roop_count2]) ==
          table_name_length &&
        !memcmp(VP_TABLE_LIST_db_str(&part_tables[roop_count2]), db_name,
          db_name_length) &&
        !memcmp(VP_TABLE_LIST_table_name_str(&part_tables[roop_count2]),
          table_name, table_name_length)
      ) {
        copy_tables->table_idx[0][0] = roop_count2;
        break;
      }
    }
    if (roop_count2 == share->table_count)
    {
      my_printf_error(ER_VP_UDF_CANT_FIND_TABLE_NUM,
        ER_VP_UDF_CANT_FIND_TABLE_STR, MYF(0), db_name, table_name);
      goto error;
    }
#ifdef WITH_PARTITION_STORAGE_ENGINE
#ifdef PARTITION_HAS_GET_CHILD_HANDLERS
  }
#endif
#endif
  table_def_version = table_list->table->s->get_table_def_version();
  if (!share->zero_record_update_mode)
  {
    my_printf_error(ER_VP_UDF_MUST_SET_ZRU_NUM,
      ER_VP_UDF_MUST_SET_ZRU_STR, MYF(0), db_name, table_name);
    goto error;
  }

  for (roop_count = 0; roop_count < 2; roop_count++)
  {
    for (roop_count2 = 0; roop_count2 < copy_tables->table_count[roop_count];
      roop_count2++)
    {
      if (roop_count == 0 && roop_count2 == 0)
        continue;

      db_name_length = copy_tables->db_names_length[roop_count][roop_count2];
      table_name_length =
        copy_tables->table_names_length[roop_count][roop_count2];
      db_name = copy_tables->db_names[roop_count][roop_count2];
      table_name = copy_tables->table_names[roop_count][roop_count2];
      for (roop_count3 = 0; roop_count3 < share->table_count; roop_count3++)
      {
        if (
          VP_TABLE_LIST_db_length(&part_tables[roop_count3]) ==
            db_name_length &&
          VP_TABLE_LIST_table_name_length(&part_tables[roop_count3]) ==
            table_name_length &&
          !memcmp(VP_TABLE_LIST_db_str(&part_tables[roop_count3]), db_name,
            db_name_length) &&
          !memcmp(VP_TABLE_LIST_table_name_str(&part_tables[roop_count3]),
            table_name, table_name_length)
        ) {
          copy_tables->table_idx[roop_count][roop_count2] = roop_count3;
          break;
        }
      }
      if (roop_count3 == share->table_count)
      {
        my_printf_error(ER_VP_UDF_CANT_FIND_TABLE_NUM,
          ER_VP_UDF_CANT_FIND_TABLE_STR, MYF(0), db_name, table_name);
        goto error;
      }
    }
  }

  key_info = &table_list->table->key_info[
    table_list->table->s->primary_key];
  if (!(src_bitmap = (uchar *)
    my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
      &src_bitmap, sizeof(uchar) * share->use_tables_size,
      &dst_bitmap, sizeof(uchar) * share->use_tables_size,
      &cpy_clm_bitmap, sizeof(uchar) * share->bitmap_size,
      &saved_key_info, sizeof(KEY),
      &saved_key_part,
        sizeof(KEY_PART_INFO) * vp_user_defined_key_parts(key_info),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  for (roop_count = 0; roop_count < copy_tables->table_count[0]; roop_count++)
  {
    vp_set_bit(src_bitmap, copy_tables->table_idx[0][roop_count]);
  }
  for (roop_count = 0; roop_count < copy_tables->table_count[1]; roop_count++)
  {
    vp_set_bit(dst_bitmap, copy_tables->table_idx[1][roop_count]);
  }
  for (roop_count = 0; roop_count < share->use_tables_size; roop_count++)
  {
    if (src_bitmap[roop_count] & dst_bitmap[roop_count])
    {
      my_printf_error(ER_VP_UDF_FIND_SAME_TABLE_NUM,
        ER_VP_UDF_FIND_SAME_TABLE_STR, MYF(0));
      goto error;
    }
    src_bitmap[roop_count] = ~src_bitmap[roop_count];
    dst_bitmap[roop_count] = ~dst_bitmap[roop_count];
  }
  for (roop_count = 0; roop_count < copy_tables->table_count[1]; roop_count++)
  {
    tmp_idx = copy_tables->table_idx[1][roop_count];
    for (roop_count2 = 0; roop_count2 < share->bitmap_size; roop_count2++)
    {
      cpy_clm_bitmap[roop_count2] |= share->correspond_columns_bit[
        (tmp_idx * share->bitmap_size) + roop_count2];
    }
  }
  select_ignore = vp_table->select_ignore;
  vp_table->select_ignore = src_bitmap;
  select_ignore_with_lock = vp_table->select_ignore_with_lock;
  vp_table->select_ignore_with_lock = src_bitmap;
  update_ignore = vp_table->update_ignore;
  vp_table->update_ignore = dst_bitmap;
  memset((uchar *) table_list->table->read_set->bitmap, 0,
    sizeof(uchar) * share->bitmap_size);
  memset((uchar *) table_list->table->write_set->bitmap, 0,
    sizeof(uchar) * share->bitmap_size);

  memcpy(saved_key_info, key_info, sizeof(KEY));
  key_part = key_info->key_part;
  for (roop_count = 0; roop_count < (int) vp_user_defined_key_parts(key_info);
    roop_count++)
  {
    memcpy(saved_key_part, key_part, sizeof(KEY_PART_INFO));
    vp_set_bit(table_list->table->read_set->bitmap,
      key_part[roop_count].field->field_index);
  }

  if (!restart)
  {
    DBUG_PRINT("info",("vp get PK max"));
    if (
      (error_num = vp_table->extra(HA_EXTRA_KEYREAD)) ||
      (error_num = vp_table->ha_index_init(table_list->table->s->primary_key,
        TRUE))
    ) {
      DBUG_PRINT("info",("vp error_num=%d", error_num));
      vp_table->print_error(error_num, MYF(0));
      goto error;
    }
    if ((error_num = vp_table->index_last(table_list->table->record[0])))
    {
      if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
      {
        DBUG_PRINT("info",("vp error_num=%d", error_num));
        vp_table->print_error(error_num, MYF(0));
        vp_table->ha_index_end();
        goto error;
      }
      /* no data */
      vp_table->ha_index_end();
      vp_table->extra(HA_EXTRA_NO_KEYREAD);
      goto end;
    }
    key_copy(
      end_key,
      table_list->table->record[0],
      key_info,
      key_info->key_length);
    vp_table->ha_index_end();
    vp_table->extra(HA_EXTRA_NO_KEYREAD);

    DBUG_PRINT("info",("vp get PK min"));
    if (
      (error_num = vp_table->extra(HA_EXTRA_KEYREAD)) ||
      (error_num = vp_table->ha_index_init(table_list->table->s->primary_key,
        TRUE))
    ) {
      DBUG_PRINT("info",("vp error_num=%d", error_num));
      vp_table->print_error(error_num, MYF(0));
      goto error;
    }
    if ((error_num = vp_table->index_first(table_list->table->record[0])))
    {
      if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
      {
        DBUG_PRINT("info",("vp error_num=%d", error_num));
        vp_table->print_error(error_num, MYF(0));
        vp_table->ha_index_end();
        goto error;
      }
      /* no data */
      vp_table->ha_index_end();
      vp_table->extra(HA_EXTRA_NO_KEYREAD);
      goto end;
    }
    key_copy(
      start_key,
      table_list->table->record[0],
      key_info,
      key_info->key_length);
    vp_table->ha_index_end();
    vp_table->extra(HA_EXTRA_NO_KEYREAD);
  }

  if (!restart)
  {
    DBUG_PRINT("info",("vp create initial range"));
    start_key_range.keypart_map = make_prev_keypart_map(
      vp_user_defined_key_parts(key_info));
    start_key_range.flag = HA_READ_KEY_OR_NEXT;
    start_key_range.length = vp_user_defined_key_parts(key_info);
    start_key_range.key = start_key;
    end_key_range.keypart_map = make_prev_keypart_map(
      vp_user_defined_key_parts(key_info));
    end_key_range.flag = HA_READ_KEY_OR_PREV;
    end_key_range.length = vp_user_defined_key_parts(key_info);
    end_key_range.key = end_key;

    goto first_close;
  }
  while (TRUE)
  {
    key_info = &table_list->table->key_info[
      table_list->table->s->primary_key];
    key_part = key_info->key_part;

    memcpy((uchar *) table_list->table->read_set->bitmap, cpy_clm_bitmap,
      sizeof(uchar) * share->bitmap_size);
    memcpy((uchar *) table_list->table->write_set->bitmap, cpy_clm_bitmap,
      sizeof(uchar) * share->bitmap_size);

    DBUG_PRINT("info",("vp search init"));
    bulk_insert_rows = vp_param_udf_ct_bulk_insert_rows(
      copy_tables->bulk_insert_rows);
    if (
      (error_num = vp_table->ha_index_init(table_list->table->s->primary_key,
        TRUE))
    ) {
      DBUG_PRINT("info",("vp error_num=%d", error_num));
      vp_table->print_error(error_num, MYF(0));
      goto error;
    }
    if ((error_num = vp_table->read_range_first(
      &start_key_range, &end_key_range, FALSE, TRUE)))
    {
      if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
      {
        DBUG_PRINT("info",("vp error_num=%d", error_num));
        vp_table->print_error(error_num, MYF(0));
        vp_table->ha_index_end();
        goto error;
      }
      /* no data */
      vp_table->ha_index_end();
      goto end;
    }

    DBUG_PRINT("info",("vp insert init"));
    if ((error_num = vp_table->extra(HA_EXTRA_IGNORE_DUP_KEY)))
    {
      DBUG_PRINT("info",("vp error_num=%d", error_num));
      vp_table->print_error(error_num, MYF(0));
      goto error;
    }
    if (!copy_tables->suppress_autoinc)
    {
      table_list->table->next_number_field =
        table_list->table->found_next_number_field;
      table_list->table->auto_increment_field_not_null = TRUE;
    } else
      vp_table->suppress_autoinc = TRUE;
    vp_table->ha_start_bulk_insert(bulk_insert_rows);

    roop_count = 0;
    while (TRUE)
    {
      DBUG_PRINT("info",("vp insert"));
      if ((error_num = vp_table->ha_write_row(table_list->table->record[0])))
      {
        if (vp_table->is_fatal_error(error_num, HA_CHECK_DUP))
        {
          DBUG_PRINT("info",("vp error_num=%d", error_num));
          vp_table->print_error(error_num, MYF(0));
          break;
        } else
          error_num = 0;
      }

      roop_count++;
      if (roop_count >= bulk_insert_rows)
        break;

      DBUG_PRINT("info",("vp search next"));
      if ((error_num = vp_table->read_range_next()))
      {
        if (error_num != HA_ERR_KEY_NOT_FOUND &&
          error_num != HA_ERR_END_OF_FILE)
        {
          DBUG_PRINT("info",("vp error_num=%d", error_num));
          vp_table->print_error(error_num, MYF(0));
          break;
        }
        /* no data */
        bulk_insert_rows = 0;
        break;
      }
    }

    vp_table->ha_end_bulk_insert();
    vp_table->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);
    vp_table->ha_release_auto_increment();
    table_list->table->next_number_field = NULL;
    table_list->table->auto_increment_field_not_null = FALSE;
    vp_table->suppress_autoinc = FALSE;
    vp_table->ha_index_end();
    if (!bulk_insert_rows)
      goto end;
    if (error_num)
      goto error;

    key_copy(
      start_key,
      table_list->table->record[0],
      key_info,
      key_info->key_length);
    start_key_range.flag = HA_READ_AFTER_KEY;

first_close:
    vp_table->select_ignore = select_ignore;
    vp_table->select_ignore_with_lock = select_ignore_with_lock;
    vp_table->update_ignore = update_ignore;
    select_ignore = NULL;
    select_ignore_with_lock = NULL;
    update_ignore = NULL;
#if MYSQL_VERSION_ID < 50500
    ha_autocommit_or_rollback(thd, 0);
#else
    (thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd));
#endif
    close_thread_tables(thd);
    table_list->table = NULL;

    DBUG_PRINT("info",("vp sleep"));
    bulk_insert_interval = vp_param_udf_ct_bulk_insert_interval(
      copy_tables->bulk_insert_interval);
    my_sleep(bulk_insert_interval);

    DBUG_PRINT("info",("vp wakeup"));
    reprepare_observer_backup = thd->m_reprepare_observer;
    thd->m_reprepare_observer = NULL;
#if MYSQL_VERSION_ID < 50500
    if (open_and_lock_tables(thd, table_list))
#else
    table_list->table = NULL;
    table_list->next_global = NULL;
    table_list->lock_type = TL_WRITE;
    table_list->mdl_request.init(
      MDL_key::TABLE,
      VP_TABLE_LIST_db_str(table_list),
      VP_TABLE_LIST_table_name_str(table_list),
      MDL_SHARED_WRITE,
      MDL_TRANSACTION
    );
    if (open_and_lock_tables(thd, table_list, FALSE, flags))
#endif
    {
      thd->m_reprepare_observer = reprepare_observer_backup;
      my_printf_error(ER_VP_UDF_CANT_OPEN_TABLE_NUM,
        ER_VP_UDF_CANT_OPEN_TABLE_STR, MYF(0));
      goto error;
    }
    thd->m_reprepare_observer = reprepare_observer_backup;
    if (table_def_version != table_list->table->s->get_table_def_version())
    {
      key_info = &table_list->table->key_info[
        table_list->table->s->primary_key];
      if (
        key_info->key_length != saved_key_info->key_length ||
        key_info->flags != saved_key_info->flags ||
        vp_user_defined_key_parts(key_info) !=
          vp_user_defined_key_parts(saved_key_info) ||
#ifdef VP_KEY_HAS_EXTRA_LENGTH
        key_info->extra_length != saved_key_info->extra_length ||
#endif
        key_info->usable_key_parts != saved_key_info->usable_key_parts ||
        key_info->block_size != saved_key_info->block_size ||
        key_info->algorithm != saved_key_info->algorithm
      ) {
        my_printf_error(ER_VP_UDF_FIND_CHANGE_TABLE_NUM,
          ER_VP_UDF_FIND_CHANGE_TABLE_STR, MYF(0));
        goto error;
      }
      key_part = key_info->key_part;
      for (roop_count = 0;
        roop_count < (int) vp_user_defined_key_parts(key_info);
        roop_count++)
      {
        if (
          key_part->offset != saved_key_part->offset ||
          key_part->null_offset != saved_key_part->null_offset ||
          key_part->length != saved_key_part->length ||
          key_part->store_length != saved_key_part->store_length ||
          key_part->key_type != saved_key_part->key_type ||
          key_part->fieldnr != saved_key_part->fieldnr ||
          key_part->key_part_flag != saved_key_part->key_part_flag ||
          key_part->type != saved_key_part->type ||
          key_part->null_bit != saved_key_part->null_bit
        ) {
          my_printf_error(ER_VP_UDF_FIND_CHANGE_TABLE_NUM,
            ER_VP_UDF_FIND_CHANGE_TABLE_STR, MYF(0));
          goto error;
        }
      }
      DBUG_PRINT("info",("vp restart"));
      restart = TRUE;
      part_idx = -1;
      vp_my_free(src_bitmap, MYF(0));
      src_bitmap = NULL;
      goto change_table_version;
    }

#ifdef WITH_PARTITION_STORAGE_ENGINE
#ifdef PARTITION_HAS_GET_CHILD_HANDLERS
    if (table_list->table->file->ht == vp_partition_hton_ptr)
    {
      ha_vp **vp_table_ptr = (ha_vp **)
        ((ha_partition *) table_list->table->file)->get_child_handlers();
      vp_table = vp_table_ptr[part_idx];
      share = vp_table->share;
      part_tables = vp_table->part_tables;
    } else {
#endif
#endif
      vp_table = (ha_vp *) table_list->table->file;
      share = vp_table->share;
      part_tables = vp_table->part_tables;
#ifdef WITH_PARTITION_STORAGE_ENGINE
#ifdef PARTITION_HAS_GET_CHILD_HANDLERS
    }
#endif
#endif
    select_ignore = vp_table->select_ignore;
    vp_table->select_ignore = src_bitmap;
    select_ignore_with_lock = vp_table->select_ignore_with_lock;
    vp_table->select_ignore_with_lock = src_bitmap;
    update_ignore = vp_table->update_ignore;
    vp_table->update_ignore = dst_bitmap;
  }

end:
  if (select_ignore)
    vp_table->select_ignore = select_ignore;
  if (select_ignore_with_lock)
    vp_table->select_ignore_with_lock = select_ignore_with_lock;
  if (update_ignore)
    vp_table->update_ignore = update_ignore;
#if MYSQL_VERSION_ID < 50500
  ha_autocommit_or_rollback(thd, 0);
#else
  (thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd));
#endif
  close_thread_tables(thd);
  if (src_bitmap)
    vp_my_free(src_bitmap, MYF(0));
  if (copy_tables)
    vp_udf_free_copy_tables_alloc(copy_tables);
  DBUG_RETURN(1);

error:
  if (select_ignore)
    vp_table->select_ignore = select_ignore;
  if (select_ignore_with_lock)
    vp_table->select_ignore_with_lock = select_ignore_with_lock;
  if (update_ignore)
    vp_table->update_ignore = update_ignore;
#if MYSQL_VERSION_ID < 50500
  ha_autocommit_or_rollback(thd, 0);
#else
  (thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd));
#endif
  close_thread_tables(thd);
  if (src_bitmap)
    vp_my_free(src_bitmap, MYF(0));
  if (copy_tables)
    vp_udf_free_copy_tables_alloc(copy_tables);
  *error = 1;
  DBUG_RETURN(0);
}

my_bool vp_copy_tables_init_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  DBUG_ENTER("vp_copy_tables_init_body");
  if (args->arg_count != 3 && args->arg_count != 4)
  {
    strcpy(message, "vp_copy_tables() requires 3 or 4 arguments");
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
    strcpy(message, "vp_copy_tables() requires string arguments");
    goto error;
  }
  DBUG_RETURN(FALSE);

error:
  DBUG_RETURN(TRUE);
}

void vp_copy_tables_deinit_body(
  UDF_INIT *initid
) {
  DBUG_ENTER("vp_copy_tables_deinit_body");
  DBUG_VOID_RETURN;
}
