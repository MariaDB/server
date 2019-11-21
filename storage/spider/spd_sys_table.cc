/* Copyright (C) 2008-2015 Kentoku Shiba

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
#include "key.h"
#include "sql_base.h"
#endif
#include "sql_select.h"
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_sys_table.h"
#include "spd_malloc.h"

extern handlerton *spider_hton_ptr;

/**
  Insert a Spider system table row.

  @param  table             The spider system table.
  @param  do_handle_error   TRUE if an error message should be printed
                            before returning.

  @return                   Error code returned by the write.
*/

inline int spider_write_sys_table_row(TABLE *table, bool do_handle_error = TRUE)
{
  int error_num;
  THD *thd = table->in_use;

  tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
  error_num = table->file->ha_write_row(table->record[0]);
  reenable_binlog(thd);

  if (error_num && do_handle_error)
    table->file->print_error(error_num, MYF(0));

  return error_num;
}

/**
  Update a Spider system table row.

  @param  table             The spider system table.

  @return                   Error code returned by the update.
*/

inline int spider_update_sys_table_row(TABLE *table)
{
  int error_num;
  THD *thd = table->in_use;

  tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
  error_num = table->file->ha_update_row(table->record[1], table->record[0]);
  reenable_binlog(thd);

  if (error_num)
  {
    if (error_num == HA_ERR_RECORD_IS_THE_SAME)
      error_num = 0;
    else
      table->file->print_error(error_num, MYF(0));
  }

  return error_num;
}

/**
  Delete a Spider system table row.

  @param  table             The spider system table.
  @param  record_number     Location of the record: 0 or 1.
  @param  do_handle_error   TRUE if an error message should be printed
                            before returning.

  @return                   Error code returned by the update.
*/

inline int spider_delete_sys_table_row(TABLE *table, int record_number = 0,
                                       bool do_handle_error = TRUE)
{
  int error_num;
  THD *thd = table->in_use;

  tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
  error_num = table->file->ha_delete_row(table->record[record_number]);
  reenable_binlog(thd);

  if (error_num && do_handle_error)
    table->file->print_error(error_num, MYF(0));

  return error_num;
}

#if MYSQL_VERSION_ID < 50500
TABLE *spider_open_sys_table(
  THD *thd,
  const char *table_name,
  int table_name_length,
  bool write,
  Open_tables_state *open_tables_backup,
  bool need_lock,
  int *error_num
)
#else
TABLE *spider_open_sys_table(
  THD *thd,
  const char *table_name,
  int table_name_length,
  bool write,
  Open_tables_backup *open_tables_backup,
  bool need_lock,
  int *error_num
)
#endif
{
  TABLE *table;
  TABLE_LIST tables;
#if MYSQL_VERSION_ID < 50500
  TABLE_SHARE *table_share;
  char table_key[MAX_DBKEY_LENGTH];
  uint table_key_length;
#endif
  DBUG_ENTER("spider_open_sys_table");

#if MYSQL_VERSION_ID < 50500
  memset(&tables, 0, sizeof(TABLE_LIST));
  tables.db = (char*)"mysql";
  tables.db_length = sizeof("mysql") - 1;
  tables.alias = tables.table_name = (char *) table_name;
  tables.table_name_length = table_name_length;
  tables.lock_type = (write ? TL_WRITE : TL_READ);
#else
  tables.init_one_table(
    "mysql", sizeof("mysql") - 1, table_name, table_name_length, table_name,
    (write ? TL_WRITE : TL_READ));
#endif

#if MYSQL_VERSION_ID < 50500
  if (need_lock)
  {
#endif
#if MYSQL_VERSION_ID < 50500
    if (!(table = open_performance_schema_table(thd, &tables,
      open_tables_backup)))
#else
    if (!(table = spider_sys_open_table(thd, &tables, open_tables_backup)))
#endif
    {
      my_printf_error(ER_SPIDER_CANT_OPEN_SYS_TABLE_NUM,
        ER_SPIDER_CANT_OPEN_SYS_TABLE_STR, MYF(0),
        "mysql", table_name);
      *error_num = ER_SPIDER_CANT_OPEN_SYS_TABLE_NUM;
      DBUG_RETURN(NULL);
    }
#if MYSQL_VERSION_ID < 50500
  } else {
    thd->reset_n_backup_open_tables_state(open_tables_backup);

    if (!(table = (TABLE*) spider_malloc(spider_current_trx, 12,
      sizeof(*table), MYF(MY_WME))))
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_malloc;
    }

    table_key_length =
      create_table_def_key(thd, table_key, &tables, FALSE);

    if (!(table_share = get_table_share(thd,
      &tables, table_key, table_key_length, 0, error_num)))
      goto error;
    if (open_table_from_share(thd, table_share, tables.alias,
      (uint) (HA_OPEN_KEYFILE | HA_OPEN_RNDFILE | HA_GET_INDEX),
      READ_KEYINFO | COMPUTE_TYPES | EXTRA_RECORD,
      (uint) HA_OPEN_IGNORE_IF_LOCKED | HA_OPEN_FROM_SQL_LAYER,
      table, FALSE)
    ) {
      release_table_share(table_share, RELEASE_NORMAL);
      my_printf_error(ER_SPIDER_CANT_OPEN_SYS_TABLE_NUM,
        ER_SPIDER_CANT_OPEN_SYS_TABLE_STR, MYF(0),
        "mysql", table_name);
      *error_num = ER_SPIDER_CANT_OPEN_SYS_TABLE_NUM;
      goto error;
    }
  }
#endif
  if (table_name_length == SPIDER_SYS_XA_TABLE_NAME_LEN)
  {
    if (
      !memcmp(table_name,
        SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN) &&
      table->s->fields != SPIDER_SYS_XA_COL_CNT
    ) {
      spider_close_sys_table(thd, table, open_tables_backup, need_lock);
      table = NULL;
      my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
        ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
        SPIDER_SYS_XA_TABLE_NAME_STR);
      *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
      goto error_col_num_chk;
    }
  } else if (table_name_length == SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN)
  {
    if (
      !memcmp(table_name,
        SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR,
        SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN) &&
      table->s->fields != SPIDER_SYS_XA_MEMBER_COL_CNT
    ) {
      spider_close_sys_table(thd, table, open_tables_backup, need_lock);
      table = NULL;
      my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
        ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
        SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR);
      *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
      goto error_col_num_chk;
    }
  } else if (table_name_length == SPIDER_SYS_TABLES_TABLE_NAME_LEN)
  {
    if (
      !memcmp(table_name,
        SPIDER_SYS_TABLES_TABLE_NAME_STR,
        SPIDER_SYS_TABLES_TABLE_NAME_LEN) &&
      table->s->fields != SPIDER_SYS_TABLES_COL_CNT
    ) {
      spider_close_sys_table(thd, table, open_tables_backup, need_lock);
      table = NULL;
      my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
        ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
        SPIDER_SYS_TABLES_TABLE_NAME_STR);
      *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
      goto error_col_num_chk;
    }
  } else if (table_name_length == SPIDER_SYS_LINK_MON_TABLE_NAME_LEN)
  {
    if (
      !memcmp(table_name,
        SPIDER_SYS_LINK_MON_TABLE_NAME_STR,
        SPIDER_SYS_LINK_MON_TABLE_NAME_LEN) &&
      table->s->fields != SPIDER_SYS_LINK_MON_TABLE_COL_CNT
    ) {
      spider_close_sys_table(thd, table, open_tables_backup, need_lock);
      table = NULL;
      my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
        ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
        SPIDER_SYS_LINK_MON_TABLE_NAME_STR);
      *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
      goto error_col_num_chk;
    }
  }
  DBUG_RETURN(table);

#if MYSQL_VERSION_ID < 50500
error:
  spider_free(spider_current_trx, table, MYF(0));
error_malloc:
  thd->restore_backup_open_tables_state(open_tables_backup);
#endif
error_col_num_chk:
  DBUG_RETURN(NULL);
}

#if MYSQL_VERSION_ID < 50500
void spider_close_sys_table(
  THD *thd,
  TABLE *table,
  Open_tables_state *open_tables_backup,
  bool need_lock
)
#else
void spider_close_sys_table(
  THD *thd,
  TABLE *table,
  Open_tables_backup *open_tables_backup,
  bool need_lock
)
#endif
{
  DBUG_ENTER("spider_close_sys_table");
#if MYSQL_VERSION_ID < 50500
  if (need_lock)
  {
    close_performance_schema_table(thd, open_tables_backup);
  } else {
    table->file->ha_reset();
    closefrm(table);
    tdc_release_share(table->s);
    spider_free(spider_current_trx, table, MYF(0));
    thd->restore_backup_open_tables_state(open_tables_backup);
  }
#else
  spider_sys_close_table(thd, open_tables_backup);
#endif
  DBUG_VOID_RETURN;
}

#if MYSQL_VERSION_ID < 50500
#else
bool spider_sys_open_tables(
  THD *thd,
  TABLE_LIST **tables,
  Open_tables_backup *open_tables_backup
) {
  uint counter;
  ulonglong utime_after_lock_backup = thd->utime_after_lock;
  DBUG_ENTER("spider_sys_open_tables");
  thd->reset_n_backup_open_tables_state(open_tables_backup);
  if (open_tables(thd, tables, &counter,
    MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
    MYSQL_OPEN_IGNORE_FLUSH | MYSQL_LOCK_IGNORE_TIMEOUT | MYSQL_LOCK_LOG_TABLE
  )) {
    thd->restore_backup_open_tables_state(open_tables_backup);
    thd->utime_after_lock = utime_after_lock_backup;
    DBUG_RETURN(TRUE);
  }
  thd->utime_after_lock = utime_after_lock_backup;
  DBUG_RETURN(FALSE);
}

