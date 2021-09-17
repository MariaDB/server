/* Copyright (C) 2012-2015 Kentoku Shiba

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
#include "sql_analyse.h"
#include "sql_base.h"
#include "tztime.h"
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
#include "sql_select.h"
#endif
#endif
#include "sql_common.h"
#include <mysql.h>
#include <errmsg.h>
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_db_mysql.h"
#include "ha_spider.h"
#include "spd_conn.h"
#include "spd_db_conn.h"
#include "spd_malloc.h"
#include "spd_sys_table.h"
#include "spd_table.h"

extern struct charset_info_st *spd_charset_utf8_bin;
extern bool volatile *spd_abort_loop;

extern handlerton *spider_hton_ptr;
extern pthread_mutex_t spider_open_conn_mutex;
extern HASH spider_open_connections;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
extern const char spider_dig_upper[];

#define SPIDER_SQL_NAME_QUOTE_STR "`"
#define SPIDER_SQL_NAME_QUOTE_LEN (sizeof(SPIDER_SQL_NAME_QUOTE_STR) - 1)
static const char *name_quote_str = SPIDER_SQL_NAME_QUOTE_STR;

#define SPIDER_SQL_ISO_READ_UNCOMMITTED_STR "set session transaction isolation level read uncommitted"
#define SPIDER_SQL_ISO_READ_UNCOMMITTED_LEN sizeof(SPIDER_SQL_ISO_READ_UNCOMMITTED_STR) - 1
#define SPIDER_SQL_ISO_READ_COMMITTED_STR "set session transaction isolation level read committed"
#define SPIDER_SQL_ISO_READ_COMMITTED_LEN sizeof(SPIDER_SQL_ISO_READ_COMMITTED_STR) - 1
#define SPIDER_SQL_ISO_REPEATABLE_READ_STR "set session transaction isolation level repeatable read"
#define SPIDER_SQL_ISO_REPEATABLE_READ_LEN sizeof(SPIDER_SQL_ISO_REPEATABLE_READ_STR) - 1
#define SPIDER_SQL_ISO_SERIALIZABLE_STR "set session transaction isolation level serializable"
#define SPIDER_SQL_ISO_SERIALIZABLE_LEN sizeof(SPIDER_SQL_ISO_SERIALIZABLE_STR) - 1

#define SPIDER_SQL_START_CONSISTENT_SNAPSHOT_STR "start transaction with consistent snapshot"
#define SPIDER_SQL_START_CONSISTENT_SNAPSHOT_LEN sizeof(SPIDER_SQL_START_CONSISTENT_SNAPSHOT_STR) - 1
#define SPIDER_SQL_START_TRANSACTION_STR "start transaction"
#define SPIDER_SQL_START_TRANSACTION_LEN sizeof(SPIDER_SQL_START_TRANSACTION_STR) - 1

#define SPIDER_SQL_AUTOCOMMIT_OFF_STR "set session autocommit = 0"
#define SPIDER_SQL_AUTOCOMMIT_OFF_LEN sizeof(SPIDER_SQL_AUTOCOMMIT_OFF_STR) - 1
#define SPIDER_SQL_AUTOCOMMIT_ON_STR "set session autocommit = 1"
#define SPIDER_SQL_AUTOCOMMIT_ON_LEN sizeof(SPIDER_SQL_AUTOCOMMIT_ON_STR) - 1

#define SPIDER_SQL_SQL_LOG_OFF_STR "set session sql_log_off = 0"
#define SPIDER_SQL_SQL_LOG_OFF_LEN sizeof(SPIDER_SQL_SQL_LOG_OFF_STR) - 1
#define SPIDER_SQL_SQL_LOG_ON_STR "set session sql_log_off = 1"
#define SPIDER_SQL_SQL_LOG_ON_LEN sizeof(SPIDER_SQL_SQL_LOG_ON_STR) - 1

#define SPIDER_SQL_TIME_ZONE_STR "set session time_zone = '"
#define SPIDER_SQL_TIME_ZONE_LEN sizeof(SPIDER_SQL_TIME_ZONE_STR) - 1

#define SPIDER_SQL_COMMIT_STR "commit"
#define SPIDER_SQL_COMMIT_LEN sizeof(SPIDER_SQL_COMMIT_STR) - 1
#define SPIDER_SQL_ROLLBACK_STR "rollback"
#define SPIDER_SQL_ROLLBACK_LEN sizeof(SPIDER_SQL_ROLLBACK_STR) - 1

#define SPIDER_SQL_XA_START_STR "xa start "
#define SPIDER_SQL_XA_START_LEN sizeof(SPIDER_SQL_XA_START_STR) - 1
#define SPIDER_SQL_XA_END_STR "xa end "
#define SPIDER_SQL_XA_END_LEN sizeof(SPIDER_SQL_XA_END_STR) - 1
#define SPIDER_SQL_XA_PREPARE_STR "xa prepare "
#define SPIDER_SQL_XA_PREPARE_LEN sizeof(SPIDER_SQL_XA_PREPARE_STR) - 1
#define SPIDER_SQL_XA_COMMIT_STR "xa commit "
#define SPIDER_SQL_XA_COMMIT_LEN sizeof(SPIDER_SQL_XA_COMMIT_STR) - 1
#define SPIDER_SQL_XA_ROLLBACK_STR "xa rollback "
#define SPIDER_SQL_XA_ROLLBACK_LEN sizeof(SPIDER_SQL_XA_ROLLBACK_STR) - 1

#define SPIDER_SQL_LOCK_TABLE_STR "lock tables "
#define SPIDER_SQL_LOCK_TABLE_LEN (sizeof(SPIDER_SQL_LOCK_TABLE_STR) - 1)
#define SPIDER_SQL_UNLOCK_TABLE_STR "unlock tables"
#define SPIDER_SQL_UNLOCK_TABLE_LEN (sizeof(SPIDER_SQL_UNLOCK_TABLE_STR) - 1)

#define SPIDER_SQL_SHOW_TABLE_STATUS_STR "show table status from "
#define SPIDER_SQL_SHOW_TABLE_STATUS_LEN sizeof(SPIDER_SQL_SHOW_TABLE_STATUS_STR) - 1
#define SPIDER_SQL_SELECT_TABLES_STATUS_STR "select `table_rows`,`avg_row_length`,`data_length`,`max_data_length`,`index_length`,`auto_increment`,`create_time`,`update_time`,`check_time` from `information_schema`.`tables` where `table_schema` = "
#define SPIDER_SQL_SELECT_TABLES_STATUS_LEN sizeof(SPIDER_SQL_SELECT_TABLES_STATUS_STR) - 1
#define SPIDER_SQL_SHOW_WARNINGS_STR "show warnings"
#define SPIDER_SQL_SHOW_WARNINGS_LEN sizeof(SPIDER_SQL_SHOW_WARNINGS_STR) - 1

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
#define SPIDER_SQL_SHOW_COLUMNS_STR "show columns from "
#define SPIDER_SQL_SHOW_COLUMNS_LEN sizeof(SPIDER_SQL_SHOW_COLUMNS_STR) - 1
#define SPIDER_SQL_SELECT_COLUMNS_STR "select `column_name`,`column_default`,`is_nullable`,`character_set_name`,`collation_name`,`column_type`,`extra` from `information_schema`.`columns` where `table_schema` = "
#define SPIDER_SQL_SELECT_COLUMNS_LEN sizeof(SPIDER_SQL_SELECT_COLUMNS_STR) - 1

#define SPIDER_SQL_AUTO_INCREMENT_STR " auto_increment"
#define SPIDER_SQL_AUTO_INCREMENT_LEN sizeof(SPIDER_SQL_AUTO_INCREMENT_STR) - 1
#define SPIDER_SQL_ORDINAL_POSITION_STR "ordinal_position"
#define SPIDER_SQL_ORDINAL_POSITION_LEN sizeof(SPIDER_SQL_ORDINAL_POSITION_STR) - 1
#define SPIDER_SQL_FULLTEXT_STR "fulltext"
#define SPIDER_SQL_FULLTEXT_LEN sizeof(SPIDER_SQL_FULLTEXT_STR) - 1
#define SPIDER_SQL_SPATIAL_STR "spatial"
#define SPIDER_SQL_SPATIAL_LEN sizeof(SPIDER_SQL_SPATIAL_STR) - 1
#define SPIDER_SQL_USING_HASH_STR " using hash"
#define SPIDER_SQL_USING_HASH_LEN sizeof(SPIDER_SQL_USING_HASH_STR) - 1
#endif

#define SPIDER_SQL_LIKE_STR " like "
#define SPIDER_SQL_LIKE_LEN (sizeof(SPIDER_SQL_LIKE_STR) - 1)
#define SPIDER_SQL_LIMIT1_STR " limit 1"
#define SPIDER_SQL_LIMIT1_LEN (sizeof(SPIDER_SQL_LIMIT1_STR) - 1)
#define SPIDER_SQL_COLLATE_STR " collate "
#define SPIDER_SQL_COLLATE_LEN (sizeof(SPIDER_SQL_COLLATE_STR) - 1)

#define SPIDER_SQL_INTERVAL_STR " + interval "
#define SPIDER_SQL_INTERVAL_LEN (sizeof(SPIDER_SQL_INTERVAL_STR) - 1)
#define SPIDER_SQL_NEGINTERVAL_STR " - interval "
#define SPIDER_SQL_NEGINTERVAL_LEN (sizeof(SPIDER_SQL_NEGINTERVAL_STR) - 1)

static uchar SPIDER_SQL_LINESTRING_HEAD_STR[] =
  {0x00,0x00,0x00,0x00,0x01,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00};
#define SPIDER_SQL_LINESTRING_HEAD_LEN sizeof(SPIDER_SQL_LINESTRING_HEAD_STR)

static const char *spider_db_table_lock_str[] =
{
  " read local,",
  " read,",
  " low_priority write,",
  " write,"
};
static const int spider_db_table_lock_len[] =
{
  sizeof(" read local,") - 1,
  sizeof(" read,") - 1,
  sizeof(" low_priority write,") - 1,
  sizeof(" write,") - 1
};
static const char *spider_db_timefunc_interval_str[] =
{
  " year", " quarter", " month", " week", " day",
  " hour", " minute", " second", " microsecond",
  " year_month", " day_hour", " day_minute",
  " day_second", " hour_minute", " hour_second",
  " minute_second", " day_microsecond", " hour_microsecond",
  " minute_microsecond", " second_microsecond"
};

int spider_mysql_init()
{
  DBUG_ENTER("spider_mysql_init");
  DBUG_RETURN(0);
}

int spider_mysql_deinit()
{
  DBUG_ENTER("spider_mysql_deinit");
  DBUG_RETURN(0);
}

spider_db_share *spider_mysql_create_share(
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_mysql_create_share");
  DBUG_RETURN(new spider_mysql_share(share));
}

spider_db_handler *spider_mysql_create_handler(
  ha_spider *spider,
  spider_db_share *db_share
) {
  DBUG_ENTER("spider_mysql_create_handler");
  DBUG_RETURN(new spider_mysql_handler(spider,
    (spider_mysql_share *) db_share));
}

spider_db_copy_table *spider_mysql_create_copy_table(
  spider_db_share *db_share
) {
  DBUG_ENTER("spider_mysql_create_copy_table");
  DBUG_RETURN(new spider_mysql_copy_table(
    (spider_mysql_share *) db_share));
}

SPIDER_DB_CONN *spider_mysql_create_conn(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_mysql_create_conn");
  DBUG_RETURN(new spider_db_mysql(conn));
}

spider_db_mysql_util spider_db_mysql_utility;

SPIDER_DBTON spider_dbton_mysql = {
  0,
  SPIDER_DB_WRAPPER_MYSQL,
  SPIDER_DB_ACCESS_TYPE_SQL,
  spider_mysql_init,
  spider_mysql_deinit,
  spider_mysql_create_share,
  spider_mysql_create_handler,
  spider_mysql_create_copy_table,
  spider_mysql_create_conn,
  &spider_db_mysql_utility
};

spider_db_mysql_row::spider_db_mysql_row() :
  spider_db_row(spider_dbton_mysql.dbton_id),
  row(NULL), lengths(NULL), cloned(FALSE)
{
  DBUG_ENTER("spider_db_mysql_row::spider_db_mysql_row");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_mysql_row::~spider_db_mysql_row()
{
  DBUG_ENTER("spider_db_mysql_row::~spider_db_mysql_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (cloned)
  {
    spider_free(spider_current_trx, row_first, MYF(0));
  }
  DBUG_VOID_RETURN;
}

int spider_db_mysql_row::store_to_field(
  Field *field,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_mysql_row::store_to_field");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!*row)
  {
    DBUG_PRINT("info", ("spider field is null"));
    field->set_null();
    field->reset();
  } else {
    field->set_notnull();
    if (field->flags & BLOB_FLAG)
    {
      DBUG_PRINT("info", ("spider blob field"));
      if (
        field->charset() == &my_charset_bin ||
        field->charset()->cset == access_charset->cset
      )
        ((Field_blob *)field)->set_ptr(*lengths, (uchar *) *row);
      else {
        DBUG_PRINT("info", ("spider blob convert"));
        if (field->table->file->ht == spider_hton_ptr)
        {
          ha_spider *spider = (ha_spider *) field->table->file;
          spider_string *str = &spider->blob_buff[field->field_index];
          str->length(0);
          if (str->append(*row, *lengths, access_charset))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          ((Field_blob *)field)->set_ptr(str->length(), (uchar *) str->ptr());
        } else {
          field->store(*row, *lengths, access_charset);
        }
      }
    } else
      field->store(*row, *lengths, access_charset);
  }
  DBUG_RETURN(0);
}

int spider_db_mysql_row::append_to_str(
  spider_string *str
) {
  DBUG_ENTER("spider_db_mysql_row::append_to_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(*lengths))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(*row, *lengths);
  DBUG_RETURN(0);
}

int spider_db_mysql_row::append_escaped_to_str(
  spider_string *str,
  uint dbton_id
) {
  DBUG_ENTER("spider_db_mysql_row::append_escaped_to_str");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_string tmp_str(*row, *lengths + 1, str->charset());
  tmp_str.init_calc_mem(133);
  tmp_str.length(*lengths);
  if (str->reserve(*lengths * 2 + 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  spider_dbton[dbton_id].db_util->append_escaped_util(str, tmp_str.get_str());
  DBUG_RETURN(0);
}

void spider_db_mysql_row::first()
{
  DBUG_ENTER("spider_db_mysql_row::first");
  DBUG_PRINT("info",("spider this=%p", this));
  row = row_first;
  lengths = lengths_first;
  DBUG_VOID_RETURN;
}

void spider_db_mysql_row::next()
{
  DBUG_ENTER("spider_db_mysql_row::next");
  DBUG_PRINT("info",("spider this=%p", this));
  row++;
  lengths++;
  DBUG_VOID_RETURN;
}

bool spider_db_mysql_row::is_null()
{
  DBUG_ENTER("spider_db_mysql_row::is_null");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(!(*row));
}

int spider_db_mysql_row::val_int()
{
  DBUG_ENTER("spider_db_mysql_row::val_int");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(*row ? atoi(*row) : 0);
}

double spider_db_mysql_row::val_real()
{
  DBUG_ENTER("spider_db_mysql_row::val_real");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(*row ? my_atof(*row) : 0.0);
}

my_decimal *spider_db_mysql_row::val_decimal(
  my_decimal *decimal_value,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_mysql_row::val_decimal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!*row)
    DBUG_RETURN(NULL);

#ifdef SPIDER_HAS_DECIMAL_OPERATION_RESULTS_VALUE_TYPE
  decimal_operation_results(str2my_decimal(0, *row, *lengths, access_charset,
    decimal_value), "", "");
#else
  decimal_operation_results(str2my_decimal(0, *row, *lengths, access_charset,
    decimal_value));
#endif

  DBUG_RETURN(decimal_value);
}

SPIDER_DB_ROW *spider_db_mysql_row::clone()
{
  spider_db_mysql_row *clone_row;
  char *tmp_char;
  MYSQL_ROW tmp_row = row_first, ctmp_row;
  ulong *tmp_lengths = lengths_first;
  uint row_size, i;
  DBUG_ENTER("spider_db_mysql_row::clone");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(clone_row = new spider_db_mysql_row()))
  {
    DBUG_RETURN(NULL);
  }
  row_size = field_count;
  for (i = 0; i < field_count; i++)
  {
    row_size += *tmp_lengths;
    tmp_lengths++;
  }
  if (!spider_bulk_malloc(spider_current_trx, 29, MYF(MY_WME),
    &clone_row->row, sizeof(char*) * field_count,
    &tmp_char, row_size,
    &clone_row->lengths, sizeof(ulong) * field_count,
    NullS)
  ) {
    delete clone_row;
    DBUG_RETURN(NULL);
  }
  memcpy(clone_row->lengths, lengths_first, sizeof(ulong) * field_count);
  tmp_lengths = lengths_first;
  ctmp_row = clone_row->row;
  for (i = 0; i < field_count; i++)
  {
    DBUG_PRINT("info",("spider *lengths=%lu", *tmp_lengths));
    if (*tmp_row == NULL)
    {
      *ctmp_row = NULL;
      *tmp_char = 0;
      tmp_char++;
    } else {
      *ctmp_row = tmp_char;
      memcpy(tmp_char, *tmp_row, *tmp_lengths + 1);
      tmp_char += *tmp_lengths + 1;
    }
    ctmp_row++;
    tmp_lengths++;
    tmp_row++;
  }
  clone_row->field_count = field_count;
  clone_row->row_first = clone_row->row;
  clone_row->lengths_first = clone_row->lengths;
  clone_row->cloned = TRUE;
  DBUG_RETURN((SPIDER_DB_ROW *) clone_row);
}

int spider_db_mysql_row::store_to_tmp_table(
  TABLE *tmp_table,
  spider_string *str
) {
  uint i;
  MYSQL_ROW tmp_row = row;
  ulong *tmp_lengths = lengths;
  DBUG_ENTER("spider_db_mysql_row::store_to_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  for (i = 0; i < field_count; i++)
  {
    if (*tmp_row)
    {
      if (str->reserve(*tmp_lengths + 1))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(*tmp_row, *tmp_lengths + 1);
    }
    tmp_lengths++;
    tmp_row++;
  }
  tmp_table->field[0]->set_notnull();
  tmp_table->field[0]->store(
    (const char *) lengths,
    sizeof(ulong) * field_count, &my_charset_bin);
  tmp_table->field[1]->set_notnull();
  tmp_table->field[1]->store(
    str->ptr(), str->length(), &my_charset_bin);
  tmp_table->field[2]->set_notnull();
  tmp_table->field[2]->store(
    (char *) row, (uint) (sizeof(char *) * field_count), &my_charset_bin);
  DBUG_RETURN(tmp_table->file->ha_write_row(tmp_table->record[0]));
}

spider_db_mysql_result::spider_db_mysql_result() :
  spider_db_result(spider_dbton_mysql.dbton_id),
  db_result(NULL)
{
  DBUG_ENTER("spider_db_mysql_result::spider_db_mysql_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_mysql_result::~spider_db_mysql_result()
{
  DBUG_ENTER("spider_db_mysql_result::~spider_db_mysql_result");
  DBUG_PRINT("info",("spider this=%p", this));
  if (db_result)
  {
    free_result();
  }
  DBUG_VOID_RETURN;
}

bool spider_db_mysql_result::has_result()
{
  DBUG_ENTER("spider_db_mysql_result::has_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(db_result);
}

void spider_db_mysql_result::free_result()
{
  DBUG_ENTER("spider_db_mysql_result::free_result");
  DBUG_PRINT("info",("spider this=%p", this));
  /* need 2 times execution design */
  if (db_result)
  {
    mysql_free_result(db_result);
    db_result = NULL;
  }
  DBUG_VOID_RETURN;
}

SPIDER_DB_ROW *spider_db_mysql_result::current_row()
{
  DBUG_ENTER("spider_db_mysql_result::current_row");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((SPIDER_DB_ROW *) row.clone());
}

SPIDER_DB_ROW *spider_db_mysql_result::fetch_row()
{
  DBUG_ENTER("spider_db_mysql_result::fetch_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(row.row = mysql_fetch_row(db_result)))
  {
    store_error_num = HA_ERR_END_OF_FILE;
    DBUG_RETURN(NULL);
  }
  row.lengths = mysql_fetch_lengths(db_result);
  row.field_count = mysql_num_fields(db_result);
  row.row_first = row.row;
  row.lengths_first = row.lengths;
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

SPIDER_DB_ROW *spider_db_mysql_result::fetch_row_from_result_buffer(
  spider_db_result_buffer *spider_res_buf
) {
  DBUG_ENTER("spider_db_mysql_result::fetch_row_from_result_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(row.row = mysql_fetch_row(db_result)))
  {
    store_error_num = HA_ERR_END_OF_FILE;
    DBUG_RETURN(NULL);
  }
  row.lengths = mysql_fetch_lengths(db_result);
  row.field_count = mysql_num_fields(db_result);
  row.row_first = row.row;
  row.lengths_first = row.lengths;
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

SPIDER_DB_ROW *spider_db_mysql_result::fetch_row_from_tmp_table(
  TABLE *tmp_table
) {
  uint i;
  spider_string tmp_str1, tmp_str2, tmp_str3;
  const char *row_ptr;
  MYSQL_ROW tmp_row;
  ulong *tmp_lengths;
  uint field_count;
  DBUG_ENTER("spider_db_mysql_result::fetch_row_from_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_str1.init_calc_mem(117);
  tmp_str2.init_calc_mem(118);
  tmp_str3.init_calc_mem(170);
  tmp_table->field[0]->val_str(tmp_str1.get_str());
  tmp_table->field[1]->val_str(tmp_str2.get_str());
  tmp_table->field[2]->val_str(tmp_str3.get_str());
  tmp_str1.mem_calc();
  tmp_str2.mem_calc();
  tmp_str3.mem_calc();
  row_ptr = tmp_str2.ptr();
  tmp_lengths = (ulong *) tmp_str1.ptr();
  tmp_row = (MYSQL_ROW) tmp_str3.ptr();
  field_count = tmp_str1.length() / sizeof(ulong);
  row.row = tmp_row;
  row.lengths = tmp_lengths;
  row.field_count = field_count;
  row.row_first = row.row;
  row.lengths_first = row.lengths;
  for (i = 0; i < field_count; i++)
  {
    if (*tmp_row)
    {
      *tmp_row = (char *) row_ptr;
      row_ptr += *tmp_lengths + 1;
    }
    tmp_row++;
    tmp_lengths++;
  }
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

int spider_db_mysql_result::fetch_table_status(
  int mode,
  ha_rows &records,
  ulong &mean_rec_length,
  ulonglong &data_file_length,
  ulonglong &max_data_file_length,
  ulonglong &index_file_length,
  ulonglong &auto_increment_value,
  time_t &create_time,
  time_t &update_time,
  time_t &check_time
) {
  int error_num;
  MYSQL_ROW mysql_row;
  MYSQL_TIME mysql_time;
#ifdef MARIADB_BASE_VERSION
  uint not_used_uint;
#else
  my_bool not_used_my_bool;
#endif
#ifdef SPIDER_HAS_TIME_STATUS
  MYSQL_TIME_STATUS time_status;
#else
  int time_status;
#endif
  long not_used_long;
  DBUG_ENTER("spider_db_mysql_result::fetch_table_status");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(mysql_row = mysql_fetch_row(db_result)))
  {
    DBUG_PRINT("info",("spider fetch row is null"));
    DBUG_RETURN(ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM);
  }
  if (mode == 1)
  {
    if (num_fields() != 18)
    {
      DBUG_PRINT("info",("spider field_count != 18"));
      DBUG_RETURN(ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM);
    }

    if (mysql_row[4])
      records =
        (ha_rows) my_strtoll10(mysql_row[4], (char**) NULL, &error_num);
    else
      records = (ha_rows) 0;
    DBUG_PRINT("info",
      ("spider records=%lld", records));
    if (mysql_row[5])
      mean_rec_length =
        (ulong) my_strtoll10(mysql_row[5], (char**) NULL, &error_num);
    else
      mean_rec_length = 0;
    DBUG_PRINT("info",
      ("spider mean_rec_length=%lu", mean_rec_length));
    if (mysql_row[6])
      data_file_length =
        (ulonglong) my_strtoll10(mysql_row[6], (char**) NULL, &error_num);
    else
      data_file_length = 0;
    DBUG_PRINT("info",
      ("spider data_file_length=%lld", data_file_length));
    if (mysql_row[7])
      max_data_file_length =
        (ulonglong) my_strtoll10(mysql_row[7], (char**) NULL, &error_num);
    else
      max_data_file_length = 0;
    DBUG_PRINT("info",
      ("spider max_data_file_length=%lld", max_data_file_length));
    if (mysql_row[8])
      index_file_length =
        (ulonglong) my_strtoll10(mysql_row[8], (char**) NULL, &error_num);
    else
      index_file_length = 0;
    DBUG_PRINT("info",
      ("spider index_file_length=%lld", index_file_length));
    if (mysql_row[10])
      auto_increment_value =
        (ulonglong) my_strtoll10(mysql_row[10], (char**) NULL, &error_num);
    else
      auto_increment_value = 1;
    DBUG_PRINT("info",
      ("spider auto_increment_value=%lld", auto_increment_value));
    if (mysql_row[11])
    {
#ifdef SPIDER_HAS_TIME_STATUS
      my_time_status_init(&time_status);
#endif
      str_to_datetime(mysql_row[11], strlen(mysql_row[11]), &mysql_time, 0,
        &time_status);
#ifdef MARIADB_BASE_VERSION
      create_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_uint);
#else
      create_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_my_bool);
#endif
    } else
      create_time = (time_t) 0;
#ifndef DBUG_OFF
    {
      struct tm *ts, tmp_ts;
      char buf[80];
      ts = localtime_r(&create_time, &tmp_ts);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
      DBUG_PRINT("info",("spider create_time=%s", buf));
    }
#endif
    if (mysql_row[12])
    {
#ifdef SPIDER_HAS_TIME_STATUS
      my_time_status_init(&time_status);
#endif
      str_to_datetime(mysql_row[12], strlen(mysql_row[12]), &mysql_time, 0,
        &time_status);
#ifdef MARIADB_BASE_VERSION
      update_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_uint);
#else
      update_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_my_bool);
#endif
    } else
      update_time = (time_t) 0;
#ifndef DBUG_OFF
    {
      struct tm *ts, tmp_ts;
      char buf[80];
      ts = localtime_r(&update_time, &tmp_ts);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
      DBUG_PRINT("info",("spider update_time=%s", buf));
    }
#endif
    if (mysql_row[13])
    {
#ifdef SPIDER_HAS_TIME_STATUS
      my_time_status_init(&time_status);
#endif
      str_to_datetime(mysql_row[13], strlen(mysql_row[13]), &mysql_time, 0,
        &time_status);
#ifdef MARIADB_BASE_VERSION
      check_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_uint);
#else
      check_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_my_bool);
#endif
    } else
      check_time = (time_t) 0;
#ifndef DBUG_OFF
    {
      struct tm *ts, tmp_ts;
      char buf[80];
      ts = localtime_r(&check_time, &tmp_ts);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
      DBUG_PRINT("info",("spider check_time=%s", buf));
    }
#endif
  } else {
    if (mysql_row[0])
      records =
        (ha_rows) my_strtoll10(mysql_row[0], (char**) NULL, &error_num);
    else
      records = (ha_rows) 0;
    DBUG_PRINT("info",
      ("spider records=%lld", records));
    if (mysql_row[1])
      mean_rec_length =
        (ulong) my_strtoll10(mysql_row[1], (char**) NULL, &error_num);
    else
      mean_rec_length = 0;
    DBUG_PRINT("info",
      ("spider mean_rec_length=%lu", mean_rec_length));
    if (mysql_row[2])
      data_file_length =
        (ulonglong) my_strtoll10(mysql_row[2], (char**) NULL, &error_num);
    else
      data_file_length = 0;
    DBUG_PRINT("info",
      ("spider data_file_length=%lld", data_file_length));
    if (mysql_row[3])
      max_data_file_length =
        (ulonglong) my_strtoll10(mysql_row[3], (char**) NULL, &error_num);
    else
      max_data_file_length = 0;
    DBUG_PRINT("info",
      ("spider max_data_file_length=%lld", max_data_file_length));
    if (mysql_row[4])
      index_file_length =
        (ulonglong) my_strtoll10(mysql_row[4], (char**) NULL, &error_num);
    else
      index_file_length = 0;
    DBUG_PRINT("info",
      ("spider index_file_length=%lld", index_file_length));
    if (mysql_row[5])
      auto_increment_value =
        (ulonglong) my_strtoll10(mysql_row[5], (char**) NULL, &error_num);
    else
      auto_increment_value = 1;
    DBUG_PRINT("info",
      ("spider auto_increment_value=%lld", auto_increment_value));
    if (mysql_row[6])
    {
#ifdef SPIDER_HAS_TIME_STATUS
      my_time_status_init(&time_status);
#endif
      str_to_datetime(mysql_row[6], strlen(mysql_row[6]), &mysql_time, 0,
        &time_status);
#ifdef MARIADB_BASE_VERSION
      create_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_uint);
#else
      create_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_my_bool);
#endif
    } else
      create_time = (time_t) 0;
#ifndef DBUG_OFF
    {
      struct tm *ts, tmp_ts;
      char buf[80];
      ts = localtime_r(&create_time, &tmp_ts);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
      DBUG_PRINT("info",("spider create_time=%s", buf));
    }
#endif
    if (mysql_row[7])
    {
#ifdef SPIDER_HAS_TIME_STATUS
      my_time_status_init(&time_status);
#endif
      str_to_datetime(mysql_row[7], strlen(mysql_row[7]), &mysql_time, 0,
        &time_status);
#ifdef MARIADB_BASE_VERSION
      update_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_uint);
#else
      update_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_my_bool);
#endif
    } else
      update_time = (time_t) 0;
#ifndef DBUG_OFF
    {
      struct tm *ts, tmp_ts;
      char buf[80];
      ts = localtime_r(&update_time, &tmp_ts);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
      DBUG_PRINT("info",("spider update_time=%s", buf));
    }
#endif
    if (mysql_row[8])
    {
#ifdef SPIDER_HAS_TIME_STATUS
      my_time_status_init(&time_status);
#endif
      str_to_datetime(mysql_row[8], strlen(mysql_row[8]), &mysql_time, 0,
        &time_status);
#ifdef MARIADB_BASE_VERSION
      check_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_uint);
#else
      check_time = (time_t) my_system_gmt_sec(&mysql_time,
        &not_used_long, &not_used_my_bool);
#endif
    } else
      check_time = (time_t) 0;
#ifndef DBUG_OFF
    {
      struct tm *ts, tmp_ts;
      char buf[80];
      ts = localtime_r(&check_time, &tmp_ts);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ts);
      DBUG_PRINT("info",("spider check_time=%s", buf));
    }
#endif
  }
  DBUG_RETURN(0);
}

int spider_db_mysql_result::fetch_table_records(
  int mode,
  ha_rows &records
) {
  int error_num;
  MYSQL_ROW mysql_row;
  DBUG_ENTER("spider_db_mysql_result::fetch_table_records");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(mysql_row = mysql_fetch_row(db_result)))
  {
    DBUG_PRINT("info",("spider fetch row is null"));
    DBUG_RETURN(ER_QUERY_ON_FOREIGN_DATA_SOURCE);
  }
  if (mode == 1)
  {
    if (mysql_row[0])
    {
      records =
        (ha_rows) my_strtoll10(mysql_row[0], (char**) NULL, &error_num);
    } else
      records = (ha_rows) 0;
    DBUG_PRINT("info",
      ("spider records=%lld", records));
  } else {
    if (num_fields() != 10)
    {
      DBUG_RETURN(ER_QUERY_ON_FOREIGN_DATA_SOURCE);
    }

    if (mysql_row[8])
    {
      records =
        (ha_rows) my_strtoll10(mysql_row[8], (char**) NULL, &error_num);
    } else
      records = 0;
  }
  DBUG_RETURN(0);
}

int spider_db_mysql_result::fetch_table_cardinality(
  int mode,
  TABLE *table,
  longlong *cardinality,
  uchar *cardinality_upd,
  int bitmap_size
) {
  int error_num;
  MYSQL_ROW mysql_row;
  Field *field;
  DBUG_ENTER("spider_db_mysql_result::fetch_table_cardinality");
  DBUG_PRINT("info",("spider this=%p", this));
  memset((uchar *) cardinality_upd, 0, sizeof(uchar) * bitmap_size);
  if (!(mysql_row = mysql_fetch_row(db_result)))
  {
    DBUG_PRINT("info",("spider fetch row is null"));
    /* no index */
    DBUG_RETURN(0);
  }
  if (mode == 1)
  {
    uint num_fields = this->num_fields();
    if (num_fields < 12 || num_fields > 13)
    {
      DBUG_PRINT("info",("spider num_fields < 12 || num_fields > 13"));
      DBUG_RETURN(ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM);
    }

    while (mysql_row)
    {
      if (
        mysql_row[4] &&
        mysql_row[6] &&
        (field = find_field_in_table_sef(table, mysql_row[4]))
      ) {
        if ((cardinality[field->field_index] =
          (longlong) my_strtoll10(mysql_row[6], (char**) NULL, &error_num))
        <= 0)
          cardinality[field->field_index] = 1;
        spider_set_bit(cardinality_upd, field->field_index);
        DBUG_PRINT("info",
          ("spider col_name=%s", mysql_row[4]));
        DBUG_PRINT("info",
          ("spider cardinality=%lld",
          cardinality[field->field_index]));
      } else if (mysql_row[4])
      {
        DBUG_PRINT("info",
          ("spider skip col_name=%s", mysql_row[4]));
      } else {
        DBUG_RETURN(ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM);
      }
      mysql_row = mysql_fetch_row(db_result);
    }
  } else {
    while (mysql_row)
    {
      if (
        mysql_row[0] &&
        mysql_row[1] &&
        (field = find_field_in_table_sef(table, mysql_row[0]))
      ) {
        if ((cardinality[field->field_index] =
          (longlong) my_strtoll10(mysql_row[1], (char**) NULL, &error_num))
        <= 0)
          cardinality[field->field_index] = 1;
        spider_set_bit(cardinality_upd, field->field_index);
        DBUG_PRINT("info",
          ("spider col_name=%s", mysql_row[0]));
        DBUG_PRINT("info",
          ("spider cardinality=%lld",
          cardinality[field->field_index]));
      } else if (mysql_row[0])
      {
        DBUG_PRINT("info",
          ("spider skip col_name=%s", mysql_row[0]));
      } else {
        DBUG_RETURN(ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM);
      }
      mysql_row = mysql_fetch_row(db_result);
    }
  }
  DBUG_RETURN(0);
}

int spider_db_mysql_result::fetch_table_mon_status(
  int &status
) {
  MYSQL_ROW mysql_row;
  DBUG_ENTER("spider_db_mysql_result::fetch_table_mon_status");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(mysql_row = mysql_fetch_row(db_result)))
  {
    DBUG_PRINT("info",("spider fetch row is null"));
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if (num_fields() != 1)
  {
    DBUG_PRINT("info",("spider num_fields != 1"));
    my_printf_error(ER_SPIDER_UNKNOWN_NUM, ER_SPIDER_UNKNOWN_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
  }
  if (mysql_row[0])
    status = atoi(mysql_row[0]);
  else
    status = SPIDER_LINK_MON_OK;
  DBUG_PRINT("info", ("spider status=%d", status));
  DBUG_RETURN(0);
}

longlong spider_db_mysql_result::num_rows()
{
  DBUG_ENTER("spider_db_mysql_result::num_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((longlong) mysql_num_rows(db_result));
}

uint spider_db_mysql_result::num_fields()
{
  DBUG_ENTER("spider_db_mysql_result::num_fields");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(mysql_num_fields(db_result));
}

void spider_db_mysql_result::move_to_pos(
  longlong pos
) {
  DBUG_ENTER("spider_db_mysql_result::move_to_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider pos=%lld", pos));
/*
  DBUG_ASSERT(first_row);
*/
  db_result->data_cursor = first_row + pos;
  DBUG_VOID_RETURN;
}

int spider_db_mysql_result::get_errno()
{
  DBUG_ENTER("spider_db_mysql_result::get_errno");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider store_error_num=%d", store_error_num));
  DBUG_RETURN(store_error_num);
}

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
int spider_db_mysql_result::fetch_columns_for_discover_table_structure(
  spider_string *str,
  CHARSET_INFO *access_charset
) {
  MYSQL_ROW mysql_row;
  DBUG_ENTER("spider_db_mysql_result::fetch_columns_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(mysql_row = mysql_fetch_row(db_result)))
  {
    DBUG_PRINT("info",("spider fetch row is null"));
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if (num_fields() != 7)
  {
    DBUG_PRINT("info",("spider num_fields != 7"));
    my_printf_error(ER_SPIDER_UNKNOWN_NUM, ER_SPIDER_UNKNOWN_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
  }
  do {
    if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    if (str->append(mysql_row[0], strlen(mysql_row[0]), access_charset))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_SPACE_LEN))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    if (str->append(mysql_row[5], strlen(mysql_row[5]), access_charset))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    if (mysql_row[3])
    {
      if (str->reserve(SPIDER_SQL_CHARACTER_SET_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_CHARACTER_SET_STR, SPIDER_SQL_CHARACTER_SET_LEN);
      str->q_append(mysql_row[3], strlen(mysql_row[3]));
    }
    if (mysql_row[4])
    {
      if (str->reserve(SPIDER_SQL_COLLATE_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_COLLATE_STR, SPIDER_SQL_COLLATE_LEN);
      str->q_append(mysql_row[4], strlen(mysql_row[4]));
    }
    if (!strcmp(mysql_row[2], "NO"))
    {
      if (str->reserve(SPIDER_SQL_NOT_NULL_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_NOT_NULL_STR, SPIDER_SQL_NOT_NULL_LEN);
      if (mysql_row[1])
      {
        if (str->reserve(SPIDER_SQL_DEFAULT_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_DEFAULT_STR, SPIDER_SQL_DEFAULT_LEN);
        if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
        if (str->append(mysql_row[1], strlen(mysql_row[1]), access_charset))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      }
    } else {
      if (str->reserve(SPIDER_SQL_DEFAULT_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_DEFAULT_STR, SPIDER_SQL_DEFAULT_LEN);
      if (mysql_row[1])
      {
        if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
        if (str->append(mysql_row[1], strlen(mysql_row[1]), access_charset))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      } else {
        if (str->reserve(SPIDER_SQL_NULL_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
      }
    }
    if (mysql_row[6] && !strcmp(mysql_row[6], "auto_increment"))
    {
      if (str->reserve(SPIDER_SQL_AUTO_INCREMENT_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_AUTO_INCREMENT_STR, SPIDER_SQL_AUTO_INCREMENT_LEN);
    }
    if (str->reserve(SPIDER_SQL_COMMA_LEN))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  } while ((mysql_row = mysql_fetch_row(db_result)));
  DBUG_RETURN(0);
}

int spider_db_mysql_result::fetch_index_for_discover_table_structure(
  spider_string *str,
  CHARSET_INFO *access_charset
) {
  MYSQL_ROW mysql_row;
  DBUG_ENTER("spider_db_mysql_result::fetch_index_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(mysql_row = mysql_fetch_row(db_result)))
  {
    DBUG_PRINT("info",("spider fetch row is null"));
    if (mysql_errno(db_result->handle))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    DBUG_RETURN(0);
  }
  if (num_fields() != 13)
  {
    DBUG_PRINT("info",("spider num_fields != 13"));
    my_printf_error(ER_SPIDER_UNKNOWN_NUM, ER_SPIDER_UNKNOWN_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
  }
  bool first = TRUE;
  bool without_size = FALSE;
  bool using_hash = FALSE;
  do {
    if (!strcmp(mysql_row[3], "1"))
    {
      without_size = FALSE;
      if (first)
      {
        first = FALSE;
      } else {
        if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_COMMA_LEN +
          (using_hash ? SPIDER_SQL_USING_HASH_LEN : 0)))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
        if (using_hash)
          str->q_append(SPIDER_SQL_USING_HASH_STR, SPIDER_SQL_USING_HASH_LEN);
        str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      }
      /* new index */
      if (!strcmp(mysql_row[2], SPIDER_DB_PK_NAME_STR))
      {
        /* primary key */
        if (str->reserve(SPIDER_DB_PK_NAME_LEN + SPIDER_SQL_SPACE_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_DB_PK_NAME_STR, SPIDER_DB_PK_NAME_LEN);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      } else if (!strcmp(mysql_row[1], "0"))
      {
        /* unique key */
        if (str->reserve(SPIDER_DB_UNIQUE_NAME_LEN + SPIDER_SQL_SPACE_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_DB_UNIQUE_NAME_STR, SPIDER_DB_UNIQUE_NAME_LEN);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      } else if (mysql_row[10] && !strcmp(mysql_row[10], "FULLTEXT"))
      {
        /* fulltext key */
        if (str->reserve(SPIDER_SQL_FULLTEXT_LEN + SPIDER_SQL_SPACE_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_FULLTEXT_STR, SPIDER_SQL_FULLTEXT_LEN);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      } else if (mysql_row[10] && !strcmp(mysql_row[10], "SPATIAL"))
      {
        /* spatial key */
        without_size = TRUE;
        if (str->reserve(SPIDER_SQL_SPATIAL_LEN + SPIDER_SQL_SPACE_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_SPATIAL_STR, SPIDER_SQL_SPATIAL_LEN);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      }
      if (str->reserve(SPIDER_DB_KEY_NAME_LEN + SPIDER_SQL_SPACE_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_DB_KEY_NAME_STR, SPIDER_DB_KEY_NAME_LEN);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      if (strcmp(mysql_row[2], SPIDER_DB_PK_NAME_STR))
      {
        if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
        if (str->append(mysql_row[2], strlen(mysql_row[2]), access_charset))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
      }
      if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
      if (str->append(mysql_row[4], strlen(mysql_row[4]), access_charset))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
      if (mysql_row[7] && !without_size)
      {
        if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
        if (str->append(mysql_row[7], strlen(mysql_row[7]), access_charset))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
      }
    } else {
      if (str->reserve(SPIDER_SQL_COMMA_LEN + SPIDER_SQL_NAME_QUOTE_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
      if (str->append(mysql_row[4], strlen(mysql_row[4]), access_charset))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
      if (mysql_row[7] && !without_size)
      {
        if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
        if (str->append(mysql_row[7], strlen(mysql_row[7]), access_charset))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
      }
    }
    if (mysql_row[10] && !strcmp(mysql_row[10], "HASH"))
      using_hash = TRUE;
    else
      using_hash = FALSE;
  } while ((mysql_row = mysql_fetch_row(db_result)));
  if (!first)
  {
    if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_COMMA_LEN +
      (using_hash ? SPIDER_SQL_USING_HASH_LEN : 0)))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
    if (using_hash)
      str->q_append(SPIDER_SQL_USING_HASH_STR, SPIDER_SQL_USING_HASH_LEN);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  DBUG_RETURN(0);
}

int spider_db_mysql_result::fetch_table_for_discover_table_structure(
  spider_string *str,
  SPIDER_SHARE *spider_share,
  CHARSET_INFO *access_charset
) {
  MYSQL_ROW mysql_row;
  DBUG_ENTER("spider_db_mysql_result::fetch_table_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(mysql_row = mysql_fetch_row(db_result)))
  {
    DBUG_PRINT("info",("spider fetch row is null"));
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if (num_fields() != 18)
  {
    DBUG_PRINT("info",("spider num_fields != 18"));
    my_printf_error(ER_SPIDER_UNKNOWN_NUM, ER_SPIDER_UNKNOWN_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
  }
  if (!mysql_row[14])
  {
    DBUG_PRINT("info",("spider mysql_row[14] is null"));
    my_printf_error(ER_SPIDER_UNKNOWN_NUM, ER_SPIDER_UNKNOWN_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
  }
  DBUG_PRINT("info",("spider mysql_row[14]=%s", mysql_row[14]));
  if (!spider_share->table_share->table_charset)
  {
    spider_share->table_share->table_charset = get_charset_by_name(mysql_row[14], MYF(MY_WME));
  }
  DBUG_RETURN(0);
}
#endif

spider_db_mysql::spider_db_mysql(
  SPIDER_CONN *conn
) : spider_db_conn(conn), lock_table_hash_inited(FALSE),
  handler_open_array_inited(FALSE)
{
  DBUG_ENTER("spider_db_mysql::spider_db_mysql");
  DBUG_PRINT("info",("spider this=%p", this));
  db_conn = NULL;
  DBUG_VOID_RETURN;
}

spider_db_mysql::~spider_db_mysql()
{
  DBUG_ENTER("spider_db_mysql::~spider_db_mysql");
  DBUG_PRINT("info",("spider this=%p", this));
  if (handler_open_array_inited)
  {
    reset_opened_handler();
    spider_free_mem_calc(spider_current_trx,
      handler_open_array_id,
      handler_open_array.max_element *
      handler_open_array.size_of_element);
    delete_dynamic(&handler_open_array);
  }
  if (lock_table_hash_inited)
  {
    spider_free_mem_calc(spider_current_trx,
      lock_table_hash_id,
      lock_table_hash.array.max_element *
      lock_table_hash.array.size_of_element);
    my_hash_free(&lock_table_hash);
  }
  DBUG_VOID_RETURN;
}

int spider_db_mysql::init()
{
  DBUG_ENTER("spider_db_mysql::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    my_hash_init(&lock_table_hash, spd_charset_utf8_bin, 32, 0, 0,
      (my_hash_get_key) spider_link_get_key, 0, 0)
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  spider_alloc_calc_mem_init(lock_table_hash, 140);
  spider_alloc_calc_mem(spider_current_trx,
    lock_table_hash,
    lock_table_hash.array.max_element *
    lock_table_hash.array.size_of_element);
  lock_table_hash_inited = TRUE;

  if (
    SPD_INIT_DYNAMIC_ARRAY2(&handler_open_array,
      sizeof(SPIDER_LINK_FOR_HASH *), NULL, 16, 16, MYF(MY_WME))
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  spider_alloc_calc_mem_init(handler_open_array, 162);
  spider_alloc_calc_mem(spider_current_trx,
    handler_open_array,
    handler_open_array.max_element *
    handler_open_array.size_of_element);
  handler_open_array_inited = TRUE;
  DBUG_RETURN(0);
}

bool spider_db_mysql::is_connected()
{
  DBUG_ENTER("spider_db_mysql::is_connected");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(db_conn);
}

void spider_db_mysql::bg_connect()
{
  DBUG_ENTER("spider_db_mysql::bg_connect");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

int spider_db_mysql::connect(
  char *tgt_host,
  char *tgt_username,
  char *tgt_password,
  long tgt_port,
  char *tgt_socket,
  char *server_name,
  int connect_retry_count,
  longlong connect_retry_interval
) {
  int error_num;
  my_bool connect_mutex = spider_param_connect_mutex();
  DBUG_ENTER("spider_db_mysql::connect");
  DBUG_PRINT("info",("spider this=%p", this));
  while (TRUE)
  {
    THD *thd = current_thd;
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

    if (!db_conn)
    {
      if (!(db_conn = mysql_init(NULL)))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }

    mysql_options(db_conn, MYSQL_OPT_READ_TIMEOUT,
      &conn->net_read_timeout);
    mysql_options(db_conn, MYSQL_OPT_WRITE_TIMEOUT,
      &conn->net_write_timeout);
    mysql_options(db_conn, MYSQL_OPT_CONNECT_TIMEOUT,
      &conn->connect_timeout);
    mysql_options(db_conn, MYSQL_OPT_USE_REMOTE_CONNECTION,
      NULL);

    if (
      conn->tgt_ssl_ca_length |
      conn->tgt_ssl_capath_length |
      conn->tgt_ssl_cert_length |
      conn->tgt_ssl_key_length
    ) {
      mysql_ssl_set(db_conn, conn->tgt_ssl_key, conn->tgt_ssl_cert,
        conn->tgt_ssl_ca, conn->tgt_ssl_capath, conn->tgt_ssl_cipher);
      if (conn->tgt_ssl_vsc)
      {
        my_bool verify_flg = TRUE;
        mysql_options(db_conn, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
          &verify_flg);
      }
    }

    if (conn->tgt_default_file)
    {
      DBUG_PRINT("info",("spider tgt_default_file=%s",
        conn->tgt_default_file));
      mysql_options(db_conn, MYSQL_READ_DEFAULT_FILE,
        conn->tgt_default_file);
    }
    if (conn->tgt_default_group)
    {
      DBUG_PRINT("info",("spider tgt_default_group=%s",
        conn->tgt_default_group));
      mysql_options(db_conn, MYSQL_READ_DEFAULT_GROUP,
        conn->tgt_default_group);
    }

    if (connect_mutex)
      pthread_mutex_lock(&spider_open_conn_mutex);
    /* tgt_db not use */
    if (
      !spider_param_dry_access() &&
      !mysql_real_connect(
        db_conn,
        tgt_host,
        tgt_username,
        tgt_password,
        NULL,
        tgt_port,
        tgt_socket,
        CLIENT_MULTI_STATEMENTS
      )
    ) {
      if (connect_mutex)
        pthread_mutex_unlock(&spider_open_conn_mutex);
      error_num = mysql_errno(db_conn);
      disconnect();
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
      if (
        (
          error_num != CR_CONN_HOST_ERROR &&
          error_num != CR_CONNECTION_ERROR
        ) ||
        !connect_retry_count
      ) {
        if (error_num == ER_CON_COUNT_ERROR)
        {
          *conn->need_mon = 0;
          my_error(ER_CON_COUNT_ERROR, MYF(0));
          DBUG_RETURN(ER_CON_COUNT_ERROR);
        }
        *conn->need_mon = ER_CONNECT_TO_FOREIGN_DATA_SOURCE;
        my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
          server_name ? server_name : tgt_host);
        DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);
      }
      connect_retry_count--;
      my_sleep((ulong) connect_retry_interval);
    } else {
      if (connect_mutex)
        pthread_mutex_unlock(&spider_open_conn_mutex);
      break;
    }
  }
  DBUG_RETURN(0);
}

int spider_db_mysql::ping(
) {
  DBUG_ENTER("spider_db_mysql::ping");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_dry_access())
    DBUG_RETURN(0);
  DBUG_RETURN(simple_command(db_conn, COM_PING, 0, 0, 0));
}

void spider_db_mysql::bg_disconnect()
{
  DBUG_ENTER("spider_db_mysql::bg_disconnect");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

void spider_db_mysql::disconnect()
{
  DBUG_ENTER("spider_db_mysql::disconnect");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider db_conn=%p", db_conn));
  if (db_conn)
  {
    mysql_close(db_conn);
    db_conn = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_db_mysql::set_net_timeout()
{
  DBUG_ENTER("spider_db_mysql::set_net_timeout");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider conn=%p", conn));
  my_net_set_read_timeout(&db_conn->net, conn->net_read_timeout);
  my_net_set_write_timeout(&db_conn->net, conn->net_write_timeout);
  DBUG_RETURN(0);
}

int spider_db_mysql::exec_query(
  const char *query,
  uint length,
  int quick_mode
) {
  int error_num = 0;
  uint log_result_errors = spider_param_log_result_errors();
  DBUG_ENTER("spider_db_mysql::exec_query");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_general_log())
  {
    const char *tgt_str = conn->tgt_host;
    uint32 tgt_len = conn->tgt_host_length;
    spider_string tmp_query_str;
    tmp_query_str.init_calc_mem(230);
    if (tmp_query_str.reserve(
      length + conn->tgt_wrapper_length +
      tgt_len + (SPIDER_SQL_SPACE_LEN * 2)))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    tmp_query_str.q_append(conn->tgt_wrapper, conn->tgt_wrapper_length);
    tmp_query_str.q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    tmp_query_str.q_append(tgt_str, tgt_len);
    tmp_query_str.q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    tmp_query_str.q_append(query, length);
    general_log_write(current_thd, COM_QUERY, tmp_query_str.ptr(),
      tmp_query_str.length());
  }
  if (!spider_param_dry_access())
  {
    error_num = mysql_real_query(db_conn, query, length);
  }
  if (
    (error_num && log_result_errors >= 1) ||
    (log_result_errors >= 2 && db_conn->warning_count > 0) ||
    (log_result_errors >= 4)
  ) {
    THD *thd = current_thd;
    uint log_result_error_with_sql = spider_param_log_result_error_with_sql();
    if (log_result_error_with_sql)
    {
      time_t cur_time = (time_t) time((time_t*) 0);
      struct tm lt;
      struct tm *l_time = localtime_r(&cur_time, &lt);
      spider_string tmp_query_str;
      tmp_query_str.init_calc_mem(243);
      uint query_length = thd->query_length();
      if ((log_result_error_with_sql & 2) && query_length)
      {
        Security_context *security_ctx = thd->security_ctx;
        tmp_query_str.length(0);
        if (tmp_query_str.reserve(query_length + 1))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        tmp_query_str.q_append(thd->query(), query_length);
        fprintf(stderr, "%04d%02d%02d %02d:%02d:%02d [RECV SPIDER SQL] "
          "from [%s][%s] to %ld:  "
          "sql: %s\n",
          l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
          l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
          security_ctx->user ? security_ctx->user : "system user",
          security_ctx->host_or_ip,
          (ulong) thd->thread_id,
          tmp_query_str.c_ptr_safe());
      }
      if (log_result_error_with_sql & 1)
      {
        tmp_query_str.length(0);
        if (tmp_query_str.reserve(length + 1))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        tmp_query_str.q_append(query, length);
        fprintf(stderr, "%04d%02d%02d %02d:%02d:%02d [SEND SPIDER SQL] "
          "from %ld to [%s] %ld:  "
          "sql: %s\n",
          l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
          l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
          (ulong) thd->thread_id, conn->tgt_host, (ulong) db_conn->thread_id,
          tmp_query_str.c_ptr_safe());
      }
    }
    if (log_result_errors >= 2 && db_conn->warning_count > 0)
    {
      time_t cur_time = (time_t) time((time_t*) 0);
      struct tm lt;
      struct tm *l_time = localtime_r(&cur_time, &lt);
      fprintf(stderr, "%04d%02d%02d %02d:%02d:%02d [WARN SPIDER RESULT] "
        "from [%s] %ld to %ld:  "
        "affected_rows: %llu  id: %llu  status: %u  warning_count: %u\n",
        l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
        l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
        conn->tgt_host, (ulong) db_conn->thread_id, (ulong) thd->thread_id,
        db_conn->affected_rows, db_conn->insert_id,
        db_conn->server_status, db_conn->warning_count);
      if (spider_param_log_result_errors() >= 3)
        print_warnings(l_time);
    } else if (log_result_errors >= 4)
    {
      time_t cur_time = (time_t) time((time_t*) 0);
      struct tm lt;
      struct tm *l_time = localtime_r(&cur_time, &lt);
      fprintf(stderr, "%04d%02d%02d %02d:%02d:%02d [INFO SPIDER RESULT] "
        "from [%s] %ld to %ld:  "
        "affected_rows: %llu  id: %llu  status: %u  warning_count: %u\n",
        l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
        l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
        conn->tgt_host, (ulong) db_conn->thread_id, (ulong) thd->thread_id,
        db_conn->affected_rows, db_conn->insert_id,
        db_conn->server_status, db_conn->warning_count);
    }
  }
  DBUG_RETURN(error_num);
}

int spider_db_mysql::get_errno()
{
  DBUG_ENTER("spider_db_mysql::get_errno");
  DBUG_PRINT("info",("spider this=%p", this));
  stored_error = mysql_errno(db_conn);
  DBUG_PRINT("info",("spider stored_error=%d", stored_error));
  DBUG_RETURN(stored_error);
}

const char *spider_db_mysql::get_error()
{
  const char *error_ptr;
  DBUG_ENTER("spider_db_mysql::get_error");
  DBUG_PRINT("info",("spider this=%p", this));
  error_ptr = mysql_error(db_conn);
  DBUG_PRINT("info",("spider error=%s", error_ptr));
  DBUG_RETURN(error_ptr);
}

bool spider_db_mysql::is_server_gone_error(
  int error_num
) {
  bool server_gone;
  DBUG_ENTER("spider_db_mysql::is_server_gone_error");
  DBUG_PRINT("info",("spider this=%p", this));
  server_gone =
    (error_num == CR_SERVER_GONE_ERROR || error_num == CR_SERVER_LOST);
  DBUG_PRINT("info",("spider server_gone=%s", server_gone ? "TRUE" : "FALSE"));
  DBUG_RETURN(server_gone);
}

bool spider_db_mysql::is_dup_entry_error(
  int error_num
) {
  bool dup_entry;
  DBUG_ENTER("spider_db_mysql::is_dup_entry_error");
  DBUG_PRINT("info",("spider this=%p", this));
  dup_entry =
    (
      error_num == ER_DUP_ENTRY ||
      error_num == ER_DUP_KEY ||
      error_num == HA_ERR_FOUND_DUPP_KEY
    );
  DBUG_PRINT("info",("spider dup_entry=%s", dup_entry ? "TRUE" : "FALSE"));
  DBUG_RETURN(dup_entry);
}

bool spider_db_mysql::is_xa_nota_error(
  int error_num
) {
  bool xa_nota;
  DBUG_ENTER("spider_db_mysql::is_xa_nota_error");
  DBUG_PRINT("info",("spider this=%p", this));
  xa_nota =
    (
      error_num == ER_XAER_NOTA ||
      error_num == ER_XA_RBTIMEOUT ||
      error_num == ER_XA_RBDEADLOCK
    );
  DBUG_PRINT("info",("spider xa_nota=%s", xa_nota ? "TRUE" : "FALSE"));
  DBUG_RETURN(xa_nota);
}

void spider_db_mysql::print_warnings(
  struct tm *l_time
) {
  DBUG_ENTER("spider_db_mysql::print_warnings");
  DBUG_PRINT("info",("spider this=%p", this));
  if (db_conn->status == MYSQL_STATUS_READY)
  {
#if MYSQL_VERSION_ID < 50500
    if (!(db_conn->last_used_con->server_status & SERVER_MORE_RESULTS_EXISTS))
#else
    if (!(db_conn->server_status & SERVER_MORE_RESULTS_EXISTS))
#endif
    {
      if (
        spider_param_dry_access() ||
        !mysql_real_query(db_conn, SPIDER_SQL_SHOW_WARNINGS_STR,
          SPIDER_SQL_SHOW_WARNINGS_LEN)
      ) {
        MYSQL_RES *res = NULL;
        MYSQL_ROW row = NULL;
        uint num_fields;
        if (
          spider_param_dry_access() ||
          !(res = mysql_store_result(db_conn)) ||
          !(row = mysql_fetch_row(res))
        ) {
          if (mysql_errno(db_conn))
          {
            if (res)
              mysql_free_result(res);
            DBUG_VOID_RETURN;
          }
          /* no record is ok */
        }
        num_fields = mysql_num_fields(res);
        if (num_fields != 3)
        {
          mysql_free_result(res);
          DBUG_VOID_RETURN;
        }
        while (row)
        {
          fprintf(stderr, "%04d%02d%02d %02d:%02d:%02d [WARN SPIDER RESULT] "
            "from [%s] %ld to %ld: %s %s %s\n",
            l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
            l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
            conn->tgt_host, (ulong) db_conn->thread_id,
            (ulong) current_thd->thread_id, row[0], row[1], row[2]);
          row = mysql_fetch_row(res);
        }
        if (res)
          mysql_free_result(res);
      }
    }
  }
  DBUG_VOID_RETURN;
}

spider_db_result *spider_db_mysql::store_result(
  spider_db_result_buffer **spider_res_buf,
  st_spider_db_request_key *request_key,
  int *error_num
) {
  spider_db_mysql_result *result;
  DBUG_ENTER("spider_db_mysql::store_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(!spider_res_buf);
  if ((result = new spider_db_mysql_result()))
  {
    *error_num = 0;
    if (
      spider_param_dry_access() ||
      !(result->db_result = mysql_store_result(db_conn))
    ) {
      delete result;
      result = NULL;
    } else {
      result->first_row = result->db_result->data_cursor;
      DBUG_PRINT("info",("spider result->first_row=%p", result->first_row));
    }
  } else {
    *error_num = HA_ERR_OUT_OF_MEM;
  }
  DBUG_RETURN(result);
}

spider_db_result *spider_db_mysql::use_result(
  st_spider_db_request_key *request_key,
  int *error_num
) {
  spider_db_mysql_result *result;
  DBUG_ENTER("spider_db_mysql::use_result");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((result = new spider_db_mysql_result()))
  {
    *error_num = 0;
    if (
      spider_param_dry_access() ||
      !(result->db_result = db_conn->methods->use_result(db_conn))
    ) {
      delete result;
      result = NULL;
    } else {
      result->first_row = NULL;
    }
  } else {
    *error_num = HA_ERR_OUT_OF_MEM;
  }
  DBUG_RETURN(result);
}

int spider_db_mysql::next_result()
{
  int status;
  DBUG_ENTER("spider_db_mysql::next_result");
  DBUG_PRINT("info",("spider this=%p", this));
  if (db_conn->status != MYSQL_STATUS_READY)
  {
    my_message(ER_SPIDER_UNKNOWN_NUM, ER_SPIDER_UNKNOWN_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
  }

  db_conn->net.last_errno = 0;
  db_conn->net.last_error[0] = '\0';
  strmov(db_conn->net.sqlstate, "00000");
  db_conn->affected_rows = ~(my_ulonglong) 0;

#if MYSQL_VERSION_ID < 50500
  if (db_conn->last_used_con->server_status & SERVER_MORE_RESULTS_EXISTS)
#else
  if (db_conn->server_status & SERVER_MORE_RESULTS_EXISTS)
#endif
  {
    if ((status = db_conn->methods->read_query_result(db_conn)) > 0)
      DBUG_RETURN(spider_db_errorno(conn));
    DBUG_RETURN(status);
  }
  DBUG_RETURN(-1);
}

uint spider_db_mysql::affected_rows()
{
  MYSQL *last_used_con;
  DBUG_ENTER("spider_db_mysql::affected_rows");
  DBUG_PRINT("info",("spider this=%p", this));
#if MYSQL_VERSION_ID < 50500
  last_used_con = db_conn->last_used_con;
#else
  last_used_con = db_conn;
#endif
  DBUG_RETURN((uint) last_used_con->affected_rows);
}

ulonglong spider_db_mysql::last_insert_id()
{
  MYSQL *last_used_con;
  DBUG_ENTER("spider_db_mysql::last_insert_id");
  DBUG_PRINT("info",("spider this=%p", this));
#if MYSQL_VERSION_ID < 50500
  last_used_con = db_conn->last_used_con;
#else
  last_used_con = db_conn;
#endif
  DBUG_RETURN((uint) last_used_con->insert_id);
}

int spider_db_mysql::set_character_set(
  const char *csname
) {
  DBUG_ENTER("spider_db_mysql::set_character_set");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_dry_access())
    DBUG_RETURN(0);
  DBUG_RETURN(mysql_set_character_set(db_conn, csname));
}

int spider_db_mysql::select_db(
  const char *dbname
) {
  DBUG_ENTER("spider_db_mysql::select_db");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_dry_access())
    DBUG_RETURN(0);
  DBUG_RETURN(mysql_select_db(db_conn, dbname));
}

int spider_db_mysql::consistent_snapshot(
  int *need_mon
) {
  DBUG_ENTER("spider_db_mysql::consistent_snapshot");
  DBUG_PRINT("info",("spider this=%p", this));
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    SPIDER_SQL_START_CONSISTENT_SNAPSHOT_STR,
    SPIDER_SQL_START_CONSISTENT_SNAPSHOT_LEN,
    -1,
    need_mon)
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    DBUG_RETURN(spider_db_errorno(conn));
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

bool spider_db_mysql::trx_start_in_bulk_sql()
{
  DBUG_ENTER("spider_db_mysql::trx_start_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

int spider_db_mysql::start_transaction(
  int *need_mon
) {
  DBUG_ENTER("spider_db_mysql::start_transaction");
  DBUG_PRINT("info",("spider this=%p", this));
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  if (spider_db_query(
    conn,
    SPIDER_SQL_START_TRANSACTION_STR,
    SPIDER_SQL_START_TRANSACTION_LEN,
    -1,
    need_mon)
  ) {
    DBUG_RETURN(spider_db_errorno(conn));
  }
  DBUG_RETURN(0);
}

int spider_db_mysql::commit(
  int *need_mon
) {
  DBUG_ENTER("spider_db_mysql::commit");
  DBUG_PRINT("info",("spider this=%p", this));
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    SPIDER_SQL_COMMIT_STR,
    SPIDER_SQL_COMMIT_LEN,
    -1,
    need_mon)
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    DBUG_RETURN(spider_db_errorno(conn));
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_db_mysql::rollback(
  int *need_mon
) {
  bool is_error;
  int error_num;
  DBUG_ENTER("spider_db_mysql::rollback");
  DBUG_PRINT("info",("spider this=%p", this));
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    SPIDER_SQL_ROLLBACK_STR,
    SPIDER_SQL_ROLLBACK_LEN,
    -1,
    need_mon)
  ) {
    is_error = conn->thd->is_error();
    error_num = spider_db_errorno(conn);
    if (
      error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
      !is_error
    )
      conn->thd->clear_error();
    else {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_RETURN(error_num);
    }
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_db_mysql::xa_start(
  XID *xid,
  int *need_mon
) {
  DBUG_ENTER("spider_db_mysql::xa_start");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

bool spider_db_mysql::xa_start_in_bulk_sql()
{
  DBUG_ENTER("spider_db_mysql::xa_start_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

int spider_db_mysql::xa_end(
  XID *xid,
  int *need_mon
) {
  char sql_buf[SPIDER_SQL_XA_END_LEN + XIDDATASIZE + sizeof(long) + 9];
  spider_string sql_str(sql_buf, sizeof(sql_buf), &my_charset_bin);
  DBUG_ENTER("spider_db_mysql::xa_end");
  DBUG_PRINT("info",("spider this=%p", this));
  sql_str.init_calc_mem(108);

  sql_str.length(0);
  sql_str.q_append(SPIDER_SQL_XA_END_STR, SPIDER_SQL_XA_END_LEN);
  spider_db_append_xid_str(&sql_str, xid);
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    sql_str.ptr(),
    sql_str.length(),
    -1,
    need_mon)
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    DBUG_RETURN(spider_db_errorno(conn));
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_db_mysql::xa_prepare(
  XID *xid,
  int *need_mon
) {
  char sql_buf[SPIDER_SQL_XA_PREPARE_LEN + XIDDATASIZE + sizeof(long) + 9];
  spider_string sql_str(sql_buf, sizeof(sql_buf), &my_charset_bin);
  DBUG_ENTER("spider_db_mysql::xa_prepare");
  DBUG_PRINT("info",("spider this=%p", this));
  sql_str.init_calc_mem(109);

  sql_str.length(0);
  sql_str.q_append(SPIDER_SQL_XA_PREPARE_STR, SPIDER_SQL_XA_PREPARE_LEN);
  spider_db_append_xid_str(&sql_str, xid);
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    sql_str.ptr(),
    sql_str.length(),
    -1,
    need_mon)
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    DBUG_RETURN(spider_db_errorno(conn));
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_db_mysql::xa_commit(
  XID *xid,
  int *need_mon
) {
  char sql_buf[SPIDER_SQL_XA_COMMIT_LEN + XIDDATASIZE + sizeof(long) + 9];
  spider_string sql_str(sql_buf, sizeof(sql_buf), &my_charset_bin);
  DBUG_ENTER("spider_db_mysql::xa_commit");
  DBUG_PRINT("info",("spider this=%p", this));
  sql_str.init_calc_mem(110);

  sql_str.length(0);
  sql_str.q_append(SPIDER_SQL_XA_COMMIT_STR, SPIDER_SQL_XA_COMMIT_LEN);
  spider_db_append_xid_str(&sql_str, xid);
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    sql_str.ptr(),
    sql_str.length(),
    -1,
    need_mon)
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    DBUG_RETURN(spider_db_errorno(conn));
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_db_mysql::xa_rollback(
  XID *xid,
  int *need_mon
) {
  char sql_buf[SPIDER_SQL_XA_ROLLBACK_LEN + XIDDATASIZE + sizeof(long) + 9];
  spider_string sql_str(sql_buf, sizeof(sql_buf), &my_charset_bin);
  DBUG_ENTER("spider_db_mysql::xa_rollback");
  DBUG_PRINT("info",("spider this=%p", this));
  sql_str.init_calc_mem(111);

  sql_str.length(0);
  sql_str.q_append(SPIDER_SQL_XA_ROLLBACK_STR, SPIDER_SQL_XA_ROLLBACK_LEN);
  spider_db_append_xid_str(&sql_str, xid);
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    sql_str.ptr(),
    sql_str.length(),
    -1,
    need_mon)
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    DBUG_RETURN(spider_db_errorno(conn));
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

bool spider_db_mysql::set_trx_isolation_in_bulk_sql()
{
  DBUG_ENTER("spider_db_mysql::set_trx_isolation_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

int spider_db_mysql::set_trx_isolation(
  int trx_isolation,
  int *need_mon
) {
  DBUG_ENTER("spider_db_mysql::set_trx_isolation");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (trx_isolation)
  {
    case ISO_READ_UNCOMMITTED:
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      conn->need_mon = need_mon;
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if (spider_db_query(
        conn,
        SPIDER_SQL_ISO_READ_UNCOMMITTED_STR,
        SPIDER_SQL_ISO_READ_UNCOMMITTED_LEN,
        -1,
        need_mon)
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        DBUG_RETURN(spider_db_errorno(conn));
      }
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      break;
    case ISO_READ_COMMITTED:
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      conn->need_mon = need_mon;
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if (spider_db_query(
        conn,
        SPIDER_SQL_ISO_READ_COMMITTED_STR,
        SPIDER_SQL_ISO_READ_COMMITTED_LEN,
        -1,
        need_mon)
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        DBUG_RETURN(spider_db_errorno(conn));
      }
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      break;
    case ISO_REPEATABLE_READ:
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      conn->need_mon = need_mon;
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if (spider_db_query(
        conn,
        SPIDER_SQL_ISO_REPEATABLE_READ_STR,
        SPIDER_SQL_ISO_REPEATABLE_READ_LEN,
        -1,
        need_mon)
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        DBUG_RETURN(spider_db_errorno(conn));
      }
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      break;
    case ISO_SERIALIZABLE:
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      conn->need_mon = need_mon;
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if (spider_db_query(
        conn,
        SPIDER_SQL_ISO_SERIALIZABLE_STR,
        SPIDER_SQL_ISO_SERIALIZABLE_LEN,
        -1,
        need_mon)
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        DBUG_RETURN(spider_db_errorno(conn));
      }
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      break;
    default:
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }
  DBUG_RETURN(0);
}

bool spider_db_mysql::set_autocommit_in_bulk_sql()
{
  DBUG_ENTER("spider_db_mysql::set_autocommit_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

int spider_db_mysql::set_autocommit(
  bool autocommit,
  int *need_mon
) {
  DBUG_ENTER("spider_db_mysql::set_autocommit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (autocommit)
  {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = need_mon;
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if (spider_db_query(
      conn,
      SPIDER_SQL_AUTOCOMMIT_ON_STR,
      SPIDER_SQL_AUTOCOMMIT_ON_LEN,
      -1,
      need_mon)
    ) {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      DBUG_RETURN(spider_db_errorno(conn));
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  } else {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = need_mon;
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if (spider_db_query(
      conn,
      SPIDER_SQL_AUTOCOMMIT_OFF_STR,
      SPIDER_SQL_AUTOCOMMIT_OFF_LEN,
      -1,
      need_mon)
    ) {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      DBUG_RETURN(spider_db_errorno(conn));
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  }
  DBUG_RETURN(0);
}

bool spider_db_mysql::set_sql_log_off_in_bulk_sql()
{
  DBUG_ENTER("spider_db_mysql::set_sql_log_off_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

int spider_db_mysql::set_sql_log_off(
  bool sql_log_off,
  int *need_mon
) {
  DBUG_ENTER("spider_db_mysql::set_sql_log_off");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_log_off)
  {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = need_mon;
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if (spider_db_query(
      conn,
      SPIDER_SQL_SQL_LOG_ON_STR,
      SPIDER_SQL_SQL_LOG_ON_LEN,
      -1,
      need_mon)
    ) {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      DBUG_RETURN(spider_db_errorno(conn));
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  } else {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = need_mon;
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if (spider_db_query(
      conn,
      SPIDER_SQL_SQL_LOG_OFF_STR,
      SPIDER_SQL_SQL_LOG_OFF_LEN,
      -1,
      need_mon)
    ) {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      DBUG_RETURN(spider_db_errorno(conn));
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  }
  DBUG_RETURN(0);
}

bool spider_db_mysql::set_time_zone_in_bulk_sql()
{
  DBUG_ENTER("spider_db_mysql::set_time_zone_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

int spider_db_mysql::set_time_zone(
  Time_zone *time_zone,
  int *need_mon
) {
  const String *tz_str = time_zone->get_name();
  char sql_buf[MAX_FIELD_WIDTH];
  spider_string sql_str(sql_buf, sizeof(sql_buf), &my_charset_bin);
  DBUG_ENTER("spider_db_mysql::set_time_zone");
  DBUG_PRINT("info",("spider this=%p", this));
  sql_str.init_calc_mem(214);
  sql_str.length(0);
  if (sql_str.reserve(SPIDER_SQL_TIME_ZONE_LEN +
    tz_str->length() + SPIDER_SQL_VALUE_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql_str.q_append(SPIDER_SQL_TIME_ZONE_STR, SPIDER_SQL_TIME_ZONE_LEN);
  sql_str.q_append(tz_str->ptr(), tz_str->length());
  sql_str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    sql_str.ptr(),
    sql_str.length(),
    -1,
    need_mon)
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    DBUG_RETURN(spider_db_errorno(conn));
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
int spider_db_mysql::append_sql(
  char *sql,
  ulong sql_length,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_mysql::append_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_mysql::append_open_handler(
  uint handler_id,
  const char *db_name,
  const char *table_name,
  const char *index_name,
  const char *sql,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_mysql::append_open_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_mysql::append_select(
  uint handler_id,
  spider_string *sql,
  SPIDER_DB_HS_STRING_REF_BUFFER *keys,
  int limit,
  int skip,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_mysql::append_select");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_mysql::append_insert(
  uint handler_id,
  SPIDER_DB_HS_STRING_REF_BUFFER *upds,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_mysql::append_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_mysql::append_update(
  uint handler_id,
  spider_string *sql,
  SPIDER_DB_HS_STRING_REF_BUFFER *keys,
  SPIDER_DB_HS_STRING_REF_BUFFER *upds,
  int limit,
  int skip,
  bool increment,
  bool decrement,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_mysql::append_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_mysql::append_delete(
  uint handler_id,
  spider_string *sql,
  SPIDER_DB_HS_STRING_REF_BUFFER *keys,
  int limit,
  int skip,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_mysql::append_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

void spider_db_mysql::reset_request_queue()
{
  DBUG_ENTER("spider_db_mysql::reset_request_queue");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}
#endif

size_t spider_db_mysql::escape_string(
  char *to,
  const char *from,
  size_t from_length
) {
  DBUG_ENTER("spider_db_mysql::escape_string");
  DBUG_PRINT("info",("spider this=%p", this));
  if (db_conn->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES)
    DBUG_RETURN(escape_quotes_for_mysql(db_conn->charset, to, 0,
      from, from_length));
  DBUG_RETURN(escape_string_for_mysql(db_conn->charset, to, 0,
    from, from_length));
}

bool spider_db_mysql::have_lock_table_list()
{
  DBUG_ENTER("spider_db_mysql::have_lock_table_list");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(lock_table_hash.records);
}

int spider_db_mysql::append_lock_tables(
  spider_string *str
) {
  int error_num;
  ha_spider *tmp_spider;
  int lock_type;
  uint conn_link_idx;
  int tmp_link_idx;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash;
  const char *db_name;
  uint db_name_length;
  CHARSET_INFO *db_name_charset;
  const char *table_name;
  uint table_name_length;
  CHARSET_INFO *table_name_charset;
  DBUG_ENTER("spider_db_mysql::lock_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_mysql_utility.append_lock_table_head(str)))
  {
    DBUG_RETURN(error_num);
  }
  while ((tmp_link_for_hash =
    (SPIDER_LINK_FOR_HASH *) my_hash_element(&lock_table_hash, 0)))
  {
    tmp_spider = tmp_link_for_hash->spider;
    tmp_link_idx = tmp_link_for_hash->link_idx;
    switch (tmp_spider->lock_type)
    {
      case TL_READ:
        lock_type = SPIDER_DB_TABLE_LOCK_READ_LOCAL;
        break;
      case TL_READ_NO_INSERT:
        lock_type = SPIDER_DB_TABLE_LOCK_READ;
        break;
      case TL_WRITE_LOW_PRIORITY:
        lock_type = SPIDER_DB_TABLE_LOCK_LOW_PRIORITY_WRITE;
        break;
      case TL_WRITE:
        lock_type = SPIDER_DB_TABLE_LOCK_WRITE;
        break;
      default:
        // no lock
        DBUG_PRINT("info",("spider lock_type=%d", tmp_spider->lock_type));
        DBUG_RETURN(0);
    }
    conn_link_idx = tmp_spider->conn_link_idx[tmp_link_idx];
    spider_mysql_share *db_share = (spider_mysql_share *)
      tmp_spider->share->dbton_share[conn->dbton_id];
    if (&db_share->db_names_str[conn_link_idx])
    {
      db_name = db_share->db_names_str[conn_link_idx].ptr();
      db_name_length = db_share->db_names_str[conn_link_idx].length();
      db_name_charset = tmp_spider->share->access_charset;
    } else {
      db_name = tmp_spider->share->tgt_dbs[conn_link_idx];
      db_name_length = tmp_spider->share->tgt_dbs_lengths[conn_link_idx];
      db_name_charset = system_charset_info;
    }
    if (&db_share->table_names_str[conn_link_idx])
    {
      table_name = db_share->table_names_str[conn_link_idx].ptr();
      table_name_length = db_share->table_names_str[conn_link_idx].length();
      table_name_charset = tmp_spider->share->access_charset;
    } else {
      table_name = tmp_spider->share->tgt_table_names[conn_link_idx];
      table_name_length =
        tmp_spider->share->tgt_table_names_lengths[conn_link_idx];
      table_name_charset = system_charset_info;
    }
    if ((error_num = spider_db_mysql_utility.
      append_lock_table_body(
        str,
        db_name,
        db_name_length,
        db_name_charset,
        table_name,
        table_name_length,
        table_name_charset,
        lock_type
      )
    )) {
      my_hash_reset(&lock_table_hash);
      DBUG_RETURN(error_num);
    }
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&lock_table_hash,
      tmp_link_for_hash->db_table_str_hash_value, (uchar*) tmp_link_for_hash);
#else
    my_hash_delete(&lock_table_hash, (uchar*) tmp_link_for_hash);
#endif
  }
  if ((error_num = spider_db_mysql_utility.append_lock_table_tail(str)))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_mysql::append_unlock_tables(
  spider_string *str
) {
  int error_num;
  DBUG_ENTER("spider_db_mysql::append_unlock_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_mysql_utility.append_unlock_table(str)))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

uint spider_db_mysql::get_lock_table_hash_count()
{
  DBUG_ENTER("spider_db_mysql::get_lock_table_hash_count");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(lock_table_hash.records);
}

void spider_db_mysql::reset_lock_table_hash()
{
  DBUG_ENTER("spider_db_mysql::reset_lock_table_hash");
  DBUG_PRINT("info",("spider this=%p", this));
  my_hash_reset(&lock_table_hash);
  DBUG_VOID_RETURN;
}

uint spider_db_mysql::get_opened_handler_count()
{
  DBUG_ENTER("spider_db_mysql::get_opened_handler_count");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(handler_open_array.elements);
}

void spider_db_mysql::reset_opened_handler()
{
  ha_spider *tmp_spider;
  int tmp_link_idx;
  SPIDER_LINK_FOR_HASH **tmp_link_for_hash;
  DBUG_ENTER("spider_db_mysql::reset_opened_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  while ((tmp_link_for_hash =
    (SPIDER_LINK_FOR_HASH **) pop_dynamic(&handler_open_array)))
  {
    tmp_spider = (*tmp_link_for_hash)->spider;
    tmp_link_idx = (*tmp_link_for_hash)->link_idx;
    tmp_spider->clear_handler_opened(tmp_link_idx, conn->conn_kind);
  }
  DBUG_VOID_RETURN;
}

void spider_db_mysql::set_dup_key_idx(
  ha_spider *spider,
  int link_idx
) {
  TABLE *table = spider->get_table();
  uint roop_count, pk_idx = table->s->primary_key;
  int key_name_length;
  int max_length = 0;
  char *key_name;
  DBUG_ENTER("spider_db_mysql::set_dup_key_idx");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider error_str=%s", conn->error_str));
  for (roop_count = 0; roop_count < table->s->keys; roop_count++)
  {
    if (roop_count == pk_idx)
    {
      DBUG_PRINT("info",("spider pk_idx=%u", roop_count));
      int all_link_idx = spider->conn_link_idx[link_idx];
      key_name = spider->share->tgt_pk_names[all_link_idx];
      key_name_length = spider->share->tgt_pk_names_lengths[all_link_idx];
    } else {
      key_name = table->s->key_info[roop_count].name;
      key_name_length = strlen(key_name);
    }
    DBUG_PRINT("info",("spider key_name=%s", key_name));
    if (
      max_length < key_name_length &&
      conn->error_length - 1 >= key_name_length &&
      *(conn->error_str + conn->error_length - 2 -
        key_name_length) == '\'' &&
      !strncasecmp(conn->error_str +
        conn->error_length - 1 - key_name_length,
        key_name, key_name_length)
    ) {
      max_length = key_name_length;
      spider->dup_key_idx = roop_count;
    }
  }
  if (max_length == 0)
    spider->dup_key_idx = (uint) -1;
  DBUG_PRINT("info",("spider dup_key_idx=%d", spider->dup_key_idx));
  DBUG_VOID_RETURN;
}

bool spider_db_mysql::cmp_request_key_to_snd(
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_mysql::cmp_request_key_to_snd");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

spider_db_mysql_util::spider_db_mysql_util() : spider_db_util()
{
  DBUG_ENTER("spider_db_mysql_util::spider_db_mysql_util");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_mysql_util::~spider_db_mysql_util()
{
  DBUG_ENTER("spider_db_mysql_util::~spider_db_mysql_util");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

int spider_db_mysql_util::append_name(
  spider_string *str,
  const char *name,
  uint name_length
) {
  DBUG_ENTER("spider_db_mysql_util::append_name");
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->q_append(name, name_length);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_name_with_charset(
  spider_string *str,
  const char *name,
  uint name_length,
  CHARSET_INFO *name_charset
) {
  DBUG_ENTER("spider_db_mysql_util::append_name_with_charset");
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN * 2 + name_length * 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->append(name, name_length, name_charset);
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

bool spider_db_mysql_util::is_name_quote(
  const char head_code
) {
  DBUG_ENTER("spider_db_mysql_util::is_name_quote");
  DBUG_RETURN(head_code == *name_quote_str);
}

int spider_db_mysql_util::append_escaped_name_quote(
  spider_string *str
) {
  DBUG_ENTER("spider_db_mysql_util::append_escaped_name_quote");
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN * 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_column_value(
  ha_spider *spider,
  spider_string *str,
  Field *field,
  const uchar *new_ptr,
  CHARSET_INFO *access_charset
) {
  char buf[MAX_FIELD_WIDTH];
  spider_string tmp_str(buf, MAX_FIELD_WIDTH, &my_charset_bin);
  String *ptr;
  uint length;
  DBUG_ENTER("spider_db_mysql_util::append_column_value");
  tmp_str.init_calc_mem(113);

  if (new_ptr)
  {
    if (
      field->type() == MYSQL_TYPE_BLOB ||
      field->real_type() == MYSQL_TYPE_VARCHAR
    ) {
      length = uint2korr(new_ptr);
      tmp_str.set_quick((char *) new_ptr + HA_KEY_BLOB_LENGTH, length,
        &my_charset_bin);
      ptr = tmp_str.get_str();
    } else if (field->type() == MYSQL_TYPE_GEOMETRY)
    {
/*
      uint mlength = SIZEOF_STORED_DOUBLE, lcnt;
      uchar *dest = (uchar *) buf;
      const uchar *source;
      for (lcnt = 0; lcnt < 4; lcnt++)
      {
        mlength = SIZEOF_STORED_DOUBLE;
        source = new_ptr + mlength + SIZEOF_STORED_DOUBLE * lcnt;
        while (mlength--)
          *dest++ = *--source;
      }
      tmp_str.length(SIZEOF_STORED_DOUBLE * lcnt);
*/
#ifndef DBUG_OFF
      double xmin, xmax, ymin, ymax;
/*
      float8store(buf,xmin);
      float8store(buf+8,xmax);
      float8store(buf+16,ymin);
      float8store(buf+24,ymax);
      memcpy(&xmin,new_ptr,sizeof(xmin));
      memcpy(&xmax,new_ptr + 8,sizeof(xmax));
      memcpy(&ymin,new_ptr + 16,sizeof(ymin));
      memcpy(&ymax,new_ptr + 24,sizeof(ymax));
      float8get(xmin, buf);
      float8get(xmax, buf + 8);
      float8get(ymin, buf + 16);
      float8get(ymax, buf + 24);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
      DBUG_PRINT("info", ("spider geo is %.14g %.14g %.14g %.14g",
        xmin, xmax, ymin, ymax));
*/
      float8get(xmin, new_ptr);
      float8get(xmax, new_ptr + 8);
      float8get(ymin, new_ptr + 16);
      float8get(ymax, new_ptr + 24);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
/*
      float8get(xmin, new_ptr + SIZEOF_STORED_DOUBLE * 4);
      float8get(xmax, new_ptr + SIZEOF_STORED_DOUBLE * 5);
      float8get(ymin, new_ptr + SIZEOF_STORED_DOUBLE * 6);
      float8get(ymax, new_ptr + SIZEOF_STORED_DOUBLE * 7);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
      float8get(xmin, new_ptr + SIZEOF_STORED_DOUBLE * 8);
      float8get(xmax, new_ptr + SIZEOF_STORED_DOUBLE * 9);
      float8get(ymin, new_ptr + SIZEOF_STORED_DOUBLE * 10);
      float8get(ymax, new_ptr + SIZEOF_STORED_DOUBLE * 11);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
      float8get(xmin, new_ptr + SIZEOF_STORED_DOUBLE * 12);
      float8get(xmax, new_ptr + SIZEOF_STORED_DOUBLE * 13);
      float8get(ymin, new_ptr + SIZEOF_STORED_DOUBLE * 14);
      float8get(ymax, new_ptr + SIZEOF_STORED_DOUBLE * 15);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
*/
#endif
/*
      tmp_str.set_quick((char *) new_ptr, SIZEOF_STORED_DOUBLE * 4,
        &my_charset_bin);
*/
      tmp_str.length(0);
      tmp_str.q_append((char *) SPIDER_SQL_LINESTRING_HEAD_STR,
        SPIDER_SQL_LINESTRING_HEAD_LEN);
      tmp_str.q_append((char *) new_ptr, SIZEOF_STORED_DOUBLE);
      tmp_str.q_append((char *) new_ptr + SIZEOF_STORED_DOUBLE * 2,
        SIZEOF_STORED_DOUBLE);
      tmp_str.q_append((char *) new_ptr + SIZEOF_STORED_DOUBLE,
        SIZEOF_STORED_DOUBLE);
      tmp_str.q_append((char *) new_ptr + SIZEOF_STORED_DOUBLE * 3,
        SIZEOF_STORED_DOUBLE);
      ptr = tmp_str.get_str();
    } else {
      ptr = field->val_str(tmp_str.get_str(), new_ptr);
      tmp_str.mem_calc();
    }
  } else {
    ptr = field->val_str(tmp_str.get_str());
    tmp_str.mem_calc();
  }
  DBUG_PRINT("info", ("spider field->type() is %d", field->type()));
  DBUG_PRINT("info", ("spider ptr->length() is %d", ptr->length()));
/*
  if (
    field->type() == MYSQL_TYPE_BIT ||
    (field->type() >= MYSQL_TYPE_TINY_BLOB &&
      field->type() <= MYSQL_TYPE_BLOB)
  ) {
    uchar *hex_ptr = (uchar *) ptr->ptr(), *end_ptr;
    char *str_ptr;
    DBUG_PRINT("info", ("spider HEX"));
    if (str->reserve(SPIDER_SQL_HEX_LEN + ptr->length() * 2))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_HEX_STR, SPIDER_SQL_HEX_LEN);
    str_ptr = (char *) str->ptr() + str->length();
    for (end_ptr = hex_ptr + ptr->length(); hex_ptr < end_ptr; hex_ptr++)
    {
      *str_ptr++ = spider_dig_upper[(*hex_ptr) >> 4];
      *str_ptr++ = spider_dig_upper[(*hex_ptr) & 0x0F];
    }
    str->length(str->length() + ptr->length() * 2);
  } else 
*/
  if (field->result_type() == STRING_RESULT)
  {
    DBUG_PRINT("info", ("spider STRING_RESULT"));
    if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    if (
      field->type() == MYSQL_TYPE_VARCHAR ||
      (field->type() >= MYSQL_TYPE_ENUM &&
        field->type() <= MYSQL_TYPE_GEOMETRY)
    ) {
      DBUG_PRINT("info", ("spider append_escaped"));
      char buf2[MAX_FIELD_WIDTH];
      spider_string tmp_str2(buf2, MAX_FIELD_WIDTH, access_charset);
      tmp_str2.init_calc_mem(114);
      tmp_str2.length(0);
      if (
        tmp_str2.append(ptr->ptr(), ptr->length(), field->charset()) ||
        str->reserve(tmp_str2.length() * 2) ||
        append_escaped_util(str, tmp_str2.get_str())
      )
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    } else if (str->append(*ptr))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  } else if (field->str_needs_quotes())
  {
    if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN * 2 + ptr->length() * 2 + 2))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    append_escaped_util(str, ptr);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  } else if (str->append(*ptr))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_from_with_alias(
  spider_string *str,
  const char **table_names,
  uint *table_name_lengths,
  const char **table_aliases,
  uint *table_alias_lengths,
  uint table_count,
  int *table_name_pos,
  bool over_write
) {
  uint roop_count, length = 0;
  DBUG_ENTER("spider_db_mysql_util::append_from_with_alias");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!over_write)
  {
    for (roop_count = 0; roop_count < table_count; roop_count++)
      length += table_name_lengths[roop_count] + SPIDER_SQL_SPACE_LEN +
        table_alias_lengths[roop_count] + SPIDER_SQL_COMMA_LEN;
    if (str->reserve(SPIDER_SQL_FROM_LEN + length))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
    *table_name_pos = str->length();
  }
  for (roop_count = 0; roop_count < table_count; roop_count++)
  {
    str->q_append(table_names[roop_count], table_name_lengths[roop_count]);
    str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    str->q_append(table_aliases[roop_count], table_alias_lengths[roop_count]);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_trx_isolation(
  spider_string *str,
  int trx_isolation
) {
  DBUG_ENTER("spider_db_mysql_util::append_trx_isolation");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SEMICOLON_LEN +
    SPIDER_SQL_ISO_READ_UNCOMMITTED_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (str->length())
  {
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }
  switch (trx_isolation)
  {
    case ISO_READ_UNCOMMITTED:
      str->q_append(SPIDER_SQL_ISO_READ_UNCOMMITTED_STR,
        SPIDER_SQL_ISO_READ_UNCOMMITTED_LEN);
      break;
    case ISO_READ_COMMITTED:
      str->q_append(SPIDER_SQL_ISO_READ_COMMITTED_STR,
        SPIDER_SQL_ISO_READ_COMMITTED_LEN);
      break;
    case ISO_REPEATABLE_READ:
      str->q_append(SPIDER_SQL_ISO_REPEATABLE_READ_STR,
        SPIDER_SQL_ISO_REPEATABLE_READ_LEN);
      break;
    case ISO_SERIALIZABLE:
      str->q_append(SPIDER_SQL_ISO_SERIALIZABLE_STR,
        SPIDER_SQL_ISO_SERIALIZABLE_LEN);
      break;
    default:
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_autocommit(
  spider_string *str,
  bool autocommit
) {
  DBUG_ENTER("spider_db_mysql_util::append_autocommit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SEMICOLON_LEN + SPIDER_SQL_AUTOCOMMIT_OFF_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (str->length())
  {
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }
  if (autocommit)
  {
    str->q_append(SPIDER_SQL_AUTOCOMMIT_ON_STR,
      SPIDER_SQL_AUTOCOMMIT_ON_LEN);
  } else {
    str->q_append(SPIDER_SQL_AUTOCOMMIT_OFF_STR,
      SPIDER_SQL_AUTOCOMMIT_OFF_LEN);
  }
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_sql_log_off(
  spider_string *str,
  bool sql_log_off
) {
  DBUG_ENTER("spider_db_mysql_util::append_sql_log_off");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SEMICOLON_LEN + SPIDER_SQL_SQL_LOG_OFF_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (str->length())
  {
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }
  if (sql_log_off)
  {
    str->q_append(SPIDER_SQL_SQL_LOG_ON_STR, SPIDER_SQL_SQL_LOG_ON_LEN);
  } else {
    str->q_append(SPIDER_SQL_SQL_LOG_OFF_STR, SPIDER_SQL_SQL_LOG_OFF_LEN);
  }
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_time_zone(
  spider_string *str,
  Time_zone *time_zone
) {
  const String *tz_str = time_zone->get_name();
  DBUG_ENTER("spider_db_mysql_util::append_time_zone");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SEMICOLON_LEN + SPIDER_SQL_TIME_ZONE_LEN +
    tz_str->length() + SPIDER_SQL_VALUE_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (str->length())
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  str->q_append(SPIDER_SQL_TIME_ZONE_STR, SPIDER_SQL_TIME_ZONE_LEN);
  str->q_append(tz_str->ptr(), tz_str->length());
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_start_transaction(
  spider_string *str
) {
  DBUG_ENTER("spider_db_mysql_util::append_start_transaction");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SEMICOLON_LEN +
    SPIDER_SQL_START_TRANSACTION_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (str->length())
  {
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }
  str->q_append(SPIDER_SQL_START_TRANSACTION_STR,
    SPIDER_SQL_START_TRANSACTION_LEN);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_xa_start(
  spider_string *str,
  XID *xid
) {
  DBUG_ENTER("spider_db_mysql_util::append_xa_start");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SEMICOLON_LEN +
    SPIDER_SQL_XA_START_LEN + XIDDATASIZE + sizeof(long) + 9))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (str->length())
  {
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }
  str->q_append(SPIDER_SQL_XA_START_STR, SPIDER_SQL_XA_START_LEN);
  spider_db_append_xid_str(str, xid);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_lock_table_head(
  spider_string *str
) {
  DBUG_ENTER("spider_db_mysql_util::append_lock_table_head");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_LOCK_TABLE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_LOCK_TABLE_STR, SPIDER_SQL_LOCK_TABLE_LEN);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_lock_table_body(
  spider_string *str,
  const char *db_name,
  uint db_name_length,
  CHARSET_INFO *db_name_charset,
  const char *table_name,
  uint table_name_length,
  CHARSET_INFO *table_name_charset,
  int lock_type
) {
  DBUG_ENTER("spider_db_mysql_util::append_lock_table_body");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  if (
    str->append(db_name, db_name_length, db_name_charset) ||
    str->reserve((SPIDER_SQL_NAME_QUOTE_LEN) * 2 + SPIDER_SQL_DOT_LEN)
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  if (
    str->append(table_name, table_name_length, table_name_charset) ||
    str->reserve(SPIDER_SQL_NAME_QUOTE_LEN +
      spider_db_table_lock_len[lock_type])
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->q_append(spider_db_table_lock_str[lock_type],
    spider_db_table_lock_len[lock_type]);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_lock_table_tail(
  spider_string *str
) {
  DBUG_ENTER("spider_db_mysql_util::append_lock_table_tail");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::append_unlock_table(
  spider_string *str
) {
  DBUG_ENTER("spider_db_mysql_util::append_unlock_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_UNLOCK_TABLE_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_UNLOCK_TABLE_STR, SPIDER_SQL_UNLOCK_TABLE_LEN);
  DBUG_RETURN(0);
}

int spider_db_mysql_util::open_item_func(
  Item_func *item_func,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  uint dbton_id = spider_dbton_mysql.dbton_id;
  int error_num;
  Item *item, **item_list = item_func->arguments();
  uint roop_count, item_count = item_func->argument_count(), start_item = 0;
  const char *func_name = SPIDER_SQL_NULL_CHAR_STR,
    *separete_str = SPIDER_SQL_NULL_CHAR_STR,
    *last_str = SPIDER_SQL_NULL_CHAR_STR;
  int func_name_length = SPIDER_SQL_NULL_CHAR_LEN,
    separete_str_length = SPIDER_SQL_NULL_CHAR_LEN,
    last_str_length = SPIDER_SQL_NULL_CHAR_LEN;
  int use_pushdown_udf;
  DBUG_ENTER("spider_db_mysql_util::open_item_func");
  if (str)
  {
    if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  }
  DBUG_PRINT("info",("spider functype = %d", item_func->functype()));
  switch (item_func->functype())
  {
    case Item_func::ISNULL_FUNC:
      last_str = SPIDER_SQL_IS_NULL_STR;
      last_str_length = SPIDER_SQL_IS_NULL_LEN;
      break;
    case Item_func::ISNOTNULL_FUNC:
      last_str = SPIDER_SQL_IS_NOT_NULL_STR;
      last_str_length = SPIDER_SQL_IS_NOT_NULL_LEN;
      break;
    case Item_func::UNKNOWN_FUNC:
      func_name = (char*) item_func->func_name();
      func_name_length = strlen(func_name);
      DBUG_PRINT("info",("spider func_name = %s", func_name));
      DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
      if (func_name_length == 1 &&
        (
          !strncasecmp("+", func_name, func_name_length) ||
          !strncasecmp("-", func_name, func_name_length) ||
          !strncasecmp("*", func_name, func_name_length) ||
          !strncasecmp("/", func_name, func_name_length) ||
          !strncasecmp("%", func_name, func_name_length) ||
          !strncasecmp("&", func_name, func_name_length) ||
          !strncasecmp("|", func_name, func_name_length) ||
          !strncasecmp("^", func_name, func_name_length)
        )
      ) {
        /* no action */
        break;
      } else if (func_name_length == 2 &&
        (
          !strncasecmp("<<", func_name, func_name_length) ||
          !strncasecmp(">>", func_name, func_name_length)
        )
      ) {
        /* no action */
        break;
      } else if (func_name_length == 3 &&
        !strncasecmp("div", func_name, func_name_length)
      ) {
        /* no action */
        break;
      } else if (func_name_length == 4)
      {
        if (
          !strncasecmp("rand", func_name, func_name_length) &&
#ifdef SPIDER_Item_args_arg_count_IS_PROTECTED
          !item_func->argument_count()
#else
          !item_func->arg_count
#endif
        ) {
          if (str)
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          DBUG_RETURN(spider_db_open_item_int(item_func, spider, str,
            alias, alias_length, dbton_id));
        } else if (
          !strncasecmp("case", func_name, func_name_length)
        ) {
#ifdef ITEM_FUNC_CASE_PARAMS_ARE_PUBLIC
          Item_func_case *item_func_case = (Item_func_case *) item_func;
          if (str)
          {
            if (str->reserve(SPIDER_SQL_CASE_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CASE_STR, SPIDER_SQL_CASE_LEN);
          }
          if (item_func_case->first_expr_num != -1)
          {
            if ((error_num = spider_db_print_item_type(
              item_list[item_func_case->first_expr_num], spider, str,
              alias, alias_length, dbton_id)))
              DBUG_RETURN(error_num);
          }
          for (roop_count = 0; roop_count < item_func_case->ncases;
            roop_count += 2)
          {
            if (str)
            {
              if (str->reserve(SPIDER_SQL_WHEN_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_WHEN_STR, SPIDER_SQL_WHEN_LEN);
            }
            if ((error_num = spider_db_print_item_type(
              item_list[roop_count], spider, str,
              alias, alias_length, dbton_id)))
              DBUG_RETURN(error_num);
            if (str)
            {
              if (str->reserve(SPIDER_SQL_THEN_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_THEN_STR, SPIDER_SQL_THEN_LEN);
            }
            if ((error_num = spider_db_print_item_type(
              item_list[roop_count + 1], spider, str,
              alias, alias_length, dbton_id)))
              DBUG_RETURN(error_num);
          }
          if (item_func_case->else_expr_num != -1)
          {
            if (str)
            {
              if (str->reserve(SPIDER_SQL_ELSE_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_ELSE_STR, SPIDER_SQL_ELSE_LEN);
            }
            if ((error_num = spider_db_print_item_type(
              item_list[item_func_case->else_expr_num], spider, str,
              alias, alias_length, dbton_id)))
              DBUG_RETURN(error_num);
          }
          if (str)
          {
            if (str->reserve(SPIDER_SQL_END_LEN + SPIDER_SQL_CLOSE_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_END_STR, SPIDER_SQL_END_LEN);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
          }
          DBUG_RETURN(0);
#else
          DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
#endif
        }
      } else if (func_name_length == 6 &&
        !strncasecmp("istrue", func_name, func_name_length)
      ) {
        last_str = SPIDER_SQL_IS_TRUE_STR;
        last_str_length = SPIDER_SQL_IS_TRUE_LEN;
        break;
      } else if (func_name_length == 7)
      {
        if (!strncasecmp("isfalse", func_name, func_name_length))
        {
          last_str = SPIDER_SQL_IS_FALSE_STR;
          last_str_length = SPIDER_SQL_IS_FALSE_LEN;
          break;
        } else if (
          !strncasecmp("sysdate", func_name, func_name_length) ||
          !strncasecmp("curdate", func_name, func_name_length) ||
          !strncasecmp("curtime", func_name, func_name_length)
        ) {
          if (str)
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          DBUG_RETURN(spider_db_open_item_string(item_func, spider, str,
            alias, alias_length, dbton_id));
        } else if (
          !strncasecmp("convert", func_name, func_name_length)
        ) {
          if (str)
          {
            if (str->reserve(func_name_length * 2 + SPIDER_SQL_OPEN_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(func_name, func_name_length);
            str->q_append(SPIDER_SQL_OPEN_PAREN_STR,
              SPIDER_SQL_OPEN_PAREN_LEN);
            last_str = SPIDER_SQL_CLOSE_PAREN_STR;
            last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
          }
          break;
        }
      } else if (func_name_length == 8 &&
        (
          !strncasecmp("utc_date", func_name, func_name_length) ||
          !strncasecmp("utc_time", func_name, func_name_length)
        )
      ) {
        if (str)
          str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
        DBUG_RETURN(spider_db_open_item_string(item_func, spider, str,
          alias, alias_length, dbton_id));
      } else if (func_name_length == 9 &&
        !strncasecmp("isnottrue", func_name, func_name_length)
      ) {
        last_str = SPIDER_SQL_IS_NOT_TRUE_STR;
        last_str_length = SPIDER_SQL_IS_NOT_TRUE_LEN;
        break;
      } else if (func_name_length == 10 &&
        !strncasecmp("isnotfalse", func_name, func_name_length)
      ) {
        last_str = SPIDER_SQL_IS_NOT_FALSE_STR;
        last_str_length = SPIDER_SQL_IS_NOT_FALSE_LEN;
        break;
      } else if (func_name_length == 12)
      {
        if (!strncasecmp("cast_as_date", func_name, func_name_length))
        {
          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(SPIDER_SQL_CAST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
          }
          last_str = SPIDER_SQL_AS_DATE_STR;
          last_str_length = SPIDER_SQL_AS_DATE_LEN;
          break;
        } else if (!strncasecmp("cast_as_time", func_name, func_name_length))
        {
          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(SPIDER_SQL_CAST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
          }
          last_str = SPIDER_SQL_AS_TIME_STR;
          last_str_length = SPIDER_SQL_AS_TIME_LEN;
          break;
        }
      } else if (func_name_length == 13)
      {
        if (!strncasecmp("utc_timestamp", func_name, func_name_length))
        {
          if (str)
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          DBUG_RETURN(spider_db_open_item_string(item_func, spider, str,
            alias, alias_length, dbton_id));
        } else if (!strncasecmp("timestampdiff", func_name, func_name_length))
        {
#ifdef ITEM_FUNC_TIMESTAMPDIFF_ARE_PUBLIC
          Item_func_timestamp_diff *item_func_timestamp_diff =
            (Item_func_timestamp_diff *) item_func;
          if (str)
          {
            const char *interval_str;
            uint interval_len;
            switch (item_func_timestamp_diff->int_type)
            {
              case INTERVAL_YEAR:
                interval_str = SPIDER_SQL_YEAR_STR;
                interval_len = SPIDER_SQL_YEAR_LEN;
                break;
              case INTERVAL_QUARTER:
                interval_str = SPIDER_SQL_QUARTER_STR;
                interval_len = SPIDER_SQL_QUARTER_LEN;
                break;
              case INTERVAL_MONTH:
                interval_str = SPIDER_SQL_MONTH_STR;
                interval_len = SPIDER_SQL_MONTH_LEN;
                break;
              case INTERVAL_WEEK:
                interval_str = SPIDER_SQL_WEEK_STR;
                interval_len = SPIDER_SQL_WEEK_LEN;
                break;
              case INTERVAL_DAY:
                interval_str = SPIDER_SQL_DAY_STR;
                interval_len = SPIDER_SQL_DAY_LEN;
                break;
              case INTERVAL_HOUR:
                interval_str = SPIDER_SQL_HOUR_STR;
                interval_len = SPIDER_SQL_HOUR_LEN;
                break;
              case INTERVAL_MINUTE:
                interval_str = SPIDER_SQL_MINUTE_STR;
                interval_len = SPIDER_SQL_MINUTE_LEN;
                break;
              case INTERVAL_SECOND:
                interval_str = SPIDER_SQL_SECOND_STR;
                interval_len = SPIDER_SQL_SECOND_LEN;
                break;
              case INTERVAL_MICROSECOND:
                interval_str = SPIDER_SQL_MICROSECOND_STR;
                interval_len = SPIDER_SQL_MICROSECOND_LEN;
                break;
              default:
                interval_str = "";
                interval_len = 0;
                break;
            }
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(func_name_length + SPIDER_SQL_OPEN_PAREN_LEN +
              interval_len + SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(func_name, func_name_length);
            str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
            str->q_append(interval_str, interval_len);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          }
          if ((error_num = spider_db_print_item_type(item_list[0], spider,
            str, alias, alias_length, dbton_id)))
            DBUG_RETURN(error_num);
          if (str)
          {
            if (str->reserve(SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          }
          if ((error_num = spider_db_print_item_type(item_list[1], spider,
            str, alias, alias_length, dbton_id)))
            DBUG_RETURN(error_num);
          if (str)
          {
            if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
          }
          DBUG_RETURN(0);
#else
          DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
#endif
        }
      } else if (func_name_length == 14)
      {
        if (!strncasecmp("cast_as_binary", func_name, func_name_length))
        {
          if (str)
          {
            char tmp_buf[MAX_FIELD_WIDTH], *tmp_ptr, *tmp_ptr2;
            spider_string tmp_str(tmp_buf, MAX_FIELD_WIDTH, str->charset());
            tmp_str.init_calc_mem(123);
            tmp_str.length(0);
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(SPIDER_SQL_CAST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
#if MYSQL_VERSION_ID < 50500
            item_func->print(tmp_str.get_str(), QT_IS);
#else
            item_func->print(tmp_str.get_str(), QT_TO_SYSTEM_CHARSET);
#endif
            tmp_str.mem_calc();
            if (tmp_str.reserve(1))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            tmp_ptr = tmp_str.c_ptr_quick();
            DBUG_PRINT("info",("spider tmp_ptr = %s", tmp_ptr));
            while ((tmp_ptr2 = strstr(tmp_ptr, SPIDER_SQL_AS_BINARY_STR)))
              tmp_ptr = tmp_ptr2 + 1;
            last_str = tmp_ptr - 1;
            last_str_length = strlen(last_str) - SPIDER_SQL_CLOSE_PAREN_LEN;
          }
          break;
        } else if (!strncasecmp("cast_as_signed", func_name, func_name_length))
        {
          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(SPIDER_SQL_CAST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
          }
          last_str = SPIDER_SQL_AS_SIGNED_STR;
          last_str_length = SPIDER_SQL_AS_SIGNED_LEN;
          break;
        }
      } else if (func_name_length == 16)
      {
        if (!strncasecmp("cast_as_unsigned", func_name, func_name_length))
        {
          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(SPIDER_SQL_CAST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
          }
          last_str = SPIDER_SQL_AS_UNSIGNED_STR;
          last_str_length = SPIDER_SQL_AS_UNSIGNED_LEN;
          break;
        } else if (!strncasecmp("decimal_typecast", func_name,
          func_name_length))
        {
          if (str)
          {
            char tmp_buf[MAX_FIELD_WIDTH], *tmp_ptr, *tmp_ptr2;
            spider_string tmp_str(tmp_buf, MAX_FIELD_WIDTH, str->charset());
            tmp_str.init_calc_mem(124);
            tmp_str.length(0);
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(SPIDER_SQL_CAST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
#if MYSQL_VERSION_ID < 50500
            item_func->print(tmp_str.get_str(), QT_IS);
#else
            item_func->print(tmp_str.get_str(), QT_TO_SYSTEM_CHARSET);
#endif
            tmp_str.mem_calc();
            if (tmp_str.reserve(1))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            tmp_ptr = tmp_str.c_ptr_quick();
            DBUG_PRINT("info",("spider tmp_ptr = %s", tmp_ptr));
            while ((tmp_ptr2 = strstr(tmp_ptr, SPIDER_SQL_AS_DECIMAL_STR)))
              tmp_ptr = tmp_ptr2 + 1;
            last_str = tmp_ptr - 1;
            last_str_length = strlen(last_str) - SPIDER_SQL_CLOSE_PAREN_LEN;
          }
          break;
        } else if (!strncasecmp("cast_as_datetime", func_name,
          func_name_length))
        {
          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(SPIDER_SQL_CAST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
          }
          last_str = SPIDER_SQL_AS_DATETIME_STR;
          last_str_length = SPIDER_SQL_AS_DATETIME_LEN;
          break;
        }
      } else if (func_name_length == 17)
      {
        if (!strncasecmp("date_add_interval", func_name, func_name_length))
        {
          Item_date_add_interval *item_date_add_interval =
            (Item_date_add_interval *) item_func;
          func_name = spider_db_timefunc_interval_str[
            item_date_add_interval->int_type];
          func_name_length = strlen(func_name);
          if ((error_num = spider_db_print_item_type(item_list[0], spider, str,
            alias, alias_length, dbton_id)))
            DBUG_RETURN(error_num);
          if (str)
          {
            if (item_date_add_interval->date_sub_interval)
            {
              if (str->reserve(SPIDER_SQL_NEGINTERVAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_NEGINTERVAL_STR,
                SPIDER_SQL_NEGINTERVAL_LEN);
            } else {
              if (str->reserve(SPIDER_SQL_INTERVAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_INTERVAL_STR, SPIDER_SQL_INTERVAL_LEN);
            }
          }
          if ((error_num = spider_db_print_item_type(item_list[1], spider, str,
            alias, alias_length, dbton_id)))
            DBUG_RETURN(error_num);
          if (str)
          {
            if (str->reserve(func_name_length + SPIDER_SQL_CLOSE_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(func_name, func_name_length);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
          }
          DBUG_RETURN(0);
        }
      }
      if (str)
      {
        if (str->reserve(func_name_length + SPIDER_SQL_OPEN_PAREN_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      }
      func_name = SPIDER_SQL_COMMA_STR;
      func_name_length = SPIDER_SQL_COMMA_LEN;
      separete_str = SPIDER_SQL_COMMA_STR;
      separete_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
    case Item_func::NOW_FUNC:
      if (str)
        str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
      DBUG_RETURN(spider_db_open_item_string(item_func, spider, str,
        alias, alias_length, dbton_id));
    case Item_func::CHAR_TYPECAST_FUNC:
      {
        if (str)
        {
          char tmp_buf[MAX_FIELD_WIDTH], *tmp_ptr, *tmp_ptr2;
          spider_string tmp_str(tmp_buf, MAX_FIELD_WIDTH, str->charset());
          tmp_str.init_calc_mem(125);
          tmp_str.length(0);
          str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          if (str->reserve(SPIDER_SQL_CAST_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
#if MYSQL_VERSION_ID < 50500
          item_func->print(tmp_str.get_str(), QT_IS);
#else
          item_func->print(tmp_str.get_str(), QT_TO_SYSTEM_CHARSET);
#endif
          tmp_str.mem_calc();
          if (tmp_str.reserve(1))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          tmp_ptr = tmp_str.c_ptr_quick();
          DBUG_PRINT("info",("spider tmp_ptr = %s", tmp_ptr));
          while ((tmp_ptr2 = strstr(tmp_ptr, SPIDER_SQL_AS_CHAR_STR)))
            tmp_ptr = tmp_ptr2 + 1;
          last_str = tmp_ptr - 1;
          last_str_length = strlen(last_str) - SPIDER_SQL_CLOSE_PAREN_LEN;
        }
      }
      break;
    case Item_func::NOT_FUNC:
      DBUG_PRINT("info",("spider NOT_FUNC"));
      if (item_list[0]->type() == Item::COND_ITEM)
      {
        DBUG_PRINT("info",("spider item_list[0] is COND_ITEM"));
        Item_cond *item_cond = (Item_cond *) item_list[0];
        if (item_cond->functype() == Item_func::COND_AND_FUNC)
        {
          DBUG_PRINT("info",("spider item_cond is COND_AND_FUNC"));
          List_iterator_fast<Item> lif(*(item_cond->argument_list()));
          bool has_expr_cache_item = FALSE;
          bool has_isnotnull_func = FALSE;
          bool has_other_item = FALSE;
          while((item = lif++))
          {
            if (
              item->type() == Item::EXPR_CACHE_ITEM
            ) {
              DBUG_PRINT("info",("spider EXPR_CACHE_ITEM"));
              has_expr_cache_item = TRUE;
            } else if (
              item->type() == Item::FUNC_ITEM &&
              ((Item_func *) item)->functype() == Item_func::ISNOTNULL_FUNC
            ) {
              DBUG_PRINT("info",("spider ISNOTNULL_FUNC"));
              has_isnotnull_func = TRUE;
            } else {
              DBUG_PRINT("info",("spider has other item"));
              DBUG_PRINT("info",("spider COND type=%d", item->type()));
              has_other_item = TRUE;
            }
          }
          if (has_expr_cache_item && has_isnotnull_func && !has_other_item)
          {
            DBUG_PRINT("info",("spider NOT EXISTS skip"));
            DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
          }
        }
      }
      if (str)
      {
        func_name = (char*) item_func->func_name();
        func_name_length = strlen(func_name);
        if (str->reserve(func_name_length + SPIDER_SQL_SPACE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      }
      break;
    case Item_func::NEG_FUNC:
      if (str)
      {
        func_name = (char*) item_func->func_name();
        func_name_length = strlen(func_name);
        if (str->reserve(func_name_length + SPIDER_SQL_SPACE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      }
      break;
    case Item_func::IN_FUNC:
      if (((Item_func_opt_neg *) item_func)->negated)
      {
        func_name = SPIDER_SQL_NOT_IN_STR;
        func_name_length = SPIDER_SQL_NOT_IN_LEN;
        separete_str = SPIDER_SQL_COMMA_STR;
        separete_str_length = SPIDER_SQL_COMMA_LEN;
        last_str = SPIDER_SQL_CLOSE_PAREN_STR;
        last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      } else {
        func_name = SPIDER_SQL_IN_STR;
        func_name_length = SPIDER_SQL_IN_LEN;
        separete_str = SPIDER_SQL_COMMA_STR;
        separete_str_length = SPIDER_SQL_COMMA_LEN;
        last_str = SPIDER_SQL_CLOSE_PAREN_STR;
        last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      }
      break;
    case Item_func::BETWEEN:
      if (((Item_func_opt_neg *) item_func)->negated)
      {
        func_name = SPIDER_SQL_NOT_BETWEEN_STR;
        func_name_length = SPIDER_SQL_NOT_BETWEEN_LEN;
        separete_str = SPIDER_SQL_AND_STR;
        separete_str_length = SPIDER_SQL_AND_LEN;
      } else {
        func_name = (char*) item_func->func_name();
        func_name_length = strlen(func_name);
        separete_str = SPIDER_SQL_AND_STR;
        separete_str_length = SPIDER_SQL_AND_LEN;
      }
      break;
    case Item_func::FUNC_SP:
    case Item_func::UDF_FUNC:
      use_pushdown_udf = spider_param_use_pushdown_udf(spider->trx->thd,
        spider->share->use_pushdown_udf);
      if (!use_pushdown_udf)
        /*
          This is the default behavior because the remote nodes may deal with
          the function in an unexpected way (e.g. not having the same
          definition). Users can turn it on if they know what they are doing.
        */
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
      if (str)
      {
        func_name = (char*) item_func->func_name();
        func_name_length = strlen(func_name);
        DBUG_PRINT("info",("spider func_name = %s", func_name));
        DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
        if (str->reserve(func_name_length + SPIDER_SQL_OPEN_PAREN_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      }
      func_name = SPIDER_SQL_COMMA_STR;
      func_name_length = SPIDER_SQL_COMMA_LEN;
      separete_str = SPIDER_SQL_COMMA_STR;
      separete_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
#ifdef MARIADB_BASE_VERSION
    case Item_func::XOR_FUNC:
#else
    case Item_func::COND_XOR_FUNC:
#endif
      if (str)
        str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
      DBUG_RETURN(
        spider_db_open_item_cond((Item_cond *) item_func, spider, str,
          alias, alias_length, dbton_id));
    case Item_func::TRIG_COND_FUNC:
      DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
    case Item_func::GUSERVAR_FUNC:
      if (str)
        str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
      if (item_func->result_type() == STRING_RESULT)
        DBUG_RETURN(spider_db_open_item_string(item_func, spider, str,
          alias, alias_length, dbton_id));
      else
        DBUG_RETURN(spider_db_open_item_int(item_func, spider, str,
          alias, alias_length, dbton_id));
    case Item_func::FT_FUNC:
      if (spider_db_check_ft_idx(item_func, spider) == MAX_KEY)
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
      start_item = 1;
      if (str)
      {
        if (str->reserve(SPIDER_SQL_MATCH_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_MATCH_STR, SPIDER_SQL_MATCH_LEN);
      }
      separete_str = SPIDER_SQL_COMMA_STR;
      separete_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
    case Item_func::SP_EQUALS_FUNC:
      if (str)
      {
        func_name = SPIDER_SQL_MBR_EQUAL_STR;
        func_name_length = SPIDER_SQL_MBR_EQUAL_LEN;
        DBUG_PRINT("info",("spider func_name = %s", func_name));
        DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
        if (str->reserve(func_name_length))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
      }
      func_name = SPIDER_SQL_COMMA_STR;
      func_name_length = SPIDER_SQL_COMMA_LEN;
      separete_str = SPIDER_SQL_COMMA_STR;
      separete_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
    case Item_func::SP_DISJOINT_FUNC:
    case Item_func::SP_INTERSECTS_FUNC:
    case Item_func::SP_TOUCHES_FUNC:
    case Item_func::SP_CROSSES_FUNC:
    case Item_func::SP_WITHIN_FUNC:
    case Item_func::SP_CONTAINS_FUNC:
    case Item_func::SP_OVERLAPS_FUNC:
      if (str)
      {
        func_name = (char*) item_func->func_name();
        func_name_length = strlen(func_name);
        DBUG_PRINT("info",("spider func_name = %s", func_name));
        DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
        if (str->reserve(
#ifndef SPIDER_ITEM_GEOFUNC_NAME_HAS_MBR
          SPIDER_SQL_MBR_LEN +
#endif
          func_name_length + SPIDER_SQL_OPEN_PAREN_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
#ifndef SPIDER_ITEM_GEOFUNC_NAME_HAS_MBR
        str->q_append(SPIDER_SQL_MBR_STR, SPIDER_SQL_MBR_LEN);
#endif
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      }
      func_name = SPIDER_SQL_COMMA_STR;
      func_name_length = SPIDER_SQL_COMMA_LEN;
      separete_str = SPIDER_SQL_COMMA_STR;
      separete_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
    case Item_func::EQ_FUNC:
    case Item_func::EQUAL_FUNC:
    case Item_func::NE_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GE_FUNC:
    case Item_func::GT_FUNC:
      if (str)
      {
        func_name = (char*) item_func->func_name();
        func_name_length = strlen(func_name);
      }
      break;
    case Item_func::LIKE_FUNC:
      if (str)
      {
         if (((Item_func_like *)item_func)->negated)
         {
            func_name = SPIDER_SQL_NOT_LIKE_STR;
            func_name_length = SPIDER_SQL_NOT_LIKE_LEN;
         }
         else
         {
            func_name = (char*)item_func->func_name();
            func_name_length = strlen(func_name);
         }
      }
      break;
    default:
      THD *thd = spider->trx->thd;
      SPIDER_SHARE *share = spider->share;
      if (spider_param_skip_default_condition(thd,
        share->skip_default_condition))
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
      if (str)
      {
        func_name = (char*) item_func->func_name();
        func_name_length = strlen(func_name);
      }
      break;
  }
  DBUG_PRINT("info",("spider func_name = %s", func_name));
  DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
  DBUG_PRINT("info",("spider separete_str = %s", separete_str));
  DBUG_PRINT("info",("spider separete_str_length = %d", separete_str_length));
  DBUG_PRINT("info",("spider last_str = %s", last_str));
  DBUG_PRINT("info",("spider last_str_length = %d", last_str_length));
  if (item_count)
  {
    item_count--;
    for (roop_count = start_item; roop_count < item_count; roop_count++)
    {
      item = item_list[roop_count];
      if ((error_num = spider_db_print_item_type(item, spider, str,
        alias, alias_length, dbton_id)))
        DBUG_RETURN(error_num);
      if (roop_count == 1)
      {
        func_name = separete_str;
        func_name_length = separete_str_length;
      }
      if (str)
      {
        if (str->reserve(func_name_length + SPIDER_SQL_SPACE_LEN * 2))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      }
    }
    item = item_list[roop_count];
    if ((error_num = spider_db_print_item_type(item, spider, str,
      alias, alias_length, dbton_id)))
      DBUG_RETURN(error_num);
  }
  if (item_func->functype() == Item_func::FT_FUNC)
  {
    Item_func_match *item_func_match = (Item_func_match *)item_func;
    if (str)
    {
      if (str->reserve(SPIDER_SQL_AGAINST_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_AGAINST_STR, SPIDER_SQL_AGAINST_LEN);
    }
    item = item_list[0];
    if ((error_num = spider_db_print_item_type(item, spider, str,
      alias, alias_length, dbton_id)))
      DBUG_RETURN(error_num);
    if (str)
    {
      if (str->reserve(
        ((item_func_match->flags & FT_BOOL) ?
          SPIDER_SQL_IN_BOOLEAN_MODE_LEN : 0) +
        ((item_func_match->flags & FT_EXPAND) ?
          SPIDER_SQL_WITH_QUERY_EXPANSION_LEN : 0)
      ))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      if (item_func_match->flags & FT_BOOL)
        str->q_append(SPIDER_SQL_IN_BOOLEAN_MODE_STR,
          SPIDER_SQL_IN_BOOLEAN_MODE_LEN);
      if (item_func_match->flags & FT_EXPAND)
        str->q_append(SPIDER_SQL_WITH_QUERY_EXPANSION_STR,
          SPIDER_SQL_WITH_QUERY_EXPANSION_LEN);
    }
  } else if (item_func->functype() == Item_func::UNKNOWN_FUNC)
  {
    if (
      func_name_length == 7 &&
      !strncasecmp("convert", func_name, func_name_length)
    ) {
      if (str)
      {
        Item_func_conv_charset *item_func_conv_charset =
          (Item_func_conv_charset *)item_func;
        CHARSET_INFO *conv_charset = item_func_conv_charset->collation.collation;
        uint cset_length = strlen(conv_charset->csname);
        if (str->reserve(SPIDER_SQL_USING_LEN + cset_length))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_USING_STR, SPIDER_SQL_USING_LEN);
        str->q_append(conv_charset->csname, cset_length);
      }
    }
  }
  if (str)
  {
    if (str->reserve(last_str_length + SPIDER_SQL_CLOSE_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(last_str, last_str_length);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  }
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
int spider_db_mysql_util::open_item_sum_func(
  Item_sum *item_sum,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  uint dbton_id = spider_dbton_mysql.dbton_id;
  uint roop_count, item_count = item_sum->get_arg_count();
  int error_num;
  DBUG_ENTER("spider_db_mysql_util::open_item_sum_func");
  DBUG_PRINT("info",("spider Sumfunctype = %d", item_sum->sum_func()));
  switch (item_sum->sum_func())
  {
    case Item_sum::COUNT_FUNC:
    case Item_sum::SUM_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
      {
        const char *func_name = item_sum->func_name();
        uint func_name_length = strlen(func_name);
        Item *item, **args = item_sum->get_args();
        if (str)
        {
          if (str->reserve(func_name_length))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(func_name, func_name_length);
        }
        if (item_count)
        {
          item_count--;
          for (roop_count = 0; roop_count < item_count; roop_count++)
          {
            item = args[roop_count];
            if ((error_num = spider_db_print_item_type(item, spider, str,
              alias, alias_length, dbton_id)))
              DBUG_RETURN(error_num);
            if (str)
            {
              if (str->reserve(SPIDER_SQL_COMMA_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
            }
          }
          item = args[roop_count];
          if ((error_num = spider_db_print_item_type(item, spider, str,
            alias, alias_length, dbton_id)))
            DBUG_RETURN(error_num);
        }
        if (str)
        {
          if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
            SPIDER_SQL_CLOSE_PAREN_LEN);
        }
      }
      break;
    case Item_sum::COUNT_DISTINCT_FUNC:
    case Item_sum::SUM_DISTINCT_FUNC:
    case Item_sum::AVG_FUNC:
    case Item_sum::AVG_DISTINCT_FUNC:
    case Item_sum::STD_FUNC:
    case Item_sum::VARIANCE_FUNC:
    case Item_sum::SUM_BIT_FUNC:
    case Item_sum::UDF_SUM_FUNC:
    case Item_sum::GROUP_CONCAT_FUNC:
    default:
      DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
  }
  DBUG_RETURN(0);
}
#endif

int spider_db_mysql_util::append_escaped_util(
  spider_string *to,
  String *from
) {
  DBUG_ENTER("spider_db_mysql_util::append_escaped_util");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider from=%s", from->charset()->csname));
  DBUG_PRINT("info",("spider to=%s", to->charset()->csname));
  to->append_escape_string(from->ptr(), from->length());
  DBUG_RETURN(0);
}

spider_mysql_share::spider_mysql_share(
  st_spider_share *share
) : spider_db_share(
  share
),
  table_select(NULL),
  table_select_pos(0),
  key_select(NULL),
  key_select_pos(NULL),
  key_hint(NULL),
  show_table_status(NULL),
  show_records(NULL),
  show_index(NULL),
  table_names_str(NULL),
  db_names_str(NULL),
  db_table_str(NULL),
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  db_table_str_hash_value(NULL),
#endif
  table_nm_max_length(0),
  db_nm_max_length(0),
  column_name_str(NULL),
  same_db_table_name(TRUE),
  first_all_link_idx(-1)
{
  DBUG_ENTER("spider_mysql_share::spider_mysql_share");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_alloc_calc_mem_init(mem_calc, 71);
  spider_alloc_calc_mem(spider_current_trx, mem_calc, sizeof(*this));
  DBUG_VOID_RETURN;
}

spider_mysql_share::~spider_mysql_share()
{
  DBUG_ENTER("spider_mysql_share::~spider_mysql_share");
  DBUG_PRINT("info",("spider this=%p", this));
  if (table_select)
    delete [] table_select;
  if (key_select)
    delete [] key_select;
  if (key_hint)
    delete [] key_hint;
  free_show_table_status();
  free_show_records();
  free_show_index();
  free_column_name_str();
  free_table_names_str();
  if (key_select_pos)
  {
    spider_free(spider_current_trx, key_select_pos, MYF(0));
  }
  spider_free_mem_calc(spider_current_trx, mem_calc_id, sizeof(*this));
  DBUG_VOID_RETURN;
}

int spider_mysql_share::init()
{
  int error_num;
  uint roop_count;
  TABLE_SHARE *table_share = spider_share->table_share;
  uint keys = table_share ? table_share->keys : 0;
  DBUG_ENTER("spider_mysql_share::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(key_select_pos = (int *)
    spider_bulk_alloc_mem(spider_current_trx, 112,
      __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
      &key_select_pos,
        sizeof(int) * keys,
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      &db_table_str_hash_value,
        sizeof(my_hash_value_type) * spider_share->all_link_count,
#endif
      NullS))
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  if (keys > 0 &&
    !(key_hint = new spider_string[keys])
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  for (roop_count = 0; roop_count < keys; roop_count++)
  {
    key_hint[roop_count].init_calc_mem(189);
    key_hint[roop_count].set_charset(spider_share->access_charset);
  }
  DBUG_PRINT("info",("spider key_hint=%p", key_hint));

  if (
    !(table_select = new spider_string[1]) ||
    (keys > 0 &&
      !(key_select = new spider_string[keys])
    ) ||
    (error_num = create_table_names_str()) ||
    (table_share &&
      (
        (error_num = create_column_name_str()) ||
        (error_num = convert_key_hint_str()) ||
        (error_num = append_show_table_status()) ||
        (error_num = append_show_records()) ||
        (error_num = append_show_index())
      )
    )
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  table_select->init_calc_mem(96);
  if (table_share && (error_num = append_table_select()))
    DBUG_RETURN(error_num);

  for (roop_count = 0; roop_count < keys; roop_count++)
  {
    key_select[roop_count].init_calc_mem(97);
    if ((error_num = append_key_select(roop_count)))
      DBUG_RETURN(error_num);
  }

  DBUG_RETURN(error_num);
}

uint spider_mysql_share::get_column_name_length(
  uint field_index
) {
  DBUG_ENTER("spider_mysql_share::get_column_name_length");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(column_name_str[field_index].length());
}

int spider_mysql_share::append_column_name(
  spider_string *str,
  uint field_index
) {
  int error_num;
  DBUG_ENTER("spider_mysql_share::append_column_name");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = spider_db_mysql_utility.append_name(str,
    column_name_str[field_index].ptr(), column_name_str[field_index].length());
  DBUG_RETURN(error_num);
}

int spider_mysql_share::append_column_name_with_alias(
  spider_string *str,
  uint field_index,
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_mysql_share::append_column_name_with_alias");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(
    alias_length +
    column_name_str[field_index].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(alias, alias_length);
  append_column_name(str, field_index);
  DBUG_RETURN(0);
}

int spider_mysql_share::append_table_name(
  spider_string *str,
  int all_link_idx
) {
  const char *db_nm = db_names_str[all_link_idx].ptr();
  uint db_nm_len = db_names_str[all_link_idx].length();
  const char *table_nm = table_names_str[all_link_idx].ptr();
  uint table_nm_len = table_names_str[all_link_idx].length();
  DBUG_ENTER("spider_mysql_share::append_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(db_nm_len + SPIDER_SQL_DOT_LEN + table_nm_len +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  spider_db_mysql_utility.append_name(str, db_nm, db_nm_len);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  spider_db_mysql_utility.append_name(str, table_nm, table_nm_len);
  DBUG_RETURN(0);
}

int spider_mysql_share::append_table_name_with_adjusting(
  spider_string *str,
  int all_link_idx
) {
  const char *db_nm = db_names_str[all_link_idx].ptr();
  uint db_nm_len = db_names_str[all_link_idx].length();
  uint db_nm_max_len = db_nm_max_length;
  const char *table_nm = table_names_str[all_link_idx].ptr();
  uint table_nm_len = table_names_str[all_link_idx].length();
  uint table_nm_max_len = table_nm_max_length;
  DBUG_ENTER("spider_mysql_share::append_table_name_with_adjusting");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_db_mysql_utility.append_name(str, db_nm, db_nm_len);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  spider_db_mysql_utility.append_name(str, table_nm, table_nm_len);
  uint length =
    db_nm_max_len - db_nm_len +
    table_nm_max_len - table_nm_len;
  memset((char *) str->ptr() + str->length(), ' ', length);
  str->length(str->length() + length);
  DBUG_RETURN(0);
}

int spider_mysql_share::append_from_with_adjusted_table_name(
  spider_string *str,
  int *table_name_pos
) {
  const char *db_nm = db_names_str[0].ptr();
  uint db_nm_len = db_names_str[0].length();
  uint db_nm_max_len = db_nm_max_length;
  const char *table_nm = table_names_str[0].ptr();
  uint table_nm_len = table_names_str[0].length();
  uint table_nm_max_len = table_nm_max_length;
  DBUG_ENTER("spider_mysql_share::append_from_with_adjusted_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_FROM_LEN + db_nm_max_length +
    SPIDER_SQL_DOT_LEN + table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
  *table_name_pos = str->length();
  spider_db_mysql_utility.append_name(str, db_nm, db_nm_len);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  spider_db_mysql_utility.append_name(str, table_nm, table_nm_len);
  uint length =
    db_nm_max_len - db_nm_len +
    table_nm_max_len - table_nm_len;
  memset((char *) str->ptr() + str->length(), ' ', length);
  str->length(str->length() + length);
  DBUG_RETURN(0);
}

int spider_mysql_share::create_table_names_str()
{
  int error_num, roop_count;
  uint table_nm_len, db_nm_len;
  spider_string *str, *first_tbl_nm_str, *first_db_nm_str, *first_db_tbl_str;
  char *first_tbl_nm, *first_db_nm;
  uint dbton_id = spider_dbton_mysql.dbton_id;
  DBUG_ENTER("spider_mysql_share::create_table_names_str");
  table_names_str = NULL;
  db_names_str = NULL;
  db_table_str = NULL;
  if (
    !(table_names_str = new spider_string[spider_share->all_link_count]) ||
    !(db_names_str = new spider_string[spider_share->all_link_count]) ||
    !(db_table_str = new spider_string[spider_share->all_link_count])
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }

  same_db_table_name = TRUE;
  first_tbl_nm = spider_share->tgt_table_names[0];
  first_db_nm = spider_share->tgt_dbs[0];
  table_nm_len = spider_share->tgt_table_names_lengths[0];
  db_nm_len = spider_share->tgt_dbs_lengths[0];
  first_tbl_nm_str = &table_names_str[0];
  first_db_nm_str = &db_names_str[0];
  first_db_tbl_str = &db_table_str[0];
  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    table_names_str[roop_count].init_calc_mem(86);
    db_names_str[roop_count].init_calc_mem(87);
    db_table_str[roop_count].init_calc_mem(88);
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
      continue;
    if (first_all_link_idx == -1)
      first_all_link_idx = roop_count;

    str = &table_names_str[roop_count];
    if (
      roop_count != 0 &&
      same_db_table_name &&
      spider_share->tgt_table_names_lengths[roop_count] == table_nm_len &&
      !memcmp(first_tbl_nm, spider_share->tgt_table_names[roop_count],
        table_nm_len)
    ) {
      if (str->copy(*first_tbl_nm_str))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
    } else {
      str->set_charset(spider_share->access_charset);
      if ((error_num = spider_db_append_name_with_quote_str(str,
        spider_share->tgt_table_names[roop_count], dbton_id)))
        goto error;
      if (roop_count)
      {
        same_db_table_name = FALSE;
        DBUG_PRINT("info", ("spider found different table name %s",
          spider_share->tgt_table_names[roop_count]));
        if (str->length() > table_nm_max_length)
          table_nm_max_length = str->length();
      } else
        table_nm_max_length = str->length();
    }

    str = &db_names_str[roop_count];
    if (
      roop_count != 0 &&
      same_db_table_name &&
      spider_share->tgt_dbs_lengths[roop_count] == db_nm_len &&
      !memcmp(first_db_nm, spider_share->tgt_dbs[roop_count],
        db_nm_len)
    ) {
      if (str->copy(*first_db_nm_str))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
    } else {
      str->set_charset(spider_share->access_charset);
      if ((error_num = spider_db_append_name_with_quote_str(str,
        spider_share->tgt_dbs[roop_count], dbton_id)))
        goto error;
      if (roop_count)
      {
        same_db_table_name = FALSE;
        DBUG_PRINT("info", ("spider found different db name %s",
          spider_share->tgt_dbs[roop_count]));
        if (str->length() > db_nm_max_length)
          db_nm_max_length = str->length();
      } else
        db_nm_max_length = str->length();
    }

    str = &db_table_str[roop_count];
    if (
      roop_count != 0 &&
      same_db_table_name
    ) {
      if (str->copy(*first_db_tbl_str))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
    } else {
      str->set_charset(spider_share->access_charset);
      if ((error_num = append_table_name(str, roop_count)))
        goto error;
    }
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    db_table_str_hash_value[roop_count] = my_calc_hash(
      &spider_open_connections, (uchar*) str->ptr(), str->length());
#endif
  }
  DBUG_RETURN(0);

error:
  if (db_table_str)
  {
    delete [] db_table_str;
    db_table_str = NULL;
  }
  if (db_names_str)
  {
    delete [] db_names_str;
    db_names_str = NULL;
  }
  if (table_names_str)
  {
    delete [] table_names_str;
    table_names_str = NULL;
  }
  DBUG_RETURN(error_num);
}

void spider_mysql_share::free_table_names_str()
{
  DBUG_ENTER("spider_mysql_share::free_table_names_str");
  if (db_table_str)
  {
    delete [] db_table_str;
    db_table_str = NULL;
  }
  if (db_names_str)
  {
    delete [] db_names_str;
    db_names_str = NULL;
  }
  if (table_names_str)
  {
    delete [] table_names_str;
    table_names_str = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_share::create_column_name_str()
{
  spider_string *str;
  int error_num;
  Field **field;
  TABLE_SHARE *table_share = spider_share->table_share;
  uint dbton_id = spider_dbton_mysql.dbton_id;
  DBUG_ENTER("spider_mysql_share::create_column_name_str");
  if (
    table_share->fields &&
    !(column_name_str = new spider_string[table_share->fields])
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  for (field = table_share->field, str = column_name_str;
   *field; field++, str++)
  {
    str->init_calc_mem(89);
    str->set_charset(spider_share->access_charset);
    if ((error_num = spider_db_append_name_with_quote_str(str,
      (char *) (*field)->field_name, dbton_id)))
      goto error;
  }
  DBUG_RETURN(0);

error:
  if (column_name_str)
  {
    delete [] column_name_str;
    column_name_str = NULL;
  }
  DBUG_RETURN(error_num);
}

void spider_mysql_share::free_column_name_str()
{
  DBUG_ENTER("spider_mysql_share::free_column_name_str");
  if (column_name_str)
  {
    delete [] column_name_str;
    column_name_str = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_share::convert_key_hint_str()
{
  spider_string *tmp_key_hint;
  int roop_count;
  TABLE_SHARE *table_share = spider_share->table_share;
  DBUG_ENTER("spider_mysql_share::convert_key_hint_str");
  if (spider_share->access_charset->cset != system_charset_info->cset)
  {
    /* need convertion */
    for (roop_count = 0, tmp_key_hint = key_hint;
      roop_count < (int) table_share->keys; roop_count++, tmp_key_hint++)
    {
      tmp_key_hint->length(0);
      if (tmp_key_hint->append(spider_share->key_hint->ptr(),
        spider_share->key_hint->length(), system_charset_info))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  } else {
    for (roop_count = 0, tmp_key_hint = key_hint;
      roop_count < (int) table_share->keys; roop_count++, tmp_key_hint++)
    {
      if (tmp_key_hint->copy(spider_share->key_hint[roop_count]))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }
  DBUG_RETURN(0);
}

int spider_mysql_share::append_show_table_status()
{
  int roop_count;
  spider_string *str;
  uint dbton_id = spider_dbton_mysql.dbton_id;
  DBUG_ENTER("spider_mysql_append_show_table_status");
  if (!(show_table_status =
    new spider_string[2 * spider_share->all_link_count]))
    goto error;

  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    show_table_status[0 + (2 * roop_count)].init_calc_mem(90);
    show_table_status[1 + (2 * roop_count)].init_calc_mem(91);
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
      continue;

    if (
      show_table_status[0 + (2 * roop_count)].reserve(
        SPIDER_SQL_SHOW_TABLE_STATUS_LEN +
        db_names_str[roop_count].length() +
        SPIDER_SQL_LIKE_LEN + table_names_str[roop_count].length() +
        ((SPIDER_SQL_NAME_QUOTE_LEN) * 2) +
        ((SPIDER_SQL_VALUE_QUOTE_LEN) * 2)) ||
      show_table_status[1 + (2 * roop_count)].reserve(
        SPIDER_SQL_SELECT_TABLES_STATUS_LEN +
        db_names_str[roop_count].length() +
        SPIDER_SQL_AND_LEN + SPIDER_SQL_TABLE_NAME_LEN + SPIDER_SQL_EQUAL_LEN +
        table_names_str[roop_count].length() +
        ((SPIDER_SQL_VALUE_QUOTE_LEN) * 4))
    )
      goto error;
    str = &show_table_status[0 + (2 * roop_count)];
    str->q_append(
      SPIDER_SQL_SHOW_TABLE_STATUS_STR, SPIDER_SQL_SHOW_TABLE_STATUS_LEN);
    str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    str->q_append(db_names_str[roop_count].ptr(),
      db_names_str[roop_count].length());
    str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    str->q_append(SPIDER_SQL_LIKE_STR, SPIDER_SQL_LIKE_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(table_names_str[roop_count].ptr(),
      table_names_str[roop_count].length());
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str = &show_table_status[1 + (2 * roop_count)];
    str->q_append(
      SPIDER_SQL_SELECT_TABLES_STATUS_STR,
      SPIDER_SQL_SELECT_TABLES_STATUS_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(db_names_str[roop_count].ptr(),
      db_names_str[roop_count].length());
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
    str->q_append(SPIDER_SQL_TABLE_NAME_STR, SPIDER_SQL_TABLE_NAME_LEN);
    str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(table_names_str[roop_count].ptr(),
      table_names_str[roop_count].length());
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  }
  DBUG_RETURN(0);

error:
  if (show_table_status)
  {
    delete [] show_table_status;
    show_table_status = NULL;
  }
  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
}

void spider_mysql_share::free_show_table_status()
{
  DBUG_ENTER("spider_mysql_free_show_table_status");
  if (show_table_status)
  {
    delete [] show_table_status;
    show_table_status = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_share::append_show_records()
{
  int roop_count;
  spider_string *str;
  uint dbton_id = spider_dbton_mysql.dbton_id;
  DBUG_ENTER("spider_mysql_share::append_show_records");
  if (!(show_records = new spider_string[spider_share->all_link_count]))
    goto error;

  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    show_records[roop_count].init_calc_mem(92);
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
      continue;

    if (
      show_records[roop_count].reserve(
        SPIDER_SQL_SHOW_RECORDS_LEN +
        db_names_str[roop_count].length() +
        SPIDER_SQL_DOT_LEN +
        table_names_str[roop_count].length() +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 4)
    )
      goto error;
    str = &show_records[roop_count];
    str->q_append(SPIDER_SQL_SHOW_RECORDS_STR, SPIDER_SQL_SHOW_RECORDS_LEN);
    append_table_name(str, roop_count);
  }
  DBUG_RETURN(0);

error:
  if (show_records)
  {
    delete [] show_records;
    show_records = NULL;
  }
  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
}

void spider_mysql_share::free_show_records()
{
  DBUG_ENTER("spider_mysql_share::free_show_records");
  if (show_records)
  {
    delete [] show_records;
    show_records = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_share::append_show_index()
{
  int roop_count;
  spider_string *str;
  uint dbton_id = spider_dbton_mysql.dbton_id;
  DBUG_ENTER("spider_mysql_share::append_show_index");
  if (!(show_index = new spider_string[2 * spider_share->all_link_count]))
    goto error;

  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    show_index[0 + (2 * roop_count)].init_calc_mem(93);
    show_index[1 + (2 * roop_count)].init_calc_mem(94);
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
      continue;

    if (
      show_index[0 + (2 * roop_count)].reserve(
        SPIDER_SQL_SHOW_INDEX_LEN + db_names_str[roop_count].length() +
        SPIDER_SQL_DOT_LEN +
        table_names_str[roop_count].length() +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 4) ||
      show_index[1 + (2 * roop_count)].reserve(
        SPIDER_SQL_SELECT_STATISTICS_LEN +
        db_names_str[roop_count].length() +
        SPIDER_SQL_AND_LEN + SPIDER_SQL_TABLE_NAME_LEN + SPIDER_SQL_EQUAL_LEN +
        table_names_str[roop_count].length() +
        ((SPIDER_SQL_VALUE_QUOTE_LEN) * 4) +
        SPIDER_SQL_GROUP_LEN + SPIDER_SQL_COLUMN_NAME_LEN)
    )
      goto error;
    str = &show_index[0 + (2 * roop_count)];
    str->q_append(
      SPIDER_SQL_SHOW_INDEX_STR, SPIDER_SQL_SHOW_INDEX_LEN);
    append_table_name(str, roop_count);
    str = &show_index[1 + (2 * roop_count)];
    str->q_append(
      SPIDER_SQL_SELECT_STATISTICS_STR, SPIDER_SQL_SELECT_STATISTICS_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(db_names_str[roop_count].ptr(),
      db_names_str[roop_count].length());
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
    str->q_append(SPIDER_SQL_TABLE_NAME_STR, SPIDER_SQL_TABLE_NAME_LEN);
    str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(table_names_str[roop_count].ptr(),
      table_names_str[roop_count].length());
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(SPIDER_SQL_GROUP_STR, SPIDER_SQL_GROUP_LEN);
    str->q_append(SPIDER_SQL_COLUMN_NAME_STR, SPIDER_SQL_COLUMN_NAME_LEN);
  }
  DBUG_RETURN(0);

error:
  if (show_index)
  {
    delete [] show_index;
    show_index = NULL;
  }
  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
}

void spider_mysql_share::free_show_index()
{
  DBUG_ENTER("spider_mysql_share::free_show_index");
  if (show_index)
  {
    delete [] show_index;
    show_index = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_share::append_table_select()
{
  Field **field;
  uint field_length;
  spider_string *str = table_select;
  TABLE_SHARE *table_share = spider_share->table_share;
  DBUG_ENTER("spider_mysql_share::append_table_select");
  for (field = table_share->field; *field; field++)
  {
    field_length = column_name_str[(*field)->field_index].length();
    if (str->reserve(field_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    append_column_name(str, (*field)->field_index);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(append_from_with_adjusted_table_name(str, &table_select_pos));
}

int spider_mysql_share::append_key_select(
  uint idx
) {
  KEY_PART_INFO *key_part;
  Field *field;
  uint part_num;
  uint field_length;
  spider_string *str = &key_select[idx];
  TABLE_SHARE *table_share = spider_share->table_share;
  const KEY *key_info = &table_share->key_info[idx];
  DBUG_ENTER("spider_mysql_share::append_key_select");
  for (key_part = key_info->key_part, part_num = 0;
    part_num < spider_user_defined_key_parts(key_info); key_part++, part_num++)
  {
    field = key_part->field;
    field_length = column_name_str[field->field_index].length();
    if (str->reserve(field_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(append_from_with_adjusted_table_name(str, &key_select_pos[idx]));
}

bool spider_mysql_share::need_change_db_table_name()
{
  DBUG_ENTER("spider_mysql_share::need_change_db_table_name");
  DBUG_RETURN(!same_db_table_name);
}

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
int spider_mysql_share::discover_table_structure(
  SPIDER_TRX *trx,
  SPIDER_SHARE *spider_share,
  spider_string *str
) {
  int roop_count, error_num = HA_ERR_WRONG_COMMAND;
  char sql_buf[MAX_FIELD_WIDTH];
  spider_string sql_str(sql_buf, sizeof(sql_buf), system_charset_info);
  uint dbton_id = spider_dbton_mysql.dbton_id;
  uint strlen = str->length();
  DBUG_ENTER("spider_mysql_share::discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  sql_str.init_calc_mem(228);
  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
    {
      DBUG_PRINT("info",("spider spider_share->sql_dbton_ids[%d]=%u",
        roop_count, spider_share->sql_dbton_ids[roop_count]));
      DBUG_PRINT("info",("spider dbton_id=%u", dbton_id));
      continue;
    }

    str->length(strlen);
    sql_str.length(0);
    if (sql_str.reserve(
      SPIDER_SQL_SELECT_COLUMNS_LEN + db_names_str[roop_count].length() +
      SPIDER_SQL_AND_LEN + SPIDER_SQL_TABLE_NAME_LEN + SPIDER_SQL_EQUAL_LEN +
      table_names_str[roop_count].length() + SPIDER_SQL_ORDER_LEN +
      SPIDER_SQL_ORDINAL_POSITION_LEN +
      /* SPIDER_SQL_VALUE_QUOTE_LEN */ 8 +
      SPIDER_SQL_SEMICOLON_LEN +
      SPIDER_SQL_SHOW_INDEX_LEN + db_names_str[roop_count].length() +
      SPIDER_SQL_DOT_LEN + table_names_str[roop_count].length() +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 +
      SPIDER_SQL_SEMICOLON_LEN +
      SPIDER_SQL_SHOW_TABLE_STATUS_LEN + db_names_str[roop_count].length() +
      SPIDER_SQL_LIKE_LEN + table_names_str[roop_count].length() +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 4
    )) {
      DBUG_PRINT("info",("spider alloc sql_str error"));
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    sql_str.q_append(SPIDER_SQL_SELECT_COLUMNS_STR,
      SPIDER_SQL_SELECT_COLUMNS_LEN);
    sql_str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    sql_str.q_append(db_names_str[roop_count].ptr(),
      db_names_str[roop_count].length());
    sql_str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    sql_str.q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
    sql_str.q_append(SPIDER_SQL_TABLE_NAME_STR, SPIDER_SQL_TABLE_NAME_LEN);
    sql_str.q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
    sql_str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    sql_str.q_append(table_names_str[roop_count].ptr(),
      table_names_str[roop_count].length());
    sql_str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    sql_str.q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
    sql_str.q_append(SPIDER_SQL_ORDINAL_POSITION_STR,
      SPIDER_SQL_ORDINAL_POSITION_LEN);
    sql_str.q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
    sql_str.q_append(SPIDER_SQL_SHOW_INDEX_STR, SPIDER_SQL_SHOW_INDEX_LEN);
    append_table_name(&sql_str, roop_count);
    sql_str.q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
    sql_str.q_append(
      SPIDER_SQL_SHOW_TABLE_STATUS_STR, SPIDER_SQL_SHOW_TABLE_STATUS_LEN);
    sql_str.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    sql_str.q_append(db_names_str[roop_count].ptr(),
      db_names_str[roop_count].length());
    sql_str.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    sql_str.q_append(SPIDER_SQL_LIKE_STR, SPIDER_SQL_LIKE_LEN);
    sql_str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    sql_str.q_append(table_names_str[roop_count].ptr(),
      table_names_str[roop_count].length());
    sql_str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);

    SPIDER_CONN *conn;
    int need_mon;
    if (!(conn = spider_get_conn(
      spider_share, 0, spider_share->conn_keys[roop_count], trx, NULL, FALSE,
      FALSE, SPIDER_CONN_KIND_MYSQL, &error_num))
    ) {
      DBUG_RETURN(error_num);
    }
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = &need_mon;
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if (!conn->disable_reconnect)
    {
      ha_spider tmp_spider;
      int need_mon = 0;
      uint tmp_conn_link_idx = 0;
      tmp_spider.trx = trx;
      tmp_spider.share = spider_share;
      tmp_spider.need_mons = &need_mon;
      tmp_spider.conn_link_idx = &tmp_conn_link_idx;
      if ((error_num = spider_db_ping(&tmp_spider, conn, 0)))
      {
        DBUG_PRINT("info",("spider spider_db_ping error"));
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        continue;
      }
    }
    spider_conn_set_timeout_from_share(conn, roop_count, trx->thd,
      spider_share);
    if (
      (error_num = spider_db_set_names_internal(trx, spider_share, conn,
        roop_count, &need_mon)) ||
      (
        spider_db_query(
          conn,
          sql_str.ptr(),
          sql_str.length(),
          -1,
          &need_mon) &&
        (error_num = spider_db_errorno(conn))
      )
    ) {
      DBUG_PRINT("info",("spider spider_get_trx error"));
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      continue;
    }
    st_spider_db_request_key request_key;
    request_key.spider_thread_id = trx->spider_thread_id;
    request_key.query_id = trx->thd->query_id;
    request_key.handler = NULL;
    request_key.request_id = 1;
    request_key.next = NULL;
    spider_db_result *res;
    /* get column list */
    if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
    {
      if (error_num || (error_num = spider_db_errorno(conn)))
      {
        DBUG_PRINT("info",("spider column store error"));
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        continue;
      }
      /* no record */
      DBUG_PRINT("info",("spider column no record error"));
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      continue;
    }
    if ((error_num = res->fetch_columns_for_discover_table_structure(str,
      spider_share->access_charset)))
    {
      DBUG_PRINT("info",("spider column fetch error"));
      res->free_result();
      delete res;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      my_printf_error(ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM,
        ER_SPIDER_REMOTE_TABLE_NOT_FOUND_STR, MYF(0),
        db_names_str[roop_count].ptr(),
        table_names_str[roop_count].ptr());
      error_num = ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM;
      continue;
    }
    res->free_result();
    delete res;
    if (conn->db_conn->next_result())
    {
      DBUG_PRINT("info",("spider single result error"));
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      continue;
    }
    /* get index list */
    if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
    {
      if (error_num || (error_num = spider_db_errorno(conn)))
      {
        DBUG_PRINT("info",("spider index store error"));
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        continue;
      }
      /* no record */
      DBUG_PRINT("info",("spider index no record error"));
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      continue;
    }
    if ((error_num = res->fetch_index_for_discover_table_structure(str,
      spider_share->access_charset)))
    {
      DBUG_PRINT("info",("spider index fetch error"));
      res->free_result();
      delete res;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      continue;
    }
    res->free_result();
    delete res;
    if (conn->db_conn->next_result())
    {
      DBUG_PRINT("info",("spider dual result error"));
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      continue;
    }
    /* get table info */
    if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
    {
      if (error_num || (error_num = spider_db_errorno(conn)))
      {
        DBUG_PRINT("info",("spider table store error"));
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        continue;
      }
      /* no record */
      DBUG_PRINT("info",("spider table no record error"));
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      continue;
    }
    if ((error_num = res->fetch_table_for_discover_table_structure(str,
      spider_share, spider_share->access_charset)))
    {
      DBUG_PRINT("info",("spider table fetch error"));
      res->free_result();
      delete res;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      continue;
    }
    res->free_result();
    delete res;
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    if (!error_num)
      break;
  }
  DBUG_RETURN(error_num);
}
#endif

spider_mysql_handler::spider_mysql_handler(
  ha_spider *spider,
  spider_mysql_share *db_share
) : spider_db_handler(
  spider,
  db_share
),
  where_pos(0),
  order_pos(0),
  limit_pos(0),
  table_name_pos(0),
  ha_read_pos(0),
  ha_next_pos(0),
  ha_where_pos(0),
  ha_limit_pos(0),
  ha_table_name_pos(0),
  insert_pos(0),
  insert_table_name_pos(0),
  upd_tmp_tbl(NULL),
  tmp_sql_pos1(0),
  tmp_sql_pos2(0),
  tmp_sql_pos3(0),
  tmp_sql_pos4(0),
  tmp_sql_pos5(0),
  reading_from_bulk_tmp_table(FALSE),
  union_table_name_pos_first(NULL),
  union_table_name_pos_current(NULL),
  mysql_share(db_share),
  link_for_hash(NULL)
{
  DBUG_ENTER("spider_mysql_handler::spider_mysql_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_alloc_calc_mem_init(mem_calc, 183);
  spider_alloc_calc_mem(spider_current_trx, mem_calc, sizeof(*this));
  DBUG_VOID_RETURN;
}

spider_mysql_handler::~spider_mysql_handler()
{
  DBUG_ENTER("spider_mysql_handler::~spider_mysql_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  while (union_table_name_pos_first)
  {
    SPIDER_INT_HLD *tmp_pos = union_table_name_pos_first;
    union_table_name_pos_first = tmp_pos->next;
    spider_free(spider_current_trx, tmp_pos, MYF(0));
  }
  if (link_for_hash)
  {
    spider_free(spider_current_trx, link_for_hash, MYF(0));
  }
  spider_free_mem_calc(spider_current_trx, mem_calc_id, sizeof(*this));
  DBUG_VOID_RETURN;
}

int spider_mysql_handler::init()
{
  uint roop_count;
  THD *thd = spider->trx->thd;
  st_spider_share *share = spider->share;
  int init_sql_alloc_size =
    spider_param_init_sql_alloc_size(thd, share->init_sql_alloc_size);
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_mysql_handler::init");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.init_calc_mem(59);
  sql_part.init_calc_mem(60);
  sql_part2.init_calc_mem(61);
  ha_sql.init_calc_mem(62);
  insert_sql.init_calc_mem(64);
  update_sql.init_calc_mem(65);
  tmp_sql.init_calc_mem(66);
  dup_update_sql.init_calc_mem(166);
  if (
    (sql.real_alloc(init_sql_alloc_size)) ||
    (insert_sql.real_alloc(init_sql_alloc_size)) ||
    (update_sql.real_alloc(init_sql_alloc_size)) ||
    (tmp_sql.real_alloc(init_sql_alloc_size))
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  sql.set_charset(share->access_charset);
  sql_part.set_charset(share->access_charset);
  ha_sql.set_charset(share->access_charset);
  insert_sql.set_charset(share->access_charset);
  update_sql.set_charset(share->access_charset);
  tmp_sql.set_charset(share->access_charset);
  upd_tmp_tbl_prm.init();
  upd_tmp_tbl_prm.field_count = 1;
  if (!(link_for_hash = (SPIDER_LINK_FOR_HASH *)
    spider_bulk_alloc_mem(spider_current_trx, 141,
      __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
      &link_for_hash,
        sizeof(SPIDER_LINK_FOR_HASH) * share->link_count,
      &minimum_select_bitmap,
        table ? sizeof(uchar) * no_bytes_in_map(table->read_set) : 0,
      NullS))
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  for (roop_count = 0; roop_count < share->link_count; roop_count++)
  {
    link_for_hash[roop_count].spider = spider;
    link_for_hash[roop_count].link_idx = roop_count;
    link_for_hash[roop_count].db_table_str =
      &mysql_share->db_table_str[roop_count];
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    link_for_hash[roop_count].db_table_str_hash_value =
      mysql_share->db_table_str_hash_value[roop_count];
#endif
  }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  hs_upds.init();
#endif
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_table_name_with_adjusting(
  spider_string *str,
  int link_idx,
  ulong sql_type
) {
  int error_num = 0;
  DBUG_ENTER("spider_mysql_handler::append_table_name_with_adjusting");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type == SPIDER_SQL_TYPE_HANDLER)
  {
    str->q_append(spider->m_handler_cid[link_idx], SPIDER_SQL_HANDLER_CID_LEN);
  } else {
    error_num = mysql_share->append_table_name_with_adjusting(str,
      spider->conn_link_idx[link_idx]);
  }
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_key_column_types(
  const key_range *start_key,
  spider_string *str
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  KEY *key_info = result_list->key_info;
  uint key_name_length, key_count;
  key_part_map full_key_part_map =
    make_prev_keypart_map(spider_user_defined_key_parts(key_info));
  key_part_map start_key_part_map;
  KEY_PART_INFO *key_part;
  Field *field;
  char tmp_buf[MAX_FIELD_WIDTH];
  spider_string tmp_str(tmp_buf, sizeof(tmp_buf), system_charset_info);
  DBUG_ENTER("spider_mysql_handler::append_key_column_types");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_str.init_calc_mem(115);

  start_key_part_map = start_key->keypart_map & full_key_part_map;
  DBUG_PRINT("info", ("spider spider_user_defined_key_parts=%u",
    spider_user_defined_key_parts(key_info)));
  DBUG_PRINT("info", ("spider full_key_part_map=%lu", full_key_part_map));
  DBUG_PRINT("info", ("spider start_key_part_map=%lu", start_key_part_map));

  if (!start_key_part_map)
    DBUG_RETURN(0);

  for (
    key_part = key_info->key_part,
    key_count = 0;
    start_key_part_map;
    start_key_part_map >>= 1,
    key_part++,
    key_count++
  ) {
    field = key_part->field;
    key_name_length = my_sprintf(tmp_buf, (tmp_buf, "c%u", key_count));
    if (str->reserve(key_name_length + SPIDER_SQL_SPACE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(tmp_buf, key_name_length);
    str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);

    if (tmp_str.ptr() != tmp_buf)
      tmp_str.set(tmp_buf, sizeof(tmp_buf), system_charset_info);
    else
      tmp_str.set_charset(system_charset_info);
    field->sql_type(*tmp_str.get_str());
    tmp_str.mem_calc();
    str->append(tmp_str);
    if (field->has_charset())
    {
      CHARSET_INFO *cs = field->charset();
      uint coll_length = strlen(cs->name);
      if (str->reserve(SPIDER_SQL_COLLATE_LEN + coll_length))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_COLLATE_STR, SPIDER_SQL_COLLATE_LEN);
      str->q_append(cs->name, coll_length);
    }

    if (str->reserve(SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);

  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_join_columns_for_bka(
  const key_range *start_key,
  spider_string *str,
  const char **table_aliases,
  uint *table_alias_lengths
) {
  KEY *key_info = spider->result_list.key_info;
  uint length, key_name_length, key_count;
  key_part_map full_key_part_map =
    make_prev_keypart_map(spider_user_defined_key_parts(key_info));
  key_part_map start_key_part_map;
  KEY_PART_INFO *key_part;
  Field *field;
  char tmp_buf[MAX_FIELD_WIDTH];
  bool start_where = ((int) str->length() == where_pos);
  DBUG_ENTER("spider_mysql_handler::append_key_join_columns_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  start_key_part_map = start_key->keypart_map & full_key_part_map;
  DBUG_PRINT("info", ("spider spider_user_defined_key_parts=%u",
    spider_user_defined_key_parts(key_info)));
  DBUG_PRINT("info", ("spider full_key_part_map=%lu", full_key_part_map));
  DBUG_PRINT("info", ("spider start_key_part_map=%lu", start_key_part_map));

  if (!start_key_part_map)
    DBUG_RETURN(0);

  if (start_where)
  {
    if (str->reserve(SPIDER_SQL_WHERE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
  } else {
    if (str->reserve(SPIDER_SQL_AND_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
  }

  for (
    key_part = key_info->key_part,
    key_count = 0;
    start_key_part_map;
    start_key_part_map >>= 1,
    key_part++,
    key_count++
  ) {
    field = key_part->field;
    key_name_length =
      mysql_share->column_name_str[field->field_index].length();
    length = my_sprintf(tmp_buf, (tmp_buf, "c%u", key_count));
    if (str->reserve(length + table_alias_lengths[0] + key_name_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
      table_alias_lengths[1] + SPIDER_SQL_PF_EQUAL_LEN + SPIDER_SQL_AND_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(table_aliases[0], table_alias_lengths[0]);
    str->q_append(tmp_buf, length);
    str->q_append(SPIDER_SQL_PF_EQUAL_STR, SPIDER_SQL_PF_EQUAL_LEN);
    str->q_append(table_aliases[1], table_alias_lengths[1]);
    mysql_share->append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
  }
  str->length(str->length() - SPIDER_SQL_AND_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_tmp_table_and_sql_for_bka(
  const key_range *start_key
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_tmp_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  char tmp_table_name[MAX_FIELD_WIDTH * 2],
    tgt_table_name[MAX_FIELD_WIDTH * 2];
  int tmp_table_name_length;
  spider_string tgt_table_name_str(tgt_table_name, MAX_FIELD_WIDTH * 2,
    mysql_share->db_names_str[0].charset());
  const char *table_names[2], *table_aliases[2], *table_dot_aliases[2];
  uint table_name_lengths[2], table_alias_lengths[2],
    table_dot_alias_lengths[2];
  tgt_table_name_str.init_calc_mem(99);
  tgt_table_name_str.length(0);
  create_tmp_bka_table_name(tmp_table_name, &tmp_table_name_length,
    first_link_idx);
  if ((error_num = append_table_name_with_adjusting(&tgt_table_name_str,
    first_link_idx, SPIDER_SQL_TYPE_SELECT_SQL)))
  {
    DBUG_RETURN(error_num);
  }
  table_names[0] = tmp_table_name;
  table_names[1] = tgt_table_name_str.c_ptr_safe();
  table_name_lengths[0] = tmp_table_name_length;
  table_name_lengths[1] = tgt_table_name_str.length();
  table_aliases[0] = SPIDER_SQL_A_STR;
  table_aliases[1] = SPIDER_SQL_B_STR;
  table_alias_lengths[0] = SPIDER_SQL_A_LEN;
  table_alias_lengths[1] = SPIDER_SQL_B_LEN;
  table_dot_aliases[0] = SPIDER_SQL_A_DOT_STR;
  table_dot_aliases[1] = SPIDER_SQL_B_DOT_STR;
  table_dot_alias_lengths[0] = SPIDER_SQL_A_DOT_LEN;
  table_dot_alias_lengths[1] = SPIDER_SQL_B_DOT_LEN;
  if (
    (error_num = append_drop_tmp_bka_table(
      &tmp_sql, tmp_table_name, tmp_table_name_length,
      &tmp_sql_pos1, &tmp_sql_pos5, TRUE)) ||
    (error_num = append_create_tmp_bka_table(
      start_key,
      &tmp_sql, tmp_table_name,
      tmp_table_name_length,
      &tmp_sql_pos2, spider->share->table_share->table_charset)) ||
    (error_num = append_insert_tmp_bka_table(
      start_key,
      &tmp_sql, tmp_table_name,
      tmp_table_name_length, &tmp_sql_pos3))
  )
    DBUG_RETURN(error_num);
  tmp_sql_pos4 = tmp_sql.length();
  if ((error_num = spider_db_append_select(spider)))
    DBUG_RETURN(error_num);
  if (sql.reserve(SPIDER_SQL_A_DOT_LEN + SPIDER_SQL_ID_LEN +
    SPIDER_SQL_COMMA_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_A_DOT_STR, SPIDER_SQL_A_DOT_LEN);
  sql.q_append(SPIDER_SQL_ID_STR, SPIDER_SQL_ID_LEN);
  sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  if (
    (error_num = append_select_columns_with_alias(&sql,
      SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN)) ||
    (error_num = spider_db_mysql_utility.append_from_with_alias(&sql,
      table_names, table_name_lengths,
      table_aliases, table_alias_lengths, 2,
      &table_name_pos, FALSE))
  )
    DBUG_RETURN(error_num);
  if (
    mysql_share->key_hint &&
    (error_num = spider_db_append_hint_after_table(spider,
      &sql, &mysql_share->key_hint[spider->active_index]))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  where_pos = sql.length();
  if (
    (error_num = append_key_join_columns_for_bka(
      start_key, &sql,
      table_dot_aliases, table_dot_alias_lengths)) ||
    (error_num = append_condition_part(
      SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN,
      SPIDER_SQL_TYPE_SELECT_SQL, FALSE))
  )
    DBUG_RETURN(error_num);
  if (spider->result_list.direct_order_limit)
  {
    if ((error_num = append_key_order_for_direct_order_limit_with_alias(&sql,
      SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN)))
      DBUG_RETURN(error_num);
  }
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  else if (spider->result_list.direct_aggregate)
  {
    if ((error_num =
      append_group_by(&sql, SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN)))
      DBUG_RETURN(error_num);
  }
#endif

  DBUG_RETURN(0);
}

int spider_mysql_handler::reuse_tmp_table_and_sql_for_bka()
{
  DBUG_ENTER("spider_mysql_handler::reuse_tmp_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_sql.length(tmp_sql_pos4);
  sql.length(limit_pos);
  ha_sql.length(ha_limit_pos);
  DBUG_RETURN(0);
}

void spider_mysql_handler::create_tmp_bka_table_name(
  char *tmp_table_name,
  int *tmp_table_name_length,
  int link_idx
) {
  uint adjust_length, length;
  DBUG_ENTER("spider_mysql_handler::create_tmp_bka_table_name");
  if (spider_param_bka_table_name_type(current_thd,
    mysql_share->spider_share->
      bka_table_name_types[spider->conn_link_idx[link_idx]]) == 1)
  {
    adjust_length =
      mysql_share->db_nm_max_length -
      mysql_share->db_names_str[spider->conn_link_idx[link_idx]].length() +
      mysql_share->table_nm_max_length -
      mysql_share->table_names_str[spider->conn_link_idx[link_idx]].length();
    *tmp_table_name_length = mysql_share->db_nm_max_length +
      mysql_share->table_nm_max_length;
    memset(tmp_table_name, ' ', adjust_length);
    tmp_table_name += adjust_length;
    memcpy(tmp_table_name, mysql_share->db_names_str[link_idx].c_ptr(),
      mysql_share->db_names_str[link_idx].length());
    tmp_table_name += mysql_share->db_names_str[link_idx].length();
    length = my_sprintf(tmp_table_name, (tmp_table_name,
      "%s%s%p%s", SPIDER_SQL_DOT_STR, SPIDER_SQL_TMP_BKA_STR, spider,
      SPIDER_SQL_UNDERSCORE_STR));
    *tmp_table_name_length += length;
    tmp_table_name += length;
    memcpy(tmp_table_name,
      mysql_share->table_names_str[spider->conn_link_idx[link_idx]].c_ptr(),
      mysql_share->table_names_str[spider->conn_link_idx[link_idx]].length());
  } else {
    adjust_length =
      mysql_share->db_nm_max_length -
      mysql_share->db_names_str[spider->conn_link_idx[link_idx]].length();
    *tmp_table_name_length = mysql_share->db_nm_max_length;
    memset(tmp_table_name, ' ', adjust_length);
    tmp_table_name += adjust_length;
    memcpy(tmp_table_name, mysql_share->db_names_str[link_idx].c_ptr(),
      mysql_share->db_names_str[link_idx].length());
    tmp_table_name += mysql_share->db_names_str[link_idx].length();
    length = my_sprintf(tmp_table_name, (tmp_table_name,
      "%s%s%p", SPIDER_SQL_DOT_STR, SPIDER_SQL_TMP_BKA_STR, spider));
    *tmp_table_name_length += length;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_handler::append_create_tmp_bka_table(
  const key_range *start_key,
  spider_string *str,
  char *tmp_table_name,
  int tmp_table_name_length,
  int *db_name_pos,
  CHARSET_INFO *table_charset
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  THD *thd = spider->trx->thd;
  char *bka_engine = spider_param_bka_engine(thd, share->bka_engine);
  uint bka_engine_length = strlen(bka_engine),
    cset_length = strlen(table_charset->csname),
    coll_length = strlen(table_charset->name);
  DBUG_ENTER("spider_mysql_handler::append_create_tmp_bka_table");
  if (str->reserve(SPIDER_SQL_CREATE_TMP_LEN + tmp_table_name_length +
    SPIDER_SQL_OPEN_PAREN_LEN + SPIDER_SQL_ID_LEN + SPIDER_SQL_ID_TYPE_LEN +
    SPIDER_SQL_COMMA_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_CREATE_TMP_STR, SPIDER_SQL_CREATE_TMP_LEN);
  *db_name_pos = str->length();
  str->q_append(tmp_table_name, tmp_table_name_length);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  str->q_append(SPIDER_SQL_ID_STR, SPIDER_SQL_ID_LEN);
  str->q_append(SPIDER_SQL_ID_TYPE_STR, SPIDER_SQL_ID_TYPE_LEN);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  if ((error_num = append_key_column_types(start_key, str)))
    DBUG_RETURN(error_num);
  if (str->reserve(SPIDER_SQL_ENGINE_LEN + bka_engine_length +
    SPIDER_SQL_DEF_CHARSET_LEN + cset_length + SPIDER_SQL_COLLATE_LEN +
    coll_length + SPIDER_SQL_SEMICOLON_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_ENGINE_STR, SPIDER_SQL_ENGINE_LEN);
  str->q_append(bka_engine, bka_engine_length);
  str->q_append(SPIDER_SQL_DEF_CHARSET_STR, SPIDER_SQL_DEF_CHARSET_LEN);
  str->q_append(table_charset->csname, cset_length);
  str->q_append(SPIDER_SQL_COLLATE_STR, SPIDER_SQL_COLLATE_LEN);
  str->q_append(table_charset->name, coll_length);
  str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_drop_tmp_bka_table(
  spider_string *str,
  char *tmp_table_name,
  int tmp_table_name_length,
  int *db_name_pos,
  int *drop_table_end_pos,
  bool with_semicolon
) {
  DBUG_ENTER("spider_mysql_handler::append_drop_tmp_bka_table");
  if (str->reserve(SPIDER_SQL_DROP_TMP_LEN + tmp_table_name_length +
    (with_semicolon ? SPIDER_SQL_SEMICOLON_LEN : 0)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_DROP_TMP_STR, SPIDER_SQL_DROP_TMP_LEN);
  *db_name_pos = str->length();
  str->q_append(tmp_table_name, tmp_table_name_length);
  *drop_table_end_pos = str->length();
  if (with_semicolon)
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_insert_tmp_bka_table(
  const key_range *start_key,
  spider_string *str,
  char *tmp_table_name,
  int tmp_table_name_length,
  int *db_name_pos
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_insert_tmp_bka_table");
  if (str->reserve(SPIDER_SQL_INSERT_LEN + SPIDER_SQL_INTO_LEN +
    tmp_table_name_length + SPIDER_SQL_OPEN_PAREN_LEN + SPIDER_SQL_ID_LEN +
    SPIDER_SQL_COMMA_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_INSERT_STR, SPIDER_SQL_INSERT_LEN);
  str->q_append(SPIDER_SQL_INTO_STR, SPIDER_SQL_INTO_LEN);
  *db_name_pos = str->length();
  str->q_append(tmp_table_name, tmp_table_name_length);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  str->q_append(SPIDER_SQL_ID_STR, SPIDER_SQL_ID_LEN);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  if ((error_num = spider_db_append_key_columns(start_key, spider, str)))
    DBUG_RETURN(error_num);
  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_VALUES_LEN +
    SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  str->q_append(SPIDER_SQL_VALUES_STR, SPIDER_SQL_VALUES_LEN);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_union_table_and_sql_for_bka(
  const key_range *start_key
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_union_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  char tgt_table_name[MAX_FIELD_WIDTH * 2];
  spider_string tgt_table_name_str(tgt_table_name, MAX_FIELD_WIDTH * 2,
    mysql_share->db_names_str[0].charset());
  const char *table_names[2], *table_aliases[2], *table_dot_aliases[2];
  uint table_name_lengths[2], table_alias_lengths[2],
    table_dot_alias_lengths[2];
  tgt_table_name_str.init_calc_mem(233);
  tgt_table_name_str.length(0);
  if ((error_num = append_table_name_with_adjusting(&tgt_table_name_str,
    first_link_idx, SPIDER_SQL_TYPE_SELECT_SQL)))
  {
    DBUG_RETURN(error_num);
  }
  table_names[0] = "";
  table_names[1] = tgt_table_name_str.c_ptr_safe();
  table_name_lengths[0] = 0;
  table_name_lengths[1] = tgt_table_name_str.length();
  table_aliases[0] = SPIDER_SQL_A_STR;
  table_aliases[1] = SPIDER_SQL_B_STR;
  table_alias_lengths[0] = SPIDER_SQL_A_LEN;
  table_alias_lengths[1] = SPIDER_SQL_B_LEN;
  table_dot_aliases[0] = SPIDER_SQL_A_DOT_STR;
  table_dot_aliases[1] = SPIDER_SQL_B_DOT_STR;
  table_dot_alias_lengths[0] = SPIDER_SQL_A_DOT_LEN;
  table_dot_alias_lengths[1] = SPIDER_SQL_B_DOT_LEN;

  if ((error_num = spider_db_append_select(spider)))
    DBUG_RETURN(error_num);
  if (sql.reserve(SPIDER_SQL_A_DOT_LEN + SPIDER_SQL_ID_LEN +
    SPIDER_SQL_COMMA_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_A_DOT_STR, SPIDER_SQL_A_DOT_LEN);
  sql.q_append(SPIDER_SQL_ID_STR, SPIDER_SQL_ID_LEN);
  sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  if ((error_num = append_select_columns_with_alias(&sql,
    SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN)))
    DBUG_RETURN(error_num);
  if (sql.reserve(SPIDER_SQL_FROM_LEN + (SPIDER_SQL_OPEN_PAREN_LEN * 2)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  tmp_sql_pos1 = sql.length();

  if (
    (error_num = spider_db_mysql_utility.append_from_with_alias(&tmp_sql,
      table_names, table_name_lengths,
      table_aliases, table_alias_lengths, 2,
      &table_name_pos, FALSE))
  )
    DBUG_RETURN(error_num);
  if (
    mysql_share->key_hint &&
    (error_num = spider_db_append_hint_after_table(spider,
      &tmp_sql, &mysql_share->key_hint[spider->active_index]))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  where_pos = tmp_sql.length();
  if (
    (error_num = append_key_join_columns_for_bka(
      start_key, &tmp_sql,
      table_dot_aliases, table_dot_alias_lengths)) ||
    (error_num = append_condition_part(
      SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN,
      SPIDER_SQL_TYPE_TMP_SQL, FALSE))
  )
    DBUG_RETURN(error_num);
  if (spider->result_list.direct_order_limit)
  {
    if ((error_num =
      append_key_order_for_direct_order_limit_with_alias(&tmp_sql,
        SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN))
    )
      DBUG_RETURN(error_num);
  }
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  else if (spider->result_list.direct_aggregate)
  {
    if ((error_num =
      append_group_by(&tmp_sql, SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN)))
      DBUG_RETURN(error_num);
  }
#endif

  DBUG_RETURN(0);
}

int spider_mysql_handler::reuse_union_table_and_sql_for_bka()
{
  DBUG_ENTER("spider_mysql_handler::reuse_union_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.length(tmp_sql_pos1);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_insert_for_recovery(
  ulong sql_type,
  int link_idx
) {
  const TABLE *table = spider->get_table();
  SPIDER_SHARE *share = spider->share;
  Field **field;
  uint field_name_length = 0;
  bool add_value = FALSE;
  spider_string *insert_sql;
  DBUG_ENTER("spider_mysql_handler::append_insert_for_recovery");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type == SPIDER_SQL_TYPE_INSERT_SQL)
  {
    insert_sql = &spider->result_list.insert_sqls[link_idx];
    insert_sql->length(0);
  } else {
    insert_sql = &spider->result_list.update_sqls[link_idx];
  }
  if (insert_sql->reserve(
    SPIDER_SQL_INSERT_LEN + SPIDER_SQL_SQL_IGNORE_LEN +
    SPIDER_SQL_INTO_LEN + mysql_share->db_nm_max_length +
    SPIDER_SQL_DOT_LEN + mysql_share->table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  insert_sql->q_append(SPIDER_SQL_INSERT_STR, SPIDER_SQL_INSERT_LEN);
  insert_sql->q_append(SPIDER_SQL_SQL_IGNORE_STR, SPIDER_SQL_SQL_IGNORE_LEN);
  insert_sql->q_append(SPIDER_SQL_INTO_STR, SPIDER_SQL_INTO_LEN);
  mysql_share->append_table_name(insert_sql, spider->conn_link_idx[link_idx]);
  insert_sql->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  for (field = table->field; *field; field++)
  {
    field_name_length =
      mysql_share->column_name_str[(*field)->field_index].length();
    if (insert_sql->reserve(field_name_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    mysql_share->append_column_name(insert_sql, (*field)->field_index);
    insert_sql->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  if (field_name_length)
    insert_sql->length(insert_sql->length() - SPIDER_SQL_COMMA_LEN);
  if (insert_sql->reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_VALUES_LEN +
    SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  insert_sql->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  insert_sql->q_append(SPIDER_SQL_VALUES_STR, SPIDER_SQL_VALUES_LEN);
  insert_sql->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  for (field = table->field; *field; field++)
  {
    add_value = TRUE;
    if ((*field)->is_null())
    {
      if (insert_sql->reserve(SPIDER_SQL_NULL_LEN + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      insert_sql->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
    } else {
      if (
        spider_db_mysql_utility.
          append_column_value(spider, insert_sql, *field, NULL,
            share->access_charset) ||
        insert_sql->reserve(SPIDER_SQL_COMMA_LEN)
      )
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    insert_sql->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  if (add_value)
    insert_sql->length(insert_sql->length() - SPIDER_SQL_COMMA_LEN);
  if (insert_sql->reserve(SPIDER_SQL_CLOSE_PAREN_LEN, SPIDER_SQL_COMMA_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  insert_sql->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  if (sql_type == SPIDER_SQL_TYPE_INSERT_SQL)
  {
    exec_insert_sql = insert_sql;
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_update(
  const TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  int error_num;
  spider_string *str = &update_sql;
  DBUG_ENTER("spider_mysql_handler::append_update");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->length() > 0)
  {
    if (str->reserve(SPIDER_SQL_SEMICOLON_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }

  if (
    (error_num = append_update(str, 0)) ||
    (error_num = append_update_set(str)) ||
    (error_num = append_update_where(str, table, ptr_diff))
  )
    DBUG_RETURN(error_num);
  filled_up = (str->length() >= (uint) spider->result_list.bulk_update_size);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_update(
  const TABLE *table,
  my_ptrdiff_t ptr_diff,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.update_sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::append_update");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->length() > 0)
  {
    if (str->reserve(SPIDER_SQL_SEMICOLON_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }

  if (
    (error_num = append_update(str, link_idx)) ||
    (error_num = append_update_set(str)) ||
    (error_num = append_update_where(str, table, ptr_diff))
  )
    DBUG_RETURN(error_num);

  if (
    spider->pk_update &&
    share->link_statuses[link_idx] == SPIDER_LINK_STATUS_RECOVERY
  ) {
    if (str->reserve(SPIDER_SQL_SEMICOLON_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
    if ((error_num = append_insert_for_recovery(
      SPIDER_SQL_TYPE_UPDATE_SQL, link_idx)))
      DBUG_RETURN(error_num);
  }

  if (!filled_up)
    filled_up = (str->length() >= (uint) spider->result_list.bulk_update_size);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_delete(
  const TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  int error_num;
  spider_string *str = &update_sql;
  DBUG_ENTER("spider_mysql_handler::append_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->length() > 0)
  {
    if (str->reserve(SPIDER_SQL_SEMICOLON_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }

  if (
    (error_num = append_delete(str)) ||
    (error_num = append_from(str, SPIDER_SQL_TYPE_DELETE_SQL,
      first_link_idx)) ||
    (error_num = append_update_where(str, table, ptr_diff))
  )
    DBUG_RETURN(error_num);
  filled_up = (str->length() >= (uint) spider->result_list.bulk_update_size);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_delete(
  const TABLE *table,
  my_ptrdiff_t ptr_diff,
  int link_idx
) {
  int error_num;
  spider_string *str = &spider->result_list.update_sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::append_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->length() > 0)
  {
    if (str->reserve(SPIDER_SQL_SEMICOLON_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }

  if (
    (error_num = append_delete(str)) ||
    (error_num = append_from(str, SPIDER_SQL_TYPE_DELETE_SQL, link_idx)) ||
    (error_num = append_update_where(str, table, ptr_diff))
  )
    DBUG_RETURN(error_num);
  if (!filled_up)
    filled_up = (str->length() >= (uint) spider->result_list.bulk_update_size);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_insert_part()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_insert_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_insert(&insert_sql, 0);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_insert(
  spider_string *str,
  int link_idx
) {
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_mysql_handler::append_insert");
  if (
    (
      spider->write_can_replace ||
      /* for direct_dup_insert without patch for partition */
      spider->sql_command == SQLCOM_REPLACE ||
      spider->sql_command == SQLCOM_REPLACE_SELECT
    ) &&
    spider->direct_dup_insert
  ) {
    if (str->reserve(SPIDER_SQL_REPLACE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_REPLACE_STR, SPIDER_SQL_REPLACE_LEN);
  } else {
    if (str->reserve(SPIDER_SQL_INSERT_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_INSERT_STR, SPIDER_SQL_INSERT_LEN);
  }
  if (spider->low_priority)
  {
    if (str->reserve(SPIDER_SQL_LOW_PRIORITY_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_LOW_PRIORITY_STR, SPIDER_SQL_LOW_PRIORITY_LEN);
  }
  else if (spider->insert_delayed)
  {
    if (share->internal_delayed)
    {
      if (str->reserve(SPIDER_SQL_SQL_DELAYED_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_SQL_DELAYED_STR, SPIDER_SQL_SQL_DELAYED_LEN);
    }
  }
  else if (
    spider->lock_type >= TL_WRITE &&
    !spider->write_can_replace &&
    /* for direct_dup_insert without patch for partition */
    spider->sql_command != SQLCOM_REPLACE &&
    spider->sql_command != SQLCOM_REPLACE_SELECT
  ) {
    if (str->reserve(SPIDER_SQL_HIGH_PRIORITY_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_HIGH_PRIORITY_STR, SPIDER_SQL_HIGH_PRIORITY_LEN);
  }
  if (
    spider->ignore_dup_key &&
    spider->direct_dup_insert &&
    !spider->write_can_replace &&
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    (!spider->insert_with_update || !dup_update_sql.length()) &&
#else
    !spider->insert_with_update &&
#endif
    /* for direct_dup_insert without patch for partition */
    spider->sql_command != SQLCOM_REPLACE &&
    spider->sql_command != SQLCOM_REPLACE_SELECT
  ) {
    if (str->reserve(SPIDER_SQL_SQL_IGNORE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_IGNORE_STR, SPIDER_SQL_SQL_IGNORE_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_update_part()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_update_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_update(&update_sql, 0);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_update(
  spider_string *str,
  int link_idx
) {
  DBUG_ENTER("spider_mysql_handler::append_update");
  if (str->reserve(SPIDER_SQL_UPDATE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_UPDATE_STR, SPIDER_SQL_UPDATE_LEN);
  if (spider->low_priority)
  {
    if (str->reserve(SPIDER_SQL_LOW_PRIORITY_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_LOW_PRIORITY_STR, SPIDER_SQL_LOW_PRIORITY_LEN);
  }
  if (
    spider->ignore_dup_key &&
    !spider->insert_with_update
  ) {
    if (str->reserve(SPIDER_SQL_SQL_IGNORE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_IGNORE_STR, SPIDER_SQL_SQL_IGNORE_LEN);
  }
  if (str->reserve(mysql_share->db_nm_max_length +
    SPIDER_SQL_DOT_LEN + mysql_share->table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  table_name_pos = str->length();
  append_table_name_with_adjusting(str, link_idx, SPIDER_SQL_TYPE_UPDATE_SQL);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_delete_part()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_delete_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_delete(&update_sql);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_delete(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_delete");
  if (str->reserve(SPIDER_SQL_DELETE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_DELETE_STR, SPIDER_SQL_DELETE_LEN);
  if (spider->low_priority)
  {
    if (str->reserve(SPIDER_SQL_LOW_PRIORITY_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_LOW_PRIORITY_STR, SPIDER_SQL_LOW_PRIORITY_LEN);
  }
  if (spider->quick_mode)
  {
    if (str->reserve(SPIDER_SQL_SQL_QUICK_MODE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_QUICK_MODE_STR,
      SPIDER_SQL_SQL_QUICK_MODE_LEN);
  }
  if (spider->ignore_dup_key)
  {
    if (str->reserve(SPIDER_SQL_SQL_IGNORE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_IGNORE_STR, SPIDER_SQL_SQL_IGNORE_LEN);
  }
  str->length(str->length() - 1);
  DBUG_RETURN(0);
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
int spider_mysql_handler::append_increment_update_set_part()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_increment_update_set_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_increment_update_set(&update_sql);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_increment_update_set(
  spider_string *str
) {
  uint field_name_length;
  uint roop_count;
  Field *field;
  DBUG_ENTER("spider_mysql_handler::append_increment_update_set");
  if (str->reserve(SPIDER_SQL_SET_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SET_STR, SPIDER_SQL_SET_LEN);
  const SPIDER_HS_STRING_REF *value = hs_upds.ptr();
  for (roop_count = 0; roop_count < hs_upds.size();
    roop_count++)
  {
    if (
      value[roop_count].size() == 1 &&
      *(value[roop_count].begin()) == '0'
    )
      continue;

    Field *top_table_field =
      spider->get_top_table_field(spider->hs_pushed_ret_fields[roop_count]);
    if (!(field = spider->field_exchange(top_table_field)))
      continue;
    field_name_length =
      mysql_share->column_name_str[field->field_index].length();

    if (str->reserve(field_name_length * 2 + /* SPIDER_SQL_NAME_QUOTE_LEN */
      4 + SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_HS_INCREMENT_LEN +
      SPIDER_SQL_COMMA_LEN + value[roop_count].size()))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

    mysql_share->append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
    mysql_share->append_column_name(str, field->field_index);
    if (spider->hs_increment)
      str->q_append(SPIDER_SQL_HS_INCREMENT_STR,
        SPIDER_SQL_HS_INCREMENT_LEN);
    else
      str->q_append(SPIDER_SQL_HS_DECREMENT_STR,
        SPIDER_SQL_HS_DECREMENT_LEN);
    str->q_append(value[roop_count].begin(), value[roop_count].size());
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}
#endif
#endif

int spider_mysql_handler::append_update_set_part()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_update_set_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_update_set(&update_sql);
  where_pos = update_sql.length();
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_update_set(
  spider_string *str
) {
  uint field_name_length;
  SPIDER_SHARE *share = spider->share;
  TABLE *table = spider->get_table();
  Field **fields;
  DBUG_ENTER("spider_mysql_handler::append_update_set");
  if (str->reserve(SPIDER_SQL_SET_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SET_STR, SPIDER_SQL_SET_LEN);
  for (fields = table->field; *fields; fields++)
  {
    if (bitmap_is_set(table->write_set, (*fields)->field_index))
    {
      field_name_length =
        mysql_share->column_name_str[(*fields)->field_index].length();
      if ((*fields)->is_null())
      {
        if (str->reserve(field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */
          2 + SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_NULL_LEN +
          SPIDER_SQL_COMMA_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        mysql_share->append_column_name(str, (*fields)->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
        str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
      } else {
        if (str->reserve(field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */
          2 + SPIDER_SQL_EQUAL_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        mysql_share->append_column_name(str, (*fields)->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
#ifndef DBUG_OFF
        MY_BITMAP *tmp_map = dbug_tmp_use_all_columns(table, &table->read_set);
#endif
        if (
          spider_db_mysql_utility.
            append_column_value(spider, str, *fields, NULL,
              share->access_charset) ||
          str->reserve(SPIDER_SQL_COMMA_LEN)
        ) {
#ifndef DBUG_OFF
          dbug_tmp_restore_column_map(&table->read_set, tmp_map);
#endif
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
#ifndef DBUG_OFF
        dbug_tmp_restore_column_map(&table->read_set, tmp_map);
#endif
      }
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
int spider_mysql_handler::append_direct_update_set_part()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_direct_update_set_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_direct_update_set(&update_sql);
  where_pos = update_sql.length();
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_direct_update_set(
  spider_string *str
) {
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint field_name_length;
  SPIDER_SHARE *share = spider->share;
#ifndef DBUG_OFF
  TABLE *table = spider->get_table();
#endif
#endif
  DBUG_ENTER("spider_mysql_handler::append_direct_update_set");
  if (
    spider->direct_update_kinds == SPIDER_SQL_KIND_SQL &&
    spider->direct_update_fields
  ) {
    if (str->reserve(SPIDER_SQL_SET_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SET_STR, SPIDER_SQL_SET_LEN);
    DBUG_RETURN(spider_db_append_update_columns(spider, str, NULL, 0,
      spider_dbton_mysql.dbton_id));
  }

  if (
    (spider->direct_update_kinds & SPIDER_SQL_KIND_SQL)
  ) {
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    size_t roop_count;
    Field *field;
    if (str->reserve(SPIDER_SQL_SET_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SET_STR, SPIDER_SQL_SET_LEN);
    for (roop_count = 0; roop_count < spider->hs_pushed_ret_fields_num;
      roop_count++)
    {
      Field *top_table_field =
        spider->get_top_table_field(spider->hs_pushed_ret_fields[roop_count]);
      if (!(field = spider->field_exchange(top_table_field)))
        continue;
      field_name_length =
        mysql_share->column_name_str[field->field_index].length();
      if (top_table_field->is_null())
      {
        if (str->reserve(field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */
          2 + SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_NULL_LEN +
          SPIDER_SQL_COMMA_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        mysql_share->append_column_name(str, field->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
        str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
      } else {
        if (str->reserve(field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */
          2 + SPIDER_SQL_EQUAL_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        mysql_share->append_column_name(str, field->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
#ifndef DBUG_OFF
        my_bitmap_map *tmp_map = dbug_tmp_use_all_columns(table,
          table->read_set);
#endif
        if (
          spider_db_mysql_utility.
            append_column_value(spider, str, top_table_field, NULL,
              share->access_charset) ||
          str->reserve(SPIDER_SQL_COMMA_LEN)
        ) {
#ifndef DBUG_OFF
          dbug_tmp_restore_column_map(table->read_set, tmp_map);
#endif
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
#ifndef DBUG_OFF
        dbug_tmp_restore_column_map(table->read_set, tmp_map);
#endif
      }
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
#else
    DBUG_ASSERT(0);
#endif
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_dup_update_pushdown_part(
  const char *alias,
  uint alias_length
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_dup_update_pushdown_part");
  DBUG_PRINT("info",("spider this=%p", this));
  dup_update_sql.length(0);
  error_num = append_update_columns(&dup_update_sql, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_update_columns_part(
  const char *alias,
  uint alias_length
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_update_columns_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_update_columns(&update_sql, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::check_update_columns_part()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::check_update_columns_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_update_columns(NULL, NULL, 0);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_update_columns(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_update_columns");
  error_num = spider_db_append_update_columns(spider, str,
    alias, alias_length, spider_dbton_mysql.dbton_id);
  DBUG_RETURN(error_num);
}
#endif

int spider_mysql_handler::append_select_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_select(str, sql_type);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_select(
  spider_string *str,
  ulong sql_type
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_mysql_handler::append_select");
  if (sql_type == SPIDER_SQL_TYPE_HANDLER)
  {
    if (str->reserve(SPIDER_SQL_HANDLER_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_HANDLER_STR, SPIDER_SQL_HANDLER_LEN);
  } else {
    if (str->reserve(SPIDER_SQL_SELECT_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SELECT_STR, SPIDER_SQL_SELECT_LEN);
    if (result_list->direct_distinct)
    {
      if (str->reserve(SPIDER_SQL_DISTINCT_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_DISTINCT_STR, SPIDER_SQL_DISTINCT_LEN);
    }
    if (result_list->lock_type != F_WRLCK && spider->lock_mode < 1)
    {
      /* no lock */
      st_select_lex *select_lex = &spider->trx->thd->lex->select_lex;
      if (
        select_lex->sql_cache == SELECT_LEX::SQL_CACHE &&
        (spider->share->query_cache_sync & 1)
      ) {
        if (str->reserve(SPIDER_SQL_SQL_CACHE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_SQL_CACHE_STR, SPIDER_SQL_SQL_CACHE_LEN);
      } else if (
        select_lex->sql_cache == SELECT_LEX::SQL_NO_CACHE &&
        (spider->share->query_cache_sync & 2)
      ) {
        if (str->reserve(SPIDER_SQL_SQL_NO_CACHE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_SQL_NO_CACHE_STR,
          SPIDER_SQL_SQL_NO_CACHE_LEN);
      } else if (spider->share->query_cache == 1)
      {
        if (str->reserve(SPIDER_SQL_SQL_CACHE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_SQL_CACHE_STR, SPIDER_SQL_SQL_CACHE_LEN);
      } else if (spider->share->query_cache == 2)
      {
        if (str->reserve(SPIDER_SQL_SQL_NO_CACHE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_SQL_NO_CACHE_STR,
          SPIDER_SQL_SQL_NO_CACHE_LEN);
      }
    }
    if (spider->high_priority)
    {
      if (str->reserve(SPIDER_SQL_HIGH_PRIORITY_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_HIGH_PRIORITY_STR,
        SPIDER_SQL_HIGH_PRIORITY_LEN);
    }
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_table_select_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_table_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_table_select(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_table_select(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_table_select");
  table_name_pos = str->length() + mysql_share->table_select_pos;
  if (str->append(*(mysql_share->table_select)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_select_part(
  ulong sql_type,
  uint idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_key_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_key_select(str, idx);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_key_select(
  spider_string *str,
  uint idx
) {
  DBUG_ENTER("spider_mysql_handler::append_key_select");
  table_name_pos = str->length() + mysql_share->key_select_pos[idx];
  if (str->append(mysql_share->key_select[idx]))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_minimum_select_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_minimum_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_minimum_select(str, sql_type);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_minimum_select(
  spider_string *str,
  ulong sql_type
) {
  TABLE *table = spider->get_table();
  Field **field;
  int field_length;
  bool appended = FALSE;
  DBUG_ENTER("spider_mysql_handler::append_minimum_select");
  minimum_select_bitmap_create();
  for (field = table->field; *field; field++)
  {
    if (minimum_select_bit_is_set((*field)->field_index))
    {
/*
      spider_set_bit(minimum_select_bitmap, (*field)->field_index);
*/
      field_length =
        mysql_share->column_name_str[(*field)->field_index].length();
      if (str->reserve(field_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      mysql_share->append_column_name(str, (*field)->field_index);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      appended = TRUE;
    }
  }
  if (appended)
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  else {
    if (str->reserve(SPIDER_SQL_ONE_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_ONE_STR, SPIDER_SQL_ONE_LEN);
  }
  DBUG_RETURN(append_from(str, sql_type, first_link_idx));
}

int spider_mysql_handler::append_table_select_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  TABLE *table = spider->get_table();
  Field **field;
  int field_length;
  DBUG_ENTER("spider_mysql_handler::append_table_select_with_alias");
  for (field = table->field; *field; field++)
  {
    field_length =
      mysql_share->column_name_str[(*field)->field_index].length();
    if (str->reserve(alias_length + field_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(alias, alias_length);
    mysql_share->append_column_name(str, (*field)->field_index);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_select_with_alias(
  spider_string *str,
  const KEY *key_info,
  const char *alias,
  uint alias_length
) {
  KEY_PART_INFO *key_part;
  Field *field;
  uint part_num;
  int field_length;
  DBUG_ENTER("spider_mysql_handler::append_key_select_with_alias");
  for (key_part = key_info->key_part, part_num = 0;
    part_num < spider_user_defined_key_parts(key_info); key_part++, part_num++)
  {
    field = key_part->field;
    field_length = mysql_share->column_name_str[field->field_index].length();
    if (str->reserve(alias_length + field_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(alias, alias_length);
    mysql_share->append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_minimum_select_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  TABLE *table = spider->get_table();
  Field **field;
  int field_length;
  bool appended = FALSE;
  DBUG_ENTER("spider_mysql_handler::append_minimum_select_with_alias");
  minimum_select_bitmap_create();
  for (field = table->field; *field; field++)
  {
    if (minimum_select_bit_is_set((*field)->field_index))
    {
/*
      spider_set_bit(minimum_select_bitmap, (*field)->field_index);
*/
      field_length =
        mysql_share->column_name_str[(*field)->field_index].length();
      if (str->reserve(alias_length + field_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(alias, alias_length);
      mysql_share->append_column_name(str, (*field)->field_index);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      appended = TRUE;
    }
  }
  if (appended)
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  else {
    if (str->reserve(SPIDER_SQL_ONE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_ONE_STR, SPIDER_SQL_ONE_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_select_columns_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_mysql_handler::append_select_columns_with_alias");
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  if (
    result_list->direct_aggregate &&
    (error_num = append_sum_select(str, alias, alias_length))
  )
    DBUG_RETURN(error_num);
#endif
  if ((error_num = append_match_select(str, alias, alias_length)))
    DBUG_RETURN(error_num);
  if (!spider->select_column_mode)
  {
    if (result_list->keyread)
      DBUG_RETURN(append_key_select_with_alias(
        str, result_list->key_info, alias, alias_length));
    else
      DBUG_RETURN(append_table_select_with_alias(
        str, alias, alias_length));
  }
  DBUG_RETURN(append_minimum_select_with_alias(str, alias, alias_length));
}

int spider_mysql_handler::append_hint_after_table_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_hint_after_table_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_hint_after_table(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_hint_after_table(
  spider_string *str
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_hint_after_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    mysql_share->key_hint &&
    (error_num = spider_db_append_hint_after_table(spider,
      str, &mysql_share->key_hint[spider->active_index]))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  DBUG_RETURN(0);
}

void spider_mysql_handler::set_where_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::set_where_pos");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      where_pos = sql.length();
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      where_pos = update_sql.length();
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      ha_read_pos = ha_sql.length();
      break;
    default:
      break;
  }
  DBUG_VOID_RETURN;
}

void spider_mysql_handler::set_where_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::set_where_to_pos");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      sql.length(where_pos);
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      update_sql.length(where_pos);
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      ha_sql.length(ha_read_pos);
      break;
    default:
      break;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_handler::check_item_type(
  Item *item
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::check_item_type");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = spider_db_print_item_type(item, spider, NULL, NULL, 0,
    spider_dbton_mysql.dbton_id);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_values_connector_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_values_connector_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_values_connector(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_values_connector(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_values_connector");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN +
    SPIDER_SQL_COMMA_LEN + SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_values_terminator_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_values_terminator_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_values_terminator(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_values_terminator(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_values_terminator");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(str->length() -
    SPIDER_SQL_COMMA_LEN - SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_union_table_connector_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_union_table_connector_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_union_table_connector(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_union_table_connector(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_union_table_connector");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve((SPIDER_SQL_SPACE_LEN * 2) + SPIDER_SQL_UNION_ALL_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
  str->q_append(SPIDER_SQL_UNION_ALL_STR, SPIDER_SQL_UNION_ALL_LEN);
  str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_union_table_terminator_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_union_table_terminator_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_union_table_terminator(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_union_table_terminator(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_union_table_terminator");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(str->length() -
    ((SPIDER_SQL_SPACE_LEN * 2) + SPIDER_SQL_UNION_ALL_LEN));
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  table_name_pos = str->length() + SPIDER_SQL_SPACE_LEN + SPIDER_SQL_A_LEN +
    SPIDER_SQL_COMMA_LEN;
  if (str->reserve(tmp_sql.length() - SPIDER_SQL_FROM_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(tmp_sql.ptr() + SPIDER_SQL_FROM_LEN,
    tmp_sql.length() - SPIDER_SQL_FROM_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_column_values_part(
  const key_range *start_key,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_key_column_values_part");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_key_column_values(str, start_key);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_key_column_values(
  spider_string *str,
  const key_range *start_key
) {
  int error_num;
  const uchar *ptr;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
  KEY *key_info = result_list->key_info;
  uint length;
  uint store_length;
  key_part_map full_key_part_map =
    make_prev_keypart_map(spider_user_defined_key_parts(key_info));
  key_part_map start_key_part_map;
  KEY_PART_INFO *key_part;
  Field *field;
  DBUG_ENTER("spider_mysql_handler::append_key_column_values");
  start_key_part_map = start_key->keypart_map & full_key_part_map;
  DBUG_PRINT("info", ("spider spider_user_defined_key_parts=%u",
    spider_user_defined_key_parts(key_info)));
  DBUG_PRINT("info", ("spider full_key_part_map=%lu", full_key_part_map));
  DBUG_PRINT("info", ("spider start_key_part_map=%lu", start_key_part_map));

  if (!start_key_part_map)
    DBUG_RETURN(0);

  for (
    key_part = key_info->key_part,
    length = 0;
    start_key_part_map;
    start_key_part_map >>= 1,
    key_part++,
    length += store_length
  ) {
    store_length = key_part->store_length;
    ptr = start_key->key + length;
    field = key_part->field;
    if ((error_num = spider_db_append_null_value(str, key_part, &ptr)))
    {
      if (error_num > 0)
        DBUG_RETURN(error_num);
    } else {
      if (spider_db_mysql_utility.append_column_value(spider, str, field, ptr,
        share->access_charset))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }

    if (str->reserve(SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_column_values_with_name_part(
  const key_range *start_key,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_key_column_values_with_name_part");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_key_column_values_with_name(str, start_key);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_key_column_values_with_name(
  spider_string *str,
  const key_range *start_key
) {
  int error_num;
  const uchar *ptr;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
  KEY *key_info = result_list->key_info;
  uint length;
  uint key_name_length, key_count;
  uint store_length;
  key_part_map full_key_part_map =
    make_prev_keypart_map(spider_user_defined_key_parts(key_info));
  key_part_map start_key_part_map;
  KEY_PART_INFO *key_part;
  Field *field;
  char tmp_buf[MAX_FIELD_WIDTH];
  DBUG_ENTER("spider_mysql_handler::append_key_column_values_with_name");
  start_key_part_map = start_key->keypart_map & full_key_part_map;
  DBUG_PRINT("info", ("spider spider_user_defined_key_parts=%u",
    spider_user_defined_key_parts(key_info)));
  DBUG_PRINT("info", ("spider full_key_part_map=%lu", full_key_part_map));
  DBUG_PRINT("info", ("spider start_key_part_map=%lu", start_key_part_map));

  if (!start_key_part_map)
    DBUG_RETURN(0);

  for (
    key_part = key_info->key_part,
    length = 0,
    key_count = 0;
    start_key_part_map;
    start_key_part_map >>= 1,
    key_part++,
    length += store_length,
    key_count++
  ) {
    store_length = key_part->store_length;
    ptr = start_key->key + length;
    field = key_part->field;
    if ((error_num = spider_db_append_null_value(str, key_part, &ptr)))
    {
      if (error_num > 0)
        DBUG_RETURN(error_num);
    } else {
      if (spider_db_mysql_utility.append_column_value(spider, str, field, ptr,
        share->access_charset))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }

    key_name_length = my_sprintf(tmp_buf, (tmp_buf, "c%u", key_count));
    if (str->reserve(SPIDER_SQL_SPACE_LEN + key_name_length +
      SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    str->q_append(tmp_buf, key_name_length);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_where_part(
  const key_range *start_key,
  const key_range *end_key,
  ulong sql_type
) {
  int error_num;
  spider_string *str, *str_part = NULL, *str_part2 = NULL;
  bool set_order;
  DBUG_ENTER("spider_mysql_handler::append_key_where_part");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      set_order = FALSE;
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      set_order = FALSE;
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      set_order = FALSE;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      ha_read_pos = str->length();
      str_part = &sql_part;
      str_part2 = &sql_part2;
      str_part->length(0);
      str_part2->length(0);
      set_order = TRUE;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_key_where(str, str_part, str_part2, start_key, end_key,
    sql_type, set_order);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_key_where(
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  const key_range *start_key,
  const key_range *end_key,
  ulong sql_type,
  bool set_order
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_key_where");
  error_num = spider_db_append_key_where_internal(str, str_part, str_part2,
    start_key, end_key, spider, set_order, sql_type,
    spider_dbton_mysql.dbton_id);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_is_null_part(
  ulong sql_type,
  KEY_PART_INFO *key_part,
  const key_range *key,
  const uchar **ptr,
  bool key_eq,
  bool tgt_final
) {
  int error_num;
  spider_string *str, *str_part = NULL, *str_part2 = NULL;
  DBUG_ENTER("spider_mysql_handler::append_is_null_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      str_part = &sql_part;
      str_part2 = &sql_part2;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_is_null(sql_type, str, str_part, str_part2,
    key_part, key, ptr, key_eq, tgt_final);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_is_null(
  ulong sql_type,
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  KEY_PART_INFO *key_part,
  const key_range *key,
  const uchar **ptr,
  bool key_eq,
  bool tgt_final
) {
  DBUG_ENTER("spider_mysql_handler::append_is_null");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider key_eq=%s", key_eq ? "TRUE" : "FALSE"));
  if (key_part->null_bit)
  {
    if (*(*ptr)++)
    {
      if (sql_type == SPIDER_SQL_TYPE_HANDLER)
      {
        if (
          key_eq ||
          key->flag == HA_READ_KEY_EXACT ||
          key->flag == HA_READ_KEY_OR_NEXT
        ) {
          if (tgt_final)
          {
            if (str->reserve(SPIDER_SQL_EQUAL_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
          }
          str = str_part;
          if (str->reserve(SPIDER_SQL_NULL_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
        } else {
          if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
          {
            str = str_part;
            /* first index column */
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            ha_next_pos = str->length();
            if (str->reserve(SPIDER_SQL_FIRST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_FIRST_STR, SPIDER_SQL_FIRST_LEN);
            spider->result_list.ha_read_kind = 1;
          } else if (tgt_final)
          {
            if (str->reserve(SPIDER_SQL_GT_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_GT_STR, SPIDER_SQL_GT_LEN);
            str = str_part;
            if (str->reserve(SPIDER_SQL_NULL_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
          }
        }
        str = str_part2;
      }
      if (
        key_eq ||
        key->flag == HA_READ_KEY_EXACT ||
        key->flag == HA_READ_KEY_OR_NEXT
      ) {
        if (str->reserve(SPIDER_SQL_IS_NULL_LEN +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
          mysql_share->column_name_str[key_part->field->field_index].length()))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        mysql_share->append_column_name(str, key_part->field->field_index);
        str->q_append(SPIDER_SQL_IS_NULL_STR, SPIDER_SQL_IS_NULL_LEN);
      } else {
        if (str->reserve(SPIDER_SQL_IS_NOT_NULL_LEN +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
          mysql_share->column_name_str[key_part->field->field_index].length()))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        mysql_share->append_column_name(str, key_part->field->field_index);
        str->q_append(SPIDER_SQL_IS_NOT_NULL_STR, SPIDER_SQL_IS_NOT_NULL_LEN);
      }
      DBUG_RETURN(-1);
    }
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_where_terminator_part(
  ulong sql_type,
  bool set_order,
  int key_count
) {
  int error_num;
  spider_string *str, *str_part = NULL, *str_part2 = NULL;
  DBUG_ENTER("spider_mysql_handler::append_where_terminator_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      str_part = &sql_part;
      str_part2 = &sql_part2;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_where_terminator(sql_type, str, str_part, str_part2,
    set_order, key_count);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_where_terminator(
  ulong sql_type,
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  bool set_order,
  int key_count
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_mysql_handler::append_where_terminator");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type != SPIDER_SQL_TYPE_HANDLER)
  {
    str->length(str->length() - SPIDER_SQL_AND_LEN);
    if (!set_order)
      result_list->key_order = key_count;
  } else {
    str_part2->length(str_part2->length() - SPIDER_SQL_AND_LEN);

    str_part->length(str_part->length() - SPIDER_SQL_COMMA_LEN);
    if (!result_list->ha_read_kind)
      str_part->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
        SPIDER_SQL_CLOSE_PAREN_LEN);
    if (str->append(*str_part))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    uint clause_length = str->length() - ha_next_pos;
    if (clause_length < SPIDER_SQL_NEXT_LEN)
    {
      int roop_count;
      clause_length = SPIDER_SQL_NEXT_LEN - clause_length;
      if (str->reserve(clause_length))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      for (roop_count = 0; roop_count < (int) clause_length; roop_count++)
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    }
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_match_where_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_match_where_part");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_ASSERT(0);
      DBUG_RETURN(0);
  }
  error_num = append_match_where(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_match_where(
  spider_string *str
) {
  int error_num;
  bool first = TRUE;
  st_spider_ft_info *ft_info = spider->ft_first;
  DBUG_ENTER("spider_mysql_handler::append_match_where");
  if (spider->ft_current)
  {
    while (TRUE)
    {
      if (ft_info->used_in_where)
      {
        if (first)
        {
          if (str->reserve(SPIDER_SQL_WHERE_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
          first = FALSE;
        }
        if ((error_num = append_match_against(str, ft_info, NULL, 0)))
          DBUG_RETURN(error_num);
        if (str->reserve(SPIDER_SQL_AND_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
      }

      if (ft_info == spider->ft_current)
        break;
      ft_info = ft_info->next;
    }
    if (!first)
      str->length(str->length() - SPIDER_SQL_AND_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_update_where(
  spider_string *str,
  const TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  uint field_name_length;
  Field **field;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_mysql_handler::append_update_where");
  DBUG_PRINT("info", ("spider table->s->primary_key=%s",
    table->s->primary_key != MAX_KEY ? "TRUE" : "FALSE"));
  if (str->reserve(SPIDER_SQL_WHERE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
  for (field = table->field; *field; field++)
  {
    DBUG_PRINT("info", ("spider bitmap=%s",
      bitmap_is_set(table->read_set, (*field)->field_index) ?
      "TRUE" : "FALSE"));
    if (
      table->s->primary_key == MAX_KEY ||
      bitmap_is_set(table->read_set, (*field)->field_index)
    ) {
      field_name_length =
        mysql_share->column_name_str[(*field)->field_index].length();
      if ((*field)->is_null(ptr_diff))
      {
        if (str->reserve(field_name_length +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
          SPIDER_SQL_IS_NULL_LEN + SPIDER_SQL_AND_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        mysql_share->append_column_name(str, (*field)->field_index);
        str->q_append(SPIDER_SQL_IS_NULL_STR, SPIDER_SQL_IS_NULL_LEN);
      } else {
        if (str->reserve(field_name_length +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
          SPIDER_SQL_EQUAL_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        mysql_share->append_column_name(str, (*field)->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
        (*field)->move_field_offset(ptr_diff);
        if (
          spider_db_mysql_utility.
            append_column_value(spider, str, *field, NULL,
              share->access_charset) ||
          str->reserve(SPIDER_SQL_AND_LEN)
        )
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        (*field)->move_field_offset(-ptr_diff);
      }
      str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
    }
  }
  str->length(str->length() - SPIDER_SQL_AND_LEN);
  if (str->reserve(SPIDER_SQL_LIMIT1_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_LIMIT1_STR, SPIDER_SQL_LIMIT1_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_condition_part(
  const char *alias,
  uint alias_length,
  ulong sql_type,
  bool test_flg
) {
  int error_num;
  spider_string *str;
  bool start_where = FALSE;
  DBUG_ENTER("spider_mysql_handler::append_condition_part");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      if (test_flg)
      {
        str = NULL;
      } else {
        str = &sql;
        start_where = ((int) str->length() == where_pos);
      }
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      if (test_flg)
      {
        str = NULL;
      } else {
        str = &tmp_sql;
        start_where = ((int) str->length() == where_pos);
      }
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      if (test_flg)
      {
        str = NULL;
      } else {
        str = &update_sql;
        start_where = ((int) str->length() == where_pos);
      }
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      if (test_flg)
      {
        str = NULL;
      } else {
        str = &ha_sql;
        start_where = TRUE;
        if (spider->active_index == MAX_KEY)
        {
          set_where_pos(SPIDER_SQL_TYPE_HANDLER);
          if (str->reserve(SPIDER_SQL_READ_LEN + SPIDER_SQL_FIRST_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_READ_STR, SPIDER_SQL_READ_LEN);
          ha_next_pos = str->length();
          str->q_append(SPIDER_SQL_FIRST_STR, SPIDER_SQL_FIRST_LEN);
          sql_part2.length(0);
        }
        ha_where_pos = str->length();

        if (
          spider->sql_command == SQLCOM_HA_READ ||
          !spider->result_list.use_both_key
        ) {
          if (sql_part2.length())
          {
            str->append(sql_part2);
            start_where = FALSE;
          }
        } else {
          DBUG_RETURN(0);
        }
      }
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_condition(str, alias, alias_length, start_where,
    sql_type);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_condition(
  spider_string *str,
  const char *alias,
  uint alias_length,
  bool start_where,
  ulong sql_type
) {
  int error_num, restart_pos = 0, start_where_pos;
  SPIDER_CONDITION *tmp_cond = spider->condition;
  DBUG_ENTER("spider_mysql_handler::append_condition");
  if (str && start_where)
  {
    start_where_pos = str->length();
  } else {
    start_where_pos = 0;
  }

  if (spider->is_clone && !tmp_cond)
  {
    tmp_cond = spider->pt_clone_source_handler->condition;
  }

  while (tmp_cond)
  {
    if (str)
    {
      restart_pos = str->length();
      if (start_where)
      {
        if (str->reserve(SPIDER_SQL_WHERE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
        start_where = FALSE;
      } else {
        if (str->reserve(SPIDER_SQL_AND_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
      }
    }
    if ((error_num = spider_db_print_item_type(
      (Item *) tmp_cond->cond, spider, str, alias, alias_length,
      spider_dbton_mysql.dbton_id)))
    {
      if (str && error_num == ER_SPIDER_COND_SKIP_NUM)
      {
        DBUG_PRINT("info",("spider COND skip"));
        str->length(restart_pos);
        start_where = (restart_pos == start_where_pos);
      } else
        DBUG_RETURN(error_num);
    }
    tmp_cond = tmp_cond->next;
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_match_against_part(
  ulong sql_type,
  st_spider_ft_info *ft_info,
  const char *alias,
  uint alias_length
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_match_against_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_match_against(str, ft_info, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_match_against(
  spider_string *str,
  st_spider_ft_info  *ft_info,
  const char *alias,
  uint alias_length
) {
  SPIDER_SHARE *share = spider->share;
  TABLE *table = spider->get_table();
  String *ft_init_key;
  KEY *key_info;
  uint key_name_length;
  int key_count;
  KEY_PART_INFO *key_part;
  Field *field;
  DBUG_ENTER("spider_mysql_handler::append_match_against");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_MATCH_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_MATCH_STR, SPIDER_SQL_MATCH_LEN);

  ft_init_key = ft_info->key;
  key_info = &table->key_info[ft_info->inx];
  DBUG_PRINT("info", ("spider spider_user_defined_key_parts=%u",
    spider_user_defined_key_parts(key_info)));

  for (
    key_part = key_info->key_part,
    key_count = 0;
    key_count < (int) spider_user_defined_key_parts(key_info);
    key_part++,
    key_count++
  ) {
    field = key_part->field;
    key_name_length =
      mysql_share->column_name_str[field->field_index].length();
    if (alias_length)
    {
      if (str->reserve(alias_length + key_name_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(alias, alias_length);
    } else {
      if (str->reserve(key_name_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    mysql_share->append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  if (str->reserve(SPIDER_SQL_AGAINST_LEN + SPIDER_SQL_VALUE_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_AGAINST_STR, SPIDER_SQL_AGAINST_LEN);
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);

  char buf[MAX_FIELD_WIDTH];
  spider_string tmp_str(buf, MAX_FIELD_WIDTH, share->access_charset);
  tmp_str.init_calc_mem(116);
  tmp_str.length(0);
  if (
    tmp_str.append(ft_init_key->ptr(), ft_init_key->length(),
      ft_init_key->charset()) ||
    str->reserve(tmp_str.length() * 2) ||
    spider_db_mysql_utility.append_escaped_util(str, tmp_str.get_str())
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->mem_calc();

  if (str->reserve(
    SPIDER_SQL_VALUE_QUOTE_LEN + SPIDER_SQL_CLOSE_PAREN_LEN +
    ((ft_info->flags & FT_BOOL) ? SPIDER_SQL_IN_BOOLEAN_MODE_LEN : 0) +
    ((ft_info->flags & FT_EXPAND) ?
      SPIDER_SQL_WITH_QUERY_EXPANSION_LEN : 0)
  ))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  if (ft_info->flags & FT_BOOL)
    str->q_append(SPIDER_SQL_IN_BOOLEAN_MODE_STR,
      SPIDER_SQL_IN_BOOLEAN_MODE_LEN);
  if (ft_info->flags & FT_EXPAND)
    str->q_append(SPIDER_SQL_WITH_QUERY_EXPANSION_STR,
      SPIDER_SQL_WITH_QUERY_EXPANSION_LEN);
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_match_select_part(
  ulong sql_type,
  const char *alias,
  uint alias_length
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_match_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_match_select(str, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_match_select(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_match_select");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider->ft_current)
  {
    st_spider_ft_info *ft_info = spider->ft_first;
    while (TRUE)
    {
      if ((error_num = append_match_against(str, ft_info,
        alias, alias_length)))
        DBUG_RETURN(error_num);
      if (str->reserve(SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      if (ft_info == spider->ft_current)
        break;
      ft_info = ft_info->next;
    }
  }
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
int spider_mysql_handler::append_sum_select_part(
  ulong sql_type,
  const char *alias,
  uint alias_length
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_sum_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_sum_select(str, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_sum_select(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  st_select_lex *select_lex;
  DBUG_ENTER("spider_mysql_handler::append_sum_select");
  DBUG_PRINT("info",("spider this=%p", this));
  select_lex = spider_get_select_lex(spider);
  JOIN *join = select_lex->join;
  Item_sum **item_sum_ptr;
  for (item_sum_ptr = join->sum_funcs; *item_sum_ptr; ++item_sum_ptr)
  {
    if ((error_num = spider_db_mysql_utility.open_item_sum_func(*item_sum_ptr,
      spider, str, alias, alias_length)))
    {
      DBUG_RETURN(error_num);
    }
    if (str->reserve(SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  DBUG_RETURN(0);
}
#endif

void spider_mysql_handler::set_order_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::set_order_pos");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      order_pos = sql.length();
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      order_pos = update_sql.length();
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      ha_next_pos = ha_sql.length();
      break;
    default:
      DBUG_ASSERT(0);
      break;
  }
  DBUG_VOID_RETURN;
}

void spider_mysql_handler::set_order_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::set_order_to_pos");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      sql.length(order_pos);
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      update_sql.length(order_pos);
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      ha_sql.length(ha_next_pos);
      break;
    default:
      DBUG_ASSERT(0);
      break;
  }
  DBUG_VOID_RETURN;
}

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
int spider_mysql_handler::append_group_by_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_group_by_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_group_by(str, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_group_by(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  st_select_lex *select_lex;
  DBUG_ENTER("spider_mysql_handler::append_group_by");
  DBUG_PRINT("info",("spider this=%p", this));
  select_lex = spider_get_select_lex(spider);
  ORDER *group = (ORDER *) select_lex->group_list.first;
  if (group)
  {
    if (str->reserve(SPIDER_SQL_GROUP_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_GROUP_STR, SPIDER_SQL_GROUP_LEN);
    for (; group; group = group->next)
    {
      if ((error_num = spider_db_print_item_type((*group->item), spider, str,
        alias, alias_length, spider_dbton_mysql.dbton_id)))
      {
        DBUG_RETURN(error_num);
      }
      if (str->reserve(SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  }
  DBUG_RETURN(0);
}
#endif

int spider_mysql_handler::append_key_order_for_merge_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_key_order_for_merge_with_alias_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      ha_limit_pos = ha_sql.length();
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_key_order_for_merge_with_alias(str, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_key_order_for_merge_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  /* sort for index merge */
  TABLE *table = spider->get_table();
  int length;
  Field *field;
  uint key_name_length;
  DBUG_ENTER("spider_mysql_handler::append_key_order_for_merge_with_alias");
  DBUG_PRINT("info",("spider this=%p", this));
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  if (spider->result_list.direct_aggregate)
  {
    int error_num;
    if ((error_num = append_group_by(str, alias, alias_length)))
      DBUG_RETURN(error_num);
  }
#endif
  if (table->s->primary_key < MAX_KEY)
  {
    /* sort by primary key */
    KEY *key_info = &table->key_info[table->s->primary_key];
    KEY_PART_INFO *key_part;
    for (
      key_part = key_info->key_part,
      length = 1;
      length <= (int) spider_user_defined_key_parts(key_info);
      key_part++,
      length++
    ) {
      field = key_part->field;
      key_name_length =
        mysql_share->column_name_str[field->field_index].length();
      if (length == 1)
      {
        if (str->reserve(SPIDER_SQL_ORDER_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
      }
      if (str->reserve(alias_length + key_name_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(alias, alias_length);
      mysql_share->append_column_name(str, field->field_index);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
    if (length > 1)
    {
      str->length(str->length() - SPIDER_SQL_COMMA_LEN);
    }
  } else {
    /* sort by all columns */
    Field **fieldp;
    for (
      fieldp = table->field, length = 1;
      *fieldp;
      fieldp++, length++
    ) {
      key_name_length =
        mysql_share->column_name_str[(*fieldp)->field_index].length();
      if (length == 1)
      {
        if (str->reserve(SPIDER_SQL_ORDER_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
      }
      if (str->reserve(alias_length + key_name_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(alias, alias_length);
      mysql_share->append_column_name(str, (*fieldp)->field_index);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
    if (length > 1)
    {
      str->length(str->length() - SPIDER_SQL_COMMA_LEN);
    }
  }
  limit_pos = str->length();
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_order_for_direct_order_limit_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_key_order_for_direct_order_limit_with_alias_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_key_order_for_direct_order_limit_with_alias(
    str, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_key_order_for_direct_order_limit_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  ORDER *order;
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  DBUG_ENTER("spider_mysql_handler::append_key_order_for_direct_order_limit_with_alias");
  DBUG_PRINT("info",("spider this=%p", this));
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  if (spider->result_list.direct_aggregate)
  {
    if ((error_num = append_group_by(str, alias, alias_length)))
      DBUG_RETURN(error_num);
  }
#endif
  spider_get_select_limit(spider, &select_lex, &select_limit,
    &offset_limit);
  if (select_lex->order_list.first)
  {
    if (str->reserve(SPIDER_SQL_ORDER_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
    for (order = (ORDER *) select_lex->order_list.first; order;
      order = order->next)
    {
      if ((error_num =
        spider_db_print_item_type((*order->item), spider, str, alias,
          alias_length, spider_dbton_mysql.dbton_id)))
      {
        DBUG_PRINT("info",("spider error=%d", error_num));
        DBUG_RETURN(error_num);
      }
      if (order->direction == ORDER::ORDER_ASC)
      {
        if (str->reserve(SPIDER_SQL_COMMA_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      } else {
        if (str->reserve(SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
        str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      }
    }
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  }
  limit_pos = str->length();
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_order_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_key_order_with_alias_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      error_num = append_key_order_for_handler(str, alias, alias_length);
      DBUG_RETURN(error_num);
    default:
      DBUG_RETURN(0);
  }
  error_num = append_key_order_with_alias(str, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_key_order_for_handler(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_mysql_handler::append_key_order_for_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider ha_next_pos=%d", ha_next_pos));
  DBUG_PRINT("info",("spider ha_where_pos=%d", ha_where_pos));
  str->q_append(alias, alias_length);
  memset((char *) str->ptr() + str->length(), ' ',
    ha_where_pos - ha_next_pos - alias_length);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_key_order_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  KEY *key_info = result_list->key_info;
  int length;
  KEY_PART_INFO *key_part;
  Field *field;
  uint key_name_length;
  DBUG_ENTER("spider_mysql_handler::append_key_order_with_alias");
  DBUG_PRINT("info",("spider this=%p", this));
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  if (spider->result_list.direct_aggregate)
  {
    int error_num;
    if ((error_num = append_group_by(str, alias, alias_length)))
      DBUG_RETURN(error_num);
  }
#endif
  if (result_list->sorted == TRUE)
  {
    if (result_list->desc_flg == TRUE)
    {
      for (
        key_part = key_info->key_part + result_list->key_order,
        length = 1;
        length + result_list->key_order <
          (int) spider_user_defined_key_parts(key_info) &&
        length < result_list->max_order;
        key_part++,
        length++
      ) {
        field = key_part->field;
        key_name_length =
          mysql_share->column_name_str[field->field_index].length();
        if (length == 1)
        {
          if (str->reserve(SPIDER_SQL_ORDER_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
        }
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          mysql_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
            SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          mysql_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        }
      }
      if (
        length + result_list->key_order <=
          (int) spider_user_defined_key_parts(key_info) &&
        length <= result_list->max_order
      ) {
        field = key_part->field;
        key_name_length =
          mysql_share->column_name_str[field->field_index].length();
        if (length == 1)
        {
          if (str->reserve(SPIDER_SQL_ORDER_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
        }
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          mysql_share->append_column_name(str, field->field_index);
        } else {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_DESC_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          mysql_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
        }
      }
    } else {
      for (
        key_part = key_info->key_part + result_list->key_order,
        length = 1;
        length + result_list->key_order <
          (int) spider_user_defined_key_parts(key_info) &&
        length < result_list->max_order;
        key_part++,
        length++
      ) {
        field = key_part->field;
        key_name_length =
          mysql_share->column_name_str[field->field_index].length();
        if (length == 1)
        {
          if (str->reserve(SPIDER_SQL_ORDER_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
        }
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
            SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          mysql_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          mysql_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        }
      }
      if (
        length + result_list->key_order <=
          (int) spider_user_defined_key_parts(key_info) &&
        length <= result_list->max_order
      ) {
        field = key_part->field;
        key_name_length =
          mysql_share->column_name_str[field->field_index].length();
        if (length == 1)
        {
          if (str->reserve(SPIDER_SQL_ORDER_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
        }
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_DESC_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          mysql_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
        } else {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          mysql_share->append_column_name(str, field->field_index);
        }
      }
    }
  }
  limit_pos = str->length();
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_limit_part(
  longlong offset,
  longlong limit,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_limit_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      limit_pos = str->length();
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      limit_pos = str->length();
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      limit_pos = str->length();
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      ha_limit_pos = str->length();
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_limit(str, offset, limit);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::reappend_limit_part(
  longlong offset,
  longlong limit,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::reappend_limit_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      str->length(limit_pos);
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      str->length(limit_pos);
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      str->length(limit_pos);
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      str->length(ha_limit_pos);
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_limit(str, offset, limit);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_limit(
  spider_string *str,
  longlong offset,
  longlong limit
) {
  char buf[SPIDER_LONGLONG_LEN + 1];
  uint32 length;
  DBUG_ENTER("spider_mysql_handler::append_limit");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info", ("spider offset=%lld", offset));
  DBUG_PRINT("info", ("spider limit=%lld", limit));
  if (offset || limit < 9223372036854775807LL)
  {
    if (str->reserve(SPIDER_SQL_LIMIT_LEN + SPIDER_SQL_COMMA_LEN +
      ((SPIDER_LONGLONG_LEN) * 2)))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_LIMIT_STR, SPIDER_SQL_LIMIT_LEN);
    if (offset)
    {
      length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
        &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, offset);
      str->q_append(buf, length);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
    length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
      &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, limit);
    str->q_append(buf, length);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_select_lock_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_select_lock_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_select_lock(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_select_lock(
  spider_string *str
) {
  int lock_mode = spider_conn_lock_mode(spider);
  DBUG_ENTER("spider_mysql_handler::append_select_lock");
  DBUG_PRINT("info",("spider this=%p", this));
  if (lock_mode == SPIDER_LOCK_MODE_EXCLUSIVE)
  {
    if (str->reserve(SPIDER_SQL_FOR_UPDATE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_FOR_UPDATE_STR, SPIDER_SQL_FOR_UPDATE_LEN);
  } else if (lock_mode == SPIDER_LOCK_MODE_SHARED)
  {
    if (str->reserve(SPIDER_SQL_SHARED_LOCK_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SHARED_LOCK_STR, SPIDER_SQL_SHARED_LOCK_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_union_all_start_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_union_all_start_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_union_all_start(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_union_all_start(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_union_all_start");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_union_all_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_union_all_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_union_all(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_union_all(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_union_all");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_UNION_ALL_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_UNION_ALL_STR, SPIDER_SQL_UNION_ALL_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_union_all_end_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_union_all_end_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_union_all_end(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_union_all_end(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_union_all_end");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(str->length() -
    SPIDER_SQL_UNION_ALL_LEN + SPIDER_SQL_CLOSE_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_multi_range_cnt_part(
  ulong sql_type,
  uint multi_range_cnt,
  bool with_comma
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_multi_range_cnt_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_multi_range_cnt(str, multi_range_cnt, with_comma);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_multi_range_cnt(
  spider_string *str,
  uint multi_range_cnt,
  bool with_comma
) {
  int range_cnt_length;
  char range_cnt_str[SPIDER_SQL_INT_LEN];
  DBUG_ENTER("spider_mysql_handler::append_multi_range_cnt");
  DBUG_PRINT("info",("spider this=%p", this));
  range_cnt_length = my_sprintf(range_cnt_str, (range_cnt_str, "%u",
    multi_range_cnt));
  if (with_comma)
  {
    if (str->reserve(range_cnt_length + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(range_cnt_str, range_cnt_length);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  } else {
    if (str->reserve(range_cnt_length))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(range_cnt_str, range_cnt_length);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_multi_range_cnt_with_name_part(
  ulong sql_type,
  uint multi_range_cnt
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_multi_range_cnt_with_name_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      str = &sql;
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      str = &tmp_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_multi_range_cnt_with_name(str, multi_range_cnt);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_multi_range_cnt_with_name(
  spider_string *str,
  uint multi_range_cnt
) {
  int range_cnt_length;
  char range_cnt_str[SPIDER_SQL_INT_LEN];
  DBUG_ENTER("spider_mysql_handler::append_multi_range_cnt_with_name");
  DBUG_PRINT("info",("spider this=%p", this));
  range_cnt_length = my_sprintf(range_cnt_str, (range_cnt_str, "%u",
    multi_range_cnt));
  if (str->reserve(range_cnt_length + SPIDER_SQL_SPACE_LEN +
    SPIDER_SQL_ID_LEN + SPIDER_SQL_COMMA_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(range_cnt_str, range_cnt_length);
  str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
  str->q_append(SPIDER_SQL_ID_STR, SPIDER_SQL_ID_LEN);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_open_handler_part(
  ulong sql_type,
  uint handler_id,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_open_handler_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_open_handler(str, handler_id, conn, link_idx);
  exec_ha_sql = str;
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_open_handler(
  spider_string *str,
  uint handler_id,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_open_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider link_idx=%d", link_idx));
  DBUG_PRINT("info",("spider m_handler_cid=%s",
    spider->m_handler_cid[link_idx]));
  if (str->reserve(SPIDER_SQL_HANDLER_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_HANDLER_STR, SPIDER_SQL_HANDLER_LEN);
  if ((error_num = mysql_share->append_table_name(str,
      spider->conn_link_idx[link_idx])))
    DBUG_RETURN(error_num);
  if (str->reserve(SPIDER_SQL_OPEN_LEN + SPIDER_SQL_AS_LEN +
    SPIDER_SQL_HANDLER_CID_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_OPEN_STR, SPIDER_SQL_OPEN_LEN);
  str->q_append(SPIDER_SQL_AS_STR, SPIDER_SQL_AS_LEN);
  str->q_append(spider->m_handler_cid[link_idx], SPIDER_SQL_HANDLER_CID_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_close_handler_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_close_handler_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_close_handler(str, link_idx);
  exec_ha_sql = str;
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_close_handler(
  spider_string *str,
  int link_idx
) {
  DBUG_ENTER("spider_mysql_handler::append_close_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_HANDLER_LEN + SPIDER_SQL_CLOSE_LEN +
    SPIDER_SQL_HANDLER_CID_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_HANDLER_STR, SPIDER_SQL_HANDLER_LEN);
  str->q_append(spider->m_handler_cid[link_idx],
    SPIDER_SQL_HANDLER_CID_LEN);
  str->q_append(SPIDER_SQL_CLOSE_STR, SPIDER_SQL_CLOSE_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_insert_terminator_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_insert_terminator_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_INSERT_SQL:
      str = &insert_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_insert_terminator(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_insert_terminator(
  spider_string *str
) {
  DBUG_ENTER("spider_mysql_handler::append_insert_terminator");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider dup_update_sql.length=%u", dup_update_sql.length()));
  if (
    spider->result_list.insert_dup_update_pushdown &&
    dup_update_sql.length()
  ) {
    DBUG_PRINT("info",("spider add duplicate key update"));
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
    if (str->reserve(SPIDER_SQL_DUPLICATE_KEY_UPDATE_LEN +
      dup_update_sql.length()))
    {
      str->length(0);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_DUPLICATE_KEY_UPDATE_STR,
      SPIDER_SQL_DUPLICATE_KEY_UPDATE_LEN);
    if (str->append(dup_update_sql))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  } else {
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_insert_values_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_insert_values_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_INSERT_SQL:
      str = &insert_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_insert_values(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_insert_values(
  spider_string *str
) {
  SPIDER_SHARE *share = spider->share;
  TABLE *table = spider->get_table();
  Field **field;
  bool add_value = FALSE;
  DBUG_ENTER("spider_mysql_handler::append_insert_values");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
  {
    str->length(0);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  for (field = table->field; *field; field++)
  {
    DBUG_PRINT("info",("spider field_index=%u", (*field)->field_index));
    if (
      bitmap_is_set(table->write_set, (*field)->field_index) ||
      bitmap_is_set(table->read_set, (*field)->field_index)
    ) {
#ifndef DBUG_OFF
      MY_BITMAP *tmp_map =
        dbug_tmp_use_all_columns(table, &table->read_set);
#endif
      add_value = TRUE;
      DBUG_PRINT("info",("spider is_null()=%s",
        (*field)->is_null() ? "TRUE" : "FALSE"));
      DBUG_PRINT("info",("spider table->next_number_field=%p",
        table->next_number_field));
      DBUG_PRINT("info",("spider *field=%p", *field));
      DBUG_PRINT("info",("spider force_auto_increment=%s",
        (table->next_number_field && spider->force_auto_increment) ?
        "TRUE" : "FALSE"));
      if (
        (*field)->is_null() ||
        (
          table->next_number_field == *field &&
          !table->auto_increment_field_not_null &&
          !spider->force_auto_increment
        )
      ) {
        if (str->reserve(SPIDER_SQL_NULL_LEN + SPIDER_SQL_COMMA_LEN))
        {
#ifndef DBUG_OFF
          dbug_tmp_restore_column_map(&table->read_set, tmp_map);
#endif
          str->length(0);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
      } else {
        if (
          spider_db_mysql_utility.
            append_column_value(spider, str, *field, NULL,
              share->access_charset) ||
          str->reserve(SPIDER_SQL_COMMA_LEN)
        ) {
#ifndef DBUG_OFF
          dbug_tmp_restore_column_map(&table->read_set, tmp_map);
#endif
          str->length(0);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
      }
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(&table->read_set, tmp_map);
#endif
    }
  }
  if (add_value)
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_COMMA_LEN))
  {
    str->length(0);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_into_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_into_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_INSERT_SQL:
      str = &insert_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_into(str);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_into(
  spider_string *str
) {
  const TABLE *table = spider->get_table();
  Field **field;
  uint field_name_length = 0;
  DBUG_ENTER("spider_mysql_handler::append_into");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_INTO_LEN + mysql_share->db_nm_max_length +
    SPIDER_SQL_DOT_LEN + mysql_share->table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_INTO_STR, SPIDER_SQL_INTO_LEN);
  insert_table_name_pos = str->length();
  append_table_name_with_adjusting(str, first_link_idx,
    SPIDER_SQL_TYPE_INSERT_SQL);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  for (field = table->field; *field; field++)
  {
    if (
      bitmap_is_set(table->write_set, (*field)->field_index) ||
      bitmap_is_set(table->read_set, (*field)->field_index)
    ) {
      field_name_length =
        mysql_share->column_name_str[(*field)->field_index].length();
      if (str->reserve(field_name_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      mysql_share->append_column_name(str, (*field)->field_index);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
  }
  if (field_name_length)
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_VALUES_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  str->q_append(SPIDER_SQL_VALUES_STR, SPIDER_SQL_VALUES_LEN);
  insert_pos = str->length();
  DBUG_RETURN(0);
}

void spider_mysql_handler::set_insert_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::set_insert_to_pos");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_INSERT_SQL:
      insert_sql.length(insert_pos);
      break;
    default:
      DBUG_ASSERT(0);
      break;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_handler::append_from_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_from_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_HANDLER:
      str = &ha_sql;
      break;
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      str = &update_sql;
      break;
    default:
      str = &sql;
      break;
  }
  error_num = append_from(str, sql_type, link_idx);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_from(
  spider_string *str,
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_mysql_handler::append_from");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider link_idx=%d", link_idx));
  if (sql_type == SPIDER_SQL_TYPE_HANDLER)
  {
    ha_table_name_pos = str->length();
    DBUG_PRINT("info",("spider ha_table_name_pos=%u", ha_table_name_pos));
    ha_sql_handler_id = spider->m_handler_id[link_idx];
    DBUG_PRINT("info",("spider ha_sql_handler_id=%u", ha_sql_handler_id));
    if (str->reserve(SPIDER_SQL_HANDLER_CID_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(spider->m_handler_cid[link_idx], SPIDER_SQL_HANDLER_CID_LEN);
    DBUG_PRINT("info",("spider m_handler_cid=%s",
      spider->m_handler_cid[link_idx]));
  } else {
    if (str->reserve(SPIDER_SQL_FROM_LEN + mysql_share->db_nm_max_length +
      SPIDER_SQL_DOT_LEN + mysql_share->table_nm_max_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_OPEN_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
    table_name_pos = str->length();
    append_table_name_with_adjusting(str, link_idx, sql_type);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_flush_tables_part(
  ulong sql_type,
  int link_idx,
  bool lock
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_flush_tables_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_SQL:
      str = &spider->result_list.sqls[link_idx];
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_flush_tables(str, link_idx, lock);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_flush_tables(
  spider_string *str,
  int link_idx,
  bool lock
) {
  DBUG_ENTER("spider_mysql_handler::append_flush_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  if (lock)
  {
    if (str->reserve(SPIDER_SQL_FLUSH_TABLES_LEN +
      SPIDER_SQL_WITH_READ_LOCK_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_FLUSH_TABLES_STR, SPIDER_SQL_FLUSH_TABLES_LEN);
    str->q_append(SPIDER_SQL_WITH_READ_LOCK_STR,
      SPIDER_SQL_WITH_READ_LOCK_LEN);
  } else {
    if (str->reserve(SPIDER_SQL_FLUSH_TABLES_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_FLUSH_TABLES_STR, SPIDER_SQL_FLUSH_TABLES_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_optimize_table_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_optimize_table_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_SQL:
      str = &spider->result_list.sqls[link_idx];
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_optimize_table(str, link_idx);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_optimize_table(
  spider_string *str,
  int link_idx
) {
  SPIDER_SHARE *share = spider->share;
  int conn_link_idx = spider->conn_link_idx[link_idx];
  int local_length = spider_param_internal_optimize_local(spider->trx->thd,
    share->internal_optimize_local) * SPIDER_SQL_SQL_LOCAL_LEN;
  DBUG_ENTER("spider_mysql_handler::append_optimize_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_OPTIMIZE_LEN + SPIDER_SQL_SQL_TABLE_LEN +
    local_length +
    mysql_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    mysql_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_OPTIMIZE_STR, SPIDER_SQL_SQL_OPTIMIZE_LEN);
  if (local_length)
    str->q_append(SPIDER_SQL_SQL_LOCAL_STR, SPIDER_SQL_SQL_LOCAL_LEN);
  str->q_append(SPIDER_SQL_SQL_TABLE_STR, SPIDER_SQL_SQL_TABLE_LEN);
  mysql_share->append_table_name(str, conn_link_idx);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_analyze_table_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_analyze_table_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_SQL:
      str = &spider->result_list.sqls[link_idx];
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_analyze_table(str, link_idx);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_analyze_table(
  spider_string *str,
  int link_idx
) {
  SPIDER_SHARE *share = spider->share;
  int conn_link_idx = spider->conn_link_idx[link_idx];
  int local_length = spider_param_internal_optimize_local(spider->trx->thd,
    share->internal_optimize_local) * SPIDER_SQL_SQL_LOCAL_LEN;
  DBUG_ENTER("spider_mysql_handler::append_analyze_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_ANALYZE_LEN + SPIDER_SQL_SQL_TABLE_LEN +
    local_length +
    mysql_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    mysql_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_ANALYZE_STR, SPIDER_SQL_SQL_ANALYZE_LEN);
  if (local_length)
    str->q_append(SPIDER_SQL_SQL_LOCAL_STR, SPIDER_SQL_SQL_LOCAL_LEN);
  str->q_append(SPIDER_SQL_SQL_TABLE_STR, SPIDER_SQL_SQL_TABLE_LEN);
  mysql_share->append_table_name(str, conn_link_idx);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_repair_table_part(
  ulong sql_type,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_repair_table_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_SQL:
      str = &spider->result_list.sqls[link_idx];
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_repair_table(str, link_idx, check_opt);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_repair_table(
  spider_string *str,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  SPIDER_SHARE *share = spider->share;
  int conn_link_idx = spider->conn_link_idx[link_idx];
  int local_length = spider_param_internal_optimize_local(spider->trx->thd,
    share->internal_optimize_local) * SPIDER_SQL_SQL_LOCAL_LEN;
  DBUG_ENTER("spider_mysql_handler::append_repair_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_REPAIR_LEN + SPIDER_SQL_SQL_TABLE_LEN +
    local_length +
    mysql_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    mysql_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_REPAIR_STR, SPIDER_SQL_SQL_REPAIR_LEN);
  if (local_length)
    str->q_append(SPIDER_SQL_SQL_LOCAL_STR, SPIDER_SQL_SQL_LOCAL_LEN);
  str->q_append(SPIDER_SQL_SQL_TABLE_STR, SPIDER_SQL_SQL_TABLE_LEN);
  mysql_share->append_table_name(str, conn_link_idx);
  if (check_opt->flags & T_QUICK)
  {
    if (str->reserve(SPIDER_SQL_SQL_QUICK_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_QUICK_STR, SPIDER_SQL_SQL_QUICK_LEN);
  }
  if (check_opt->flags & T_EXTEND)
  {
    if (str->reserve(SPIDER_SQL_SQL_EXTENDED_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_EXTENDED_STR, SPIDER_SQL_SQL_EXTENDED_LEN);
  }
  if (check_opt->sql_flags & TT_USEFRM)
  {
    if (str->reserve(SPIDER_SQL_SQL_USE_FRM_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_USE_FRM_STR, SPIDER_SQL_SQL_USE_FRM_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_check_table_part(
  ulong sql_type,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_check_table_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_SQL:
      str = &spider->result_list.sqls[link_idx];
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_check_table(str, link_idx, check_opt);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_check_table(
  spider_string *str,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int conn_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_mysql_handler::append_check_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_CHECK_TABLE_LEN +
    mysql_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    mysql_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_CHECK_TABLE_STR,
    SPIDER_SQL_SQL_CHECK_TABLE_LEN);
  mysql_share->append_table_name(str, conn_link_idx);
  if (check_opt->flags & T_QUICK)
  {
    if (str->reserve(SPIDER_SQL_SQL_QUICK_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_QUICK_STR, SPIDER_SQL_SQL_QUICK_LEN);
  }
  if (check_opt->flags & T_FAST)
  {
    if (str->reserve(SPIDER_SQL_SQL_FAST_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_FAST_STR, SPIDER_SQL_SQL_FAST_LEN);
  }
  if (check_opt->flags & T_MEDIUM)
  {
    if (str->reserve(SPIDER_SQL_SQL_MEDIUM_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_MEDIUM_STR, SPIDER_SQL_SQL_MEDIUM_LEN);
  }
  if (check_opt->flags & T_EXTEND)
  {
    if (str->reserve(SPIDER_SQL_SQL_EXTENDED_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SQL_EXTENDED_STR, SPIDER_SQL_SQL_EXTENDED_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_enable_keys_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_enable_keys_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_SQL:
      str = &spider->result_list.sqls[link_idx];
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_enable_keys(str, link_idx);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_enable_keys(
  spider_string *str,
  int link_idx
) {
  int conn_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_mysql_handler::append_enable_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_ALTER_TABLE_LEN +
    mysql_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    mysql_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_SQL_ENABLE_KEYS_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_ALTER_TABLE_STR,
    SPIDER_SQL_SQL_ALTER_TABLE_LEN);
  mysql_share->append_table_name(str, conn_link_idx);
  str->q_append(SPIDER_SQL_SQL_ENABLE_KEYS_STR,
    SPIDER_SQL_SQL_ENABLE_KEYS_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_disable_keys_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_disable_keys_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_SQL:
      str = &spider->result_list.sqls[link_idx];
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_disable_keys(str, link_idx);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_disable_keys(
  spider_string *str,
  int link_idx
) {
  int conn_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_mysql_handler::append_disable_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_ALTER_TABLE_LEN +
    mysql_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    mysql_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_SQL_DISABLE_KEYS_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_ALTER_TABLE_STR,
    SPIDER_SQL_SQL_ALTER_TABLE_LEN);
  mysql_share->append_table_name(str, conn_link_idx);
  str->q_append(SPIDER_SQL_SQL_DISABLE_KEYS_STR,
    SPIDER_SQL_SQL_DISABLE_KEYS_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_delete_all_rows_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_delete_all_rows_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_DELETE_SQL:
      str = &update_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_delete_all_rows(str, sql_type);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_delete_all_rows(
  spider_string *str,
  ulong sql_type
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_delete_all_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider->sql_command == SQLCOM_TRUNCATE)
  {
    if ((error_num = append_truncate(str, sql_type, first_link_idx)))
      DBUG_RETURN(error_num);
  } else {
    if (
      (error_num = append_delete(str)) ||
      (error_num = append_from(str, sql_type, first_link_idx))
    )
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_truncate(
  spider_string *str,
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_mysql_handler::append_truncate");
  if (str->reserve(SPIDER_SQL_TRUNCATE_TABLE_LEN +
    mysql_share->db_nm_max_length +
    SPIDER_SQL_DOT_LEN + mysql_share->table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_TRUNCATE_TABLE_STR, SPIDER_SQL_TRUNCATE_TABLE_LEN);
  table_name_pos = str->length();
  append_table_name_with_adjusting(str, link_idx, sql_type);
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_explain_select_part(
  key_range *start_key,
  key_range *end_key,
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_explain_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_SQL:
      str = &spider->result_list.sqls[link_idx];
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num =
    append_explain_select(str, start_key, end_key, sql_type, link_idx);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::append_explain_select(
  spider_string *str,
  key_range *start_key,
  key_range *end_key,
  ulong sql_type,
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::append_explain_select");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_EXPLAIN_SELECT_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_EXPLAIN_SELECT_STR, SPIDER_SQL_EXPLAIN_SELECT_LEN);
  if (
    (error_num = append_from(str, sql_type, link_idx)) ||
    (error_num = append_key_where(str, NULL, NULL, start_key, end_key,
      sql_type, FALSE))
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  DBUG_RETURN(0);
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
bool spider_mysql_handler::is_sole_projection_field( uint16 field_index )
{
    // Determine whether the projection list consists solely of the field of interest
    bool            is_field_in_projection_list = FALSE;
    TABLE*          table                       = spider->get_table();
    uint16          projection_field_count      = 0;
    uint16          projection_field_index;
    Field**         field;
    DBUG_ENTER( "spider_mysql_handler::is_sole_projection_field" );

    for ( field = table->field; *field ; field++ )
    {
        projection_field_index = ( *field )->field_index;

        if ( !( minimum_select_bit_is_set( projection_field_index ) ) )
        {
            // Current field is not in the projection list
            continue;
        }

        projection_field_count++;

        if ( !is_field_in_projection_list )
        {
            if ( field_index == projection_field_index )
            {
                // Field of interest is in the projection list
                is_field_in_projection_list = TRUE;
            }
        }

        if ( is_field_in_projection_list && ( projection_field_count != 1 ) )
        {
            // Field of interest is not the sole column in the projection list
            DBUG_RETURN( FALSE );
        }
    }

    if ( is_field_in_projection_list && ( projection_field_count == 1 ) )
    {
        // Field of interest is the only column in the projection list
        DBUG_RETURN( TRUE );
    }

    DBUG_RETURN( FALSE );
}

bool spider_mysql_handler::is_bulk_insert_exec_period(
  bool bulk_end
) {
  DBUG_ENTER("spider_mysql_handler::is_bulk_insert_exec_period");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider insert_sql.length=%u", insert_sql.length()));
  DBUG_PRINT("info",("spider insert_pos=%d", insert_pos));
  DBUG_PRINT("info",("spider insert_sql=%s", insert_sql.c_ptr_safe()));
  if (
    (bulk_end || (int) insert_sql.length() >= spider->bulk_size) &&
    (int) insert_sql.length() > insert_pos
  ) {
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

bool spider_mysql_handler::sql_is_filled_up(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::sql_is_filled_up");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(filled_up);
}

bool spider_mysql_handler::sql_is_empty(
  ulong sql_type
) {
  bool is_empty;
  DBUG_ENTER("spider_mysql_handler::sql_is_empty");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      is_empty = (sql.length() == 0);
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
      is_empty = (insert_sql.length() == 0);
      break;
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      is_empty = (update_sql.length() == 0);
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      is_empty = (tmp_sql.length() == 0);
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      is_empty = (ha_sql.length() == 0);
      break;
    default:
      is_empty = TRUE;
      break;
  }
  DBUG_RETURN(is_empty);
}

bool spider_mysql_handler::support_multi_split_read()
{
  DBUG_ENTER("spider_mysql_handler::support_multi_split_read");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

bool spider_mysql_handler::support_bulk_update()
{
  DBUG_ENTER("spider_mysql_handler::support_bulk_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

int spider_mysql_handler::bulk_tmp_table_insert()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::bulk_tmp_table_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = store_sql_to_bulk_tmp_table(&update_sql, upd_tmp_tbl);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::bulk_tmp_table_insert(
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::bulk_tmp_table_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = store_sql_to_bulk_tmp_table(
    &spider->result_list.update_sqls[link_idx],
    spider->result_list.upd_tmp_tbls[link_idx]);
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::bulk_tmp_table_end_bulk_insert()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::bulk_tmp_table_end_bulk_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = upd_tmp_tbl->file->ha_end_bulk_insert()))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::bulk_tmp_table_rnd_init()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::bulk_tmp_table_rnd_init");
  DBUG_PRINT("info",("spider this=%p", this));
  upd_tmp_tbl->file->extra(HA_EXTRA_CACHE);
  if ((error_num = upd_tmp_tbl->file->ha_rnd_init(TRUE)))
  {
    DBUG_RETURN(error_num);
  }
  reading_from_bulk_tmp_table = TRUE;
  DBUG_RETURN(0);
}

int spider_mysql_handler::bulk_tmp_table_rnd_next()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::bulk_tmp_table_rnd_next");
  DBUG_PRINT("info",("spider this=%p", this));
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
  error_num = upd_tmp_tbl->file->ha_rnd_next(upd_tmp_tbl->record[0]);
#else
  error_num = upd_tmp_tbl->file->rnd_next(upd_tmp_tbl->record[0]);
#endif
  if (!error_num)
  {
    error_num = restore_sql_from_bulk_tmp_table(&insert_sql, upd_tmp_tbl);
  }
  DBUG_RETURN(error_num);
}

int spider_mysql_handler::bulk_tmp_table_rnd_end()
{
  int error_num;
  DBUG_ENTER("spider_mysql_handler::bulk_tmp_table_rnd_end");
  DBUG_PRINT("info",("spider this=%p", this));
  reading_from_bulk_tmp_table = FALSE;
  if ((error_num = upd_tmp_tbl->file->ha_rnd_end()))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

bool spider_mysql_handler::need_copy_for_update(
  int link_idx
) {
  int all_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_mysql_handler::need_copy_for_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(!mysql_share->same_db_table_name ||
    spider->share->link_statuses[all_link_idx] == SPIDER_LINK_STATUS_RECOVERY);
}

bool spider_mysql_handler::bulk_tmp_table_created()
{
  DBUG_ENTER("spider_mysql_handler::bulk_tmp_table_created");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(upd_tmp_tbl);
}

int spider_mysql_handler::mk_bulk_tmp_table_and_bulk_start()
{
  THD *thd = spider->trx->thd;
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_mysql_handler::mk_bulk_tmp_table_and_bulk_start");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!upd_tmp_tbl)
  {
    if (!(upd_tmp_tbl = spider_mk_sys_tmp_table(
      thd, table, &upd_tmp_tbl_prm, "a", update_sql.charset())))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    upd_tmp_tbl->file->extra(HA_EXTRA_WRITE_CACHE);
    upd_tmp_tbl->file->ha_start_bulk_insert((ha_rows) 0);
  }
  DBUG_RETURN(0);
}

void spider_mysql_handler::rm_bulk_tmp_table()
{
  DBUG_ENTER("spider_mysql_handler::rm_bulk_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (upd_tmp_tbl)
  {
    spider_rm_sys_tmp_table(spider->trx->thd, upd_tmp_tbl, &upd_tmp_tbl_prm);
    upd_tmp_tbl = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_handler::store_sql_to_bulk_tmp_table(
  spider_string *str,
  TABLE *tmp_table
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::store_sql_to_bulk_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_table->field[0]->set_notnull();
  tmp_table->field[0]->store(str->ptr(), str->length(), str->charset());
  if ((error_num = tmp_table->file->ha_write_row(tmp_table->record[0])))
    DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}

int spider_mysql_handler::restore_sql_from_bulk_tmp_table(
  spider_string *str,
  TABLE *tmp_table
) {
  DBUG_ENTER("spider_mysql_handler::restore_sql_from_bulk_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_table->field[0]->val_str(str->get_str());
  str->mem_calc();
  DBUG_RETURN(0);
}

int spider_mysql_handler::insert_lock_tables_list(
  SPIDER_CONN *conn,
  int link_idx
) {
  spider_db_mysql *db_conn = (spider_db_mysql *) conn->db_conn;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash2 = &link_for_hash[link_idx];
  DBUG_ENTER("spider_mysql_handler::insert_lock_tables_list");
  DBUG_PRINT("info",("spider this=%p", this));
  uint old_elements =
    db_conn->lock_table_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
  if (my_hash_insert_with_hash_value(
    &db_conn->lock_table_hash,
    tmp_link_for_hash2->db_table_str_hash_value,
    (uchar*) tmp_link_for_hash2))
#else
  if (my_hash_insert(&db_conn->lock_table_hash,
    (uchar*) tmp_link_for_hash2))
#endif
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if (db_conn->lock_table_hash.array.max_element > old_elements)
  {
    spider_alloc_calc_mem(spider_current_trx,
      db_conn->lock_table_hash,
      (db_conn->lock_table_hash.array.max_element - old_elements) *
      db_conn->lock_table_hash.array.size_of_element);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::append_lock_tables_list(
  SPIDER_CONN *conn,
  int link_idx,
  int *appended
) {
  int error_num;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash, *tmp_link_for_hash2;
  int conn_link_idx = spider->conn_link_idx[link_idx];
  spider_db_mysql *db_conn = (spider_db_mysql *) conn->db_conn;
  DBUG_ENTER("spider_mysql_handler::append_lock_tables_list");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_link_for_hash2 = &link_for_hash[link_idx];
  tmp_link_for_hash2->db_table_str =
    &mysql_share->db_table_str[conn_link_idx];
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  tmp_link_for_hash2->db_table_str_hash_value =
    mysql_share->db_table_str_hash_value[conn_link_idx];
  if (!(tmp_link_for_hash = (SPIDER_LINK_FOR_HASH *)
    my_hash_search_using_hash_value(
      &db_conn->lock_table_hash,
      tmp_link_for_hash2->db_table_str_hash_value,
      (uchar*) tmp_link_for_hash2->db_table_str->ptr(),
      tmp_link_for_hash2->db_table_str->length())))
#else
  if (!(tmp_link_for_hash = (SPIDER_LINK_FOR_HASH *) my_hash_search(
    &db_conn->lock_table_hash,
    (uchar*) tmp_link_for_hash2->db_table_str->ptr(),
    tmp_link_for_hash2->db_table_str->length())))
#endif
  {
    if ((error_num = insert_lock_tables_list(conn, link_idx)))
      DBUG_RETURN(error_num);
    *appended = 1;
  } else {
    if (tmp_link_for_hash->spider->lock_type < spider->lock_type)
    {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      my_hash_delete_with_hash_value(
        &db_conn->lock_table_hash,
        tmp_link_for_hash->db_table_str_hash_value,
        (uchar*) tmp_link_for_hash);
#else
      my_hash_delete(&db_conn->lock_table_hash,
        (uchar*) tmp_link_for_hash);
#endif
      uint old_elements =
        db_conn->lock_table_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      if (my_hash_insert_with_hash_value(
        &db_conn->lock_table_hash,
        tmp_link_for_hash2->db_table_str_hash_value,
        (uchar*) tmp_link_for_hash2))
#else
      if (my_hash_insert(&db_conn->lock_table_hash,
        (uchar*) tmp_link_for_hash2))
#endif
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      if (db_conn->lock_table_hash.array.max_element > old_elements)
      {
        spider_alloc_calc_mem(spider_current_trx,
          db_conn->lock_table_hash,
          (db_conn->lock_table_hash.array.max_element - old_elements) *
          db_conn->lock_table_hash.array.size_of_element);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::realloc_sql(
  ulong *realloced
) {
  THD *thd = spider->trx->thd;
  st_spider_share *share = spider->share;
  int init_sql_alloc_size =
    spider_param_init_sql_alloc_size(thd, share->init_sql_alloc_size);
  DBUG_ENTER("spider_mysql_handler::realloc_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((int) sql.alloced_length() > init_sql_alloc_size * 2)
  {
    sql.free();
    if (sql.real_alloc(init_sql_alloc_size))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    *realloced |= SPIDER_SQL_TYPE_SELECT_SQL;
  }
  if ((int) ha_sql.alloced_length() > init_sql_alloc_size * 2)
  {
    ha_sql.free();
    if (ha_sql.real_alloc(init_sql_alloc_size))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    *realloced |= SPIDER_SQL_TYPE_SELECT_SQL;
  }
  if ((int) dup_update_sql.alloced_length() > init_sql_alloc_size * 2)
  {
    dup_update_sql.free();
    if (dup_update_sql.real_alloc(init_sql_alloc_size))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if ((int) insert_sql.alloced_length() > init_sql_alloc_size * 2)
  {
    insert_sql.free();
    if (insert_sql.real_alloc(init_sql_alloc_size))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    *realloced |= SPIDER_SQL_TYPE_INSERT_SQL;
  }
  if ((int) update_sql.alloced_length() > init_sql_alloc_size * 2)
  {
    update_sql.free();
    if (update_sql.real_alloc(init_sql_alloc_size))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    *realloced |= (SPIDER_SQL_TYPE_UPDATE_SQL | SPIDER_SQL_TYPE_DELETE_SQL);
  }
  update_sql.length(0);
  if ((int) tmp_sql.alloced_length() > init_sql_alloc_size * 2)
  {
    tmp_sql.free();
    if (tmp_sql.real_alloc(init_sql_alloc_size))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    *realloced |= SPIDER_SQL_TYPE_TMP_SQL;
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::reset_sql(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::reset_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & SPIDER_SQL_TYPE_SELECT_SQL)
  {
    sql.length(0);
  }
  if (sql_type & SPIDER_SQL_TYPE_INSERT_SQL)
  {
    insert_sql.length(0);
  }
  if (sql_type & (SPIDER_SQL_TYPE_UPDATE_SQL | SPIDER_SQL_TYPE_DELETE_SQL |
    SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
  {
    update_sql.length(0);
  }
  if (sql_type & SPIDER_SQL_TYPE_TMP_SQL)
  {
    tmp_sql.length(0);
  }
  if (sql_type & SPIDER_SQL_TYPE_HANDLER)
  {
    ha_sql.length(0);
  }
  DBUG_RETURN(0);
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
int spider_mysql_handler::reset_keys(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::reset_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_mysql_handler::reset_upds(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::reset_upds");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_upds.clear();
  DBUG_RETURN(0);
}

int spider_mysql_handler::reset_strs(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::reset_strs");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_mysql_handler::reset_strs_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::reset_strs_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_mysql_handler::push_back_upds(
  SPIDER_HS_STRING_REF &info
) {
  int error_num;
  DBUG_ENTER("spider_mysql_handler::push_back_upds");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = hs_upds.push_back(info);
  DBUG_RETURN(error_num);
}
#endif

bool spider_mysql_handler::need_lock_before_set_sql_for_exec(
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::need_lock_before_set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_mysql_handler::set_sql_for_exec(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  uint tmp_pos;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  int all_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_mysql_handler::set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & (SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_TMP_SQL))
  {
    if (mysql_share->same_db_table_name || link_idx == first_link_idx)
    {
      if (sql_type & SPIDER_SQL_TYPE_SELECT_SQL)
        exec_sql = &sql;
      if (sql_type & SPIDER_SQL_TYPE_TMP_SQL)
        exec_tmp_sql = &tmp_sql;
    } else {
      char tmp_table_name[MAX_FIELD_WIDTH * 2],
        tgt_table_name[MAX_FIELD_WIDTH * 2];
      int tmp_table_name_length;
      spider_string tgt_table_name_str(tgt_table_name,
        MAX_FIELD_WIDTH * 2,
        mysql_share->db_names_str[link_idx].charset());
      const char *table_names[2], *table_aliases[2];
      uint table_name_lengths[2], table_alias_lengths[2];
      tgt_table_name_str.init_calc_mem(104);
      tgt_table_name_str.length(0);
      if (result_list->tmp_table_join && spider->bka_mode != 2)
      {
        create_tmp_bka_table_name(tmp_table_name, &tmp_table_name_length,
          link_idx);
        append_table_name_with_adjusting(&tgt_table_name_str, link_idx,
          SPIDER_SQL_TYPE_TMP_SQL);
        table_names[0] = tmp_table_name;
        table_names[1] = tgt_table_name_str.ptr();
        table_name_lengths[0] = tmp_table_name_length;
        table_name_lengths[1] = tgt_table_name_str.length();
        table_aliases[0] = SPIDER_SQL_A_STR;
        table_aliases[1] = SPIDER_SQL_B_STR;
        table_alias_lengths[0] = SPIDER_SQL_A_LEN;
        table_alias_lengths[1] = SPIDER_SQL_B_LEN;
      }
      if (sql_type & SPIDER_SQL_TYPE_SELECT_SQL)
      {
        exec_sql = &result_list->sqls[link_idx];
        if (exec_sql->copy(sql))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        else if (result_list->use_union)
        {
          if ((error_num = reset_union_table_name(exec_sql, link_idx,
            SPIDER_SQL_TYPE_SELECT_SQL)))
            DBUG_RETURN(error_num);
        } else {
          tmp_pos = exec_sql->length();
          exec_sql->length(table_name_pos);
          if (result_list->tmp_table_join && spider->bka_mode != 2)
          {
            if ((error_num = spider_db_mysql_utility.append_from_with_alias(
              exec_sql, table_names, table_name_lengths,
              table_aliases, table_alias_lengths, 2,
              &table_name_pos, TRUE))
            )
              DBUG_RETURN(error_num);
            exec_sql->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
          } else {
            append_table_name_with_adjusting(exec_sql, link_idx,
              SPIDER_SQL_TYPE_SELECT_SQL);
          }
          exec_sql->length(tmp_pos);
        }
      }
      if (sql_type & SPIDER_SQL_TYPE_TMP_SQL)
      {
        exec_tmp_sql = &result_list->tmp_sqls[link_idx];
        if (result_list->tmp_table_join && spider->bka_mode != 2)
        {
          if (exec_tmp_sql->copy(tmp_sql))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          else {
            tmp_pos = exec_tmp_sql->length();
            exec_tmp_sql->length(tmp_sql_pos1);
            exec_tmp_sql->q_append(tmp_table_name, tmp_table_name_length);
            exec_tmp_sql->length(tmp_sql_pos2);
            exec_tmp_sql->q_append(tmp_table_name, tmp_table_name_length);
            exec_tmp_sql->length(tmp_sql_pos3);
            exec_tmp_sql->q_append(tmp_table_name, tmp_table_name_length);
            exec_tmp_sql->length(tmp_pos);
          }
        }
      }
    }
  }
  if (sql_type & SPIDER_SQL_TYPE_INSERT_SQL)
  {
    if (mysql_share->same_db_table_name || link_idx == first_link_idx)
      exec_insert_sql = &insert_sql;
    else {
      exec_insert_sql = &result_list->insert_sqls[link_idx];
      if (exec_insert_sql->copy(insert_sql))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      DBUG_PRINT("info",("spider exec_insert_sql=%s",
        exec_insert_sql->c_ptr_safe()));
      tmp_pos = exec_insert_sql->length();
      exec_insert_sql->length(insert_table_name_pos);
      append_table_name_with_adjusting(exec_insert_sql, link_idx,
        sql_type);
      exec_insert_sql->length(tmp_pos);
      DBUG_PRINT("info",("spider exec_insert_sql->length=%u",
        exec_insert_sql->length()));
      DBUG_PRINT("info",("spider exec_insert_sql=%s",
        exec_insert_sql->c_ptr_safe()));
    }
  }
  if (sql_type & SPIDER_SQL_TYPE_BULK_UPDATE_SQL)
  {
    if (reading_from_bulk_tmp_table)
    {
      if (
        mysql_share->same_db_table_name &&
        share->link_statuses[all_link_idx] != SPIDER_LINK_STATUS_RECOVERY
      ) {
        exec_update_sql = &insert_sql;
      } else if (!spider->result_list.upd_tmp_tbls[link_idx])
      {
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
      } else {
        exec_update_sql = &spider->result_list.insert_sqls[link_idx];
        if ((error_num = restore_sql_from_bulk_tmp_table(exec_update_sql,
          spider->result_list.upd_tmp_tbls[link_idx])))
        {
          DBUG_RETURN(error_num);
        }
      }
    } else {
      if (
        mysql_share->same_db_table_name &&
        share->link_statuses[all_link_idx] != SPIDER_LINK_STATUS_RECOVERY
      ) {
        exec_update_sql = &update_sql;
      } else {
        exec_update_sql = &spider->result_list.update_sqls[link_idx];
      }
    }
  } else if (sql_type &
    (SPIDER_SQL_TYPE_UPDATE_SQL | SPIDER_SQL_TYPE_DELETE_SQL))
  {
    if (mysql_share->same_db_table_name || link_idx == first_link_idx)
      exec_update_sql = &update_sql;
    else {
      exec_update_sql = &spider->result_list.update_sqls[link_idx];
      if (exec_update_sql->copy(update_sql))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      tmp_pos = exec_update_sql->length();
      exec_update_sql->length(table_name_pos);
      append_table_name_with_adjusting(exec_update_sql, link_idx,
        sql_type);
      exec_update_sql->length(tmp_pos);
    }
  }
  if (sql_type & SPIDER_SQL_TYPE_HANDLER)
  {
    if (spider->m_handler_id[link_idx] == ha_sql_handler_id)
      exec_ha_sql = &ha_sql;
    else {
      exec_ha_sql = &result_list->sqls[link_idx];
      if (exec_ha_sql->copy(ha_sql))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      else {
        tmp_pos = exec_ha_sql->length();
        exec_ha_sql->length(ha_table_name_pos);
        append_table_name_with_adjusting(exec_ha_sql, link_idx,
          SPIDER_SQL_TYPE_HANDLER);
        exec_ha_sql->length(tmp_pos);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::set_sql_for_exec(
  spider_db_copy_table *tgt_ct,
  ulong sql_type
) {
  spider_mysql_copy_table *mysql_ct = (spider_mysql_copy_table *) tgt_ct;
  DBUG_ENTER("spider_mysql_handler::set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_INSERT_SQL:
      exec_insert_sql = &mysql_ct->sql;
      break;
    default:
      DBUG_ASSERT(0);
      break;
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::execute_sql(
  ulong sql_type,
  SPIDER_CONN *conn,
  int quick_mode,
  int *need_mon
) {
  spider_string *tgt_sql;
  uint tgt_length;
  DBUG_ENTER("spider_mysql_handler::execute_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      DBUG_PRINT("info",("spider SPIDER_SQL_TYPE_SELECT_SQL"));
      tgt_sql = exec_sql;
      tgt_length = tgt_sql->length();
      break;
    case SPIDER_SQL_TYPE_INSERT_SQL:
      DBUG_PRINT("info",("spider SPIDER_SQL_TYPE_SELECT_SQL"));
      tgt_sql = exec_insert_sql;
      tgt_length = tgt_sql->length();
      break;
    case SPIDER_SQL_TYPE_UPDATE_SQL:
    case SPIDER_SQL_TYPE_DELETE_SQL:
    case SPIDER_SQL_TYPE_BULK_UPDATE_SQL:
      DBUG_PRINT("info",("spider %s",
        sql_type == SPIDER_SQL_TYPE_UPDATE_SQL ? "SPIDER_SQL_TYPE_UPDATE_SQL" :
        sql_type == SPIDER_SQL_TYPE_DELETE_SQL ? "SPIDER_SQL_TYPE_DELETE_SQL" :
        "SPIDER_SQL_TYPE_BULK_UPDATE_SQL"
      ));
      tgt_sql = exec_update_sql;
      tgt_length = tgt_sql->length();
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      DBUG_PRINT("info",("spider SPIDER_SQL_TYPE_TMP_SQL"));
      tgt_sql = exec_tmp_sql;
      tgt_length = tgt_sql->length();
      break;
    case SPIDER_SQL_TYPE_DROP_TMP_TABLE_SQL:
      DBUG_PRINT("info",("spider SPIDER_SQL_TYPE_DROP_TMP_TABLE_SQL"));
      tgt_sql = exec_tmp_sql;
      tgt_length = tmp_sql_pos5;
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      DBUG_PRINT("info",("spider SPIDER_SQL_TYPE_HANDLER"));
      tgt_sql = exec_ha_sql;
      tgt_length = tgt_sql->length();
      break;
    default:
      /* nothing to do */
      DBUG_PRINT("info",("spider default"));
      DBUG_RETURN(0);
  }
  DBUG_RETURN(spider_db_query(
    conn,
    tgt_sql->ptr(),
    tgt_length,
    quick_mode,
    need_mon
  ));
}

int spider_mysql_handler::reset()
{
  DBUG_ENTER("spider_mysql_handler::reset");
  DBUG_PRINT("info",("spider this=%p", this));
  update_sql.length(0);
  DBUG_RETURN(0);
}

int spider_mysql_handler::sts_mode_exchange(
  int sts_mode
) {
  DBUG_ENTER("spider_mysql_handler::sts_mode_exchange");
  DBUG_PRINT("info",("spider sts_mode=%d", sts_mode));
  DBUG_RETURN(sts_mode);
}

int spider_mysql_handler::show_table_status(
  int link_idx,
  int sts_mode,
  uint flag
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  SPIDER_DB_RESULT *res;
  SPIDER_SHARE *share = spider->share;
  uint pos = (2 * spider->conn_link_idx[link_idx]);
  ulonglong auto_increment_value = 0;
  DBUG_ENTER("spider_mysql_handler::show_table_status");
  DBUG_PRINT("info",("spider sts_mode=%d", sts_mode));

  if (sts_mode == 1)
  {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = &spider->need_mons[link_idx];
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    conn->disable_connect_retry = TRUE;
    spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
      share);
    if (
      (error_num = spider_db_set_names(spider, conn, link_idx)) ||
      (
        spider_db_query(
          conn,
          mysql_share->show_table_status[0 + pos].ptr(),
          mysql_share->show_table_status[0 + pos].length(),
          -1,
          &spider->need_mons[link_idx]) &&
        (error_num = spider_db_errorno(conn))
      )
    ) {
      if (
        error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
        !conn->disable_reconnect
      ) {
        /* retry */
        if ((error_num = spider_db_ping(spider, conn, link_idx)))
        {
          conn->disable_connect_retry = FALSE;
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        if ((error_num = spider_db_set_names(spider, conn, link_idx)))
        {
          conn->disable_connect_retry = FALSE;
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
          share);
        if (spider_db_query(
          conn,
          mysql_share->show_table_status[0 + pos].ptr(),
          mysql_share->show_table_status[0 + pos].length(),
          -1,
          &spider->need_mons[link_idx])
        ) {
          conn->disable_connect_retry = FALSE;
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          DBUG_RETURN(spider_db_errorno(conn));
        }
      } else {
        conn->disable_connect_retry = FALSE;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
    }
    st_spider_db_request_key request_key;
    request_key.spider_thread_id = spider->trx->spider_thread_id;
    request_key.query_id = spider->trx->thd->query_id;
    request_key.handler = spider;
    request_key.request_id = 1;
    request_key.next = NULL;
    if (spider_param_dry_access())
    {
      conn->disable_connect_retry = FALSE;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_RETURN(0);
    }
    if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
    {
      conn->disable_connect_retry = FALSE;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      if (error_num)
      {
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
      else if ((error_num = spider_db_errorno(conn)))
        DBUG_RETURN(error_num);
      else
      {
        my_printf_error(ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM,
          ER_SPIDER_REMOTE_TABLE_NOT_FOUND_STR, MYF(0),
          mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
          mysql_share->table_names_str[spider->conn_link_idx[
            link_idx]].ptr());
        DBUG_RETURN(ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM);
      }
    }
    conn->disable_connect_retry = FALSE;
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    error_num = res->fetch_table_status(
      sts_mode,
      share->records,
      share->mean_rec_length,
      share->data_file_length,
      share->max_data_file_length,
      share->index_file_length,
      auto_increment_value,
      share->create_time,
      share->update_time,
      share->check_time
    );
    res->free_result();
    delete res;
    if (error_num)
    {
      switch (error_num)
      {
        case ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM:
          my_printf_error(ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM,
            ER_SPIDER_REMOTE_TABLE_NOT_FOUND_STR, MYF(0),
            mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
            mysql_share->table_names_str[spider->conn_link_idx[
              link_idx]].ptr());
          break;
        case ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM:
          my_printf_error(ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM,
            ER_SPIDER_INVALID_REMOTE_TABLE_INFO_STR, MYF(0),
            mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
            mysql_share->table_names_str[spider->conn_link_idx[
              link_idx]].ptr());
          break;
        default:
          break;
      }
      DBUG_RETURN(error_num);
    }
  } else {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = &spider->need_mons[link_idx];
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    conn->disable_connect_retry = TRUE;
    spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
      share);
    if (
      (error_num = spider_db_set_names(spider, conn, link_idx)) ||
      (
        spider_db_query(
          conn,
          mysql_share->show_table_status[1 + pos].ptr(),
          mysql_share->show_table_status[1 + pos].length(),
          -1,
          &spider->need_mons[link_idx]) &&
        (error_num = spider_db_errorno(conn))
      )
    ) {
      if (
        error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
        !conn->disable_reconnect
      ) {
        /* retry */
        if ((error_num = spider_db_ping(spider, conn, link_idx)))
        {
          conn->disable_connect_retry = FALSE;
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        if ((error_num = spider_db_set_names(spider, conn, link_idx)))
        {
          conn->disable_connect_retry = FALSE;
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
          share);
        if (spider_db_query(
          conn,
          mysql_share->show_table_status[1 + pos].ptr(),
          mysql_share->show_table_status[1 + pos].length(),
          -1,
          &spider->need_mons[link_idx])
        ) {
          conn->disable_connect_retry = FALSE;
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          DBUG_RETURN(spider_db_errorno(conn));
        }
      } else {
        conn->disable_connect_retry = FALSE;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
    }
    st_spider_db_request_key request_key;
    request_key.spider_thread_id = spider->trx->spider_thread_id;
    request_key.query_id = spider->trx->thd->query_id;
    request_key.handler = spider;
    request_key.request_id = 1;
    request_key.next = NULL;
    if (spider_param_dry_access())
    {
      conn->disable_connect_retry = FALSE;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_RETURN(0);
    }
    if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
    {
      conn->disable_connect_retry = FALSE;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      if (error_num || (error_num = spider_db_errorno(conn)))
        DBUG_RETURN(error_num);
      else
        DBUG_RETURN(ER_QUERY_ON_FOREIGN_DATA_SOURCE);
    }
    conn->disable_connect_retry = FALSE;
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    error_num = res->fetch_table_status(
      sts_mode,
      share->records,
      share->mean_rec_length,
      share->data_file_length,
      share->max_data_file_length,
      share->index_file_length,
      auto_increment_value,
      share->create_time,
      share->update_time,
      share->check_time
    );
    res->free_result();
    delete res;
    if (error_num)
    {
      switch (error_num)
      {
        case ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM:
          my_printf_error(ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM,
            ER_SPIDER_REMOTE_TABLE_NOT_FOUND_STR, MYF(0),
            mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
            mysql_share->table_names_str[spider->conn_link_idx[
              link_idx]].ptr());
          break;
        case ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM:
          my_printf_error(ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM,
            ER_SPIDER_INVALID_REMOTE_TABLE_INFO_STR, MYF(0),
            mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
            mysql_share->table_names_str[spider->conn_link_idx[
              link_idx]].ptr());
          break;
        default:
          break;
      }
      DBUG_RETURN(error_num);
    }
  }
  if (share->static_records_for_status != -1)
  {
    share->records = (ha_rows) share->static_records_for_status;
  }
  if (share->static_mean_rec_length != -1)
  {
    share->mean_rec_length = (ulong) share->static_mean_rec_length;
  }
  if (auto_increment_value > share->lgtm_tblhnd_share->auto_increment_value)
  {
    share->lgtm_tblhnd_share->auto_increment_value = auto_increment_value;
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      share->lgtm_tblhnd_share->auto_increment_value));
  }

  DBUG_RETURN(0);
}

int spider_mysql_handler::crd_mode_exchange(
  int crd_mode
) {
  DBUG_ENTER("spider_mysql_handler::crd_mode_exchange");
  DBUG_PRINT("info",("spider crd_mode=%d", crd_mode));
  DBUG_RETURN(crd_mode);
}

int spider_mysql_handler::show_index(
  int link_idx,
  int crd_mode
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  SPIDER_SHARE *share = spider->share;
  TABLE *table = spider->get_table();
  SPIDER_DB_RESULT *res;
  int roop_count;
  longlong *tmp_cardinality;
  uint pos = (2 * spider->conn_link_idx[link_idx]);
  DBUG_ENTER("spider_mysql_handler::show_index");
  DBUG_PRINT("info",("spider crd_mode=%d", crd_mode));
  if (crd_mode == 1)
  {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = &spider->need_mons[link_idx];
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
      share);
    if (
      (error_num = spider_db_set_names(spider, conn, link_idx)) ||
      (
        spider_db_query(
          conn,
          mysql_share->show_index[0 + pos].ptr(),
          mysql_share->show_index[0 + pos].length(),
          -1,
          &spider->need_mons[link_idx]) &&
        (error_num = spider_db_errorno(conn))
      )
    ) {
      if (
        error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
        !conn->disable_reconnect
      ) {
        /* retry */
        if ((error_num = spider_db_ping(spider, conn, link_idx)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        if ((error_num = spider_db_set_names(spider, conn, link_idx)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
          share);
        if (spider_db_query(
          conn,
          mysql_share->show_index[0 + pos].ptr(),
          mysql_share->show_index[0 + pos].length(),
          -1,
          &spider->need_mons[link_idx])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          DBUG_RETURN(spider_db_errorno(conn));
        }
      } else {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
    }
    st_spider_db_request_key request_key;
    request_key.spider_thread_id = spider->trx->spider_thread_id;
    request_key.query_id = spider->trx->thd->query_id;
    request_key.handler = spider;
    request_key.request_id = 1;
    request_key.next = NULL;
    if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
    {
      if (error_num || (error_num = spider_db_errorno(conn)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
      /* no record is ok */
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    if (res)
    {
      error_num = res->fetch_table_cardinality(
        crd_mode,
        table,
        share->cardinality,
        share->cardinality_upd,
        share->bitmap_size
      );
    }
    for (roop_count = 0, tmp_cardinality = share->cardinality;
      roop_count < (int) table->s->fields;
      roop_count++, tmp_cardinality++)
    {
      if (!spider_bit_is_set(share->cardinality_upd, roop_count))
      {
        DBUG_PRINT("info",
          ("spider uninitialized column cardinality id=%d", roop_count));
        *tmp_cardinality = -1;
      }
    }
    if (res)
    {
      res->free_result();
      delete res;
    }
    if (error_num)
    {
      switch (error_num)
      {
        case ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM:
          my_printf_error(ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM,
            ER_SPIDER_REMOTE_TABLE_NOT_FOUND_STR, MYF(0),
            mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
            mysql_share->table_names_str[spider->conn_link_idx[
              link_idx]].ptr());
          break;
        case ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM:
          my_printf_error(ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM,
            ER_SPIDER_INVALID_REMOTE_TABLE_INFO_STR, MYF(0),
            mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
            mysql_share->table_names_str[spider->conn_link_idx[
              link_idx]].ptr());
          break;
        default:
          break;
      }
      DBUG_RETURN(error_num);
    }
  } else {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = &spider->need_mons[link_idx];
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
      share);
    if (
      (error_num = spider_db_set_names(spider, conn, link_idx)) ||
      (
        spider_db_query(
          conn,
          mysql_share->show_index[1 + pos].ptr(),
          mysql_share->show_index[1 + pos].length(),
          -1,
          &spider->need_mons[link_idx]) &&
        (error_num = spider_db_errorno(conn))
      )
    ) {
      if (
        error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
        !conn->disable_reconnect
      ) {
        /* retry */
        if ((error_num = spider_db_ping(spider, conn, link_idx)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        if ((error_num = spider_db_set_names(spider, conn, link_idx)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
          share);
        if (spider_db_query(
          conn,
          mysql_share->show_index[1 + pos].ptr(),
          mysql_share->show_index[1 + pos].length(),
          -1,
          &spider->need_mons[link_idx])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          DBUG_RETURN(spider_db_errorno(conn));
        }
      } else {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
    }
    st_spider_db_request_key request_key;
    request_key.spider_thread_id = spider->trx->spider_thread_id;
    request_key.query_id = spider->trx->thd->query_id;
    request_key.handler = spider;
    request_key.request_id = 1;
    request_key.next = NULL;
    if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
    {
      if (error_num || (error_num = spider_db_errorno(conn)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
      /* no record is ok */
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    if (res)
    {
      error_num = res->fetch_table_cardinality(
        crd_mode,
        table,
        share->cardinality,
        share->cardinality_upd,
        share->bitmap_size
      );
    }
    for (roop_count = 0, tmp_cardinality = share->cardinality;
      roop_count < (int) table->s->fields;
      roop_count++, tmp_cardinality++)
    {
      if (!spider_bit_is_set(share->cardinality_upd, roop_count))
      {
        DBUG_PRINT("info",
          ("spider uninitialized column cardinality id=%d", roop_count));
        *tmp_cardinality = -1;
      }
    }
    if (res)
    {
      res->free_result();
      delete res;
    }
    if (error_num)
    {
      switch (error_num)
      {
        case ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM:
          my_printf_error(ER_SPIDER_REMOTE_TABLE_NOT_FOUND_NUM,
            ER_SPIDER_REMOTE_TABLE_NOT_FOUND_STR, MYF(0),
            mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
            mysql_share->table_names_str[spider->conn_link_idx[
              link_idx]].ptr());
          break;
        case ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM:
          my_printf_error(ER_SPIDER_INVALID_REMOTE_TABLE_INFO_NUM,
            ER_SPIDER_INVALID_REMOTE_TABLE_INFO_STR, MYF(0),
            mysql_share->db_names_str[spider->conn_link_idx[link_idx]].ptr(),
            mysql_share->table_names_str[spider->conn_link_idx[
              link_idx]].ptr());
          break;
        default:
          break;
      }
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::show_records(
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  SPIDER_DB_RESULT *res;
  SPIDER_SHARE *share = spider->share;
  uint pos = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_mysql_handler::show_records");
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  if (
    (error_num = spider_db_set_names(spider, conn, link_idx)) ||
    (
      spider_db_query(
        conn,
        mysql_share->show_records[pos].ptr(),
        mysql_share->show_records[pos].length(),
        -1,
        &spider->need_mons[link_idx]) &&
      (error_num = spider_db_errorno(conn))
    )
  ) {
    if (
      error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
      !conn->disable_reconnect
    ) {
      /* retry */
      if ((error_num = spider_db_ping(spider, conn, link_idx)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_PRINT("info", ("spider error_num=%d 1", error_num));
        DBUG_RETURN(error_num);
      }
      if ((error_num = spider_db_set_names(spider, conn, link_idx)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_PRINT("info", ("spider error_num=%d 2", error_num));
        DBUG_RETURN(error_num);
      }
      spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
        share);
      if (spider_db_query(
        conn,
        mysql_share->show_records[pos].ptr(),
        mysql_share->show_records[pos].length(),
        -1,
        &spider->need_mons[link_idx])
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        DBUG_PRINT("info", ("spider error_num=%d 3", error_num));
        DBUG_RETURN(spider_db_errorno(conn));
      }
    } else {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_PRINT("info", ("spider error_num=%d 4", error_num));
      DBUG_RETURN(error_num);
    }
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = spider->trx->spider_thread_id;
  request_key.query_id = spider->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = 1;
  request_key.next = NULL;
  if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    if (error_num)
    {
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_PRINT("info", ("spider error_num=%d 5", error_num));
      DBUG_RETURN(error_num);
    }
    else if ((error_num = spider_db_errorno(conn)))
    {
      DBUG_PRINT("info", ("spider error_num=%d 6", error_num));
      DBUG_RETURN(error_num);
    } else {
      DBUG_PRINT("info", ("spider error_num=%d 7",
        ER_QUERY_ON_FOREIGN_DATA_SOURCE));
      DBUG_RETURN(ER_QUERY_ON_FOREIGN_DATA_SOURCE);
    }
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  error_num = res->fetch_table_records(
    1,
    spider->table_rows
  );
  res->free_result();
  delete res;
  if (error_num)
  {
    DBUG_PRINT("info", ("spider error_num=%d 7", error_num));
    DBUG_RETURN(error_num);
  }
  spider->trx->direct_aggregate_count++;
  DBUG_RETURN(0);
}

int spider_mysql_handler::show_last_insert_id(
  int link_idx,
  ulonglong &last_insert_id
) {
  SPIDER_CONN *conn = spider->conns[link_idx];
  DBUG_ENTER("spider_mysql_handler::show_last_insert_id");
  last_insert_id = conn->db_conn->last_insert_id();
  DBUG_RETURN(0);
}

ha_rows spider_mysql_handler::explain_select(
  key_range *start_key,
  key_range *end_key,
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  spider_string *str = &result_list->sqls[link_idx];
  SPIDER_DB_RESULT *res;
  ha_rows rows;
  spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
  DBUG_ENTER("spider_mysql_handler::explain_select");
  if ((error_num = dbton_hdl->append_explain_select_part(
    start_key, end_key, SPIDER_SQL_TYPE_OTHER_SQL, link_idx)))
  {
    my_errno = error_num;
    DBUG_RETURN(HA_POS_ERROR);
  }

  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    spider->share);
  if (
    (error_num = spider_db_set_names(spider, conn, link_idx)) ||
    (
      spider_db_query(
        conn,
        str->ptr(),
        str->length(),
        -1,
        &spider->need_mons[link_idx]) &&
      (error_num = spider_db_errorno(conn))
    )
  ) {
    if (
      error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
      !conn->disable_reconnect
    ) {
      /* retry */
      if ((error_num = spider_db_ping(spider, conn, link_idx)))
      {
        if (spider->check_error_mode(error_num))
          my_errno = error_num;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(HA_POS_ERROR);
      }
      if ((error_num = spider_db_set_names(spider, conn, link_idx)))
      {
        if (spider->check_error_mode(error_num))
          my_errno = error_num;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(HA_POS_ERROR);
      }
      spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
        spider->share);
      if (spider_db_query(
        conn,
        str->ptr(),
        str->length(),
        -1,
        &spider->need_mons[link_idx])
      ) {
        error_num = spider_db_errorno(conn);
        if (spider->check_error_mode(error_num))
          my_errno = error_num;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(HA_POS_ERROR);
      }
    } else {
      if (spider->check_error_mode(error_num))
        my_errno = error_num;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_RETURN(HA_POS_ERROR);
    }
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = spider->trx->spider_thread_id;
  request_key.query_id = spider->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = 1;
  request_key.next = NULL;
  if (!(res = conn->db_conn->store_result(NULL, &request_key, &error_num)))
  {
    if (error_num || (error_num = spider_db_errorno(conn)))
    {
      if (spider->check_error_mode(error_num))
        my_errno = error_num;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_RETURN(HA_POS_ERROR);
    } else {
      my_errno = ER_QUERY_ON_FOREIGN_DATA_SOURCE;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_RETURN(HA_POS_ERROR);
    }
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  error_num = res->fetch_table_records(
    2,
    rows
  );
  res->free_result();
  delete res;
  if (error_num)
  {
    my_errno = error_num;
    DBUG_RETURN(HA_POS_ERROR);
  }
  DBUG_RETURN(rows);
}

int spider_mysql_handler::lock_tables(
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  spider_string *str = &sql;
  DBUG_ENTER("spider_mysql_handler::lock_tables");
  str->length(0);
  if ((error_num = conn->db_conn->append_lock_tables(str)))
  {
    DBUG_RETURN(error_num);
  }
  if (str->length())
  {
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = &spider->need_mons[link_idx];
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if ((error_num = spider_db_set_names(spider, conn, link_idx)))
    {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_RETURN(error_num);
    }
    spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
      spider->share);
    if (spider_db_query(
      conn,
      str->ptr(),
      str->length(),
      -1,
      &spider->need_mons[link_idx])
    ) {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      DBUG_RETURN(spider_db_errorno(conn));
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  }
  if (!conn->table_locked)
  {
    conn->table_locked = TRUE;
    spider->trx->locked_connections++;
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::unlock_tables(
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  DBUG_ENTER("spider_mysql_handler::unlock_tables");
  if (conn->table_locked)
  {
    spider_string *str = &sql;
    conn->table_locked = FALSE;
    spider->trx->locked_connections--;

    str->length(0);
    if ((error_num = conn->db_conn->append_unlock_tables(str)))
    {
      DBUG_RETURN(error_num);
    }
    if (str->length())
    {
      spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
        spider->share);
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      conn->need_mon = &spider->need_mons[link_idx];
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if (spider_db_query(
        conn,
        str->ptr(),
        str->length(),
        -1,
        &spider->need_mons[link_idx])
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        DBUG_RETURN(spider_db_errorno(conn));
      }
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
    }
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::disable_keys(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::disable_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  if ((error_num = append_disable_keys_part(SPIDER_SQL_TYPE_OTHER_HS,
    link_idx)))
  {
    DBUG_RETURN(error_num);
  }
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if ((error_num = spider_db_set_names(spider, conn, link_idx)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  if (spider_db_query(
    conn,
    str->ptr(),
    str->length(),
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    DBUG_RETURN(error_num);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_mysql_handler::enable_keys(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::enable_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  if ((error_num = append_enable_keys_part(SPIDER_SQL_TYPE_OTHER_HS,
    link_idx)))
  {
    DBUG_RETURN(error_num);
  }
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if ((error_num = spider_db_set_names(spider, conn, link_idx)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  if (spider_db_query(
    conn,
    str->ptr(),
    str->length(),
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    DBUG_RETURN(error_num);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_mysql_handler::check_table(
  SPIDER_CONN *conn,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::check_table");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  if ((error_num = append_check_table_part(SPIDER_SQL_TYPE_OTHER_HS,
    link_idx, check_opt)))
  {
    DBUG_RETURN(error_num);
  }
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if ((error_num = spider_db_set_names(spider, conn, link_idx)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  if (spider_db_query(
    conn,
    str->ptr(),
    str->length(),
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    DBUG_RETURN(error_num);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_mysql_handler::repair_table(
  SPIDER_CONN *conn,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::repair_table");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  if ((error_num = append_repair_table_part(SPIDER_SQL_TYPE_OTHER_HS,
    link_idx, check_opt)))
  {
    DBUG_RETURN(error_num);
  }
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if ((error_num = spider_db_set_names(spider, conn, link_idx)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  if (spider_db_query(
    conn,
    str->ptr(),
    str->length(),
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    DBUG_RETURN(error_num);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_mysql_handler::analyze_table(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::analyze_table");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  if ((error_num = append_analyze_table_part(SPIDER_SQL_TYPE_OTHER_HS,
    link_idx)))
  {
    DBUG_RETURN(error_num);
  }
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if ((error_num = spider_db_set_names(spider, conn, link_idx)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  if (spider_db_query(
    conn,
    str->ptr(),
    str->length(),
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    DBUG_RETURN(error_num);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_mysql_handler::optimize_table(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::optimize_table");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  if ((error_num = append_optimize_table_part(SPIDER_SQL_TYPE_OTHER_HS,
    link_idx)))
  {
    DBUG_RETURN(error_num);
  }
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if ((error_num = spider_db_set_names(spider, conn, link_idx)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  if (spider_db_query(
    conn,
    str->ptr(),
    str->length(),
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    DBUG_RETURN(error_num);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_mysql_handler::flush_tables(
  SPIDER_CONN *conn,
  int link_idx,
  bool lock
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_mysql_handler::flush_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  if ((error_num = append_flush_tables_part(SPIDER_SQL_TYPE_OTHER_HS,
    link_idx, lock)))
  {
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    str->ptr(),
    str->length(),
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    DBUG_RETURN(error_num);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_mysql_handler::flush_logs(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_mysql_handler::flush_logs");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_conn_set_timeout_from_share(conn, link_idx, spider->trx->thd,
    share);
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider_db_query(
    conn,
    SPIDER_SQL_FLUSH_LOGS_STR,
    SPIDER_SQL_FLUSH_LOGS_LEN,
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    DBUG_RETURN(error_num);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_mysql_handler::insert_opened_handler(
  SPIDER_CONN *conn,
  int link_idx
) {
  spider_db_mysql *db_conn = (spider_db_mysql *) conn->db_conn;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash = &link_for_hash[link_idx];
  DBUG_ASSERT(tmp_link_for_hash->spider == spider);
  DBUG_ASSERT(tmp_link_for_hash->link_idx == link_idx);
  uint old_elements = db_conn->handler_open_array.max_element;
  DBUG_ENTER("spider_mysql_handler::insert_opened_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  if (insert_dynamic(&db_conn->handler_open_array,
    (uchar*) &tmp_link_for_hash))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if (db_conn->handler_open_array.max_element > old_elements)
  {
    spider_alloc_calc_mem(spider_current_trx,
      db_conn->handler_open_array,
      (db_conn->handler_open_array.max_element - old_elements) *
      db_conn->handler_open_array.size_of_element);
  }
  DBUG_RETURN(0);
}

int spider_mysql_handler::delete_opened_handler(
  SPIDER_CONN *conn,
  int link_idx
) {
  spider_db_mysql *db_conn = (spider_db_mysql *) conn->db_conn;
  uint roop_count, elements = db_conn->handler_open_array.elements;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash;
  DBUG_ENTER("spider_mysql_handler::delete_opened_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  for (roop_count = 0; roop_count < elements; roop_count++)
  {
    get_dynamic(&db_conn->handler_open_array, (uchar *) &tmp_link_for_hash,
      roop_count);
    if (tmp_link_for_hash == &link_for_hash[link_idx])
    {
      delete_dynamic_element(&db_conn->handler_open_array, roop_count);
      break;
    }
  }
  DBUG_ASSERT(roop_count < elements);
  DBUG_RETURN(0);
}

int spider_mysql_handler::sync_from_clone_source(
  spider_db_handler *dbton_hdl
) {
  DBUG_ENTER("spider_mysql_handler::sync_from_clone_source");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

bool spider_mysql_handler::support_use_handler(
  int use_handler
) {
  DBUG_ENTER("spider_mysql_handler::support_use_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

void spider_mysql_handler::minimum_select_bitmap_create()
{
  TABLE *table = spider->get_table();
  Field **field_p;
  DBUG_ENTER("spider_mysql_handler::minimum_select_bitmap_create");
  DBUG_PRINT("info",("spider this=%p", this));
  memset(minimum_select_bitmap, 0, no_bytes_in_map(table->read_set));
  if (
    spider->use_index_merge ||
#ifdef HA_CAN_BULK_ACCESS
    (spider->is_clone && !spider->is_bulk_access_clone)
#else
    spider->is_clone
#endif
  ) {
    /* need preparing for cmp_ref */
    TABLE_SHARE *table_share = table->s;
    if (
      table_share->primary_key == MAX_KEY
    ) {
      /* need all columns */
      memset(minimum_select_bitmap, 0xFF, no_bytes_in_map(table->read_set));
      DBUG_VOID_RETURN;
    } else {
      /* need primary key columns */
      uint roop_count;
      KEY *key_info;
      KEY_PART_INFO *key_part;
      Field *field;
      key_info = &table_share->key_info[table_share->primary_key];
      key_part = key_info->key_part;
      for (roop_count = 0;
        roop_count < spider_user_defined_key_parts(key_info);
        roop_count++)
      {
        field = key_part[roop_count].field;
        spider_set_bit(minimum_select_bitmap, field->field_index);
      }
    }
  }
  DBUG_PRINT("info",("spider searched_bitmap=%p", spider->searched_bitmap));
  for (field_p = table->field; *field_p; field_p++)
  {
    uint field_index = (*field_p)->field_index;
    DBUG_PRINT("info",("spider field_index=%u", field_index));
    DBUG_PRINT("info",("spider ft_discard_bitmap=%s",
      spider_bit_is_set(spider->ft_discard_bitmap, field_index) ?
        "TRUE" : "FALSE"));
    DBUG_PRINT("info",("spider searched_bitmap=%s",
      spider_bit_is_set(spider->searched_bitmap, field_index) ?
        "TRUE" : "FALSE"));
    DBUG_PRINT("info",("spider read_set=%s",
      bitmap_is_set(table->read_set, field_index) ?
        "TRUE" : "FALSE"));
    DBUG_PRINT("info",("spider write_set=%s",
      bitmap_is_set(table->write_set, field_index) ?
        "TRUE" : "FALSE"));
    if (
      spider_bit_is_set(spider->ft_discard_bitmap, field_index) &
      (
        spider_bit_is_set(spider->searched_bitmap, field_index) |
        bitmap_is_set(table->read_set, field_index) |
        bitmap_is_set(table->write_set, field_index)
      )
    ) {
      spider_set_bit(minimum_select_bitmap, field_index);
    }
  }
  DBUG_VOID_RETURN;
}

bool spider_mysql_handler::minimum_select_bit_is_set(
  uint field_index
) {
  DBUG_ENTER("spider_mysql_handler::minimum_select_bit_is_set");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider field_index=%u", field_index));
  DBUG_PRINT("info",("spider minimum_select_bitmap=%s",
    spider_bit_is_set(minimum_select_bitmap, field_index) ?
      "TRUE" : "FALSE"));
  DBUG_RETURN(spider_bit_is_set(minimum_select_bitmap, field_index));
}

void spider_mysql_handler::copy_minimum_select_bitmap(
  uchar *bitmap
) {
  int roop_count;
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_mysql_handler::copy_minimum_select_bitmap");
  for (roop_count = 0;
    roop_count < (int) ((table->s->fields + 7) / 8);
    roop_count++)
  {
    bitmap[roop_count] =
      minimum_select_bitmap[roop_count];
    DBUG_PRINT("info",("spider roop_count=%d", roop_count));
    DBUG_PRINT("info",("spider bitmap=%d",
      bitmap[roop_count]));
  }
  DBUG_VOID_RETURN;
}

int spider_mysql_handler::init_union_table_name_pos()
{
  DBUG_ENTER("spider_mysql_handler::init_union_table_name_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!union_table_name_pos_first)
  {
    if (!spider_bulk_malloc(spider_current_trx, 236, MYF(MY_WME),
      &union_table_name_pos_first, sizeof(SPIDER_INT_HLD),
      NullS)
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    union_table_name_pos_first->next = NULL;
  }
  union_table_name_pos_current = union_table_name_pos_first;
  union_table_name_pos_current->tgt_num = 0;
  DBUG_RETURN(0);
}

int spider_mysql_handler::set_union_table_name_pos()
{
  DBUG_ENTER("spider_mysql_handler::set_union_table_name_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  if (union_table_name_pos_current->tgt_num >= SPIDER_INT_HLD_TGT_SIZE)
  {
    if (!union_table_name_pos_current->next)
    {
      if (!spider_bulk_malloc(spider_current_trx, 237, MYF(MY_WME),
        &union_table_name_pos_current->next, sizeof(SPIDER_INT_HLD),
        NullS)
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      union_table_name_pos_current->next->next = NULL;
    }
    union_table_name_pos_current = union_table_name_pos_current->next;
    union_table_name_pos_current->tgt_num = 0;
  }
  union_table_name_pos_current->tgt[union_table_name_pos_current->tgt_num] =
    table_name_pos;
  ++union_table_name_pos_current->tgt_num;
  DBUG_RETURN(0);
}

int spider_mysql_handler::reset_union_table_name(
  spider_string *str,
  int link_idx,
  ulong sql_type
) {
  DBUG_ENTER("spider_mysql_handler::reset_union_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!union_table_name_pos_current)
    DBUG_RETURN(0);

  SPIDER_INT_HLD *tmp_pos = union_table_name_pos_first;
  uint cur_num, pos_backup = str->length();
  while(TRUE)
  {
    for (cur_num = 0; cur_num < tmp_pos->tgt_num; ++cur_num)
    {
      str->length(tmp_pos->tgt[cur_num]);
      append_table_name_with_adjusting(str, link_idx, sql_type);
    }
    if (tmp_pos == union_table_name_pos_current)
      break;
    tmp_pos = tmp_pos->next;
  }
  str->length(pos_backup);
  DBUG_RETURN(0);
}

spider_mysql_copy_table::spider_mysql_copy_table(
  spider_mysql_share *db_share
) : spider_db_copy_table(
  db_share
),
  mysql_share(db_share)
{
  DBUG_ENTER("spider_mysql_copy_table::spider_mysql_copy_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_mysql_copy_table::~spider_mysql_copy_table()
{
  DBUG_ENTER("spider_mysql_copy_table::~spider_mysql_copy_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

int spider_mysql_copy_table::init()
{
  DBUG_ENTER("spider_mysql_copy_table::init");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.init_calc_mem(78);
  DBUG_RETURN(0);
}

void spider_mysql_copy_table::set_sql_charset(
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_mysql_copy_table::set_sql_charset");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.set_charset(cs);
  DBUG_VOID_RETURN;
}

int spider_mysql_copy_table::append_select_str()
{
  DBUG_ENTER("spider_mysql_copy_table::append_select_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_SELECT_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_SELECT_STR, SPIDER_SQL_SELECT_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_insert_str(
  int insert_flg
) {
  DBUG_ENTER("spider_mysql_copy_table::append_insert_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (insert_flg & SPIDER_DB_INSERT_REPLACE)
  {
    if (sql.reserve(SPIDER_SQL_REPLACE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_REPLACE_STR, SPIDER_SQL_REPLACE_LEN);
  } else {
    if (sql.reserve(SPIDER_SQL_INSERT_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_INSERT_STR, SPIDER_SQL_INSERT_LEN);
  }
  if (insert_flg & SPIDER_DB_INSERT_LOW_PRIORITY)
  {
    if (sql.reserve(SPIDER_SQL_LOW_PRIORITY_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_LOW_PRIORITY_STR, SPIDER_SQL_LOW_PRIORITY_LEN);
  }
  else if (insert_flg & SPIDER_DB_INSERT_DELAYED)
  {
    if (sql.reserve(SPIDER_SQL_SQL_DELAYED_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_SQL_DELAYED_STR, SPIDER_SQL_SQL_DELAYED_LEN);
  }
  else if (insert_flg & SPIDER_DB_INSERT_HIGH_PRIORITY)
  {
    if (sql.reserve(SPIDER_SQL_HIGH_PRIORITY_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_HIGH_PRIORITY_STR, SPIDER_SQL_HIGH_PRIORITY_LEN);
  }
  if (insert_flg & SPIDER_DB_INSERT_IGNORE)
  {
    if (sql.reserve(SPIDER_SQL_SQL_IGNORE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_SQL_IGNORE_STR, SPIDER_SQL_SQL_IGNORE_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_table_columns(
  TABLE_SHARE *table_share
) {
  int error_num;
  Field **field;
  DBUG_ENTER("spider_mysql_copy_table::append_table_columns");
  DBUG_PRINT("info",("spider this=%p", this));
  for (field = table_share->field; *field; field++)
  {
    if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    if ((error_num = spider_db_append_name_with_quote_str(&sql,
      (char *) (*field)->field_name, spider_dbton_mysql.dbton_id)))
      DBUG_RETURN(error_num);
    if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  sql.length(sql.length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_from_str()
{
  DBUG_ENTER("spider_mysql_copy_table::append_from_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_FROM_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_table_name(
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_mysql_copy_table::append_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = mysql_share->append_table_name(&sql, link_idx);
  DBUG_RETURN(error_num);
}

void spider_mysql_copy_table::set_sql_pos()
{
  DBUG_ENTER("spider_mysql_copy_table::set_sql_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  pos = sql.length();
  DBUG_VOID_RETURN;
}

void spider_mysql_copy_table::set_sql_to_pos()
{
  DBUG_ENTER("spider_mysql_copy_table::set_sql_to_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.length(pos);
  DBUG_VOID_RETURN;
}

int spider_mysql_copy_table::append_copy_where(
  spider_db_copy_table *source_ct,
  KEY *key_info,
  ulong *last_row_pos,
  ulong *last_lengths
) {
  int error_num, roop_count, roop_count2;
  DBUG_ENTER("spider_mysql_copy_table::append_copy_where");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_WHERE_LEN + SPIDER_SQL_OPEN_PAREN_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  sql.q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  Field *field;
  KEY_PART_INFO *key_part = key_info->key_part;
  for (roop_count = spider_user_defined_key_parts(key_info) - 1;
    roop_count >= 0; roop_count--)
  {
    for (roop_count2 = 0; roop_count2 < roop_count; roop_count2++)
    {
      field = key_part[roop_count2].field;
      if ((error_num = copy_key_row(source_ct,
        field, &last_row_pos[field->field_index],
        &last_lengths[field->field_index],
        SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN)))
      {
        DBUG_RETURN(error_num);
      }
    }
    field = key_part[roop_count2].field;
    if ((error_num = copy_key_row(source_ct,
      field, &last_row_pos[field->field_index],
      &last_lengths[field->field_index],
      SPIDER_SQL_GT_STR, SPIDER_SQL_GT_LEN)))
    {
      DBUG_RETURN(error_num);
    }
    sql.length(sql.length() - SPIDER_SQL_AND_LEN);
    if (sql.reserve(SPIDER_SQL_CLOSE_PAREN_LEN +
      SPIDER_SQL_OR_LEN + SPIDER_SQL_OPEN_PAREN_LEN))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    sql.q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
    sql.q_append(SPIDER_SQL_OR_STR, SPIDER_SQL_OR_LEN);
    sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  }
  sql.length(sql.length() - SPIDER_SQL_OR_LEN - SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_key_order_str(
  KEY *key_info,
  int start_pos,
  bool desc_flg
) {
  int length, error_num;
  KEY_PART_INFO *key_part;
  Field *field;
  DBUG_ENTER("spider_mysql_copy_table::append_key_order_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((int) spider_user_defined_key_parts(key_info) > start_pos)
  {
    if (sql.reserve(SPIDER_SQL_ORDER_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_ORDER_STR, SPIDER_SQL_ORDER_LEN);
    if (desc_flg == TRUE)
    {
      for (
        key_part = key_info->key_part + start_pos,
        length = 0;
        length + start_pos < (int) spider_user_defined_key_parts(key_info);
        key_part++,
        length++
      ) {
        field = key_part->field;
        if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
        if ((error_num = spider_db_append_name_with_quote_str(&sql,
          (char *) field->field_name, spider_dbton_mysql.dbton_id)))
          DBUG_RETURN(error_num);
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
          sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_DESC_LEN +
            SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
          sql.q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        }
      }
    } else {
      for (
        key_part = key_info->key_part + start_pos,
        length = 0;
        length + start_pos < (int) spider_user_defined_key_parts(key_info);
        key_part++,
        length++
      ) {
        field = key_part->field;
        if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
        if ((error_num = spider_db_append_name_with_quote_str(&sql,
          (char *) field->field_name, spider_dbton_mysql.dbton_id)))
          DBUG_RETURN(error_num);
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_DESC_LEN +
            SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
          sql.q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
          sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        }
      }
    }
    sql.length(sql.length() - SPIDER_SQL_COMMA_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_limit(
  longlong offset,
  longlong limit
) {
  char buf[SPIDER_LONGLONG_LEN + 1];
  uint32 length;
  DBUG_ENTER("spider_mysql_copy_table::append_limit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (offset || limit < 9223372036854775807LL)
  {
    if (sql.reserve(SPIDER_SQL_LIMIT_LEN + SPIDER_SQL_COMMA_LEN +
      ((SPIDER_LONGLONG_LEN) * 2)))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_LIMIT_STR, SPIDER_SQL_LIMIT_LEN);
    if (offset)
    {
      length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
        &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, offset);
      sql.q_append(buf, length);
      sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
    length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
      &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, limit);
    sql.q_append(buf, length);
  }
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_into_str()
{
  DBUG_ENTER("spider_mysql_copy_table::append_into_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_INTO_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_INTO_STR, SPIDER_SQL_INTO_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_open_paren_str()
{
  DBUG_ENTER("spider_mysql_copy_table::append_open_paren_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_values_str()
{
  DBUG_ENTER("spider_mysql_copy_table::append_values_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_VALUES_LEN +
    SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  sql.q_append(SPIDER_SQL_VALUES_STR, SPIDER_SQL_VALUES_LEN);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_select_lock_str(
  int lock_mode
) {
  DBUG_ENTER("spider_mysql_copy_table::append_select_lock_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (lock_mode == SPIDER_LOCK_MODE_EXCLUSIVE)
  {
    if (sql.reserve(SPIDER_SQL_FOR_UPDATE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_FOR_UPDATE_STR, SPIDER_SQL_FOR_UPDATE_LEN);
  } else if (lock_mode == SPIDER_LOCK_MODE_SHARED)
  {
    if (sql.reserve(SPIDER_SQL_SHARED_LOCK_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_SHARED_LOCK_STR, SPIDER_SQL_SHARED_LOCK_LEN);
  }
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::exec_query(
  SPIDER_CONN *conn,
  int quick_mode,
  int *need_mon
) {
  int error_num;
  DBUG_ENTER("spider_mysql_copy_table::exec_query");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = spider_db_query(conn, sql.ptr(), sql.length(), quick_mode,
    need_mon);
  DBUG_RETURN(error_num);
}

int spider_mysql_copy_table::copy_key_row(
  spider_db_copy_table *source_ct,
  Field *field,
  ulong *row_pos,
  ulong *length,
  const char *joint_str,
  const int joint_length
) {
  int error_num;
  spider_string *source_str = &((spider_mysql_copy_table *) source_ct)->sql;
  DBUG_ENTER("spider_mysql_copy_table::copy_key_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  if ((error_num = spider_db_append_name_with_quote_str(&sql,
    (char *) field->field_name, spider_dbton_mysql.dbton_id)))
    DBUG_RETURN(error_num);
  if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + joint_length + *length +
    SPIDER_SQL_AND_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  sql.q_append(joint_str, joint_length);
  sql.q_append(source_str->ptr() + *row_pos, *length);
  sql.q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::copy_row(
  Field *field,
  SPIDER_DB_ROW *row
) {
  int error_num;
  DBUG_ENTER("spider_mysql_copy_table::copy_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (row->is_null())
  {
    if (sql.reserve(SPIDER_SQL_NULL_LEN + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
  } else if (field->str_needs_quotes())
  {
    if (sql.reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    if ((error_num = row->append_escaped_to_str(&sql,
      spider_dbton_mysql.dbton_id)))
      DBUG_RETURN(error_num);
    if (sql.reserve(SPIDER_SQL_VALUE_QUOTE_LEN + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  } else {
    if ((error_num = row->append_to_str(&sql)))
      DBUG_RETURN(error_num);
    if (sql.reserve(SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::copy_rows(
  TABLE *table,
  SPIDER_DB_ROW *row,
  ulong **last_row_pos,
  ulong **last_lengths
) {
  int error_num;
  Field **field;
  ulong *lengths2, *row_pos2;
  DBUG_ENTER("spider_mysql_copy_table::copy_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  row_pos2 = *last_row_pos;
  lengths2 = *last_lengths;

  for (
    field = table->field;
    *field;
    field++,
    lengths2++
  ) {
    *row_pos2 = sql.length();
    if ((error_num =
      copy_row(*field, row)))
      DBUG_RETURN(error_num);
    *lengths2 = sql.length() - *row_pos2 - SPIDER_SQL_COMMA_LEN;
    row->next();
    row_pos2++;
  }
  sql.length(sql.length() - SPIDER_SQL_COMMA_LEN);
  if (sql.reserve(SPIDER_SQL_CLOSE_PAREN_LEN +
    SPIDER_SQL_COMMA_LEN + SPIDER_SQL_OPEN_PAREN_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  sql.q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::copy_rows(
  TABLE *table,
  SPIDER_DB_ROW *row
) {
  int error_num;
  Field **field;
  DBUG_ENTER("spider_mysql_copy_table::copy_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  for (
    field = table->field;
    *field;
    field++
  ) {
    if ((error_num =
      copy_row(*field, row)))
      DBUG_RETURN(error_num);
    row->next();
  }
  sql.length(sql.length() - SPIDER_SQL_COMMA_LEN);
  if (sql.reserve(SPIDER_SQL_CLOSE_PAREN_LEN +
    SPIDER_SQL_COMMA_LEN + SPIDER_SQL_OPEN_PAREN_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  sql.q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::append_insert_terminator()
{
  DBUG_ENTER("spider_mysql_copy_table::append_insert_terminator");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.length(sql.length() - SPIDER_SQL_COMMA_LEN - SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_mysql_copy_table::copy_insert_values(
  spider_db_copy_table *source_ct
) {
  spider_mysql_copy_table *tmp_ct = (spider_mysql_copy_table *) source_ct;
  spider_string *source_str = &tmp_ct->sql;
  int values_length = source_str->length() - tmp_ct->pos;
  const char *values_ptr = source_str->ptr() + tmp_ct->pos;
  DBUG_ENTER("spider_mysql_copy_table::copy_insert_values");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(values_length))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  sql.q_append(values_ptr, values_length);
  DBUG_RETURN(0);
}