TABLE *spider_sys_open_table(
  THD *thd,
  TABLE_LIST *tables,
  Open_tables_backup *open_tables_backup
) {
  TABLE *table;
  ulonglong utime_after_lock_backup = thd->utime_after_lock;
  DBUG_ENTER("spider_sys_open_table");
  thd->reset_n_backup_open_tables_state(open_tables_backup);
  if ((table = open_ltable(thd, tables, tables->lock_type,
    MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
    MYSQL_OPEN_IGNORE_FLUSH | MYSQL_LOCK_IGNORE_TIMEOUT | MYSQL_LOCK_LOG_TABLE
  ))) {
    table->use_all_columns();
    table->no_replicate = 1;
  } else
    thd->restore_backup_open_tables_state(open_tables_backup);
  thd->utime_after_lock = utime_after_lock_backup;
  DBUG_RETURN(table);
}

void spider_sys_close_table(
  THD *thd,
  Open_tables_backup *open_tables_backup
) {
  DBUG_ENTER("spider_sys_close_table");
  close_thread_tables(thd);
  thd->restore_backup_open_tables_state(open_tables_backup);
  DBUG_VOID_RETURN;
}
#endif

int spider_sys_index_init(
  TABLE *table,
  uint idx,
  bool sorted
) {
  DBUG_ENTER("spider_sys_index_init");
  DBUG_RETURN(table->file->ha_index_init(idx, sorted));
}

int spider_sys_index_end(
  TABLE *table
) {
  DBUG_ENTER("spider_sys_index_end");
  DBUG_RETURN(table->file->ha_index_end());
}

int spider_sys_rnd_init(
  TABLE *table,
  bool scan
) {
  DBUG_ENTER("spider_sys_rnd_init");
  DBUG_RETURN(table->file->ha_rnd_init(scan));
}

int spider_sys_rnd_end(
  TABLE *table
) {
  DBUG_ENTER("spider_sys_rnd_end");
  DBUG_RETURN(table->file->ha_rnd_end());
}

int spider_check_sys_table(
  TABLE *table,
  char *table_key
) {
  DBUG_ENTER("spider_check_sys_table");

  key_copy(
    (uchar *) table_key,
    table->record[0],
    table->key_info,
    table->key_info->key_length);

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
  DBUG_RETURN(table->file->ha_index_read_idx_map(
    table->record[0], 0, (uchar *) table_key,
    HA_WHOLE_KEY, HA_READ_KEY_EXACT));
#else
  DBUG_RETURN(table->file->index_read_idx_map(
    table->record[0], 0, (uchar *) table_key,
    HA_WHOLE_KEY, HA_READ_KEY_EXACT));
#endif
}

int spider_check_sys_table_with_find_flag(
  TABLE *table,
  char *table_key,
  enum ha_rkey_function find_flag
) {
  DBUG_ENTER("spider_check_sys_table");

  key_copy(
    (uchar *) table_key,
    table->record[0],
    table->key_info,
    table->key_info->key_length);

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
  DBUG_RETURN(table->file->ha_index_read_idx_map(
    table->record[0], 0, (uchar *) table_key,
    HA_WHOLE_KEY, find_flag));
#else
  DBUG_RETURN(table->file->index_read_idx_map(
    table->record[0], 0, (uchar *) table_key,
    HA_WHOLE_KEY, find_flag));
#endif
}

int spider_get_sys_table_by_idx(
  TABLE *table,
  char *table_key,
  const int idx,
  const int col_count
) {
  int error_num;
  uint key_length;
  KEY *key_info = table->key_info;
  DBUG_ENTER("spider_get_sys_table_by_idx");
  if ((error_num = spider_sys_index_init(table, idx, FALSE)))
    DBUG_RETURN(error_num);

  if ((int) spider_user_defined_key_parts(key_info) == col_count)
  {
    key_length = key_info->key_length;
  } else {
    int roop_count;
    key_length = 0;
    for (roop_count = 0; roop_count < col_count; ++roop_count)
    {
      key_length += key_info->key_part[roop_count].store_length;
    }
  }

  key_copy(
    (uchar *) table_key,
    table->record[0],
    key_info,
    key_length);

  if (
/*
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    (error_num = table->file->ha_index_read_idx_map(
      table->record[0], idx, (uchar *) table_key,
      make_prev_keypart_map(col_count), HA_READ_KEY_EXACT))
#else
    (error_num = table->file->index_read_idx_map(
      table->record[0], idx, (uchar *) table_key,
      make_prev_keypart_map(col_count), HA_READ_KEY_EXACT))
#endif
*/
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    (error_num = table->file->ha_index_read_map(
      table->record[0], (uchar *) table_key,
      make_prev_keypart_map(col_count), HA_READ_KEY_EXACT))
#else
    (error_num = table->file->index_read_map(
      table->record[0], (uchar *) table_key,
      make_prev_keypart_map(col_count), HA_READ_KEY_EXACT))
#endif
  ) {
    spider_sys_index_end(table);
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_sys_index_next_same(
  TABLE *table,
  char *table_key
) {
  DBUG_ENTER("spider_sys_index_next_same");
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
  DBUG_RETURN(table->file->ha_index_next_same(
    table->record[0],
    (const uchar*) table_key,
    table->key_info->key_length));
#else
  DBUG_RETURN(table->file->index_next_same(
    table->record[0],
    (const uchar*) table_key,
    table->key_info->key_length));
#endif
}

int spider_sys_index_first(
  TABLE *table,
  const int idx
) {
  int error_num;
  DBUG_ENTER("spider_sys_index_first");
  if ((error_num = spider_sys_index_init(table, idx, FALSE)))
    DBUG_RETURN(error_num);

  if (
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
    (error_num = table->file->ha_index_first(table->record[0]))
#else
    (error_num = table->file->index_first(table->record[0]))
#endif
  ) {
    spider_sys_index_end(table);
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_sys_index_next(
  TABLE *table
) {
  DBUG_ENTER("spider_sys_index_next");
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
  DBUG_RETURN(table->file->ha_index_next(table->record[0]));
#else
  DBUG_RETURN(table->file->index_next(table->record[0]));
#endif
}

void spider_store_xa_pk(
  TABLE *table,
  XID *xid
) {
  DBUG_ENTER("spider_store_xa_pk");
  table->field[0]->store(xid->formatID);
  table->field[1]->store(xid->gtrid_length);
  table->field[3]->store(
    xid->data,
    (uint) xid->gtrid_length + xid->bqual_length,
    system_charset_info);
  DBUG_VOID_RETURN;
}

void spider_store_xa_bqual_length(
  TABLE *table,
  XID *xid
) {
  DBUG_ENTER("spider_store_xa_bqual_length");
  table->field[2]->store(xid->bqual_length);
  DBUG_VOID_RETURN;
}

void spider_store_xa_status(
  TABLE *table,
  const char *status
) {
  DBUG_ENTER("spider_store_xa_status");
  table->field[4]->store(
    status,
    (uint) strlen(status),
    system_charset_info);
  DBUG_VOID_RETURN;
}

void spider_store_xa_member_pk(
  TABLE *table,
  XID *xid,
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_store_xa_member_pk");
  table->field[0]->store(xid->formatID);
  table->field[1]->store(xid->gtrid_length);
  table->field[3]->store(
    xid->data,
    (uint) xid->gtrid_length + xid->bqual_length,
    system_charset_info);
  table->field[5]->store(
    conn->tgt_host,
    (uint) conn->tgt_host_length,
    system_charset_info);
  table->field[6]->store(
    conn->tgt_port);
  table->field[7]->store(
    conn->tgt_socket,
    (uint) conn->tgt_socket_length,
    system_charset_info);
  DBUG_VOID_RETURN;
}

void spider_store_xa_member_info(
  TABLE *table,
  XID *xid,
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_store_xa_member_info");
  table->field[2]->store(xid->bqual_length);
  table->field[4]->store(
    conn->tgt_wrapper,
    (uint) conn->tgt_wrapper_length,
    system_charset_info);
  table->field[8]->store(
    conn->tgt_username,
    (uint) conn->tgt_username_length,
    system_charset_info);
  table->field[9]->store(
    conn->tgt_password,
    (uint) conn->tgt_password_length,
    system_charset_info);
  if (conn->tgt_ssl_ca)
  {
    table->field[10]->set_notnull();
    table->field[10]->store(
      conn->tgt_ssl_ca,
      (uint) conn->tgt_ssl_ca_length,
      system_charset_info);
  } else {
    table->field[10]->set_null();
    table->field[10]->reset();
  }
  if (conn->tgt_ssl_capath)
  {
    table->field[11]->set_notnull();
    table->field[11]->store(
      conn->tgt_ssl_capath,
      (uint) conn->tgt_ssl_capath_length,
      system_charset_info);
  } else {
    table->field[11]->set_null();
    table->field[11]->reset();
  }
  if (conn->tgt_ssl_cert)
  {
    table->field[12]->set_notnull();
    table->field[12]->store(
      conn->tgt_ssl_cert,
      (uint) conn->tgt_ssl_cert_length,
      system_charset_info);
  } else {
    table->field[12]->set_null();
    table->field[12]->reset();
  }
  if (conn->tgt_ssl_cipher)
  {
    table->field[13]->set_notnull();
    table->field[13]->store(
      conn->tgt_ssl_cipher,
      (uint) conn->tgt_ssl_cipher_length,
      system_charset_info);
  } else {
    table->field[13]->set_null();
    table->field[13]->reset();
  }
  if (conn->tgt_ssl_key)
  {
    table->field[14]->set_notnull();
    table->field[14]->store(
      conn->tgt_ssl_key,
      (uint) conn->tgt_ssl_key_length,
      system_charset_info);
  } else {
    table->field[14]->set_null();
    table->field[14]->reset();
  }
  if (conn->tgt_ssl_vsc >= 0)
  {
    table->field[15]->set_notnull();
    table->field[15]->store(
      conn->tgt_ssl_vsc);
  } else {
    table->field[15]->set_null();
    table->field[15]->reset();
  }
  if (conn->tgt_default_file)
  {
    table->field[16]->set_notnull();
    table->field[16]->store(
      conn->tgt_default_file,
      (uint) conn->tgt_default_file_length,
      system_charset_info);
  } else {
    table->field[16]->set_null();
    table->field[16]->reset();
  }
  if (conn->tgt_default_group)
  {
    table->field[17]->set_notnull();
    table->field[17]->store(
      conn->tgt_default_group,
      (uint) conn->tgt_default_group_length,
      system_charset_info);
  } else {
    table->field[17]->set_null();
    table->field[17]->reset();
  }
  DBUG_VOID_RETURN;
}

void spider_store_tables_name(
  TABLE *table,
  const char *name,
  const uint name_length
) {
  const char *ptr_db, *ptr_table;
  my_ptrdiff_t ptr_diff_db, ptr_diff_table;
  DBUG_ENTER("spider_store_tables_name");
  if (name[0] == FN_CURLIB && name[1] == FN_LIBCHAR)
  {
    ptr_db = strchr(name, FN_LIBCHAR);
    ptr_db++;
    ptr_diff_db = PTR_BYTE_DIFF(ptr_db, name);
    DBUG_PRINT("info",("spider ptr_diff_db = %lld", (longlong) ptr_diff_db));
    ptr_table = strchr(ptr_db, FN_LIBCHAR);
    ptr_table++;
    ptr_diff_table = PTR_BYTE_DIFF(ptr_table, ptr_db);
    DBUG_PRINT("info",("spider ptr_diff_table = %lld",
      (longlong) ptr_diff_table));
  } else {
    DBUG_PRINT("info",("spider temporary table"));
    ptr_db = "";
    ptr_diff_db = 1;
    ptr_table = "";
    ptr_diff_table = 1;
  }
  table->field[0]->store(
    ptr_db,
    (uint)(ptr_diff_table - 1),
    system_charset_info);
  DBUG_PRINT("info",("spider field[0]->null_bit = %d",
    table->field[0]->null_bit));
  table->field[1]->store(
    ptr_table,
    (uint)(name_length - ptr_diff_db - ptr_diff_table),
    system_charset_info);
  DBUG_PRINT("info",("spider field[1]->null_bit = %d",
    table->field[1]->null_bit));
  DBUG_VOID_RETURN;
}

void spider_store_db_and_table_name(
  TABLE *table,
  const char *db_name,
  const uint db_name_length,
  const char *table_name,
  const uint table_name_length
) {
  DBUG_ENTER("spider_store_db_and_table_name");
  table->field[0]->store(
    db_name,
    db_name_length,
    system_charset_info);
  DBUG_PRINT("info",("spider field[0]->null_bit = %d",
    table->field[0]->null_bit));
  table->field[1]->store(
    table_name,
    table_name_length,
    system_charset_info);
  DBUG_PRINT("info",("spider field[1]->null_bit = %d",
    table->field[1]->null_bit));
  DBUG_VOID_RETURN;
}

void spider_store_tables_link_idx(
  TABLE *table,
  int link_idx
) {
  DBUG_ENTER("spider_store_tables_link_idx");
  table->field[2]->set_notnull();
  table->field[2]->store(link_idx);
  DBUG_VOID_RETURN;
}

void spider_store_tables_link_idx_str(
  TABLE *table,
  const char *link_idx,
  const uint link_idx_length
) {
  DBUG_ENTER("spider_store_tables_link_idx_str");
  table->field[2]->store(
    link_idx,
    link_idx_length,
    system_charset_info);
  DBUG_PRINT("info",("spider field[2]->null_bit = %d",
    table->field[2]->null_bit));
  DBUG_VOID_RETURN;
}

void spider_store_tables_priority(
  TABLE *table,
  longlong priority
) {
  DBUG_ENTER("spider_store_tables_priority");
  DBUG_PRINT("info",("spider priority = %lld", priority));
  table->field[3]->store(priority, FALSE);
  DBUG_VOID_RETURN;
}

void spider_store_tables_connect_info(
  TABLE *table,
  SPIDER_ALTER_TABLE *alter_table,
  int link_idx
) {
  DBUG_ENTER("spider_store_tables_connect_info");
  if (alter_table->tmp_server_names[link_idx])
  {
    table->field[4]->set_notnull();
    table->field[4]->store(
      alter_table->tmp_server_names[link_idx],
      (uint) alter_table->tmp_server_names_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[4]->set_null();
    table->field[4]->reset();
  }
  if (alter_table->tmp_tgt_wrappers[link_idx])
  {
    table->field[5]->set_notnull();
    table->field[5]->store(
      alter_table->tmp_tgt_wrappers[link_idx],
      (uint) alter_table->tmp_tgt_wrappers_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[5]->set_null();
    table->field[5]->reset();
  }
  if (alter_table->tmp_tgt_hosts[link_idx])
  {
    table->field[6]->set_notnull();
    table->field[6]->store(
      alter_table->tmp_tgt_hosts[link_idx],
      (uint) alter_table->tmp_tgt_hosts_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[6]->set_null();
    table->field[6]->reset();
  }
  if (alter_table->tmp_tgt_ports[link_idx] >= 0)
  {
    table->field[7]->set_notnull();
    table->field[7]->store(
      alter_table->tmp_tgt_ports[link_idx]);
  } else {
    table->field[7]->set_null();
    table->field[7]->reset();
  }
  if (alter_table->tmp_tgt_sockets[link_idx])
  {
    table->field[8]->set_notnull();
    table->field[8]->store(
      alter_table->tmp_tgt_sockets[link_idx],
      (uint) alter_table->tmp_tgt_sockets_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[8]->set_null();
    table->field[8]->reset();
  }
  if (alter_table->tmp_tgt_usernames[link_idx])
  {
    table->field[9]->set_notnull();
    table->field[9]->store(
      alter_table->tmp_tgt_usernames[link_idx],
      (uint) alter_table->tmp_tgt_usernames_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[9]->set_null();
    table->field[9]->reset();
  }
  if (alter_table->tmp_tgt_passwords[link_idx])
  {
    table->field[10]->set_notnull();
    table->field[10]->store(
      alter_table->tmp_tgt_passwords[link_idx],
      (uint) alter_table->tmp_tgt_passwords_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[10]->set_null();
    table->field[10]->reset();
  }
  if (alter_table->tmp_tgt_ssl_cas[link_idx])
  {
    table->field[11]->set_notnull();
    table->field[11]->store(
      alter_table->tmp_tgt_ssl_cas[link_idx],
      (uint) alter_table->tmp_tgt_ssl_cas_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[11]->set_null();
    table->field[11]->reset();
  }
  if (alter_table->tmp_tgt_ssl_capaths[link_idx])
  {
    table->field[12]->set_notnull();
    table->field[12]->store(
      alter_table->tmp_tgt_ssl_capaths[link_idx],
      (uint) alter_table->tmp_tgt_ssl_capaths_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[12]->set_null();
    table->field[12]->reset();
  }
  if (alter_table->tmp_tgt_ssl_certs[link_idx])
  {
    table->field[13]->set_notnull();
    table->field[13]->store(
      alter_table->tmp_tgt_ssl_certs[link_idx],
      (uint) alter_table->tmp_tgt_ssl_certs_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[13]->set_null();
    table->field[13]->reset();
  }
  if (alter_table->tmp_tgt_ssl_ciphers[link_idx])
  {
    table->field[14]->set_notnull();
    table->field[14]->store(
      alter_table->tmp_tgt_ssl_ciphers[link_idx],
      (uint) alter_table->tmp_tgt_ssl_ciphers_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[14]->set_null();
    table->field[14]->reset();
  }
  if (alter_table->tmp_tgt_ssl_keys[link_idx])
  {
    table->field[15]->set_notnull();
    table->field[15]->store(
      alter_table->tmp_tgt_ssl_keys[link_idx],
      (uint) alter_table->tmp_tgt_ssl_keys_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[15]->set_null();
    table->field[15]->reset();
  }
  if (alter_table->tmp_tgt_ssl_vscs[link_idx] >= 0)
  {
    table->field[16]->set_notnull();
    table->field[16]->store(
      alter_table->tmp_tgt_ssl_vscs[link_idx]);
  } else {
    table->field[16]->set_null();
    table->field[16]->reset();
  }
  if (alter_table->tmp_tgt_default_files[link_idx])
  {
    table->field[17]->set_notnull();
    table->field[17]->store(
      alter_table->tmp_tgt_default_files[link_idx],
      (uint) alter_table->tmp_tgt_default_files_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[17]->set_null();
    table->field[17]->reset();
  }
  if (alter_table->tmp_tgt_default_groups[link_idx])
  {
    table->field[18]->set_notnull();
    table->field[18]->store(
      alter_table->tmp_tgt_default_groups[link_idx],
      (uint) alter_table->tmp_tgt_default_groups_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[18]->set_null();
    table->field[18]->reset();
  }
  if (alter_table->tmp_tgt_dbs[link_idx])
  {
    table->field[19]->set_notnull();
    table->field[19]->store(
      alter_table->tmp_tgt_dbs[link_idx],
      (uint) alter_table->tmp_tgt_dbs_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[19]->set_null();
    table->field[19]->reset();
  }
  if (alter_table->tmp_tgt_table_names[link_idx])
  {
    table->field[20]->set_notnull();
    table->field[20]->store(
      alter_table->tmp_tgt_table_names[link_idx],
      (uint) alter_table->tmp_tgt_table_names_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[20]->set_null();
    table->field[20]->reset();
  }
  DBUG_VOID_RETURN;
}

void spider_store_tables_link_status(
  TABLE *table,
  long link_status
) {
  DBUG_ENTER("spider_store_tables_link_status");
  DBUG_PRINT("info",("spider link_status = %ld", link_status));
  if (link_status > SPIDER_LINK_STATUS_NO_CHANGE)
    table->field[21]->store(link_status, FALSE);
  DBUG_VOID_RETURN;
}

void spider_store_link_chk_server_id(
  TABLE *table,
  uint32 server_id
) {
  DBUG_ENTER("spider_store_link_chk_server_id");
  table->field[3]->set_notnull();
  table->field[3]->store(server_id);
  DBUG_VOID_RETURN;
}

int spider_insert_xa(
  TABLE *table,
  XID *xid,
  const char *status
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_insert_xa");
  table->use_all_columns();
  empty_record(table);
  spider_store_xa_pk(table, xid);

  if ((error_num = spider_check_sys_table(table, table_key)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
    table->use_all_columns();
    spider_store_xa_bqual_length(table, xid);
    spider_store_xa_status(table, status);
    if ((error_num = spider_write_sys_table_row(table)))
      DBUG_RETURN(error_num);
  } else {
    my_message(ER_SPIDER_XA_EXISTS_NUM, ER_SPIDER_XA_EXISTS_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_XA_EXISTS_NUM);
  }

  DBUG_RETURN(0);
}

int spider_insert_xa_member(
  TABLE *table,
  XID *xid,
  SPIDER_CONN *conn
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_insert_xa_member");
  table->use_all_columns();
  empty_record(table);
  spider_store_xa_member_pk(table, xid, conn);

  if ((error_num = spider_check_sys_table(table, table_key)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
    table->use_all_columns();
    spider_store_xa_member_info(table, xid, conn);
    if ((error_num = spider_write_sys_table_row(table)))
      DBUG_RETURN(error_num);
  } else {
    my_message(ER_SPIDER_XA_MEMBER_EXISTS_NUM, ER_SPIDER_XA_MEMBER_EXISTS_STR,
      MYF(0));
    DBUG_RETURN(ER_SPIDER_XA_MEMBER_EXISTS_NUM);
  }

  DBUG_RETURN(0);
}

int spider_insert_tables(
  TABLE *table,
  SPIDER_SHARE *share
) {
  int error_num, roop_count;
  DBUG_ENTER("spider_insert_tables");
  table->use_all_columns();
  empty_record(table);

  spider_store_tables_name(table, share->table_name, share->table_name_length);
  spider_store_tables_priority(table, share->priority);
  for (roop_count = 0; roop_count < (int) share->all_link_count; roop_count++)
  {
    spider_store_tables_link_idx(table, roop_count);
    spider_store_tables_connect_info(table, &share->alter_table, roop_count);
    spider_store_tables_link_status(table,
      share->alter_table.tmp_link_statuses[roop_count] >
      SPIDER_LINK_STATUS_NO_CHANGE ?
      share->alter_table.tmp_link_statuses[roop_count] :
      SPIDER_LINK_STATUS_OK);
    if ((error_num = spider_write_sys_table_row(table)))
      DBUG_RETURN(error_num);
  }

  DBUG_RETURN(0);
}

int spider_log_tables_link_failed(
  TABLE *table,
  char *name,
  uint name_length,
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_log_tables_link_failed");
  table->use_all_columns();
  spider_store_tables_name(table, name, name_length);
  spider_store_tables_link_idx(table, link_idx);
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
#else
  if (table->field[3] == table->timestamp_field)
    table->timestamp_field->set_time();
#endif
  if ((error_num = spider_write_sys_table_row(table)))
    DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}

int spider_log_xa_failed(
  THD *thd,
  TABLE *table,
  XID *xid,
  SPIDER_CONN *conn,
  const char *status
) {
  int error_num;
  DBUG_ENTER("spider_log_xa_failed");
  table->use_all_columns();
  spider_store_xa_member_pk(table, xid, conn);
  spider_store_xa_member_info(table, xid, conn);
  if (thd)
  {
    table->field[18]->set_notnull();
    table->field[18]->store(thd->thread_id, TRUE);
  } else {
    table->field[18]->set_null();
    table->field[18]->reset();
  }
  table->field[19]->store(
    status,
    (uint) strlen(status),
    system_charset_info);

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
#else
  if (table->field[20] == table->timestamp_field)
    table->timestamp_field->set_time();
#endif
  if ((error_num = spider_write_sys_table_row(table)))
    DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}

int spider_update_xa(
  TABLE *table,
  XID *xid,
  const char *status
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_update_xa");
  table->use_all_columns();
  spider_store_xa_pk(table, xid);

  if ((error_num = spider_check_sys_table(table, table_key)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
    my_message(ER_SPIDER_XA_NOT_EXISTS_NUM, ER_SPIDER_XA_NOT_EXISTS_STR,
      MYF(0));
    DBUG_RETURN(ER_SPIDER_XA_NOT_EXISTS_NUM);
  } else {
    store_record(table, record[1]);
    table->use_all_columns();
    spider_store_xa_status(table, status);
    if ((error_num = spider_update_sys_table_row(table)))
      DBUG_RETURN(error_num);
  }

  DBUG_RETURN(0);
}

int spider_update_tables_name(
  TABLE *table,
  const char *from,
  const char *to,
  int *old_link_count
) {
  int error_num, roop_count = 0;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_update_tables_name");
  table->use_all_columns();
  while (TRUE)
  {
    spider_store_tables_name(table, from, strlen(from));
    spider_store_tables_link_idx(table, roop_count);
    if ((error_num = spider_check_sys_table(table, table_key)))
    {
      if (
        roop_count &&
        (error_num == HA_ERR_KEY_NOT_FOUND || error_num == HA_ERR_END_OF_FILE)
      )
        break;
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    } else {
      store_record(table, record[1]);
      table->use_all_columns();
      spider_store_tables_name(table, to, strlen(to));
      if ((error_num = spider_update_sys_table_row(table)))
        DBUG_RETURN(error_num);
    }
    roop_count++;
  }

  *old_link_count = roop_count;
  DBUG_RETURN(0);
}

int spider_update_tables_priority(
  TABLE *table,
  SPIDER_ALTER_TABLE *alter_table,
  const char *name,
  int *old_link_count
) {
  int error_num, roop_count;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_update_tables_priority");
  table->use_all_columns();
  for (roop_count = 0; roop_count < (int) alter_table->all_link_count;
    roop_count++)
  {
    spider_store_tables_name(table, alter_table->table_name,
      alter_table->table_name_length);
    spider_store_tables_link_idx(table, roop_count);
    if ((error_num = spider_check_sys_table(table, table_key)))
    {
      if (
        roop_count &&
        (error_num == HA_ERR_KEY_NOT_FOUND || error_num == HA_ERR_END_OF_FILE)
      ) {
        *old_link_count = roop_count;

        /* insert for adding link */
        spider_store_tables_name(table, name, strlen(name));
        spider_store_tables_priority(table, alter_table->tmp_priority);
        do {
          spider_store_tables_link_idx(table, roop_count);
          spider_store_tables_connect_info(table, alter_table, roop_count);
          spider_store_tables_link_status(table,
            alter_table->tmp_link_statuses[roop_count] !=
            SPIDER_LINK_STATUS_NO_CHANGE ?
            alter_table->tmp_link_statuses[roop_count] :
            SPIDER_LINK_STATUS_OK);
          if ((error_num = spider_write_sys_table_row(table)))
            DBUG_RETURN(error_num);
          roop_count++;
        } while (roop_count < (int) alter_table->all_link_count);
        DBUG_RETURN(0);
      } else {
        table->file->print_error(error_num, MYF(0));
        DBUG_RETURN(error_num);
      }
    } else {
      store_record(table, record[1]);
      table->use_all_columns();
      spider_store_tables_name(table, name, strlen(name));
      spider_store_tables_priority(table, alter_table->tmp_priority);
      spider_store_tables_connect_info(table, alter_table, roop_count);
      spider_store_tables_link_status(table,
        alter_table->tmp_link_statuses[roop_count]);
      if ((error_num = spider_update_sys_table_row(table)))
        DBUG_RETURN(error_num);
    }
  }
  while (TRUE)
  {
    /* delete for subtracting link */
    spider_store_tables_link_idx(table, roop_count);
    if ((error_num = spider_check_sys_table(table, table_key)))
    {
      if (
        roop_count &&
        (error_num == HA_ERR_KEY_NOT_FOUND || error_num == HA_ERR_END_OF_FILE)
      )
        break;
      else {
        table->file->print_error(error_num, MYF(0));
        DBUG_RETURN(error_num);
      }
      if ((error_num = spider_delete_sys_table_row(table)))
        DBUG_RETURN(error_num);
    }
    roop_count++;
  }

  *old_link_count = roop_count;
  DBUG_RETURN(0);
}

int spider_update_tables_link_status(
  TABLE *table,
  char *name,
  uint name_length,
  int link_idx,
  long link_status
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_update_tables_link_status");
  table->use_all_columns();
  spider_store_tables_name(table, name, name_length);
  spider_store_tables_link_idx(table, link_idx);
  if ((error_num = spider_check_sys_table(table, table_key)))
  {
    if (
      (error_num == HA_ERR_KEY_NOT_FOUND || error_num == HA_ERR_END_OF_FILE)
    )
      DBUG_RETURN(0);
    else {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
  } else {
    store_record(table, record[1]);
    table->use_all_columns();
    spider_store_tables_link_status(table, link_status);
    if ((error_num = spider_update_sys_table_row(table)))
      DBUG_RETURN(error_num);
  }

  DBUG_RETURN(0);
}

int spider_delete_xa(
  TABLE *table,
  XID *xid
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_delete_xa");
  table->use_all_columns();
  spider_store_xa_pk(table, xid);

  if ((error_num = spider_check_sys_table(table, table_key)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
    my_message(ER_SPIDER_XA_NOT_EXISTS_NUM, ER_SPIDER_XA_NOT_EXISTS_STR,
      MYF(0));
    DBUG_RETURN(ER_SPIDER_XA_NOT_EXISTS_NUM);
  } else {
    if ((error_num = spider_delete_sys_table_row(table)))
      DBUG_RETURN(error_num);
  }

  DBUG_RETURN(0);
}

int spider_delete_xa_member(
  TABLE *table,
  XID *xid
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_delete_xa_member");
  table->use_all_columns();
  spider_store_xa_pk(table, xid);

  if ((error_num = spider_get_sys_table_by_idx(table, table_key, 0,
    SPIDER_SYS_XA_PK_COL_CNT)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
    DBUG_RETURN(0);
  } else {
    do {
      if ((error_num = spider_delete_sys_table_row(table, 0, FALSE)))
      {
        spider_sys_index_end(table);
        table->file->print_error(error_num, MYF(0));
        DBUG_RETURN(error_num);
      }
      error_num = spider_sys_index_next_same(table, table_key);
    } while (error_num == 0);
  }
  if ((error_num = spider_sys_index_end(table)))
  {
    table->file->print_error(error_num, MYF(0));
    DBUG_RETURN(error_num);
  }

  DBUG_RETURN(0);
}

int spider_delete_tables(
  TABLE *table,
  const char *name,
  int *old_link_count
) {
  int error_num, roop_count = 0;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_delete_tables");
  table->use_all_columns();
  spider_store_tables_name(table, name, strlen(name));

  while (TRUE)
  {
    spider_store_tables_link_idx(table, roop_count);
    if ((error_num = spider_check_sys_table(table, table_key)))
      break;
    else {
      if ((error_num = spider_delete_sys_table_row(table)))
        DBUG_RETURN(error_num);
    }
    roop_count++;
  }

  *old_link_count = roop_count;
  DBUG_RETURN(0);
}

int spider_get_sys_xid(
  TABLE *table,
  XID *xid,
  MEM_ROOT *mem_root
) {
  char *ptr;
  DBUG_ENTER("spider_get_sys_xid");
  ptr = get_field(mem_root, table->field[0]);
  if (ptr)
  {
    xid->formatID = atoi(ptr);
  } else
    xid->formatID = 0;
  ptr = get_field(mem_root, table->field[1]);
  if (ptr)
  {
    xid->gtrid_length = atoi(ptr);
  } else
    xid->gtrid_length = 0;
  ptr = get_field(mem_root, table->field[2]);
  if (ptr)
  {
    xid->bqual_length = atoi(ptr);
  } else
    xid->bqual_length = 0;
  ptr = get_field(mem_root, table->field[3]);
  if (ptr)
  {
    strmov(xid->data, ptr);
  }
  DBUG_RETURN(0);
}

int spider_get_sys_server_info(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
) {
  char *ptr;
  DBUG_ENTER("spider_get_sys_server_info");
  if ((ptr = get_field(mem_root, table->field[4])))
  {
    share->tgt_wrappers_lengths[link_idx] = strlen(ptr);
    share->tgt_wrappers[link_idx] = spider_create_string(ptr,
      share->tgt_wrappers_lengths[link_idx]);
  } else {
    share->tgt_wrappers_lengths[link_idx] = 0;
    share->tgt_wrappers[link_idx] = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[5])))
  {
    share->tgt_hosts_lengths[link_idx] = strlen(ptr);
    share->tgt_hosts[link_idx] = spider_create_string(ptr,
      share->tgt_hosts_lengths[link_idx]);
  } else {
    share->tgt_hosts_lengths[link_idx] = 0;
    share->tgt_hosts[link_idx] = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[6])))
  {
    share->tgt_ports[link_idx] = atol(ptr);
  } else
    share->tgt_ports[link_idx] = MYSQL_PORT;
  if ((ptr = get_field(mem_root, table->field[7])))
  {
    share->tgt_sockets_lengths[link_idx] = strlen(ptr);
    share->tgt_sockets[link_idx] = spider_create_string(ptr,
      share->tgt_sockets_lengths[link_idx]);
  } else {
    share->tgt_sockets_lengths[link_idx] = 0;
    share->tgt_sockets[link_idx] = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[8])))
  {
    share->tgt_usernames_lengths[link_idx] = strlen(ptr);
    share->tgt_usernames[link_idx] =
      spider_create_string(ptr, share->tgt_usernames_lengths[link_idx]);
  } else {
    share->tgt_usernames_lengths[link_idx] = 0;
    share->tgt_usernames[link_idx] = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[9])))
  {
    share->tgt_passwords_lengths[link_idx] = strlen(ptr);
    share->tgt_passwords[link_idx] =
      spider_create_string(ptr, share->tgt_passwords_lengths[link_idx]);
  } else {
    share->tgt_passwords_lengths[link_idx] = 0;
    share->tgt_passwords[link_idx] = NULL;
  }
  if (
    !table->field[10]->is_null() &&
    (ptr = get_field(mem_root, table->field[10]))
  ) {
    share->tgt_ssl_cas_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_cas[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_cas_lengths[link_idx]);
  } else {
    share->tgt_ssl_cas_lengths[link_idx] = 0;
    share->tgt_ssl_cas[link_idx] = NULL;
  }
  if (
    !table->field[11]->is_null() &&
    (ptr = get_field(mem_root, table->field[11]))
  ) {
    share->tgt_ssl_capaths_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_capaths[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_capaths_lengths[link_idx]);
  } else {
    share->tgt_ssl_capaths_lengths[link_idx] = 0;
    share->tgt_ssl_capaths[link_idx] = NULL;
  }
  if (
    !table->field[12]->is_null() &&
    (ptr = get_field(mem_root, table->field[12]))
  ) {
    share->tgt_ssl_certs_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_certs[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_certs_lengths[link_idx]);
  } else {
    share->tgt_ssl_certs_lengths[link_idx] = 0;
    share->tgt_ssl_certs[link_idx] = NULL;
  }
  if (
    !table->field[13]->is_null() &&
    (ptr = get_field(mem_root, table->field[13]))
  ) {
    share->tgt_ssl_ciphers_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_ciphers[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_ciphers_lengths[link_idx]);
  } else {
    share->tgt_ssl_ciphers_lengths[link_idx] = 0;
    share->tgt_ssl_ciphers[link_idx] = NULL;
  }
  if (
    !table->field[14]->is_null() &&
    (ptr = get_field(mem_root, table->field[14]))
  ) {
    share->tgt_ssl_keys_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_keys[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_keys_lengths[link_idx]);
  } else {
    share->tgt_ssl_keys_lengths[link_idx] = 0;
    share->tgt_ssl_keys[link_idx] = NULL;
  }
  if (
    !table->field[15]->is_null() &&
    (ptr = get_field(mem_root, table->field[15]))
  ) {
    share->tgt_ssl_vscs[link_idx] = atol(ptr);
  } else
    share->tgt_ssl_vscs[link_idx] = 0;
  if (
    !table->field[16]->is_null() &&
    (ptr = get_field(mem_root, table->field[16]))
  ) {
    share->tgt_default_files_lengths[link_idx] = strlen(ptr);
    share->tgt_default_files[link_idx] =
      spider_create_string(ptr, share->tgt_default_files_lengths[link_idx]);
  } else {
    share->tgt_default_files_lengths[link_idx] = 0;
    share->tgt_default_files[link_idx] = NULL;
  }
  if (
    !table->field[17]->is_null() &&
    (ptr = get_field(mem_root, table->field[17]))
  ) {
    share->tgt_default_groups_lengths[link_idx] = strlen(ptr);
    share->tgt_default_groups[link_idx] =
      spider_create_string(ptr, share->tgt_default_groups_lengths[link_idx]);
  } else {
    share->tgt_default_groups_lengths[link_idx] = 0;
    share->tgt_default_groups[link_idx] = NULL;
  }
  DBUG_RETURN(0);
}

int spider_check_sys_xa_status(
  TABLE *table,
  const char *status1,
  const char *status2,
  const char *status3,
  const int check_error_num,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num;
  DBUG_ENTER("spider_check_sys_xa_status");
  ptr = get_field(mem_root, table->field[4]);
  if (ptr)
  {
    if (
      strcmp(ptr, status1) &&
      (status2 == NULL || strcmp(ptr, status2)) &&
      (status3 == NULL || strcmp(ptr, status3))
    )
      error_num = check_error_num;
    else
      error_num = 0;
  } else
    error_num = check_error_num;
  DBUG_RETURN(error_num);
}

int spider_get_sys_tables(
  TABLE *table,
  char **db_name,
  char **table_name,
  MEM_ROOT *mem_root
) {
  char *ptr;
  DBUG_ENTER("spider_get_sys_tables");
  if ((ptr = get_field(mem_root, table->field[0])))
  {
    *db_name = spider_create_string(ptr, strlen(ptr));
  } else {
    *db_name = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[1])))
  {
    *table_name = spider_create_string(ptr, strlen(ptr));
  } else {
    *table_name = NULL;
  }
  DBUG_RETURN(0);
}

int spider_get_sys_tables_connect_info(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_tables_connect_info");
  if ((ptr = get_field(mem_root, table->field[3])))
  {
    share->priority = my_strtoll10(ptr, (char**) NULL, &error_num);
  } else
    share->priority = 1000000;
  if (
    !table->field[4]->is_null() &&
    (ptr = get_field(mem_root, table->field[4]))
  ) {
    share->server_names_lengths[link_idx] = strlen(ptr);
    share->server_names[link_idx] =
      spider_create_string(ptr, share->server_names_lengths[link_idx]);
  } else {
    share->server_names_lengths[link_idx] = 0;
    share->server_names[link_idx] = NULL;
  }
  if (
    !table->field[5]->is_null() &&
    (ptr = get_field(mem_root, table->field[5]))
  ) {
    share->tgt_wrappers_lengths[link_idx] = strlen(ptr);
    share->tgt_wrappers[link_idx] =
      spider_create_string(ptr, share->tgt_wrappers_lengths[link_idx]);
  } else {
    share->tgt_wrappers_lengths[link_idx] = 0;
    share->tgt_wrappers[link_idx] = NULL;
  }
  if (
    !table->field[6]->is_null() &&
    (ptr = get_field(mem_root, table->field[6]))
  ) {
    share->tgt_hosts_lengths[link_idx] = strlen(ptr);
    share->tgt_hosts[link_idx] =
      spider_create_string(ptr, share->tgt_hosts_lengths[link_idx]);
  } else {
    share->tgt_hosts_lengths[link_idx] = 0;
    share->tgt_hosts[link_idx] = NULL;
  }
  if (
    !table->field[7]->is_null() &&
    (ptr = get_field(mem_root, table->field[7]))
  ) {
    share->tgt_ports[link_idx] = atol(ptr);
  } else {
    share->tgt_ports[link_idx] = -1;
  }
  if (
    !table->field[8]->is_null() &&
    (ptr = get_field(mem_root, table->field[8]))
  ) {
    share->tgt_sockets_lengths[link_idx] = strlen(ptr);
    share->tgt_sockets[link_idx] =
      spider_create_string(ptr, share->tgt_sockets_lengths[link_idx]);
  } else {
    share->tgt_sockets_lengths[link_idx] = 0;
    share->tgt_sockets[link_idx] = NULL;
  }
  if (
    !table->field[9]->is_null() &&
    (ptr = get_field(mem_root, table->field[9]))
  ) {
    share->tgt_usernames_lengths[link_idx] = strlen(ptr);
    share->tgt_usernames[link_idx] =
      spider_create_string(ptr, share->tgt_usernames_lengths[link_idx]);
  } else {
    share->tgt_usernames_lengths[link_idx] = 0;
    share->tgt_usernames[link_idx] = NULL;
  }
  if (
    !table->field[10]->is_null() &&
    (ptr = get_field(mem_root, table->field[10]))
  ) {
    share->tgt_passwords_lengths[link_idx] = strlen(ptr);
    share->tgt_passwords[link_idx] =
      spider_create_string(ptr, share->tgt_passwords_lengths[link_idx]);
  } else {
    share->tgt_passwords_lengths[link_idx] = 0;
    share->tgt_passwords[link_idx] = NULL;
  }
  if (
    !table->field[11]->is_null() &&
    (ptr = get_field(mem_root, table->field[11]))
  ) {
    share->tgt_ssl_cas_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_cas[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_cas_lengths[link_idx]);
  } else {
    share->tgt_ssl_cas_lengths[link_idx] = 0;
    share->tgt_ssl_cas[link_idx] = NULL;
  }
  if (
    !table->field[12]->is_null() &&
    (ptr = get_field(mem_root, table->field[12]))
  ) {
    share->tgt_ssl_capaths_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_capaths[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_capaths_lengths[link_idx]);
  } else {
    share->tgt_ssl_capaths_lengths[link_idx] = 0;
    share->tgt_ssl_capaths[link_idx] = NULL;
  }
  if (
    !table->field[13]->is_null() &&
    (ptr = get_field(mem_root, table->field[13]))
  ) {
    share->tgt_ssl_certs_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_certs[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_certs_lengths[link_idx]);
  } else {
    share->tgt_ssl_certs_lengths[link_idx] = 0;
    share->tgt_ssl_certs[link_idx] = NULL;
  }
  if (
    !table->field[14]->is_null() &&
    (ptr = get_field(mem_root, table->field[14]))
  ) {
    share->tgt_ssl_ciphers_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_ciphers[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_ciphers_lengths[link_idx]);
  } else {
    share->tgt_ssl_ciphers_lengths[link_idx] = 0;
    share->tgt_ssl_ciphers[link_idx] = NULL;
  }
  if (
    !table->field[15]->is_null() &&
    (ptr = get_field(mem_root, table->field[15]))
  ) {
    share->tgt_ssl_keys_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_keys[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_keys_lengths[link_idx]);
  } else {
    share->tgt_ssl_keys_lengths[link_idx] = 0;
    share->tgt_ssl_keys[link_idx] = NULL;
  }
  if (
    !table->field[16]->is_null() &&
    (ptr = get_field(mem_root, table->field[16]))
  ) {
    share->tgt_ssl_vscs[link_idx] = atol(ptr);
  } else
    share->tgt_ssl_vscs[link_idx] = -1;
  if (
    !table->field[17]->is_null() &&
    (ptr = get_field(mem_root, table->field[17]))
  ) {
    share->tgt_default_files_lengths[link_idx] = strlen(ptr);
    share->tgt_default_files[link_idx] =
      spider_create_string(ptr, share->tgt_default_files_lengths[link_idx]);
  } else {
    share->tgt_default_files_lengths[link_idx] = 0;
    share->tgt_default_files[link_idx] = NULL;
  }
  if (
    !table->field[18]->is_null() &&
    (ptr = get_field(mem_root, table->field[18]))
  ) {
    share->tgt_default_groups_lengths[link_idx] = strlen(ptr);
    share->tgt_default_groups[link_idx] =
      spider_create_string(ptr, share->tgt_default_groups_lengths[link_idx]);
  } else {
    share->tgt_default_groups_lengths[link_idx] = 0;
    share->tgt_default_groups[link_idx] = NULL;
  }
  if (
    !table->field[19]->is_null() &&
    (ptr = get_field(mem_root, table->field[19]))
  ) {
    share->tgt_dbs_lengths[link_idx] = strlen(ptr);
    share->tgt_dbs[link_idx] =
      spider_create_string(ptr, share->tgt_dbs_lengths[link_idx]);
  } else {
    share->tgt_dbs_lengths[link_idx] = 0;
    share->tgt_dbs[link_idx] = NULL;
  }
  if (
    !table->field[20]->is_null() &&
    (ptr = get_field(mem_root, table->field[20]))
  ) {
    share->tgt_table_names_lengths[link_idx] = strlen(ptr);
    share->tgt_table_names[link_idx] =
      spider_create_string(ptr, share->tgt_table_names_lengths[link_idx]);
  } else {
    share->tgt_table_names_lengths[link_idx] = 0;
    share->tgt_table_names[link_idx] = NULL;
  }
  DBUG_RETURN(error_num);
}

int spider_get_sys_tables_link_status(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_tables_link_status");
  if ((ptr = get_field(mem_root, table->field[21])))
  {
    share->link_statuses[link_idx] =
      (long) my_strtoll10(ptr, (char**) NULL, &error_num);
  } else
    share->link_statuses[link_idx] = 1;
  DBUG_PRINT("info",("spider link_statuses[%d]=%ld",
    link_idx, share->link_statuses[link_idx]));
  DBUG_RETURN(error_num);
}

int spider_get_sys_tables_link_idx(
  TABLE *table,
  int *link_idx,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_tables_link_status");
  if ((ptr = get_field(mem_root, table->field[2])))
    *link_idx = (int) my_strtoll10(ptr, (char**) NULL, &error_num);
  else
    *link_idx = 1;
  DBUG_PRINT("info",("spider link_idx=%d", *link_idx));
  DBUG_RETURN(error_num);
}

int spider_sys_update_tables_link_status(
  THD *thd,
  char *name,
  uint name_length,
  int link_idx,
  long link_status,
  bool need_lock
) {
  int error_num;
  TABLE *table_tables = NULL;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  DBUG_ENTER("spider_sys_update_tables_link_status");
  if (
    !(table_tables = spider_open_sys_table(
      thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
      SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, need_lock,
      &error_num))
  ) {
    goto error;
  }
  if ((error_num = spider_update_tables_link_status(table_tables,
    name, name_length, link_idx, link_status)))
    goto error;
  spider_close_sys_table(thd, table_tables,
    &open_tables_backup, need_lock);
  table_tables = NULL;
  DBUG_RETURN(0);

error:
  if (table_tables)
    spider_close_sys_table(thd, table_tables,
      &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

int spider_sys_log_tables_link_failed(
  THD *thd,
  char *name,
  uint name_length,
  int link_idx,
  bool need_lock
) {
  int error_num;
  TABLE *table_tables = NULL;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  DBUG_ENTER("spider_sys_log_tables_link_failed");
  if (
    !(table_tables = spider_open_sys_table(
      thd, SPIDER_SYS_LINK_FAILED_TABLE_NAME_STR,
      SPIDER_SYS_LINK_FAILED_TABLE_NAME_LEN, TRUE, &open_tables_backup,
      need_lock, &error_num))
  ) {
    goto error;
  }
  empty_record(table_tables);
  if ((error_num = spider_log_tables_link_failed(table_tables,
    name, name_length, link_idx)))
    goto error;
  spider_close_sys_table(thd, table_tables,
    &open_tables_backup, need_lock);
  table_tables = NULL;
  DBUG_RETURN(0);

error:
  if (table_tables)
    spider_close_sys_table(thd, table_tables,
      &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

int spider_sys_log_xa_failed(
  THD *thd,
  XID *xid,
  SPIDER_CONN *conn,
  const char *status,
  bool need_lock
) {
  int error_num;
  TABLE *table_tables = NULL;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  DBUG_ENTER("spider_sys_log_xa_failed");
  if (
    !(table_tables = spider_open_sys_table(
      thd, SPIDER_SYS_XA_FAILED_TABLE_NAME_STR,
      SPIDER_SYS_XA_FAILED_TABLE_NAME_LEN, TRUE, &open_tables_backup,
      need_lock, &error_num))
  ) {
    goto error;
  }
  empty_record(table_tables);
  if ((error_num = spider_log_xa_failed(thd, table_tables, xid, conn, status)))
    goto error;
  spider_close_sys_table(thd, table_tables, &open_tables_backup, need_lock);
  table_tables = NULL;
  DBUG_RETURN(0);

error:
  if (table_tables)
    spider_close_sys_table(thd, table_tables, &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

int spider_get_sys_link_mon_key(
  TABLE *table,
  SPIDER_MON_KEY *mon_key,
  MEM_ROOT *mem_root,
  int *same
) {
  char *db_name, *table_name, *link_id;
  uint db_name_length, table_name_length, link_id_length;
  DBUG_ENTER("spider_get_sys_link_mon_key");
  if (
    table->field[0]->is_null() ||
    table->field[1]->is_null() ||
    table->field[2]->is_null()
  ) {
    my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
      ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
      SPIDER_SYS_LINK_MON_TABLE_NAME_STR);
    DBUG_RETURN(ER_SPIDER_SYS_TABLE_VERSION_NUM);
  }

  if (
    !(db_name = get_field(mem_root, table->field[0])) ||
    !(table_name = get_field(mem_root, table->field[1])) ||
    !(link_id = get_field(mem_root, table->field[2]))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  db_name_length = strlen(db_name);
  table_name_length = strlen(table_name);
  link_id_length = strlen(link_id);

  if (
    db_name_length > SPIDER_SYS_LINK_MON_TABLE_DB_NAME_SIZE ||
    table_name_length > SPIDER_SYS_LINK_MON_TABLE_TABLE_NAME_SIZE ||
    link_id_length > SPIDER_SYS_LINK_MON_TABLE_LINK_ID_SIZE
  ) {
    my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
      ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
      SPIDER_SYS_LINK_MON_TABLE_NAME_STR);
    DBUG_RETURN(ER_SPIDER_SYS_TABLE_VERSION_NUM);
  }

  if (
    db_name_length == mon_key->db_name_length &&
    table_name_length == mon_key->table_name_length &&
    link_id_length == mon_key->link_id_length &&
    !memcmp(db_name, mon_key->db_name, db_name_length) &&
    !memcmp(table_name, mon_key->table_name, table_name_length) &&
    !memcmp(link_id, mon_key->link_id, link_id_length)
  ) {
    /* same key */
    *same = 1;
    DBUG_RETURN(0);
  }

  *same = 0;
  mon_key->db_name_length = db_name_length;
  memcpy(mon_key->db_name, db_name, db_name_length + 1);
  mon_key->table_name_length = table_name_length;
  memcpy(mon_key->table_name, table_name, table_name_length + 1);
  mon_key->link_id_length = link_id_length;
  memcpy(mon_key->link_id, link_id, link_id_length + 1);
  DBUG_RETURN(0);
}

int spider_get_sys_link_mon_server_id(
  TABLE *table,
  uint32 *server_id,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_link_mon_server_id");
  if ((ptr = get_field(mem_root, table->field[3])))
    *server_id = (uint32) my_strtoll10(ptr, (char**) NULL, &error_num);
  else
    *server_id = ~(uint32) 0;
  DBUG_RETURN(error_num);
}

int spider_get_sys_link_mon_connect_info(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_link_mon_connect_info");
  if (
    !table->field[4]->is_null() &&
    (ptr = get_field(mem_root, table->field[4]))
  ) {
    share->server_names_lengths[link_idx] = strlen(ptr);
    share->server_names[link_idx] =
      spider_create_string(ptr, share->server_names_lengths[link_idx]);
  } else {
    share->server_names_lengths[link_idx] = 0;
    share->server_names[link_idx] = NULL;
  }
  if (
    !table->field[5]->is_null() &&
    (ptr = get_field(mem_root, table->field[5]))
  ) {
    share->tgt_wrappers_lengths[link_idx] = strlen(ptr);
    share->tgt_wrappers[link_idx] =
      spider_create_string(ptr, share->tgt_wrappers_lengths[link_idx]);
  } else {
    share->tgt_wrappers_lengths[link_idx] = 0;
    share->tgt_wrappers[link_idx] = NULL;
  }
  if (
    !table->field[6]->is_null() &&
    (ptr = get_field(mem_root, table->field[6]))
  ) {
    share->tgt_hosts_lengths[link_idx] = strlen(ptr);
    share->tgt_hosts[link_idx] =
      spider_create_string(ptr, share->tgt_hosts_lengths[link_idx]);
  } else {
    share->tgt_hosts_lengths[link_idx] = 0;
    share->tgt_hosts[link_idx] = NULL;
  }
  if (
    !table->field[7]->is_null() &&
    (ptr = get_field(mem_root, table->field[7]))
  ) {
    share->tgt_ports[link_idx] = atol(ptr);
  } else {
    share->tgt_ports[link_idx] = -1;
  }
  if (
    !table->field[8]->is_null() &&
    (ptr = get_field(mem_root, table->field[8]))
  ) {
    share->tgt_sockets_lengths[link_idx] = strlen(ptr);
    share->tgt_sockets[link_idx] =
      spider_create_string(ptr, share->tgt_sockets_lengths[link_idx]);
  } else {
    share->tgt_sockets_lengths[link_idx] = 0;
    share->tgt_sockets[link_idx] = NULL;
  }
  if (
    !table->field[9]->is_null() &&
    (ptr = get_field(mem_root, table->field[9]))
  ) {
    share->tgt_usernames_lengths[link_idx] = strlen(ptr);
    share->tgt_usernames[link_idx] =
      spider_create_string(ptr, share->tgt_usernames_lengths[link_idx]);
  } else {
    share->tgt_usernames_lengths[link_idx] = 0;
    share->tgt_usernames[link_idx] = NULL;
  }
  if (
    !table->field[10]->is_null() &&
    (ptr = get_field(mem_root, table->field[10]))
  ) {
    share->tgt_passwords_lengths[link_idx] = strlen(ptr);
    share->tgt_passwords[link_idx] =
      spider_create_string(ptr, share->tgt_passwords_lengths[link_idx]);
  } else {
    share->tgt_passwords_lengths[link_idx] = 0;
    share->tgt_passwords[link_idx] = NULL;
  }
  if (
    !table->field[11]->is_null() &&
    (ptr = get_field(mem_root, table->field[11]))
  ) {
    share->tgt_ssl_cas_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_cas[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_cas_lengths[link_idx]);
  } else {
    share->tgt_ssl_cas_lengths[link_idx] = 0;
    share->tgt_ssl_cas[link_idx] = NULL;
  }
  if (
    !table->field[12]->is_null() &&
    (ptr = get_field(mem_root, table->field[12]))
  ) {
    share->tgt_ssl_capaths_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_capaths[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_capaths_lengths[link_idx]);
  } else {
    share->tgt_ssl_capaths_lengths[link_idx] = 0;
    share->tgt_ssl_capaths[link_idx] = NULL;
  }
  if (
    !table->field[13]->is_null() &&
    (ptr = get_field(mem_root, table->field[13]))
  ) {
    share->tgt_ssl_certs_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_certs[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_certs_lengths[link_idx]);
  } else {
    share->tgt_ssl_certs_lengths[link_idx] = 0;
    share->tgt_ssl_certs[link_idx] = NULL;
  }
  if (
    !table->field[14]->is_null() &&
    (ptr = get_field(mem_root, table->field[14]))
  ) {
    share->tgt_ssl_ciphers_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_ciphers[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_ciphers_lengths[link_idx]);
  } else {
    share->tgt_ssl_ciphers_lengths[link_idx] = 0;
    share->tgt_ssl_ciphers[link_idx] = NULL;
  }
  if (
    !table->field[15]->is_null() &&
    (ptr = get_field(mem_root, table->field[15]))
  ) {
    share->tgt_ssl_keys_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_keys[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_keys_lengths[link_idx]);
  } else {
    share->tgt_ssl_keys_lengths[link_idx] = 0;
    share->tgt_ssl_keys[link_idx] = NULL;
  }
  if (
    !table->field[16]->is_null() &&
    (ptr = get_field(mem_root, table->field[16]))
  ) {
    share->tgt_ssl_vscs[link_idx] = atol(ptr);
  } else
    share->tgt_ssl_vscs[link_idx] = -1;
  if (
    !table->field[17]->is_null() &&
    (ptr = get_field(mem_root, table->field[17]))
  ) {
    share->tgt_default_files_lengths[link_idx] = strlen(ptr);
    share->tgt_default_files[link_idx] =
      spider_create_string(ptr, share->tgt_default_files_lengths[link_idx]);
  } else {
    share->tgt_default_files_lengths[link_idx] = 0;
    share->tgt_default_files[link_idx] = NULL;
  }
  if (
    !table->field[18]->is_null() &&
    (ptr = get_field(mem_root, table->field[18]))
  ) {
    share->tgt_default_groups_lengths[link_idx] = strlen(ptr);
    share->tgt_default_groups[link_idx] =
      spider_create_string(ptr, share->tgt_default_groups_lengths[link_idx]);
  } else {
    share->tgt_default_groups_lengths[link_idx] = 0;
    share->tgt_default_groups[link_idx] = NULL;
  }
  DBUG_RETURN(error_num);
}

int spider_get_link_statuses(
  TABLE *table,
  SPIDER_SHARE *share,
  MEM_ROOT *mem_root
) {
  int error_num, roop_count;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_get_link_statuses");
  table->use_all_columns();
  spider_store_tables_name(table, share->table_name,
    share->table_name_length);
  for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
  {
    spider_store_tables_link_idx(table, roop_count);
    if ((error_num = spider_check_sys_table(table, table_key)))
    {
      if (
        (error_num == HA_ERR_KEY_NOT_FOUND || error_num == HA_ERR_END_OF_FILE)
      ) {
/*
        table->file->print_error(error_num, MYF(0));
*/
        DBUG_RETURN(error_num);
      }
    } else if ((error_num =
      spider_get_sys_tables_link_status(table, share, roop_count, mem_root)))
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_sys_replace(
  TABLE *table,
  bool *modified_non_trans_table
) {
  int error_num, key_num;
  bool last_uniq_key;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_sys_replace");

  while ((error_num = spider_write_sys_table_row(table, FALSE)))
  {
    if (
      table->file->is_fatal_error(error_num, HA_CHECK_DUP) ||
      (key_num = table->file->get_dup_key(error_num)) < 0
    )
      goto error;

    if (table->file->ha_table_flags() & HA_DUPLICATE_POS)
    {
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
      error_num = table->file->ha_rnd_pos(table->record[1],
        table->file->dup_ref);
#else
      error_num = table->file->rnd_pos(table->record[1], table->file->dup_ref);
#endif
      if (error_num)
      {
        if (error_num == HA_ERR_RECORD_DELETED)
          error_num = HA_ERR_KEY_NOT_FOUND;
        goto error;
      }
    } else {
      if ((error_num = table->file->extra(HA_EXTRA_FLUSH_CACHE)))
        goto error;

      key_copy((uchar*)table_key, table->record[0],
        table->key_info + key_num, 0);
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
      error_num = table->file->ha_index_read_idx_map(table->record[1], key_num,
        (const uchar*)table_key, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
#else
      error_num = table->file->index_read_idx_map(table->record[1], key_num,
        (const uchar*)table_key, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
#endif
      if (error_num)
      {
        if (error_num == HA_ERR_RECORD_DELETED)
          error_num = HA_ERR_KEY_NOT_FOUND;
        goto error;
      }
    }

    last_uniq_key = TRUE;
    while (++key_num < (int) table->s->keys)
      if (table->key_info[key_num].flags & HA_NOSAME)
        last_uniq_key = FALSE;

    if (
      last_uniq_key &&
      !table->file->referenced_by_foreign_key()
    ) {
      if ((error_num = spider_update_sys_table_row(table)))
        goto error;
      DBUG_RETURN(0);
    } else {
      if ((error_num = spider_delete_sys_table_row(table, 1, FALSE)))
        goto error;
      *modified_non_trans_table = TRUE;
    }
  }

  DBUG_RETURN(0);

error:
  DBUG_RETURN(error_num);
}

TABLE *spider_mk_sys_tmp_table(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const char *field_name,
  CHARSET_INFO *cs
) {
  Field_blob *field;
  Item_field *i_field;
  List<Item> i_list;
  TABLE *tmp_table;
  DBUG_ENTER("spider_mk_sys_tmp_table");

  if (!(field = new Field_blob(
    4294967295U, FALSE, field_name, cs, TRUE)))
    goto error_alloc_field;
  field->init(table);

#ifdef SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
  if (!(i_field = new (thd->mem_root) Item_field(thd, (Field *) field)))
    goto error_alloc_item_field;
#else
  if (!(i_field = new Item_field((Field *) field)))
    goto error_alloc_item_field;
#endif

  if (i_list.push_back(i_field))
    goto error_push_item;

  if (!(tmp_table = create_tmp_table(thd, tmp_tbl_prm,
    i_list, (ORDER*) NULL, FALSE, FALSE, TMP_TABLE_FORCE_MYISAM,
    HA_POS_ERROR, (char *) "")))
    goto error_create_tmp_table;
  DBUG_RETURN(tmp_table);

error_create_tmp_table:
error_push_item:
  delete i_field;
error_alloc_item_field:
  delete field;
error_alloc_field:
  DBUG_RETURN(NULL);
}

void spider_rm_sys_tmp_table(
  THD *thd,
  TABLE *tmp_table,
  TMP_TABLE_PARAM *tmp_tbl_prm
) {
  DBUG_ENTER("spider_rm_sys_tmp_table");
  free_tmp_table(thd, tmp_table);
  tmp_tbl_prm->cleanup();
  tmp_tbl_prm->field_count = 1;
  DBUG_VOID_RETURN;
}

TABLE *spider_mk_sys_tmp_table_for_result(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const char *field_name1,
  const char *field_name2,
  const char *field_name3,
  CHARSET_INFO *cs
) {
  Field_blob *field1, *field2, *field3;
  Item_field *i_field1, *i_field2, *i_field3;
  List<Item> i_list;
  TABLE *tmp_table;
  DBUG_ENTER("spider_mk_sys_tmp_table_for_result");

  if (!(field1 = new Field_blob(
    4294967295U, FALSE, field_name1, cs, TRUE)))
    goto error_alloc_field1;
  field1->init(table);

#ifdef SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
  if (!(i_field1 = new (thd->mem_root) Item_field(thd, (Field *) field1)))
    goto error_alloc_item_field1;
#else
  if (!(i_field1 = new Item_field((Field *) field1)))
    goto error_alloc_item_field1;
#endif

  if (i_list.push_back(i_field1))
    goto error_push_item1;

  if (!(field2 = new (thd->mem_root) Field_blob(
    4294967295U, FALSE, field_name2, cs, TRUE)))
    goto error_alloc_field2;
  field2->init(table);

#ifdef SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
  if (!(i_field2 = new (thd->mem_root) Item_field(thd, (Field *) field2)))
    goto error_alloc_item_field2;
#else
  if (!(i_field2 = new Item_field((Field *) field2)))
    goto error_alloc_item_field2;
#endif

  if (i_list.push_back(i_field2))
    goto error_push_item2;

  if (!(field3 = new (thd->mem_root) Field_blob(
    4294967295U, FALSE, field_name3, cs, TRUE)))
    goto error_alloc_field3;
  field3->init(table);

#ifdef SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
  if (!(i_field3 = new (thd->mem_root) Item_field(thd, (Field *) field3)))
    goto error_alloc_item_field3;
#else
  if (!(i_field3 = new Item_field((Field *) field3)))
    goto error_alloc_item_field3;
#endif

  if (i_list.push_back(i_field3))
    goto error_push_item3;

  if (!(tmp_table = create_tmp_table(thd, tmp_tbl_prm,
    i_list, (ORDER*) NULL, FALSE, FALSE, TMP_TABLE_FORCE_MYISAM,
    HA_POS_ERROR, (char *) "")))
    goto error_create_tmp_table;
  DBUG_RETURN(tmp_table);

error_create_tmp_table:
error_push_item3:
  delete i_field3;
error_alloc_item_field3:
  delete field3;
error_alloc_field3:
error_push_item2:
  delete i_field2;
error_alloc_item_field2:
  delete field2;
error_alloc_field2:
error_push_item1:
  delete i_field1;
error_alloc_item_field1:
  delete field1;
error_alloc_field1:
  DBUG_RETURN(NULL);
}

void spider_rm_sys_tmp_table_for_result(
  THD *thd,
  TABLE *tmp_table,
  TMP_TABLE_PARAM *tmp_tbl_prm
) {
  DBUG_ENTER("spider_rm_sys_tmp_table_for_result");
  free_tmp_table(thd, tmp_table);
  tmp_tbl_prm->cleanup();
  tmp_tbl_prm->field_count = 3;
  DBUG_VOID_RETURN;
}
