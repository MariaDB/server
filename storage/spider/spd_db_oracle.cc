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
#include "sql_partition.h"
#include "sql_analyse.h"
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
#include "sql_select.h"
#endif
#endif

#ifdef HAVE_ORACLE_OCI
#if (defined(WIN32) || defined(_WIN32) || defined(WINDOWS) || defined(_WINDOWS))
#include <Shlwapi.h>
#define strcasestr StrStr
#endif
#include <oci.h>
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_db_oracle.h"
#include "ha_spider.h"
#include "spd_conn.h"
#include "spd_db_conn.h"
#include "spd_malloc.h"
#include "spd_sys_table.h"
#include "spd_table.h"

extern struct charset_info_st *spd_charset_utf8_bin;

extern handlerton *spider_hton_ptr;
extern pthread_mutex_t spider_open_conn_mutex;
extern HASH spider_open_connections;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
extern const char spider_dig_upper[];

#define SPIDER_DB_WRAPPER_ORACLE "oracle"

#define SPIDER_SQL_NAME_QUOTE_STR "\""
#define SPIDER_SQL_NAME_QUOTE_LEN (sizeof(SPIDER_SQL_NAME_QUOTE_STR) - 1)
static const char *name_quote_str = SPIDER_SQL_NAME_QUOTE_STR;

#define SPIDER_SQL_ISO_READ_COMMITTED_STR "set transaction isolation level read committed"
#define SPIDER_SQL_ISO_READ_COMMITTED_LEN sizeof(SPIDER_SQL_ISO_READ_COMMITTED_STR) - 1
#define SPIDER_SQL_ISO_SERIALIZABLE_STR "set transaction isolation level serializable"
#define SPIDER_SQL_ISO_SERIALIZABLE_LEN sizeof(SPIDER_SQL_ISO_SERIALIZABLE_STR) - 1

#define SPIDER_SQL_START_TRANSACTION_STR "set transaction read write"
#define SPIDER_SQL_START_TRANSACTION_LEN sizeof(SPIDER_SQL_START_TRANSACTION_STR) - 1

#define SPIDER_SQL_AUTOCOMMIT_OFF_STR "set autocommit off"
#define SPIDER_SQL_AUTOCOMMIT_OFF_LEN sizeof(SPIDER_SQL_AUTOCOMMIT_OFF_STR) - 1
#define SPIDER_SQL_AUTOCOMMIT_ON_STR "set autocommit on"
#define SPIDER_SQL_AUTOCOMMIT_ON_LEN sizeof(SPIDER_SQL_AUTOCOMMIT_ON_STR) - 1

#define SPIDER_SQL_LOCK_TABLE_STR "lock table "
#define SPIDER_SQL_LOCK_TABLE_LEN (sizeof(SPIDER_SQL_LOCK_TABLE_STR) - 1)
#define SPIDER_SQL_UNLOCK_TABLE_STR "unlock tables"
#define SPIDER_SQL_UNLOCK_TABLE_LEN (sizeof(SPIDER_SQL_UNLOCK_TABLE_STR) - 1)
#define SPIDER_SQL_LOCK_TABLE_SHARE_MODE_STR " in share mode"
#define SPIDER_SQL_LOCK_TABLE_SHARE_MODE_LEN (sizeof(SPIDER_SQL_LOCK_TABLE_SHARE_MODE_STR) - 1)
#define SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_STR " in exclusive mode"
#define SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_LEN (sizeof(SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_STR) - 1)

#define SPIDER_SQL_COMMIT_STR "commit"
#define SPIDER_SQL_COMMIT_LEN sizeof(SPIDER_SQL_COMMIT_STR) - 1

#define SPIDER_SQL_SET_NLS_DATE_FORMAT_STR "alter session set nls_date_format='YYYY-MM-DD HH24:MI:SS'"
#define SPIDER_SQL_SET_NLS_DATE_FORMAT_LEN sizeof(SPIDER_SQL_SET_NLS_DATE_FORMAT_STR) - 1
#define SPIDER_SQL_SET_NLS_TIME_FORMAT_STR "alter session set nls_time_format='HH24:MI:SSXFF'"
#define SPIDER_SQL_SET_NLS_TIME_FORMAT_LEN sizeof(SPIDER_SQL_SET_NLS_TIME_FORMAT_STR) - 1
#define SPIDER_SQL_SET_NLS_TIMESTAMP_FORMAT_STR "alter session set nls_timestamp_format='YYYY-MM-DD HH24:MI:SSXFF'"
#define SPIDER_SQL_SET_NLS_TIMESTAMP_FORMAT_LEN sizeof(SPIDER_SQL_SET_NLS_TIMESTAMP_FORMAT_STR) - 1

#define SPIDER_SQL_SELECT_WRAPPER_HEAD_STR "select * from ("
#define SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN sizeof(SPIDER_SQL_SELECT_WRAPPER_HEAD_STR) - 1
#define SPIDER_SQL_UPDATE_WRAPPER_HEAD_STR " where rowid in (select rowid from (select rowid, row_number() over (order by "
#define SPIDER_SQL_UPDATE_WRAPPER_HEAD_LEN sizeof(SPIDER_SQL_UPDATE_WRAPPER_HEAD_STR) - 1
#define SPIDER_SQL_ROW_NUMBER_HEAD_STR ", row_number() over (order by "
#define SPIDER_SQL_ROW_NUMBER_HEAD_LEN sizeof(SPIDER_SQL_ROW_NUMBER_HEAD_STR) - 1
#define SPIDER_SQL_ROW_NUMBER_TAIL_STR "rowid) row_num"
#define SPIDER_SQL_ROW_NUMBER_TAIL_LEN sizeof(SPIDER_SQL_ROW_NUMBER_TAIL_STR) - 1
#define SPIDER_SQL_ROW_NUMBER_DESC_TAIL_STR "rowid desc) row_num"
#define SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN sizeof(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_STR) - 1
#define SPIDER_SQL_SELECT_WRAPPER_TAIL_STR ") where row_num "
#define SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN sizeof(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR) - 1
#define SPIDER_SQL_ROW_NUM_STR "row_num"
#define SPIDER_SQL_ROW_NUM_LEN sizeof(SPIDER_SQL_ROW_NUM_STR) - 1
#define SPIDER_SQL_ROWNUM_STR "rownum"
#define SPIDER_SQL_ROWNUM_LEN sizeof(SPIDER_SQL_ROWNUM_STR) - 1
#define SPIDER_SQL_NEXTVAL_STR ".nextval"
#define SPIDER_SQL_NEXTVAL_LEN sizeof(SPIDER_SQL_NEXTVAL_STR) - 1
#define SPIDER_SQL_CURRVAL_STR ".currval"
#define SPIDER_SQL_CURRVAL_LEN sizeof(SPIDER_SQL_CURRVAL_STR) - 1
#define SPIDER_SQL_FROM_DUAL_STR " from dual"
#define SPIDER_SQL_FROM_DUAL_LEN sizeof(SPIDER_SQL_FROM_DUAL_STR) - 1

#define SPIDER_SQL_SHOW_TABLE_STATUS_STR "show table status from "
#define SPIDER_SQL_SHOW_TABLE_STATUS_LEN sizeof(SPIDER_SQL_SHOW_TABLE_STATUS_STR) - 1
#define SPIDER_SQL_SELECT_TABLES_STATUS_STR "select `table_rows`,`avg_row_length`,`data_length`,`max_data_length`,`index_length`,`auto_increment`,`create_time`,`update_time`,`check_time` from `information_schema`.`tables` where `table_schema` = "
#define SPIDER_SQL_SELECT_TABLES_STATUS_LEN sizeof(SPIDER_SQL_SELECT_TABLES_STATUS_STR) - 1

#define SPIDER_SQL_LIKE_STR " like "
#define SPIDER_SQL_LIKE_LEN (sizeof(SPIDER_SQL_LIKE_STR) - 1)
#define SPIDER_SQL_LIMIT1_STR "rownum = 1"
#define SPIDER_SQL_LIMIT1_LEN (sizeof(SPIDER_SQL_LIMIT1_STR) - 1)

#define SPIDER_SQL_ADD_MONTHS_STR "add_months"
#define SPIDER_SQL_ADD_MONTHS_LEN (sizeof(SPIDER_SQL_ADD_MONTHS_STR) - 1)

#define SPIDER_ORACLE_ERR_BUF_LEN 512

static uchar SPIDER_SQL_LINESTRING_HEAD_STR[] =
  {0x00,0x00,0x00,0x00,0x01,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00};
#define SPIDER_SQL_LINESTRING_HEAD_LEN sizeof(SPIDER_SQL_LINESTRING_HEAD_STR)

static const char *spider_db_table_lock_str[] =
{
  " in share mode",
  " in share mode",
  " in exclusive mode",
  " in exclusive mode"
};
static const int spider_db_table_lock_len[] =
{
  sizeof(" in share mode") - 1,
  sizeof(" in share mode") - 1,
  sizeof(" in exclusive mode") - 1,
  sizeof(" in exclusive mode") - 1
};

int spider_db_oracle_get_error(
  sword res,
  dvoid *hndlp,
  int error_num,
  const char *error1,
  const char *error2,
  CHARSET_INFO *access_charset,
  char *stored_error_msg
) {
  sb4 error_code;
  char buf[SPIDER_ORACLE_ERR_BUF_LEN];
  char buf2[SPIDER_ORACLE_ERR_BUF_LEN];
  spider_string tmp_str(buf2, SPIDER_ORACLE_ERR_BUF_LEN, system_charset_info);
  DBUG_ENTER("spider_db_oracle_get_error");
  tmp_str.init_calc_mem(176);
  tmp_str.length(0);

  switch (res)
  {
    case OCI_SUCCESS:
      DBUG_PRINT("info",("spider res=OCI_SUCCESS"));
      break;
    case OCI_SUCCESS_WITH_INFO:
      DBUG_PRINT("info",("spider res=OCI_SUCCESS_WITH_INFO"));
      OCIErrorGet(hndlp, 1, NULL, &error_code, (OraText *) buf, sizeof(buf),
        OCI_HTYPE_ERROR);
      DBUG_PRINT("info",("spider error_code=%d error='%s'",error_code ,buf));
      if (access_charset && access_charset->cset != system_charset_info->cset)
      {
        tmp_str.append(buf, strlen(buf), access_charset);
      } else {
        tmp_str.set(buf, strlen(buf), system_charset_info);
      }
      push_warning_printf(current_thd, SPIDER_WARN_LEVEL_WARN,
        ER_SPIDER_ORACLE_NUM, ER_SPIDER_ORACLE_STR, res, error_code,
        tmp_str.c_ptr_safe());
      break;
    case OCI_NO_DATA:
      DBUG_PRINT("info",("spider res=OCI_NO_DATA"));
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    case OCI_ERROR:
      DBUG_PRINT("info",("spider res=OCI_ERROR"));
      OCIErrorGet(hndlp, 1, NULL, &error_code, (OraText *) buf, sizeof(buf),
        OCI_HTYPE_ERROR);
      DBUG_PRINT("info",("spider error_code=%d error='%s'",error_code ,buf));
      if (error_code == 1)
      {
        DBUG_PRINT("info",("spider found dupp key"));
        if (stored_error_msg)
          strmov(stored_error_msg, buf);
        DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
      }
      if (error_num)
      {
        if (error1)
        {
          if (error2)
          {
            my_printf_error(error_num, error1, MYF(0), error2);
          } else {
            my_printf_error(error_num, error1, MYF(0));
          }
        } else if (error2) {
          my_error(error_num, MYF(0), error2);
        } else {
          my_error(error_num, MYF(0));
        }
      }
      if (access_charset && access_charset->cset != system_charset_info->cset)
      {
        tmp_str.append(buf, strlen(buf), access_charset);
      } else {
        tmp_str.set(buf, strlen(buf), system_charset_info);
      }
      my_printf_error(ER_SPIDER_ORACLE_NUM, ER_SPIDER_ORACLE_STR, MYF(0),
        res, error_code, tmp_str.c_ptr_safe());
      if (error_num)
      {
        DBUG_RETURN(error_num);
      } else {
        DBUG_RETURN(ER_SPIDER_ORACLE_NUM);
      }
    case OCI_INVALID_HANDLE:
    case OCI_NEED_DATA:
      if (res == OCI_INVALID_HANDLE)
        DBUG_PRINT("info",("spider res=OCI_INVALID_HANDLE"));
      else
        DBUG_PRINT("info",("spider res=OCI_NEED_DATA"));
    default:
      DBUG_PRINT("info",("spider res=%d", res));
      if (error_num)
      {
        if (error1)
        {
          if (error2)
          {
            my_printf_error(error_num, error1, MYF(0), error2);
          } else {
            my_printf_error(error_num, error1, MYF(0));
          }
        } else if (error2) {
          my_error(error_num, MYF(0), error2);
        } else {
          my_error(error_num, MYF(0));
        }
      }
      my_printf_error(ER_SPIDER_ORACLE_NUM, ER_SPIDER_ORACLE_STR, MYF(0),
        res, 0, "");
      if (error_num)
      {
        DBUG_RETURN(error_num);
      } else {
        DBUG_RETURN(ER_SPIDER_ORACLE_NUM);
      }
  }
  DBUG_RETURN(0);
}

int spider_oracle_init()
{
  DBUG_ENTER("spider_oracle_init");
  DBUG_RETURN(0);
}

int spider_oracle_deinit()
{
  DBUG_ENTER("spider_oracle_deinit");
  DBUG_RETURN(0);
}

spider_db_share *spider_oracle_create_share(
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_oracle_create_share");
  DBUG_RETURN(new spider_oracle_share(share));
}

spider_db_handler *spider_oracle_create_handler(
  ha_spider *spider,
  spider_db_share *db_share
) {
  DBUG_ENTER("spider_oracle_create_handler");
  DBUG_RETURN(new spider_oracle_handler(spider,
    (spider_oracle_share *) db_share));
}

spider_db_copy_table *spider_oracle_create_copy_table(
  spider_db_share *db_share
) {
  DBUG_ENTER("spider_oracle_create_copy_table");
  DBUG_RETURN(new spider_oracle_copy_table(
    (spider_oracle_share *) db_share));
}

SPIDER_DB_CONN *spider_oracle_create_conn(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_oracle_create_conn");
  DBUG_RETURN(new spider_db_oracle(conn));
}

spider_db_oracle_util spider_db_oracle_utility;

SPIDER_DBTON spider_dbton_oracle = {
  0,
  SPIDER_DB_WRAPPER_ORACLE,
  SPIDER_DB_ACCESS_TYPE_SQL,
  spider_oracle_init,
  spider_oracle_deinit,
  spider_oracle_create_share,
  spider_oracle_create_handler,
  spider_oracle_create_copy_table,
  spider_oracle_create_conn,
  &spider_db_oracle_utility
};

spider_db_oracle_row::spider_db_oracle_row() :
  spider_db_row(spider_dbton_oracle.dbton_id),
  db_conn(NULL), result(NULL),
  ind(NULL), val(NULL), rlen(NULL), ind_first(NULL), val_first(NULL),
  rlen_first(NULL), val_str(NULL), val_str_first(NULL), defnp(NULL),
  lobhp(NULL), colhp(NULL), coltp(NULL), colsz(NULL), field_count(0),
  row_size(NULL), row_size_first(NULL), access_charset(NULL), cloned(FALSE)
{
  DBUG_ENTER("spider_db_oracle_row::spider_db_oracle_row");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_oracle_row::~spider_db_oracle_row()
{
  DBUG_ENTER("spider_db_oracle_row::~spider_db_oracle_row");
  DBUG_PRINT("info",("spider this=%p", this));
  deinit();
  DBUG_VOID_RETURN;
}

int spider_db_oracle_row::store_to_field(
  Field *field,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_oracle_row::store_to_field");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider ind=%d", *ind));
  if (*ind == -1)
  {
    DBUG_PRINT("info", ("spider field is null"));
    field->set_null();
    field->reset();
  } else {
    DBUG_PRINT("info", ("spider field->type()=%u", field->type()));
    field->set_notnull();
    if (field->type() == MYSQL_TYPE_YEAR)
    {
      field->store(val_str->ptr(), 4,
        field->table->s->table_charset);
    } else if (field->type() == MYSQL_TYPE_DATE)
    {
      field->store(val_str->ptr(), 10,
        field->table->s->table_charset);
    } else if (field->type() == MYSQL_TYPE_TIME)
    {
      field->store(val_str->ptr() + 11, 8,
        field->table->s->table_charset);
    } else {
      DBUG_PRINT("info", ("spider val_str->length()=%u", val_str->length()));
      if (field->flags & BLOB_FLAG)
      {
        DBUG_PRINT("info", ("spider blob field"));
        ((Field_blob *)field)->set_ptr(
          val_str->length(), (uchar *) val_str->ptr());
      } else {
        field->store(val_str->ptr(), val_str->length(),
          field->table->s->table_charset);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_oracle_row::append_to_str(
  spider_string *str
) {
  DBUG_ENTER("spider_db_oracle_row::append_to_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(val_str->length()))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(val_str->ptr(), val_str->length());
  DBUG_RETURN(0);
}

int spider_db_oracle_row::append_escaped_to_str(
  spider_string *str,
  uint dbton_id
) {
  DBUG_ENTER("spider_db_oracle_row::append_escaped_to_str");
  DBUG_PRINT("info",("spider this=%p", this));
/*
  spider_string tmp_str(*val, *rlen, str->charset());
  tmp_str.init_calc_mem(174);
  tmp_str.length(*rlen);
#ifndef DBUG_OFF
  tmp_str.c_ptr_safe();
#endif
  if (str->reserve(*rlen * 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  util.append_escaped(str, tmp_str.get_str());
*/
  if (str->reserve(val_str->length() * 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  spider_dbton[dbton_id].db_util->append_escaped_util(str, val_str->get_str());
  DBUG_RETURN(0);
}

void spider_db_oracle_row::first()
{
  DBUG_ENTER("spider_db_oracle_row::first");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider ind_first=%p", ind_first));
  ind = ind_first;
  DBUG_PRINT("info",("spider val_first=%p", val_first));
  val = val_first;
  DBUG_PRINT("info",("spider rlen_first=%p", rlen_first));
  rlen = rlen_first;
  DBUG_PRINT("info",("spider row_size_first=%p", row_size_first));
  row_size = row_size_first;
  DBUG_PRINT("info",("spider val_str_first=%p", val_str_first));
  val_str = val_str_first;
  DBUG_VOID_RETURN;
}

void spider_db_oracle_row::next()
{
  DBUG_ENTER("spider_db_oracle_row::next");
  DBUG_PRINT("info",("spider this=%p", this));
  ind++;
  val++;
  rlen++;
  row_size++;
  val_str++;
  DBUG_VOID_RETURN;
}

bool spider_db_oracle_row::is_null()
{
  DBUG_ENTER("spider_db_oracle_row::is_null");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((*ind == -1));
}

int spider_db_oracle_row::val_int()
{
  DBUG_ENTER("spider_db_oracle_row::val_int");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((*ind != -1) ? atoi(*val) : 0);
}

double spider_db_oracle_row::val_real()
{
  DBUG_ENTER("spider_db_oracle_row::val_real");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((*ind != -1) ? my_atof(*val) : 0.0);
}

my_decimal *spider_db_oracle_row::val_decimal(
  my_decimal *decimal_value,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_oracle_row::val_decimal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (*ind == -1)
    DBUG_RETURN(NULL);

#ifdef SPIDER_HAS_DECIMAL_OPERATION_RESULTS_VALUE_TYPE
  decimal_operation_results(str2my_decimal(0, *val, *rlen, access_charset,
    decimal_value), "", "");
#else
  decimal_operation_results(str2my_decimal(0, *val, *rlen, access_charset,
    decimal_value));
#endif

  DBUG_RETURN(decimal_value);
}

SPIDER_DB_ROW *spider_db_oracle_row::clone()
{
  uint i;
  spider_db_oracle_row *clone_row;
  DBUG_ENTER("spider_db_oracle_row::clone");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(clone_row = new spider_db_oracle_row()))
  {
    DBUG_RETURN(NULL);
  }
  clone_row->db_conn = db_conn;
  clone_row->result = result;
  clone_row->field_count = field_count;
  clone_row->access_charset = access_charset;
  clone_row->cloned = TRUE;
  if (clone_row->init())
  {
    delete clone_row;
    DBUG_RETURN(NULL);
  }
  memcpy(clone_row->ind, ind_first, sizeof(ub2) * field_count * 4 +
    sizeof(ulong) * field_count);
  for (i = 0; i < field_count; i++)
  {
    if (clone_row->val_str[i].copy(val_str_first[i]))
    {
      delete clone_row;
      DBUG_RETURN(NULL);
    }
  }
  DBUG_RETURN((SPIDER_DB_ROW *) clone_row);
}

int spider_db_oracle_row::store_to_tmp_table(
  TABLE *tmp_table,
  spider_string *str
) {
  uint i;
  DBUG_ENTER("spider_db_oracle_row::store_to_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  for (i = 0; i < field_count; i++)
  {
    if (row_size_first[i])
    {
      if (str->reserve(val_str_first[i].length()))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(val_str_first[i].ptr(), val_str_first[i].length());
    }
  }
  tmp_table->field[0]->set_notnull();
  tmp_table->field[0]->store(
    (const char *) row_size_first,
    sizeof(ulong) * field_count, &my_charset_bin);
  tmp_table->field[1]->set_notnull();
  tmp_table->field[1]->store(
    str->ptr(), str->length(), &my_charset_bin);
  tmp_table->field[2]->set_notnull();
  tmp_table->field[2]->store(
    (char *) ind_first, (uint) (sizeof(sb2) * field_count), &my_charset_bin);
  DBUG_RETURN(tmp_table->file->ha_write_row(tmp_table->record[0]));
}

int spider_db_oracle_row::init()
{
  char *tmp_val;
  uint i;
  DBUG_ENTER("spider_db_oracle_row::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    !(ind = (sb2 *)
      spider_bulk_malloc(spider_current_trx, 161, MYF(MY_WME | MY_ZEROFILL),
        &ind, sizeof(sb2) * field_count,
        &rlen, sizeof(ub2) * field_count,
        &coltp, sizeof(ub2) * field_count,
        &colsz, sizeof(ub2) * field_count,
        &row_size, sizeof(ulong) * field_count,
        &val, sizeof(char *) * field_count,
        &tmp_val, MAX_FIELD_WIDTH * field_count,
        &defnp, sizeof(OCIDefine *) * field_count,
        &lobhp, sizeof(OCILobLocator *) * field_count,
        &colhp, sizeof(OCIParam *) * field_count,
        NullS)
    ) ||
    !(val_str = new spider_string[field_count])
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  ind_first = ind;
  val_first = val;
  rlen_first = rlen;
  row_size_first = row_size;
  val_str_first = val_str;
  for (i = 0; i < field_count; i++)
  {
    val[i] = tmp_val;
    val_str[i].init_calc_mem(177);
    val_str[i].set(tmp_val, MAX_FIELD_WIDTH, access_charset);
    tmp_val += MAX_FIELD_WIDTH;
  }
  DBUG_RETURN(0);
}

void spider_db_oracle_row::deinit()
{
  uint i;
  DBUG_ENTER("spider_db_oracle_row::deinit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!cloned)
  {
    for (i = 0; i < field_count; i++)
    {
      if (defnp && defnp[i])
      {
        OCIHandleFree(defnp[i], OCI_HTYPE_DEFINE);
        defnp[i] = NULL;
      }
      if (lobhp && lobhp[i])
      {
        OCIDescriptorFree(lobhp[i], OCI_DTYPE_LOB);
        lobhp[i] = NULL;
      }
    }
  }
  if (val_str_first)
  {
    delete [] val_str_first;
    val_str_first = NULL;
  }
  if (ind_first)
  {
    spider_free(spider_current_trx, ind_first, MYF(0));
    ind_first = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_db_oracle_row::define()
{
  sword res;
  uint i;
  DBUG_ENTER("spider_db_oracle_row::define");
  DBUG_PRINT("info",("spider this=%p", this));
  for (i = 0; i < field_count; i++)
  {
    if (coltp[i] == SQLT_BLOB)
    {
      res = OCIDescriptorAlloc(db_conn->envhp, (dvoid **) &lobhp[i],
        OCI_DTYPE_LOB, 0, 0);
      if (res != OCI_SUCCESS)
      {
        DBUG_RETURN(
          spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL, NULL,
            access_charset, NULL));
      }
      res = OCIDefineByPos(result->stmtp, &defnp[i], db_conn->errhp, i + 1,
        &lobhp[i], 0, SQLT_BLOB, &ind[i], &rlen[i], NULL,
        OCI_DEFAULT);
    } else if (coltp[i] == SQLT_DAT)
    {
      res = OCIDefineByPos(result->stmtp, &defnp[i], db_conn->errhp, i + 1,
        (char *) val_str[i].ptr() + 20, sizeof(ub1) * 7, SQLT_DAT, &ind[i],
        &rlen[i], NULL, OCI_DEFAULT);
    } else {
      if (val_str[i].alloc(colsz[i]))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      res = OCIDefineByPos(result->stmtp, &defnp[i], db_conn->errhp, i + 1,
        (char *) val_str[i].ptr(), colsz[i], SQLT_CHR, &ind[i], &rlen[i], NULL,
        OCI_DEFAULT);
    }
    if (res != OCI_SUCCESS)
    {
      DBUG_RETURN(
        spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL, NULL,
          access_charset, NULL));
    }
  }
  DBUG_RETURN(0);
}

int spider_db_oracle_row::fetch()
{
  sword res;
  uint i;
  DBUG_ENTER("spider_db_oracle_row::fetch");
  DBUG_PRINT("info",("spider this=%p", this));
  for (i = 0; i < field_count; i++)
  {
    if (ind[i] == -1)
    {
      DBUG_PRINT("info",("spider NULL"));
      val_str[i].length(0);
    } else {
      if (coltp[i] == SQLT_BLOB)
      {
        DBUG_PRINT("info",("spider SQLT_BLOB"));
        oraub8 len;
        res = OCILobGetLength2(db_conn->svchp, db_conn->errhp, lobhp[i], &len);
        if (res != OCI_SUCCESS)
        {
          DBUG_RETURN(
            spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL, NULL,
              access_charset, NULL));
        }
#ifndef DBUG_OFF
        {
          ulonglong print_len = len;
          DBUG_PRINT("info",("spider len=%llu", print_len));
        }
#endif
        if (val_str[i].alloc(len))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        res = OCILobRead2(db_conn->svchp, db_conn->errhp, lobhp[i], &len,
          NULL, 1, (char *) val_str[i].ptr(), len, OCI_ONE_PIECE, NULL, NULL,
          0, 0);
        if (res != OCI_SUCCESS)
        {
          DBUG_RETURN(
            spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL, NULL,
              access_charset, NULL));
        }
#ifndef DBUG_OFF
        {
          ulonglong print_len = len;
          DBUG_PRINT("info",("spider lenb=%llu", print_len));
        }
#endif
        val_str[i].length(len);
      } else if (coltp[i] == SQLT_DAT)
      {
        DBUG_PRINT("info",("spider SQLT_DAT"));
        char *val = (char *) val_str[i].ptr();
        ub1 *src = (ub1 *) val + 20;
        val_str[i].length(19);
        if (src[0] < 100)
          my_sprintf(val, (val, "0000-00-00 00:00:00"));
        else
          my_sprintf(val, (val, "%02u%02u-%02u-%02u %02u:%02u:%02u",
            src[0] - 100, src[1] - 100, src[2], src[3],
            src[4] - 1, src[5] - 1, src[6] - 1));
      } else {
        val_str[i].length(rlen[i]);
      }
    }
    row_size[i] = val_str[i].length();
  }
  DBUG_RETURN(0);
}

spider_db_oracle_result::spider_db_oracle_result() :
  spider_db_result(spider_dbton_oracle.dbton_id),
  db_conn(NULL), stmtp(NULL), field_count(0), access_charset(NULL),
  fetched(FALSE)
{
  DBUG_ENTER("spider_db_oracle_result::spider_db_oracle_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_oracle_result::~spider_db_oracle_result()
{
  DBUG_ENTER("spider_db_oracle_result::~spider_db_oracle_result");
  DBUG_PRINT("info",("spider this=%p", this));
  free_result();
  DBUG_VOID_RETURN;
}

bool spider_db_oracle_result::has_result()
{
  DBUG_ENTER("spider_db_oracle_result::has_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(stmtp);
}

void spider_db_oracle_result::free_result()
{
  DBUG_ENTER("spider_db_oracle_result::free_result");
  DBUG_PRINT("info",("spider this=%p", this));
  if (stmtp)
  {
    OCIHandleFree(stmtp, OCI_HTYPE_STMT);
    stmtp = NULL;
  }
  DBUG_VOID_RETURN;
}

SPIDER_DB_ROW *spider_db_oracle_result::current_row()
{
  DBUG_ENTER("spider_db_oracle_result::current_row");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((SPIDER_DB_ROW *) row.clone());
}

SPIDER_DB_ROW *spider_db_oracle_result::fetch_row()
{
  sword res;
  DBUG_ENTER("spider_db_oracle_result::fetch_row");
  DBUG_PRINT("info",("spider this=%p", this));
  row.ind = row.ind_first;
  row.val = row.val_first;
  row.rlen = row.rlen_first;
  row.row_size = row.row_size_first;
  row.val_str = row.val_str_first;
  if (fetched)
  {
    /* already fetched */
    fetched = FALSE;
  } else {
    res = OCIStmtFetch2(stmtp, db_conn->errhp, 1, OCI_FETCH_NEXT, 0,
      OCI_DEFAULT);
    if (res != OCI_SUCCESS)
    {
      store_error_num = spider_db_oracle_get_error(res, db_conn->errhp, 0,
        NULL, NULL, access_charset, NULL);
      DBUG_RETURN(NULL);
    }
  }
  if ((store_error_num = row.fetch()))
  {
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

SPIDER_DB_ROW *spider_db_oracle_result::fetch_row_from_result_buffer(
  spider_db_result_buffer *spider_res_buf
) {
  sword res;
  DBUG_ENTER("spider_db_oracle_result::fetch_row_from_result_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  row.ind = row.ind_first;
  row.val = row.val_first;
  row.rlen = row.rlen_first;
  row.row_size = row.row_size_first;
  row.val_str = row.val_str_first;
  if (fetched)
  {
    /* already fetched */
    fetched = FALSE;
  } else {
    res = OCIStmtFetch2(stmtp, db_conn->errhp, 1, OCI_FETCH_NEXT, 0,
      OCI_DEFAULT);
    if (res != OCI_SUCCESS)
    {
      store_error_num = spider_db_oracle_get_error(res, db_conn->errhp, 0,
        NULL, NULL, access_charset, NULL);
      DBUG_RETURN(NULL);
    }
  }
  if ((store_error_num = row.fetch()))
  {
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

SPIDER_DB_ROW *spider_db_oracle_result::fetch_row_from_tmp_table(
  TABLE *tmp_table
) {
  uint i;
  const char *str;
  spider_string tmp_str1, tmp_str2, tmp_str3;
  DBUG_ENTER("spider_db_oracle_result::fetch_row_from_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_str1.init_calc_mem(175);
  tmp_str2.init_calc_mem(178);
  tmp_str3.init_calc_mem(179);
  tmp_table->field[0]->val_str(tmp_str1.get_str());
  tmp_table->field[1]->val_str(tmp_str2.get_str());
  tmp_table->field[2]->val_str(tmp_str3.get_str());
  tmp_str1.mem_calc();
  tmp_str2.mem_calc();
  tmp_str3.mem_calc();
  row.ind = row.ind_first;
  row.val = row.val_first;
  row.rlen = row.rlen_first;
  row.row_size = row.row_size_first;
  row.val_str = row.val_str_first;
  DBUG_PRINT("info",("spider tmp_str1.length()=%u", tmp_str1.length()));
  DBUG_PRINT("info",("spider tmp_str2.length()=%u", tmp_str2.length()));
  DBUG_PRINT("info",("spider tmp_str3.length()=%u", tmp_str3.length()));
  memcpy(row.ind, tmp_str3.ptr(), tmp_str3.length());
  memcpy(row.row_size, tmp_str1.ptr(), tmp_str1.length());
  row.field_count = tmp_str1.length() / sizeof(ulong);
  str = tmp_str2.ptr();
  for (i = 0; i < row.field_count; i++)
  {
    row.val_str[i].length(0);
    if (row.row_size[i])
    {
      if (row.val_str[i].reserve(row.row_size[i]))
      {
        store_error_num = HA_ERR_OUT_OF_MEM;
        DBUG_RETURN(NULL);
      }
      row.val_str[i].q_append(str, row.row_size[i]);
      str += row.row_size[i];
    }
  }
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

int spider_db_oracle_result::fetch_table_status(
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
  DBUG_ENTER("spider_db_oracle_result::fetch_table_status");
  DBUG_PRINT("info",("spider this=%p", this));
  /* TODO: develop later */
  records = 2;
  mean_rec_length = 65535;
  data_file_length = 65535;
  max_data_file_length = 65535;
  index_file_length = 65535;
/*
  auto_increment_value = 0;
*/
  create_time = (time_t) 0;
  update_time = (time_t) 0;
  check_time = (time_t) 0;
  DBUG_RETURN(0);
}

int spider_db_oracle_result::fetch_table_records(
  int mode,
  ha_rows &records
) {
  DBUG_ENTER("spider_db_oracle_result::fetch_table_records");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!fetch_row())
  {
    records = 0;
  } else {
    records = row.val_int();
  }
  DBUG_RETURN(0);
}

int spider_db_oracle_result::fetch_table_cardinality(
  int mode,
  TABLE *table,
  longlong *cardinality,
  uchar *cardinality_upd,
  int bitmap_size
) {
  DBUG_ENTER("spider_db_oracle_result::fetch_table_cardinality");
  DBUG_PRINT("info",("spider this=%p", this));
  /* TODO: develop later */
  DBUG_RETURN(0);
}

int spider_db_oracle_result::fetch_table_mon_status(
  int &status
) {
  DBUG_ENTER("spider_db_oracle_result::fetch_table_mon_status");
  DBUG_PRINT("info",("spider this=%p", this));
  /* TODO: develop later */
  status = SPIDER_LINK_MON_OK;
  DBUG_RETURN(0);
}

longlong spider_db_oracle_result::num_rows()
{
  sword res;
  ub4 rowcnt;
  DBUG_ENTER("spider_db_oracle_result::num_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  res = OCIAttrGet(stmtp, OCI_HTYPE_STMT, &rowcnt, 0,
    OCI_ATTR_ROW_COUNT, db_conn->errhp);
  if (res != OCI_SUCCESS)
  {
    spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL, NULL,
      access_charset, NULL);
    DBUG_RETURN(0);
  }
  DBUG_PRINT("info",("spider rowcnt=%u", rowcnt));
  DBUG_RETURN((longlong) rowcnt);
}

uint spider_db_oracle_result::num_fields()
{
  sword res;
  ub4 parmcnt;
  DBUG_ENTER("spider_db_oracle_result::num_fields");
  DBUG_PRINT("info",("spider this=%p", this));
  res = OCIAttrGet(stmtp, OCI_HTYPE_STMT, &parmcnt, 0,
    OCI_ATTR_PARAM_COUNT, db_conn->errhp);
  if (res != OCI_SUCCESS)
  {
    spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL, NULL,
      access_charset, NULL);
    DBUG_RETURN(0);
  }
  DBUG_RETURN((uint) parmcnt);
}

void spider_db_oracle_result::move_to_pos(
  longlong pos
) {
  sword res;
  DBUG_ENTER("spider_db_oracle_result::move_to_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider pos=%lld", pos));
  res = OCIStmtFetch2(stmtp, db_conn->errhp, 1, OCI_FETCH_ABSOLUTE, pos,
    OCI_DEFAULT);
  if (res != OCI_SUCCESS)
  {
    spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL, NULL,
      access_charset, NULL);
  }
  DBUG_VOID_RETURN;
}

int spider_db_oracle_result::set_column_info()
{
  sword res;
  uint i;
  DBUG_ENTER("spider_db_oracle_result::set_column_info");
  DBUG_PRINT("info",("spider this=%p", this));
  for (i = 0; i < field_count; i++)
  {
    res = OCIParamGet(stmtp, OCI_HTYPE_STMT, db_conn->errhp,
      (dvoid **) &row.colhp[i], i + 1);
    if (res != OCI_SUCCESS)
    {
      DBUG_RETURN(spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL,
        NULL, access_charset, NULL));
    }
    res = OCIAttrGet(row.colhp[i], OCI_DTYPE_PARAM, &row.coltp[i], NULL,
      OCI_ATTR_DATA_TYPE, db_conn->errhp);
    if (res != OCI_SUCCESS)
    {
      DBUG_RETURN(spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL,
        NULL, access_charset, NULL));
    }
    res = OCIAttrGet(row.colhp[i], OCI_DTYPE_PARAM, &row.colsz[i], NULL,
      OCI_ATTR_DATA_SIZE, db_conn->errhp);
    if (res != OCI_SUCCESS)
    {
      DBUG_RETURN(spider_db_oracle_get_error(res, db_conn->errhp, 0, NULL,
        NULL, access_charset, NULL));
    }
  }
  DBUG_RETURN(0);
}

int spider_db_oracle_result::get_errno()
{
  DBUG_ENTER("spider_db_oracle_result::get_errno");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider store_error_num=%d", store_error_num));
  DBUG_RETURN(store_error_num);
}

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
int spider_db_oracle_result::fetch_columns_for_discover_table_structure(
  spider_string *str,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_oracle_result::fetch_columns_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int spider_db_oracle_result::fetch_index_for_discover_table_structure(
  spider_string *str,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_oracle_result::fetch_index_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int spider_db_oracle_result::fetch_table_for_discover_table_structure(
  spider_string *str,
  SPIDER_SHARE *spider_share,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_oracle_result::fetch_table_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif

spider_db_oracle::spider_db_oracle(
  SPIDER_CONN *conn
) : spider_db_conn(conn), envhp(NULL), errhp(NULL), srvhp(NULL), svchp(NULL),
  usrhp(NULL), stmtp(NULL), txnhp(NULL), result(NULL), table_lock_mode(0),
  lock_table_hash_inited(FALSE), handler_open_array_inited(FALSE)
{
  DBUG_ENTER("spider_db_oracle::spider_db_oracle");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_oracle::~spider_db_oracle()
{
  DBUG_ENTER("spider_db_oracle::~spider_db_oracle");
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
  disconnect();
  DBUG_VOID_RETURN;
}

int spider_db_oracle::init()
{
  DBUG_ENTER("spider_db_oracle::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    my_hash_init(&lock_table_hash, spd_charset_utf8_bin, 32, 0, 0,
      (my_hash_get_key) spider_link_get_key, 0, 0)
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  spider_alloc_calc_mem_init(lock_table_hash, 199);
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
  spider_alloc_calc_mem_init(handler_open_array, 164);
  spider_alloc_calc_mem(spider_current_trx,
    handler_open_array,
    handler_open_array.max_element *
    handler_open_array.size_of_element);
  handler_open_array_inited = TRUE;
  DBUG_RETURN(0);
}

bool spider_db_oracle::is_connected()
{
  DBUG_ENTER("spider_db_oracle::is_connected");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(svchp);
}

void spider_db_oracle::bg_connect()
{
  sword res;
  DBUG_ENTER("spider_db_oracle::bg_connect");
  DBUG_PRINT("info",("spider this=%p", this));
  res = OCIEnvNlsCreate(&envhp, OCI_DEFAULT, 0, 0, 0, 0, 0, 0, 0, 0);
/*
  res = OCIEnvCreate(&envhp, OCI_THREADED, 0, 0, 0, 0, 0, 0);
*/
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider create environment error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    goto error;
  }
  DBUG_PRINT("info",("spider OCI init envhp=%p", envhp));

  res = OCIHandleAlloc(envhp, (dvoid **) &errhp, OCI_HTYPE_ERROR, 0, 0);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider create error handler error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }
  DBUG_PRINT("info",("spider OCI init errhp=%p", errhp));

  res = OCIHandleAlloc(envhp, (dvoid **) &srvhp, OCI_HTYPE_SERVER, 0, 0);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider create server handler error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }
  DBUG_PRINT("info",("spider OCI init srvhp=%p", srvhp));

  res = OCIServerAttach(srvhp, errhp, (OraText *) tgt_host, strlen(tgt_host),
    OCI_DEFAULT);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider attach server error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }

  res = OCIHandleAlloc(envhp, (dvoid **) &svchp, OCI_HTYPE_SVCCTX, 0, 0);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider create service context error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }
  DBUG_PRINT("info",("spider OCI init svchp=%p", svchp));

  res = OCIAttrSet(svchp, OCI_HTYPE_SVCCTX, srvhp, 0, OCI_ATTR_SERVER, errhp);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider set server attr error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }

  res = OCIHandleAlloc(envhp, (dvoid **) &usrhp, OCI_HTYPE_SESSION, 0, 0);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider create session handler error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }
  DBUG_PRINT("info",("spider OCI init usrhp=%p", usrhp));

  res = OCIAttrSet(usrhp, OCI_HTYPE_SESSION,
    tgt_username, strlen(tgt_username), OCI_ATTR_USERNAME, errhp);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider set username attr error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }

  res = OCIAttrSet(usrhp, OCI_HTYPE_SESSION,
    tgt_password, strlen(tgt_password), OCI_ATTR_PASSWORD, errhp);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider set password attr error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }

  res = OCISessionBegin(svchp, errhp, usrhp, OCI_CRED_RDBMS, OCI_DEFAULT);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider session begin error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }
  DBUG_PRINT("info",("spider OCISessionBegin"));

  // set the session in the context handle
  res = OCIAttrSet(svchp, OCI_HTYPE_SVCCTX, usrhp, 0, OCI_ATTR_SESSION, errhp);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider set session attr error"));
    stored_error_num = set_error(res, errhp, 0, NULL, NULL);
    bg_disconnect();
    goto error;
  }

  if (
    (stored_error_num = exec_query(SPIDER_SQL_SET_NLS_DATE_FORMAT_STR,
      SPIDER_SQL_SET_NLS_DATE_FORMAT_LEN, -1)) ||
    (stored_error_num = exec_query(SPIDER_SQL_SET_NLS_TIME_FORMAT_STR,
      SPIDER_SQL_SET_NLS_TIME_FORMAT_LEN, -1)) ||
    (stored_error_num = exec_query(SPIDER_SQL_SET_NLS_TIMESTAMP_FORMAT_STR,
      SPIDER_SQL_SET_NLS_TIMESTAMP_FORMAT_LEN, -1))
  ) {
    DBUG_PRINT("info",("spider init connection error"));
    bg_disconnect();
    goto error;
  }
  DBUG_VOID_RETURN;

error:
  strmov(stored_error_msg, spider_stmt_da_message(current_thd));
  current_thd->clear_error();
  DBUG_VOID_RETURN;
}

int spider_db_oracle::connect(
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
  DBUG_ENTER("spider_db_oracle::connect");
  DBUG_PRINT("info",("spider this=%p", this));
  this->tgt_host = tgt_host;
  this->tgt_username = tgt_username;
  this->tgt_password = tgt_password;
  this->tgt_port = tgt_port;
  this->tgt_socket = tgt_socket;
  this->server_name = server_name;
  this->connect_retry_count = connect_retry_count;
  this->connect_retry_interval = connect_retry_interval;
  if ((error_num = spider_create_conn_thread(conn)))
    DBUG_RETURN(error_num);
  spider_bg_conn_simple_action(conn, SPIDER_BG_SIMPLE_CONNECT, TRUE, NULL,
    0, NULL);

  if (stored_error_num)
  {
    my_message(stored_error_num, stored_error_msg, MYF(0));
    DBUG_RETURN(stored_error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_oracle::ping(
) {
  sword res;
  DBUG_ENTER("spider_db_oracle::ping");
  DBUG_PRINT("info",("spider this=%p", this));
  res = OCIPing(svchp, errhp, OCI_DEFAULT);
  if (res != OCI_SUCCESS)
  {
    DBUG_PRINT("info",("spider ping error %d", res));
    DBUG_RETURN(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM);
  }
  DBUG_RETURN(0);
}

void spider_db_oracle::bg_disconnect()
{
  DBUG_ENTER("spider_db_oracle::bg_disconnect");
  DBUG_PRINT("info",("spider this=%p", this));
  if (result)
  {
    delete result;
    result = NULL;
  }
  if (txnhp)
  {
    DBUG_PRINT("info",("spider OCI free txnhp=%p", txnhp));
    OCIHandleFree(txnhp, OCI_HTYPE_TRANS);
    txnhp = NULL;
  }
  if (stmtp)
  {
    DBUG_PRINT("info",("spider OCI free stmtp=%p", stmtp));
    OCIHandleFree(stmtp, OCI_HTYPE_STMT);
    stmtp = NULL;
  }
  if (svchp && errhp && usrhp)
  {
    DBUG_PRINT("info",("spider OCISessionEnd"));
    OCISessionEnd(svchp, errhp, usrhp, OCI_DEFAULT);
  }
  if (usrhp)
  {
    DBUG_PRINT("info",("spider OCI free usrhp=%p", usrhp));
    OCIHandleFree(usrhp, OCI_HTYPE_SESSION);
    usrhp = NULL;
  }
  if (svchp)
  {
    DBUG_PRINT("info",("spider OCI free svchp=%p", svchp));
    OCIHandleFree(svchp, OCI_HTYPE_SVCCTX);
    svchp = NULL;
  }
  if (srvhp)
  {
    DBUG_PRINT("info",("spider OCI free srvhp=%p", srvhp));
    OCIServerDetach(srvhp, errhp, OCI_DEFAULT);
    OCIHandleFree(srvhp, OCI_HTYPE_SERVER);
    srvhp = NULL;
  }
  if (errhp)
  {
    DBUG_PRINT("info",("spider OCI free errhp=%p", errhp));
    OCIHandleFree(errhp, OCI_HTYPE_ERROR);
    errhp = NULL;
  }
  if (envhp)
  {
    DBUG_PRINT("info",("spider OCI free envhp=%p", envhp));
    OCIHandleFree(envhp, OCI_HTYPE_ENV);
    envhp = NULL;
  }
  DBUG_VOID_RETURN;
}

void spider_db_oracle::disconnect()
{
  DBUG_ENTER("spider_db_oracle::disconnect");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!conn->bg_init)
    DBUG_VOID_RETURN;
  spider_bg_conn_simple_action(conn, SPIDER_BG_SIMPLE_DISCONNECT, TRUE, NULL,
    0, NULL);
  DBUG_VOID_RETURN;
}

int spider_db_oracle::set_net_timeout()
{
  DBUG_ENTER("spider_db_oracle::set_net_timeout");
  DBUG_PRINT("info",("spider this=%p", this));
  /* TODO: develop later */
  DBUG_RETURN(0);
}

int spider_db_oracle::exec_query(
  const char *query,
  uint length,
  int quick_mode
) {
  sword res;
  int error_num;
  DBUG_ENTER("spider_db_oracle::exec_query");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider_param_general_log())
  {
    const char *tgt_str = conn->tgt_host;
    uint32 tgt_len = conn->tgt_host_length;
    spider_string tmp_query_str(length + conn->tgt_wrapper_length +
      tgt_len + (SPIDER_SQL_SPACE_LEN * 2));
    tmp_query_str.init_calc_mem(232);
    tmp_query_str.length(0);
    tmp_query_str.q_append(conn->tgt_wrapper, conn->tgt_wrapper_length);
    tmp_query_str.q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    tmp_query_str.q_append(tgt_str, tgt_len);
    tmp_query_str.q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    tmp_query_str.q_append(query, length);
    general_log_write(current_thd, COM_QUERY, tmp_query_str.ptr(),
      tmp_query_str.length());
  }
  stored_error_num = 0;
  if (table_lock_mode && !conn->in_before_query)
  {
    DBUG_PRINT("info",("spider table_lock_mode=%d", table_lock_mode));
    table_lock_mode = 0;
    if ((error_num = exec_query(exec_lock_sql->ptr(), exec_lock_sql->length(),
      -1))) {
      DBUG_RETURN(error_num);
    }
  }

  if (length)
  {
    if (result)
    {
      delete result;
      result = NULL;
    }

    if (!stmtp)
    {
      DBUG_PRINT("info",("spider create stmt"));
      res = OCIHandleAlloc(envhp, (dvoid **) &stmtp, OCI_HTYPE_STMT, 0, 0);
      if (res != OCI_SUCCESS)
      {
        DBUG_PRINT("info",("spider create stmt handler error"));
        DBUG_RETURN(set_error(res, errhp, 0, NULL, NULL));
      }
    }

    res = OCIStmtPrepare(stmtp, errhp, (OraText *) query, length,
      OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (res != OCI_SUCCESS)
    {
      DBUG_PRINT("info",("spider stmt prepare error"));
      DBUG_RETURN(set_error(res, errhp, 0, NULL, NULL));
    }

/*
    if ((result = new spider_db_oracle_result()))
    {
      result->db_conn = this;
      result->stmtp = stmtp;
      stmtp = NULL;
      result->field_count = result->num_fields();
      result->row.field_count = result->field_count;
      result->row.db_conn = this;
      result->row.result = result;
      if ((error_num = result->row.init()))
      {
        delete result;
        result = NULL;
        DBUG_RETURN(error_num);
      }
    } else {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
*/

    /* select statement check */
    ub4 iters;
    if (
      !strncasecmp(query, "select ", sizeof("select ") - 1) ||
      !strncasecmp(query, "(select ", sizeof("(select ") - 1)
    ) {
      iters = 0;
    } else {
      iters = 1;
    }

    if (quick_mode)
    {
      DBUG_PRINT("info",("spider use OCI_DEFAULT"));
      res = OCIStmtExecute(svchp, stmtp, errhp, iters, 0, NULL, NULL,
        OCI_DEFAULT);
    } else {
      DBUG_PRINT("info",("spider use OCI_STMT_SCROLLABLE_READONLY"));
      res = OCIStmtExecute(svchp, stmtp, errhp, iters, 0, NULL, NULL,
        OCI_STMT_SCROLLABLE_READONLY);
/*
      if (res == OCI_SUCCESS)
      {
        DBUG_PRINT("info",("spider fetch last for row count"));
        res = OCIStmtFetch2(result->stmtp, errhp, 1, OCI_FETCH_LAST, 0,
          OCI_DEFAULT);
      }
      if (res == OCI_SUCCESS)
      {
        DBUG_PRINT("info",("spider fetch first for row count"));
        res = OCIStmtFetch2(result->stmtp, errhp, 1, OCI_FETCH_FIRST, 0,
          OCI_DEFAULT);
      }
*/
    }
    if (res == OCI_SUCCESS && iters)
    {
      DBUG_PRINT("info",("spider get row count"));
      ub4 row_count;
      res = OCIAttrGet(stmtp, OCI_HTYPE_STMT, &row_count, 0,
        OCI_ATTR_ROW_COUNT, errhp);
      update_rows = (uint) row_count;
      DBUG_PRINT("info",("spider row_count=%u", update_rows));
    }
    if (res != OCI_SUCCESS)
    {
      DBUG_PRINT("info",("spider stmt execute error"));
      error_num = set_error(res, errhp, 0, NULL, NULL);
      if (error_num == HA_ERR_END_OF_FILE)
        DBUG_RETURN(0);
      DBUG_RETURN(error_num);
    }

    if ((result = new spider_db_oracle_result()))
    {
      result->db_conn = this;
      result->stmtp = stmtp;
      stmtp = NULL;
      result->field_count = result->num_fields();
      result->row.field_count = result->field_count;
      result->row.db_conn = this;
      result->row.result = result;
      result->row.access_charset = conn->access_charset;
      result->access_charset = conn->access_charset;
      if (
        (error_num = result->row.init()) ||
        (error_num = result->set_column_info())
      ) {
        delete result;
        result = NULL;
        DBUG_RETURN(error_num);
      }
      result->row.define();
    } else {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }

    if (!quick_mode && !iters)
    {
      if (res == OCI_SUCCESS)
      {
        DBUG_PRINT("info",("spider fetch last for row count"));
        res = OCIStmtFetch2(result->stmtp, errhp, 1, OCI_FETCH_LAST, 0,
          OCI_DEFAULT);
      }
      if (res == OCI_SUCCESS)
      {
        DBUG_PRINT("info",("spider fetch first for row count"));
        res = OCIStmtFetch2(result->stmtp, errhp, 1, OCI_FETCH_FIRST, 0,
          OCI_DEFAULT);
      }
      if (res != OCI_SUCCESS)
      {
        DBUG_PRINT("info",("spider stmt execute error"));
        error_num = set_error(res, errhp, 0, NULL, NULL);
        if (error_num == HA_ERR_END_OF_FILE)
          DBUG_RETURN(0);
        DBUG_RETURN(error_num);
      }
      result->fetched = TRUE;
    }
  }
  DBUG_RETURN(0);
}

int spider_db_oracle::get_errno()
{
  DBUG_ENTER("spider_db_oracle::get_errno");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider stored_error=%d", stored_error_num));
  DBUG_RETURN(stored_error_num);
}

const char *spider_db_oracle::get_error()
{
  DBUG_ENTER("spider_db_oracle::get_error");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider error=%s", stored_error));
  DBUG_RETURN(stored_error);
}

bool spider_db_oracle::is_server_gone_error(
  int error_num
) {
  DBUG_ENTER("spider_db_oracle::is_server_gone_error");
  DBUG_PRINT("info",("spider this=%p", this));
  /* TODO: develop later */
  DBUG_RETURN(FALSE);
}

bool spider_db_oracle::is_dup_entry_error(
  int error_num
) {
  DBUG_ENTER("spider_db_oracle::is_dup_entry_error");
  DBUG_PRINT("info",("spider this=%p", this));
  if (error_num == HA_ERR_FOUND_DUPP_KEY)
    DBUG_RETURN(TRUE);
  DBUG_RETURN(FALSE);
}

bool spider_db_oracle::is_xa_nota_error(
  int error_num
) {
  DBUG_ENTER("spider_db_oracle::is_xa_nota_error");
  DBUG_PRINT("info",("spider this=%p", this));
  /* TODO: develop later */
  DBUG_RETURN(FALSE);
}

spider_db_result *spider_db_oracle::store_result(
  spider_db_result_buffer **spider_res_buf,
  st_spider_db_request_key *request_key,
  int *error_num
) {
  spider_db_oracle_result *tmp_result = result;
  DBUG_ENTER("spider_db_oracle::store_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(!spider_res_buf);
  if (stored_error_num == HA_ERR_END_OF_FILE)
  {
    *error_num = HA_ERR_END_OF_FILE;
    DBUG_RETURN(NULL);
  }

  *error_num = 0;
  result = NULL;
  DBUG_RETURN(tmp_result);
}

spider_db_result *spider_db_oracle::use_result(
  st_spider_db_request_key *request_key,
  int *error_num
) {
  spider_db_oracle_result *tmp_result = result;
  DBUG_ENTER("spider_db_oracle::use_result");
  DBUG_PRINT("info",("spider this=%p", this));
  if (stored_error_num == HA_ERR_END_OF_FILE)
  {
    *error_num = HA_ERR_END_OF_FILE;
    DBUG_RETURN(NULL);
  }

  *error_num = 0;
  result = NULL;
  DBUG_RETURN(tmp_result);
}

int spider_db_oracle::next_result()
{
  DBUG_ENTER("spider_db_oracle::next_result");
  DBUG_PRINT("info",("spider this=%p", this));
  /* TODO: develop later */
  DBUG_RETURN(-1);
}

uint spider_db_oracle::affected_rows()
{
  DBUG_ENTER("spider_db_oracle::affected_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(update_rows);
}

ulonglong spider_db_oracle::last_insert_id()
{
  DBUG_ENTER("spider_db_oracle::last_insert_id");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(stored_last_insert_id);
}

int spider_db_oracle::set_character_set(
  const char *csname
) {
  DBUG_ENTER("spider_db_oracle::set_character_set");
  DBUG_PRINT("info",("spider this=%p", this));
  /* TODO: develop later */
  DBUG_RETURN(0);
}

int spider_db_oracle::select_db(
  const char *dbname
) {
  DBUG_ENTER("spider_db_oracle::select_db");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do for oracle */
  DBUG_RETURN(0);
}

int spider_db_oracle::consistent_snapshot(
  int *need_mon
) {
  DBUG_ENTER("spider_db_oracle::consistent_snapshot");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do for oracle */
  DBUG_RETURN(0);
}

bool spider_db_oracle::trx_start_in_bulk_sql()
{
  DBUG_ENTER("spider_db_oracle::trx_start_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_oracle::start_transaction(
  int *need_mon
) {
  DBUG_ENTER("spider_db_oracle::start_transaction");
  DBUG_PRINT("info",("spider this=%p", this));
  if (conn->in_before_query)
  {
    if (conn->queued_semi_trx_isolation)
    {
      if (conn->queued_semi_trx_isolation_val != conn->trx_isolation)
      {
        /* nothing to do */
        DBUG_RETURN(0);
      }
    } else if (conn->queued_trx_isolation)
    {
      if (conn->queued_trx_isolation_val != conn->trx_isolation)
      {
        /* nothing to do */
        DBUG_RETURN(0);
      }
    }
    DBUG_RETURN(set_trx_isolation(conn->trx_isolation, need_mon));
  }
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

int spider_db_oracle::commit(
  int *need_mon
) {
  sword res;
  DBUG_ENTER("spider_db_oracle::commit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (conn->table_locked)
  {
    conn->table_locked = FALSE;
    spider_current_trx->locked_connections--;
  }
  res = OCITransCommit(svchp, errhp, OCI_DEFAULT);
  if (res != OCI_SUCCESS)
  {
    *need_mon = set_error(res, errhp, 0, NULL, NULL);
    DBUG_RETURN(*need_mon);
  }
  DBUG_RETURN(0);
}

int spider_db_oracle::rollback(
  int *need_mon
) {
  sword res;
  DBUG_ENTER("spider_db_oracle::rollback");
  DBUG_PRINT("info",("spider this=%p", this));
  if (conn->table_locked)
  {
    conn->table_locked = FALSE;
    spider_current_trx->locked_connections--;
  }
  if (svchp && errhp)
  {
    res = OCITransRollback(svchp, errhp, OCI_DEFAULT);
    if (res != OCI_SUCCESS)
    {
      *need_mon = set_error(res, errhp, 0, NULL, NULL);
      DBUG_RETURN(*need_mon);
    }
  }
  DBUG_RETURN(0);
}

bool spider_db_oracle::xa_start_in_bulk_sql()
{
  DBUG_ENTER("spider_db_oracle::xa_start_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_oracle::xa_start(
  XID *xid,
  int *need_mon
) {
  sword res;
  DBUG_ENTER("spider_db_oracle::xa_start");
  DBUG_PRINT("info",("spider this=%p", this));
  if (txnhp)
  {
    OCIHandleFree(txnhp, OCI_HTYPE_TRANS);
    txnhp = NULL;
  }
  OCIHandleAlloc((dvoid *)envhp, (dvoid **)&txnhp, OCI_HTYPE_TRANS, 0, 0);
  OCIAttrSet((dvoid *)svchp, OCI_HTYPE_SVCCTX, (dvoid *)txnhp, 0,
    OCI_ATTR_TRANS, errhp);
  OCIAttrSet((dvoid *)txnhp, OCI_HTYPE_TRANS, (dvoid *)xid, sizeof(XID),
    OCI_ATTR_XID, errhp);

  res = OCITransStart(svchp, errhp, 31622400, OCI_TRANS_NEW);
  if (res != OCI_SUCCESS)
  {
    *need_mon = set_error(res, errhp, 0, NULL, NULL);
    DBUG_RETURN(*need_mon);
  }
  DBUG_RETURN(0);
}

int spider_db_oracle::xa_end(
  XID *xid,
  int *need_mon
) {
  DBUG_ENTER("spider_db_oracle::xa_end");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do for oracle */
  DBUG_RETURN(0);
}

int spider_db_oracle::xa_prepare(
  XID *xid,
  int *need_mon
) {
  sword res;
  DBUG_ENTER("spider_db_oracle::xa_prepare");
  DBUG_PRINT("info",("spider this=%p", this));
  res = OCITransPrepare(svchp, errhp, OCI_DEFAULT);
  if (res != OCI_SUCCESS)
  {
    *need_mon = set_error(res, errhp, 0, NULL, NULL);
    DBUG_RETURN(*need_mon);
  }
  DBUG_RETURN(0);
}

int spider_db_oracle::xa_commit(
  XID *xid,
  int *need_mon
) {
  sword res;
  DBUG_ENTER("spider_db_oracle::xa_commit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (conn->table_locked)
  {
    conn->table_locked = FALSE;
    spider_current_trx->locked_connections--;
  }
  res = OCITransCommit(svchp, errhp, OCI_TRANS_TWOPHASE);
  if (res != OCI_SUCCESS)
  {
    *need_mon = set_error(res, errhp, 0, NULL, NULL);
    if (txnhp)
    {
      OCIHandleFree(txnhp, OCI_HTYPE_TRANS);
      txnhp = NULL;
    }
    DBUG_RETURN(*need_mon);
  }
  if (txnhp)
  {
    OCIHandleFree(txnhp, OCI_HTYPE_TRANS);
    txnhp = NULL;
  }
  DBUG_RETURN(0);
}

int spider_db_oracle::xa_rollback(
  XID *xid,
  int *need_mon
) {
  sword res;
  DBUG_ENTER("spider_db_oracle::xa_rollback");
  DBUG_PRINT("info",("spider this=%p", this));
  if (svchp && errhp)
  {
    res = OCITransRollback(svchp, errhp, OCI_DEFAULT);
    if (res != OCI_SUCCESS)
    {
      *need_mon = set_error(res, errhp, 0, NULL, NULL);
      if (txnhp)
      {
        OCIHandleFree(txnhp, OCI_HTYPE_TRANS);
        txnhp = NULL;
      }
      DBUG_RETURN(*need_mon);
    }
  }
  if (txnhp)
  {
    OCIHandleFree(txnhp, OCI_HTYPE_TRANS);
    txnhp = NULL;
  }
  DBUG_RETURN(0);
}

bool spider_db_oracle::set_trx_isolation_in_bulk_sql()
{
  DBUG_ENTER("spider_db_oracle::set_trx_isolation_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_oracle::set_trx_isolation(
  int trx_isolation,
  int *need_mon
) {
  DBUG_ENTER("spider_db_oracle::set_trx_isolation");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (trx_isolation)
  {
    case ISO_READ_UNCOMMITTED:
    case ISO_READ_COMMITTED:
      if (conn->in_before_query)
      {
        DBUG_RETURN(exec_query(SPIDER_SQL_ISO_READ_COMMITTED_STR,
          SPIDER_SQL_ISO_READ_COMMITTED_LEN, -1));
      }
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
    case ISO_SERIALIZABLE:
      if (conn->in_before_query)
      {
        DBUG_RETURN(exec_query(SPIDER_SQL_ISO_SERIALIZABLE_STR,
          SPIDER_SQL_ISO_SERIALIZABLE_LEN, -1));
      }
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

bool spider_db_oracle::set_autocommit_in_bulk_sql()
{
  DBUG_ENTER("spider_db_oracle::set_autocommit_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_oracle::set_autocommit(
  bool autocommit,
  int *need_mon
) {
  DBUG_ENTER("spider_db_oracle::set_autocommit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (autocommit)
  {
    if (conn->in_before_query)
    {
      DBUG_RETURN(exec_query(SPIDER_SQL_AUTOCOMMIT_ON_STR,
        SPIDER_SQL_AUTOCOMMIT_ON_LEN, -1));
    }
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
    if (conn->in_before_query)
    {
      DBUG_RETURN(exec_query(SPIDER_SQL_AUTOCOMMIT_OFF_STR,
        SPIDER_SQL_AUTOCOMMIT_OFF_LEN, -1));
    }
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

bool spider_db_oracle::set_sql_log_off_in_bulk_sql()
{
  DBUG_ENTER("spider_db_oracle::set_sql_log_off_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_oracle::set_sql_log_off(
  bool sql_log_off,
  int *need_mon
) {
  DBUG_ENTER("spider_db_oracle::set_sql_log_off");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_oracle::set_time_zone_in_bulk_sql()
{
  DBUG_ENTER("spider_db_oracle::set_time_zone_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_oracle::set_time_zone(
  Time_zone *time_zone,
  int *need_mon
) {
  DBUG_ENTER("spider_db_oracle::set_time_zone");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
int spider_db_oracle::append_sql(
  char *sql,
  ulong sql_length,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_oracle::append_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_oracle::append_open_handler(
  uint handler_id,
  const char *db_name,
  const char *table_name,
  const char *index_name,
  const char *sql,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_oracle::append_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_oracle::append_select(
  uint handler_id,
  spider_string *sql,
  SPIDER_DB_HS_STRING_REF_BUFFER *keys,
  int limit,
  int skip,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_oracle::append_select");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_oracle::append_insert(
  uint handler_id,
  SPIDER_DB_HS_STRING_REF_BUFFER *upds,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_oracle::append_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_oracle::append_update(
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
  DBUG_ENTER("spider_db_oracle::append_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_oracle::append_delete(
  uint handler_id,
  spider_string *sql,
  SPIDER_DB_HS_STRING_REF_BUFFER *keys,
  int limit,
  int skip,
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_oracle::append_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

void spider_db_oracle::reset_request_queue()
{
  DBUG_ENTER("spider_db_oracle::reset_request_queue");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}
#endif

size_t spider_db_oracle::escape_string(
  char *to,
  const char *from,
  size_t from_length
) {
  DBUG_ENTER("spider_db_oracle::escape_string");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(util.escape_string(to, from, from_length, conn->access_charset));
}

bool spider_db_oracle::have_lock_table_list()
{
  DBUG_ENTER("spider_db_oracle::have_lock_table_list");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(lock_table_hash.records);
}

int spider_db_oracle::append_lock_tables(
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
  DBUG_ENTER("spider_db_oracle::lock_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((tmp_link_for_hash =
    (SPIDER_LINK_FOR_HASH *) my_hash_element(&lock_table_hash, 0)))
  {
    if ((error_num = spider_db_oracle_utility.append_lock_table_head(str)))
    {
      DBUG_RETURN(error_num);
    }

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
    spider_oracle_share *db_share = (spider_oracle_share *)
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
    if ((error_num = spider_db_oracle_utility.
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

    if ((error_num = spider_db_oracle_utility.append_lock_table_tail(str)))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_db_oracle::append_unlock_tables(
  spider_string *str
) {
  int error_num;
  DBUG_ENTER("spider_db_oracle::append_unlock_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_oracle_utility.append_unlock_table(str)))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

uint spider_db_oracle::get_lock_table_hash_count()
{
  DBUG_ENTER("spider_db_oracle::get_lock_table_hash_count");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(lock_table_hash.records);
}

void spider_db_oracle::reset_lock_table_hash()
{
  DBUG_ENTER("spider_db_oracle::reset_lock_table_hash");
  DBUG_PRINT("info",("spider this=%p", this));
  my_hash_reset(&lock_table_hash);
  DBUG_VOID_RETURN;
}

uint spider_db_oracle::get_opened_handler_count()
{
  DBUG_ENTER("spider_db_oracle::get_opened_handler_count");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(handler_open_array.elements);
}

void spider_db_oracle::reset_opened_handler()
{
  ha_spider *tmp_spider;
  int tmp_link_idx;
  SPIDER_LINK_FOR_HASH **tmp_link_for_hash;
  DBUG_ENTER("spider_db_oracle::reset_opened_handler");
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

void spider_db_oracle::set_dup_key_idx(
  ha_spider *spider,
  int link_idx
) {
  TABLE *table = spider->get_table();
  uint roop_count, pk_idx = table->s->primary_key;
  int key_name_length;
  int max_length = 0;
  char *key_name, *tmp_pos;
  char buf[SPIDER_ORACLE_ERR_BUF_LEN];
  DBUG_ENTER("spider_db_oracle::set_dup_key_idx");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider error_str=%s", stored_error_msg));
  memcpy(buf, spider->share->tgt_dbs[link_idx],
    spider->share->tgt_dbs_lengths[link_idx]);
  tmp_pos = buf + spider->share->tgt_dbs_lengths[link_idx];
  *tmp_pos = '.';
  ++tmp_pos;
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
    memcpy(tmp_pos, key_name, key_name_length + 1);
    DBUG_PRINT("info",("spider key_name=%s", key_name));
    DBUG_PRINT("info",("spider full key name=%s", buf));
    if (
      max_length < key_name_length &&
      strcasestr(stored_error_msg, buf)
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

bool spider_db_oracle::cmp_request_key_to_snd(
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_oracle::cmp_request_key_to_snd");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

int spider_db_oracle::set_error(
  sword res,
  dvoid *hndlp,
  int error_num,
  const char *error1,
  const char *error2
) {
  DBUG_ENTER("spider_db_oracle::set_error");
  DBUG_PRINT("info",("spider this=%p", this));
  stored_error_num =
    spider_db_oracle_get_error(res, hndlp, error_num, error1, error2,
      conn->access_charset, stored_error_msg);
  if (stored_error_num)
    stored_error = ER_SPIDER_ORACLE_ERR;
  else
    stored_error = "";
  DBUG_RETURN(stored_error_num);
}

spider_db_oracle_util::spider_db_oracle_util() : spider_db_util()
{
  DBUG_ENTER("spider_db_oracle_util::spider_db_oracle_util");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_oracle_util::~spider_db_oracle_util()
{
  DBUG_ENTER("spider_db_oracle_util::~spider_db_oracle_util");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

int spider_db_oracle_util::append_name(
  spider_string *str,
  const char *name,
  uint name_length
) {
  DBUG_ENTER("spider_db_oracle_util::append_name");
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->q_append(name, name_length);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

int spider_db_oracle_util::append_name_with_charset(
  spider_string *str,
  const char *name,
  uint name_length,
  CHARSET_INFO *name_charset
) {
  DBUG_ENTER("spider_db_oracle_util::append_name_with_charset");
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN * 2 + name_length * 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->append(name, name_length, name_charset);
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

bool spider_db_oracle_util::is_name_quote(
  const char head_code
) {
  DBUG_ENTER("spider_db_oracle_util::is_name_quote");
  DBUG_RETURN(head_code == *name_quote_str);
}

int spider_db_oracle_util::append_escaped_name_quote(
  spider_string *str
) {
  DBUG_ENTER("spider_db_oracle_util::append_escaped_name_quote");
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN * 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

int spider_db_oracle_util::append_column_value(
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
  DBUG_ENTER("spider_db_oracle_util::append_column_value");
  tmp_str.init_calc_mem(181);

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
      tmp_str2.init_calc_mem(182);
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

int spider_db_oracle_util::append_from_with_alias(
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
  DBUG_ENTER("spider_db_oracle_util::append_from_with_alias");
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

int spider_db_oracle_util::append_trx_isolation(
  spider_string *str,
  int trx_isolation
) {
  DBUG_ENTER("spider_db_oracle_util::append_trx_isolation");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SEMICOLON_LEN +
    SPIDER_SQL_ISO_READ_COMMITTED_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (str->length())
  {
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }
  switch (trx_isolation)
  {
    case ISO_READ_UNCOMMITTED:
    case ISO_READ_COMMITTED:
      str->q_append(SPIDER_SQL_ISO_READ_COMMITTED_STR,
        SPIDER_SQL_ISO_READ_COMMITTED_LEN);
      break;
    case ISO_REPEATABLE_READ:
    case ISO_SERIALIZABLE:
      str->q_append(SPIDER_SQL_ISO_SERIALIZABLE_STR,
        SPIDER_SQL_ISO_SERIALIZABLE_LEN);
      break;
    default:
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }
  DBUG_RETURN(0);
}

int spider_db_oracle_util::append_autocommit(
  spider_string *str,
  bool autocommit
) {
  DBUG_ENTER("spider_db_oracle_util::append_autocommit");
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

int spider_db_oracle_util::append_sql_log_off(
  spider_string *str,
  bool sql_log_off
) {
  DBUG_ENTER("spider_db_oracle_util::append_sql_log_off");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_oracle_util::append_time_zone(
  spider_string *str,
  Time_zone *time_zone
) {
  DBUG_ENTER("spider_db_oracle_util::append_time_zone");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_oracle_util::append_start_transaction(
  spider_string *str
) {
  DBUG_ENTER("spider_db_oracle_util::append_start_transaction");
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

int spider_db_oracle_util::append_xa_start(
  spider_string *str,
  XID *xid
) {
  DBUG_ENTER("spider_db_oracle_util::append_xa_start");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_oracle_util::append_lock_table_head(
  spider_string *str
) {
  DBUG_ENTER("spider_db_oracle_util::append_lock_table_head");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_oracle_util::append_lock_table_body(
  spider_string *str,
  const char *db_name,
  uint db_name_length,
  CHARSET_INFO *db_name_charset,
  const char *table_name,
  uint table_name_length,
  CHARSET_INFO *table_name_charset,
  int lock_type
) {
  DBUG_ENTER("spider_db_oracle_util::append_lock_table_body");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SEMICOLON_LEN + SPIDER_SQL_LOCK_TABLE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (str->length())
  {
    str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  }
  str->q_append(SPIDER_SQL_LOCK_TABLE_STR, SPIDER_SQL_LOCK_TABLE_LEN);
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

int spider_db_oracle_util::append_lock_table_tail(
  spider_string *str
) {
  DBUG_ENTER("spider_db_oracle_util::append_lock_table_tail");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_oracle_util::append_unlock_table(
  spider_string *str
) {
  DBUG_ENTER("spider_db_oracle_util::append_unlock_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_COMMIT_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_COMMIT_STR, SPIDER_SQL_COMMIT_LEN);
  DBUG_RETURN(0);
}

int spider_db_oracle_util::open_item_func(
  Item_func *item_func,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  uint dbton_id = spider_dbton_oracle.dbton_id;
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
  DBUG_ENTER("spider_db_oracle_util::open_item_func");
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
          switch (item_date_add_interval->int_type)
          {
            case INTERVAL_YEAR:
            case INTERVAL_QUARTER:
            case INTERVAL_MONTH:
              if (str)
              {
                if (str->reserve(SPIDER_SQL_ADD_MONTHS_LEN +
                  SPIDER_SQL_OPEN_PAREN_LEN))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                str->q_append(SPIDER_SQL_ADD_MONTHS_STR,
                  SPIDER_SQL_ADD_MONTHS_LEN);
                str->q_append(SPIDER_SQL_OPEN_PAREN_STR,
                  SPIDER_SQL_OPEN_PAREN_LEN);
              }
              if ((error_num = spider_db_print_item_type(item_list[0], spider,
                str, alias, alias_length, dbton_id)))
                DBUG_RETURN(error_num);
              if (str)
              {
                if (item_date_add_interval->date_sub_interval)
                {
                  if (str->reserve(SPIDER_SQL_COMMA_LEN +
                    SPIDER_SQL_MINUS_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
                  str->q_append(SPIDER_SQL_MINUS_STR, SPIDER_SQL_MINUS_LEN);
                } else {
                  if (str->reserve(SPIDER_SQL_COMMA_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
                }
              }
              if ((error_num = spider_db_print_item_type(item_list[1], spider,
                str, alias, alias_length, dbton_id)))
                DBUG_RETURN(error_num);
              if (str)
              {
                if (item_date_add_interval->int_type == INTERVAL_YEAR)
                {
                  func_name = " * 12";
                  func_name_length = sizeof(" * 12") - 1;
                  if (str->reserve(func_name_length +
                    (SPIDER_SQL_CLOSE_PAREN_LEN * 2)))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(func_name, func_name_length);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                } else if (item_date_add_interval->int_type ==
                  INTERVAL_QUARTER)
                {
                  func_name = " * 3";
                  func_name_length = sizeof(" * 3") - 1;
                  if (str->reserve(func_name_length +
                    (SPIDER_SQL_CLOSE_PAREN_LEN * 2)))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(func_name, func_name_length);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                } else {
                  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN * 2))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                }
              }
              break;
            case INTERVAL_WEEK:
            case INTERVAL_DAY:
            case INTERVAL_HOUR:
            case INTERVAL_MINUTE:
            case INTERVAL_SECOND:
            case INTERVAL_MICROSECOND:
              if ((error_num = spider_db_print_item_type(item_list[0], spider,
                str, alias, alias_length, dbton_id)))
                DBUG_RETURN(error_num);
              if (str)
              {
                if (item_date_add_interval->date_sub_interval)
                {
                  if (str->reserve(SPIDER_SQL_MINUS_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_MINUS_STR, SPIDER_SQL_MINUS_LEN);
                } else {
                  if (str->reserve(SPIDER_SQL_PLUS_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_PLUS_STR, SPIDER_SQL_PLUS_LEN);
                }
              }
              if ((error_num = spider_db_print_item_type(item_list[1], spider,
                str, alias, alias_length, dbton_id)))
                DBUG_RETURN(error_num);
              if (str)
              {
                if (item_date_add_interval->int_type == INTERVAL_WEEK)
                {
                  func_name = " * 7";
                  func_name_length = sizeof(" * 7") - 1;
                  if (str->reserve(func_name_length +
                    (SPIDER_SQL_CLOSE_PAREN_LEN)))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(func_name, func_name_length);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                } else if (item_date_add_interval->int_type == INTERVAL_HOUR)
                {
                  func_name = " / 24";
                  func_name_length = sizeof(" / 24") - 1;
                  if (str->reserve(func_name_length +
                    (SPIDER_SQL_CLOSE_PAREN_LEN)))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(func_name, func_name_length);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                } else if (item_date_add_interval->int_type == INTERVAL_MINUTE)
                {
                  func_name = " / 1440";
                  func_name_length = sizeof(" / 1440") - 1;
                  if (str->reserve(func_name_length +
                    (SPIDER_SQL_CLOSE_PAREN_LEN)))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(func_name, func_name_length);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                } else if (item_date_add_interval->int_type == INTERVAL_SECOND)
                {
                  func_name = " / 86400";
                  func_name_length = sizeof(" / 86400") - 1;
                  if (str->reserve(func_name_length +
                    (SPIDER_SQL_CLOSE_PAREN_LEN)))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(func_name, func_name_length);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                } else if (item_date_add_interval->int_type ==
                  INTERVAL_MICROSECOND)
                {
                  func_name = " / 86400000000";
                  func_name_length = sizeof(" / 86400000000") - 1;
                  if (str->reserve(func_name_length +
                    (SPIDER_SQL_CLOSE_PAREN_LEN)))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(func_name, func_name_length);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                } else {
                  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
                    SPIDER_SQL_CLOSE_PAREN_LEN);
                }
              }
              break;
            default:
              DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
          }
          DBUG_RETURN(0);
          break;
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
    case Item_func::UDF_FUNC:
      use_pushdown_udf = spider_param_use_pushdown_udf(spider->trx->thd,
        spider->share->use_pushdown_udf);
      if (!use_pushdown_udf)
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
    case Item_func::LIKE_FUNC:
      if (str)
      {
        func_name = (char*) item_func->func_name();
        func_name_length = strlen(func_name);
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
int spider_db_oracle_util::open_item_sum_func(
  Item_sum *item_sum,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  uint dbton_id = spider_dbton_oracle.dbton_id;
  uint roop_count, item_count = item_sum->get_arg_count();
  int error_num;
  DBUG_ENTER("spider_db_oracle_util::open_item_sum_func");
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

size_t spider_db_oracle_util::escape_string(
  char *to,
  const char *from,
  size_t from_length,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_oracle::escape_string");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(escape_quotes_for_mysql(access_charset, to, 0,
    from, from_length));
}

int spider_db_oracle_util::append_escaped_util(
  spider_string *to,
  String *from
) {
  size_t copy_length;
  DBUG_ENTER("spider_db_oracle_util::append_escaped_util");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider to=%s", to->c_ptr_safe()));
  DBUG_PRINT("info",("spider from=%s", from->c_ptr_safe()));
  copy_length = escape_string((char *) to->ptr() + to->length(), from->ptr(),
    from->length(), to->charset());
  DBUG_PRINT("info",("spider copy_length=%zu", copy_length));
  to->length(to->length() + copy_length);
  to->mem_calc();
  DBUG_RETURN(0);
}

spider_oracle_share::spider_oracle_share(
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
  show_autoinc(NULL),
  show_last_insert_id(NULL),
  show_index(NULL),
  table_names_str(NULL),
  db_names_str(NULL),
  db_table_str(NULL),
  nextval_str(NULL),
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  db_table_str_hash_value(NULL),
#endif
  table_nm_max_length(0),
  db_nm_max_length(0),
  nextval_max_length(0),
  column_name_str(NULL),
  same_db_table_name(TRUE),
  first_all_link_idx(-1)
{
  DBUG_ENTER("spider_oracle_share::spider_oracle_share");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_alloc_calc_mem_init(mem_calc, 220);
  spider_alloc_calc_mem(spider_current_trx, mem_calc, sizeof(*this));
  DBUG_VOID_RETURN;
}

spider_oracle_share::~spider_oracle_share()
{
  DBUG_ENTER("spider_oracle_share::~spider_oracle_share");
  DBUG_PRINT("info",("spider this=%p", this));
  if (table_select)
    delete [] table_select;
  if (key_select)
    delete [] key_select;
  if (key_hint)
    delete [] key_hint;
  free_show_table_status();
  free_show_records();
  free_show_autoinc();
  free_show_last_insert_id();
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

int spider_oracle_share::init()
{
  int error_num;
  uint roop_count;
  TABLE_SHARE *table_share = spider_share->table_share;
  uint keys = table_share ? table_share->keys : 0;
  DBUG_ENTER("spider_oracle_share::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(key_select_pos = (int *)
    spider_bulk_alloc_mem(spider_current_trx, 221,
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
    key_hint[roop_count].init_calc_mem(190);
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
        (error_num = append_show_autoinc()) ||
        (error_num = append_show_last_insert_id()) ||
        (error_num = append_show_index())
      )
    )
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  table_select->init_calc_mem(191);
  if (table_share && (error_num = append_table_select()))
    DBUG_RETURN(error_num);

  for (roop_count = 0; roop_count < keys; roop_count++)
  {
    key_select[roop_count].init_calc_mem(192);
    if ((error_num = append_key_select(roop_count)))
      DBUG_RETURN(error_num);
  }

  DBUG_RETURN(error_num);
}

uint spider_oracle_share::get_column_name_length(
  uint field_index
) {
  DBUG_ENTER("spider_oracle_share::get_column_name_length");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(column_name_str[field_index].length());
}

int spider_oracle_share::append_column_name(
  spider_string *str,
  uint field_index
) {
  int error_num;
  DBUG_ENTER("spider_oracle_share::append_column_name");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = spider_db_oracle_utility.append_name(str,
    column_name_str[field_index].ptr(), column_name_str[field_index].length());
  DBUG_RETURN(error_num);
}

int spider_oracle_share::append_column_name_with_alias(
  spider_string *str,
  uint field_index,
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_oracle_share::append_column_name_with_alias");
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

int spider_oracle_share::append_table_name(
  spider_string *str,
  int all_link_idx
) {
  const char *db_nm = db_names_str[all_link_idx].ptr();
  uint db_nm_len = db_names_str[all_link_idx].length();
  const char *table_nm = table_names_str[all_link_idx].ptr();
  uint table_nm_len = table_names_str[all_link_idx].length();
  DBUG_ENTER("spider_oracle_share::append_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(db_nm_len + SPIDER_SQL_DOT_LEN + table_nm_len +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  spider_db_oracle_utility.append_name(str, db_nm, db_nm_len);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  spider_db_oracle_utility.append_name(str, table_nm, table_nm_len);
  DBUG_RETURN(0);
}

int spider_oracle_share::append_table_name_with_adjusting(
  spider_string *str,
  int all_link_idx
) {
  const char *db_nm = db_names_str[all_link_idx].ptr();
  uint db_nm_len = db_names_str[all_link_idx].length();
  uint db_nm_max_len = db_nm_max_length;
  const char *table_nm = table_names_str[all_link_idx].ptr();
  uint table_nm_len = table_names_str[all_link_idx].length();
  uint table_nm_max_len = table_nm_max_length;
  DBUG_ENTER("spider_oracle_share::append_table_name_with_adjusting");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_db_oracle_utility.append_name(str, db_nm, db_nm_len);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  spider_db_oracle_utility.append_name(str, table_nm, table_nm_len);
  uint length =
    db_nm_max_len - db_nm_len +
    table_nm_max_len - table_nm_len;
  memset((char *) str->ptr() + str->length(), ' ', length);
  str->length(str->length() + length);
  DBUG_RETURN(0);
}

int spider_oracle_share::append_from_with_adjusted_table_name(
  spider_string *str,
  int *table_name_pos
) {
  const char *db_nm = db_names_str[0].ptr();
  uint db_nm_len = db_names_str[0].length();
  uint db_nm_max_len = db_nm_max_length;
  const char *table_nm = table_names_str[0].ptr();
  uint table_nm_len = table_names_str[0].length();
  uint table_nm_max_len = table_nm_max_length;
  DBUG_ENTER("spider_oracle_share::append_from_with_adjusted_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_FROM_LEN + db_nm_max_length +
    SPIDER_SQL_DOT_LEN + table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
  *table_name_pos = str->length();
  spider_db_oracle_utility.append_name(str, db_nm, db_nm_len);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  spider_db_oracle_utility.append_name(str, table_nm, table_nm_len);
  uint length =
    db_nm_max_len - db_nm_len +
    table_nm_max_len - table_nm_len;
  memset((char *) str->ptr() + str->length(), ' ', length);
  str->length(str->length() + length);
  DBUG_RETURN(0);
}

int spider_oracle_share::create_table_names_str()
{
  int error_num, roop_count;
  uint table_nm_len, db_nm_len;
  spider_string *str, *first_tbl_nm_str, *first_db_nm_str, *first_db_tbl_str;
  char *first_tbl_nm, *first_db_nm;
  uint dbton_id = spider_dbton_oracle.dbton_id;
  DBUG_ENTER("spider_oracle_share::create_table_names_str");
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
    table_names_str[roop_count].init_calc_mem(193);
    db_names_str[roop_count].init_calc_mem(194);
    db_table_str[roop_count].init_calc_mem(195);
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

void spider_oracle_share::free_table_names_str()
{
  DBUG_ENTER("spider_oracle_share::free_table_names_str");
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

int spider_oracle_share::create_column_name_str()
{
  spider_string *str;
  int error_num;
  Field **field;
  TABLE_SHARE *table_share = spider_share->table_share;
  uint dbton_id = spider_dbton_oracle.dbton_id;
  DBUG_ENTER("spider_oracle_share::create_column_name_str");
  if (
    table_share->fields &&
    !(column_name_str = new spider_string[table_share->fields])
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  for (field = table_share->field, str = column_name_str;
   *field; field++, str++)
  {
    str->init_calc_mem(196);
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

void spider_oracle_share::free_column_name_str()
{
  DBUG_ENTER("spider_oracle_share::free_column_name_str");
  if (column_name_str)
  {
    delete [] column_name_str;
    column_name_str = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_share::convert_key_hint_str()
{
  spider_string *tmp_key_hint;
  int roop_count;
  TABLE_SHARE *table_share = spider_share->table_share;
  DBUG_ENTER("spider_oracle_share::convert_key_hint_str");
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

int spider_oracle_share::append_show_table_status()
{
  int roop_count;
  spider_string *str;
  uint dbton_id = spider_dbton_oracle.dbton_id;
  DBUG_ENTER("spider_oracle_append_show_table_status");
  if (!(show_table_status =
    new spider_string[2 * spider_share->all_link_count]))
    goto error;

  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    show_table_status[0 + (2 * roop_count)].init_calc_mem(197);
    show_table_status[1 + (2 * roop_count)].init_calc_mem(207);
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

void spider_oracle_share::free_show_table_status()
{
  DBUG_ENTER("spider_oracle_free_show_table_status");
  if (show_table_status)
  {
    delete [] show_table_status;
    show_table_status = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_share::append_show_records()
{
  int roop_count;
  spider_string *str;
  uint dbton_id = spider_dbton_oracle.dbton_id;
  DBUG_ENTER("spider_oracle_share::append_show_records");
  if (!(show_records = new spider_string[spider_share->all_link_count]))
    goto error;

  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    show_records[roop_count].init_calc_mem(208);
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

void spider_oracle_share::free_show_records()
{
  DBUG_ENTER("spider_oracle_share::free_show_records");
  if (show_records)
  {
    delete [] show_records;
    show_records = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_share::append_show_autoinc()
{
  uint roop_count, field_length;
  spider_string *str;
  uint dbton_id = spider_dbton_oracle.dbton_id;
  Field **found_next_number_field =
    spider_share->table_share->found_next_number_field;
  DBUG_ENTER("spider_oracle_share::append_show_autoinc");
  if (!found_next_number_field)
    DBUG_RETURN(0);

  if (!(show_autoinc = new spider_string[spider_share->all_link_count]))
    goto error;

  field_length =
    column_name_str[(*found_next_number_field)->field_index].length();
  for (roop_count = 0; roop_count < spider_share->all_link_count;
    roop_count++)
  {
    show_autoinc[roop_count].init_calc_mem(224);
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
      continue;

    if (
      show_autoinc[roop_count].reserve(
        SPIDER_SQL_SELECT_LEN +
        SPIDER_SQL_MAX_LEN +
        SPIDER_SQL_OPEN_PAREN_LEN +
        field_length +
        SPIDER_SQL_CLOSE_PAREN_LEN +
        SPIDER_SQL_FROM_LEN +
        db_names_str[roop_count].length() +
        SPIDER_SQL_DOT_LEN +
        table_names_str[roop_count].length() +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 6)
    )
      goto error;
    str = &show_autoinc[roop_count];
    str->q_append(SPIDER_SQL_SELECT_STR, SPIDER_SQL_SELECT_LEN);
    str->q_append(SPIDER_SQL_MAX_STR, SPIDER_SQL_MAX_LEN);
    str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
    append_column_name(str, (*found_next_number_field)->field_index);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
    str->q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
    append_table_name(str, roop_count);
  }
  DBUG_RETURN(0);

error:
  if (show_autoinc)
  {
    delete [] show_autoinc;
    show_autoinc = NULL;
  }
  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
}

void spider_oracle_share::free_show_autoinc()
{
  DBUG_ENTER("spider_oracle_share::free_show_autoinc");
  if (show_autoinc)
  {
    delete [] show_autoinc;
    show_autoinc = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_share::append_show_last_insert_id()
{
  uint roop_count;
  spider_string *str;
  uint dbton_id = spider_dbton_oracle.dbton_id;
  Field **found_next_number_field =
    spider_share->table_share->found_next_number_field;
  uint seq_nm_max_length = 0;
  DBUG_ENTER("spider_oracle_share::append_show_last_insert_id");
  if (!found_next_number_field)
    DBUG_RETURN(0);

  if (
    !(show_last_insert_id = new spider_string[spider_share->all_link_count]) ||
    !(nextval_str = new spider_string[spider_share->all_link_count])
  )
    goto error;

  for (roop_count = 0; roop_count < spider_share->all_link_count;
    roop_count++)
  {
    show_last_insert_id[roop_count].init_calc_mem(225);
    nextval_str[roop_count].init_calc_mem(226);
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
      continue;

    if (
      show_last_insert_id[roop_count].reserve(
        SPIDER_SQL_SELECT_LEN +
        spider_share->tgt_sequence_names_lengths[roop_count] + 
        SPIDER_SQL_CURRVAL_LEN +
        SPIDER_SQL_FROM_DUAL_LEN +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2)
    )
      goto error;
    str = &show_last_insert_id[roop_count];
    str->q_append(SPIDER_SQL_SELECT_STR, SPIDER_SQL_SELECT_LEN);
    spider_db_oracle_utility.append_name(str,
      spider_share->tgt_sequence_names[roop_count],
      spider_share->tgt_sequence_names_lengths[roop_count]);
    str->q_append(SPIDER_SQL_CURRVAL_STR, SPIDER_SQL_CURRVAL_LEN);
    str->q_append(SPIDER_SQL_FROM_DUAL_STR, SPIDER_SQL_FROM_DUAL_LEN);

    if (seq_nm_max_length <
      spider_share->tgt_sequence_names_lengths[roop_count])
    {
      seq_nm_max_length =
        spider_share->tgt_sequence_names_lengths[roop_count];
    }
  }
  for (roop_count = 0; roop_count < spider_share->all_link_count;
    roop_count++)
  {
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
      continue;

    if (
      nextval_str[roop_count].reserve(
        seq_nm_max_length +
        SPIDER_SQL_NEXTVAL_LEN +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2)
    )
      goto error;
    str = &nextval_str[roop_count];
    spider_db_oracle_utility.append_name(str,
      spider_share->tgt_sequence_names[roop_count],
      spider_share->tgt_sequence_names_lengths[roop_count]);
    str->q_append(SPIDER_SQL_NEXTVAL_STR, SPIDER_SQL_NEXTVAL_LEN);
    uint length =
      seq_nm_max_length - spider_share->tgt_sequence_names_lengths[roop_count];
    memset((char *) str->ptr() + str->length(), ' ', length);
    str->length(str->length() + length);
    nextval_max_length = str->length();
  }
  DBUG_RETURN(0);

error:
  if (show_last_insert_id)
  {
    delete [] show_last_insert_id;
    show_last_insert_id = NULL;
  }
  if (nextval_str)
  {
    delete [] nextval_str;
    nextval_str = NULL;
  }
  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
}

void spider_oracle_share::free_show_last_insert_id()
{
  DBUG_ENTER("spider_oracle_share::free_show_last_insert_id");
  if (show_last_insert_id)
  {
    delete [] show_last_insert_id;
    show_last_insert_id = NULL;
  }
  if (nextval_str)
  {
    delete [] nextval_str;
    nextval_str = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_share::append_show_index()
{
  int roop_count;
  spider_string *str;
  uint dbton_id = spider_dbton_oracle.dbton_id;
  DBUG_ENTER("spider_oracle_share::append_show_index");
  if (!(show_index = new spider_string[2 * spider_share->all_link_count]))
    goto error;

  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    show_index[0 + (2 * roop_count)].init_calc_mem(209);
    show_index[1 + (2 * roop_count)].init_calc_mem(210);
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

void spider_oracle_share::free_show_index()
{
  DBUG_ENTER("spider_oracle_share::free_show_index");
  if (show_index)
  {
    delete [] show_index;
    show_index = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_share::append_table_select()
{
  Field **field;
  uint field_length;
  spider_string *str = table_select;
  TABLE_SHARE *table_share = spider_share->table_share;
  DBUG_ENTER("spider_oracle_share::append_table_select");
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

int spider_oracle_share::append_key_select(
  uint idx
) {
  KEY_PART_INFO *key_part;
  Field *field;
  uint part_num;
  uint field_length;
  spider_string *str = &key_select[idx];
  TABLE_SHARE *table_share = spider_share->table_share;
  const KEY *key_info = &table_share->key_info[idx];
  DBUG_ENTER("spider_oracle_share::append_key_select");
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

bool spider_oracle_share::need_change_db_table_name()
{
  DBUG_ENTER("spider_oracle_share::need_change_db_table_name");
  DBUG_RETURN(!same_db_table_name);
}

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
int spider_oracle_share::discover_table_structure(
  SPIDER_TRX *trx,
  SPIDER_SHARE *spider_share,
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_share::discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif

spider_oracle_handler::spider_oracle_handler(
  ha_spider *spider,
  spider_oracle_share *db_share
) : spider_db_handler(
  spider,
  db_share
),
  where_pos(0),
  order_pos(0),
  limit_pos(0),
  table_name_pos(0),
  update_set_pos(0),
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
  table_lock_mode(0),
  reading_from_bulk_tmp_table(FALSE),
  filled_up(FALSE),
  select_rownum_appended(FALSE),
  update_rownum_appended(FALSE),
  union_table_name_pos_first(NULL),
  union_table_name_pos_current(NULL),
  oracle_share(db_share),
  link_for_hash(NULL)
{
  DBUG_ENTER("spider_oracle_handler::spider_oracle_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_alloc_calc_mem_init(mem_calc, 222);
  spider_alloc_calc_mem(spider_current_trx, mem_calc, sizeof(*this));
  DBUG_VOID_RETURN;
}

spider_oracle_handler::~spider_oracle_handler()
{
  DBUG_ENTER("spider_oracle_handler::~spider_oracle_handler");
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

int spider_oracle_handler::init()
{
  uint roop_count;
  THD *thd = spider->trx->thd;
  st_spider_share *share = spider->share;
  int init_sql_alloc_size =
    spider_param_init_sql_alloc_size(thd, share->init_sql_alloc_size);
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_oracle_handler::init");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.init_calc_mem(67);
  sql_part.init_calc_mem(68);
  sql_part2.init_calc_mem(69);
  ha_sql.init_calc_mem(70);
  insert_sql.init_calc_mem(72);
  update_sql.init_calc_mem(73);
  tmp_sql.init_calc_mem(74);
  dup_update_sql.init_calc_mem(167);
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
    spider_bulk_alloc_mem(spider_current_trx, 223,
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
      &oracle_share->db_table_str[roop_count];
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    link_for_hash[roop_count].db_table_str_hash_value =
      oracle_share->db_table_str_hash_value[roop_count];
#endif
  }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  hs_upds.init();
#endif
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_table_name_with_adjusting(
  spider_string *str,
  int link_idx,
  ulong sql_type
) {
  int error_num = 0;
  DBUG_ENTER("spider_oracle_handler::append_table_name_with_adjusting");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type == SPIDER_SQL_TYPE_HANDLER)
  {
    str->q_append(spider->m_handler_cid[link_idx], SPIDER_SQL_HANDLER_CID_LEN);
  } else {
    error_num = oracle_share->append_table_name_with_adjusting(str,
      spider->conn_link_idx[link_idx]);
  }
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_key_column_types(
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
  DBUG_ENTER("spider_oracle_handler::append_key_column_types");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_str.init_calc_mem(227);

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

    if (str->reserve(SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);

  DBUG_RETURN(0);
}

int spider_oracle_handler::append_key_join_columns_for_bka(
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
  DBUG_ENTER("spider_oracle_handler::append_key_join_columns_for_bka");
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
      oracle_share->column_name_str[field->field_index].length();
    length = my_sprintf(tmp_buf, (tmp_buf, "c%u", key_count));
    if (str->reserve(length + table_alias_lengths[0] + key_name_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
      table_alias_lengths[1] + SPIDER_SQL_PF_EQUAL_LEN + SPIDER_SQL_AND_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(table_aliases[0], table_alias_lengths[0]);
    str->q_append(tmp_buf, length);
    str->q_append(SPIDER_SQL_PF_EQUAL_STR, SPIDER_SQL_PF_EQUAL_LEN);
    str->q_append(table_aliases[1], table_alias_lengths[1]);
    oracle_share->append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
  }
  str->length(str->length() - SPIDER_SQL_AND_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_tmp_table_and_sql_for_bka(
  const key_range *start_key
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_tmp_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  char tmp_table_name[MAX_FIELD_WIDTH * 2],
    tgt_table_name[MAX_FIELD_WIDTH * 2];
  int tmp_table_name_length;
  spider_string tgt_table_name_str(tgt_table_name, MAX_FIELD_WIDTH * 2,
    oracle_share->db_names_str[0].charset());
  const char *table_names[2], *table_aliases[2], *table_dot_aliases[2];
  uint table_name_lengths[2], table_alias_lengths[2],
    table_dot_alias_lengths[2];
  tgt_table_name_str.init_calc_mem(200);
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
    (error_num = spider_db_oracle_utility.append_from_with_alias(&sql,
      table_names, table_name_lengths,
      table_aliases, table_alias_lengths, 2,
      &table_name_pos, FALSE))
  )
    DBUG_RETURN(error_num);
  if (
    oracle_share->key_hint &&
    (error_num = spider_db_append_hint_after_table(spider,
      &sql, &oracle_share->key_hint[spider->active_index]))
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

int spider_oracle_handler::reuse_tmp_table_and_sql_for_bka()
{
  DBUG_ENTER("spider_oracle_handler::reuse_tmp_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_sql.length(tmp_sql_pos4);
  sql.length(limit_pos);
  ha_sql.length(ha_limit_pos);
  DBUG_RETURN(0);
}

void spider_oracle_handler::create_tmp_bka_table_name(
  char *tmp_table_name,
  int *tmp_table_name_length,
  int link_idx
) {
  uint adjust_length, length;
  DBUG_ENTER("spider_oracle_handler::create_tmp_bka_table_name");
  if (spider_param_bka_table_name_type(current_thd,
    oracle_share->spider_share->
      bka_table_name_types[spider->conn_link_idx[link_idx]]) == 1)
  {
    adjust_length =
      oracle_share->db_nm_max_length -
      oracle_share->db_names_str[spider->conn_link_idx[link_idx]].length() +
      oracle_share->table_nm_max_length -
      oracle_share->table_names_str[spider->conn_link_idx[link_idx]].length();
    *tmp_table_name_length = oracle_share->db_nm_max_length +
      oracle_share->table_nm_max_length;
    memset(tmp_table_name, ' ', adjust_length);
    tmp_table_name += adjust_length;
    memcpy(tmp_table_name, oracle_share->db_names_str[link_idx].c_ptr(),
      oracle_share->db_names_str[link_idx].length());
    tmp_table_name += oracle_share->db_names_str[link_idx].length();
    length = my_sprintf(tmp_table_name, (tmp_table_name,
      "%s%s%p%s", SPIDER_SQL_DOT_STR, SPIDER_SQL_TMP_BKA_STR, spider,
      SPIDER_SQL_UNDERSCORE_STR));
    *tmp_table_name_length += length;
    tmp_table_name += length;
    memcpy(tmp_table_name,
      oracle_share->table_names_str[spider->conn_link_idx[link_idx]].c_ptr(),
      oracle_share->table_names_str[spider->conn_link_idx[link_idx]].length());
  } else {
    adjust_length =
      oracle_share->db_nm_max_length -
      oracle_share->db_names_str[spider->conn_link_idx[link_idx]].length();
    *tmp_table_name_length = oracle_share->db_nm_max_length;
    memset(tmp_table_name, ' ', adjust_length);
    tmp_table_name += adjust_length;
    memcpy(tmp_table_name, oracle_share->db_names_str[link_idx].c_ptr(),
      oracle_share->db_names_str[link_idx].length());
    tmp_table_name += oracle_share->db_names_str[link_idx].length();
    length = my_sprintf(tmp_table_name, (tmp_table_name,
      "%s%s%p", SPIDER_SQL_DOT_STR, SPIDER_SQL_TMP_BKA_STR, spider));
    *tmp_table_name_length += length;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_handler::append_create_tmp_bka_table(
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
    cset_length = strlen(table_charset->csname);
  DBUG_ENTER("spider_oracle_handler::append_create_tmp_bka_table");
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
    SPIDER_SQL_DEF_CHARSET_LEN + cset_length + SPIDER_SQL_SEMICOLON_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_ENGINE_STR, SPIDER_SQL_ENGINE_LEN);
  str->q_append(bka_engine, bka_engine_length);
  str->q_append(SPIDER_SQL_DEF_CHARSET_STR, SPIDER_SQL_DEF_CHARSET_LEN);
  str->q_append(table_charset->csname, cset_length);
  str->q_append(SPIDER_SQL_SEMICOLON_STR, SPIDER_SQL_SEMICOLON_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_drop_tmp_bka_table(
  spider_string *str,
  char *tmp_table_name,
  int tmp_table_name_length,
  int *db_name_pos,
  int *drop_table_end_pos,
  bool with_semicolon
) {
  DBUG_ENTER("spider_oracle_handler::append_drop_tmp_bka_table");
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

int spider_oracle_handler::append_insert_tmp_bka_table(
  const key_range *start_key,
  spider_string *str,
  char *tmp_table_name,
  int tmp_table_name_length,
  int *db_name_pos
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_insert_tmp_bka_table");
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

int spider_oracle_handler::append_union_table_and_sql_for_bka(
  const key_range *start_key
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_union_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  char tgt_table_name[MAX_FIELD_WIDTH * 2];
  spider_string tgt_table_name_str(tgt_table_name, MAX_FIELD_WIDTH * 2,
    oracle_share->db_names_str[0].charset());
  const char *table_names[2], *table_aliases[2], *table_dot_aliases[2];
  uint table_name_lengths[2], table_alias_lengths[2],
    table_dot_alias_lengths[2];
  tgt_table_name_str.init_calc_mem(234);
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
    (error_num = spider_db_oracle_utility.append_from_with_alias(&tmp_sql,
      table_names, table_name_lengths,
      table_aliases, table_alias_lengths, 2,
      &table_name_pos, FALSE))
  )
    DBUG_RETURN(error_num);
  if (
    oracle_share->key_hint &&
    (error_num = spider_db_append_hint_after_table(spider,
      &tmp_sql, &oracle_share->key_hint[spider->active_index]))
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
    if ((error_num = append_key_order_for_direct_order_limit_with_alias(
      &tmp_sql, SPIDER_SQL_B_DOT_STR, SPIDER_SQL_B_DOT_LEN)))
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

int spider_oracle_handler::reuse_union_table_and_sql_for_bka()
{
  DBUG_ENTER("spider_oracle_handler::reuse_union_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.length(tmp_sql_pos1);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_insert_for_recovery(
  ulong sql_type,
  int link_idx
) {
  const TABLE *table = spider->get_table();
  SPIDER_SHARE *share = spider->share;
  Field **field;
  uint field_name_length = 0;
  bool add_value = FALSE;
  spider_string *insert_sql;
  DBUG_ENTER("spider_oracle_handler::append_insert_for_recovery");
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
    SPIDER_SQL_INTO_LEN + oracle_share->db_nm_max_length +
    SPIDER_SQL_DOT_LEN + oracle_share->table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  insert_sql->q_append(SPIDER_SQL_INSERT_STR, SPIDER_SQL_INSERT_LEN);
  insert_sql->q_append(SPIDER_SQL_SQL_IGNORE_STR, SPIDER_SQL_SQL_IGNORE_LEN);
  insert_sql->q_append(SPIDER_SQL_INTO_STR, SPIDER_SQL_INTO_LEN);
  oracle_share->append_table_name(insert_sql, spider->conn_link_idx[link_idx]);
  insert_sql->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  for (field = table->field; *field; field++)
  {
    field_name_length =
      oracle_share->column_name_str[(*field)->field_index].length();
    if (insert_sql->reserve(field_name_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    oracle_share->append_column_name(insert_sql, (*field)->field_index);
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
        spider_db_oracle_utility.
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

int spider_oracle_handler::append_update(
  const TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  int error_num;
  spider_string *str = &update_sql;
  DBUG_ENTER("spider_oracle_handler::append_update");
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

int spider_oracle_handler::append_update(
  const TABLE *table,
  my_ptrdiff_t ptr_diff,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.update_sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::append_update");
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

int spider_oracle_handler::append_delete(
  const TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  int error_num;
  spider_string *str = &update_sql;
  DBUG_ENTER("spider_oracle_handler::append_delete");
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

int spider_oracle_handler::append_delete(
  const TABLE *table,
  my_ptrdiff_t ptr_diff,
  int link_idx
) {
  int error_num;
  spider_string *str = &spider->result_list.update_sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::append_delete");
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

int spider_oracle_handler::append_insert_part()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_insert_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_insert(&insert_sql, 0);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_insert(
  spider_string *str,
  int link_idx
) {
  DBUG_ENTER("spider_oracle_handler::append_insert");
  if (str->reserve(SPIDER_SQL_INSERT_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_INSERT_STR, SPIDER_SQL_INSERT_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_update_part()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_update_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_update(&update_sql, 0);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_update(
  spider_string *str,
  int link_idx
) {
  DBUG_ENTER("spider_oracle_handler::append_update");
  if (str->reserve(SPIDER_SQL_UPDATE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_UPDATE_STR, SPIDER_SQL_UPDATE_LEN);
  if (str->reserve(oracle_share->db_nm_max_length +
    SPIDER_SQL_DOT_LEN + oracle_share->table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  table_name_pos = str->length();
  append_table_name_with_adjusting(str, link_idx, SPIDER_SQL_TYPE_UPDATE_SQL);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_delete_part()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_delete_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_delete(&update_sql);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_delete(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_delete");
  if (str->reserve(SPIDER_SQL_DELETE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_DELETE_STR, SPIDER_SQL_DELETE_LEN);
  str->length(str->length() - 1);
  DBUG_RETURN(0);
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
int spider_oracle_handler::append_increment_update_set_part()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_increment_update_set_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_increment_update_set(&update_sql);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_increment_update_set(
  spider_string *str
) {
  uint field_name_length;
  uint roop_count;
  Field *field;
  DBUG_ENTER("spider_oracle_handler::append_increment_update_set");
  if (str->reserve(SPIDER_SQL_SET_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SET_STR, SPIDER_SQL_SET_LEN);
  const SPIDER_HS_STRING_REF *value = hs_upds.ptr();
  for (roop_count = 0; roop_count < hs_upds.size();
    roop_count++)
  {
    DBUG_PRINT("info",("spider value_size[%u]=%zu", roop_count,
      value[roop_count].size()));
#ifndef DBUG_OFF
    char print_buf[MAX_FIELD_WIDTH];
    if (value[roop_count].size() < MAX_FIELD_WIDTH)
    {
      memcpy(print_buf, value[roop_count].begin(), value[roop_count].size());
      print_buf[value[roop_count].size()] = '\0';
      DBUG_PRINT("info",("spider value[%u]=%s", roop_count, print_buf));
    }
#endif
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
      oracle_share->column_name_str[field->field_index].length();

    if (str->reserve(field_name_length * 2 + /* SPIDER_SQL_NAME_QUOTE_LEN */
      4 + SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_HS_INCREMENT_LEN +
      SPIDER_SQL_COMMA_LEN + value[roop_count].size()))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

    oracle_share->append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
    oracle_share->append_column_name(str, field->field_index);
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

int spider_oracle_handler::append_update_set_part()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_update_set_part");
  DBUG_PRINT("info",("spider this=%p", this));
  update_set_pos = update_sql.length();
  error_num = append_update_set(&update_sql);
  where_pos = update_sql.length();
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_update_set(
  spider_string *str
) {
  uint field_name_length;
  SPIDER_SHARE *share = spider->share;
  TABLE *table = spider->get_table();
  Field **fields;
  DBUG_ENTER("spider_oracle_handler::append_update_set");
  if (str->reserve(SPIDER_SQL_SET_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SET_STR, SPIDER_SQL_SET_LEN);
  for (fields = table->field; *fields; fields++)
  {
    if (bitmap_is_set(table->write_set, (*fields)->field_index))
    {
      field_name_length =
        oracle_share->column_name_str[(*fields)->field_index].length();
      if ((*fields)->is_null())
      {
        if (str->reserve(field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */
          2 + SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_NULL_LEN +
          SPIDER_SQL_COMMA_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        oracle_share->append_column_name(str, (*fields)->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
        str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
      } else {
        if (str->reserve(field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */
          2 + SPIDER_SQL_EQUAL_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        oracle_share->append_column_name(str, (*fields)->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
#ifndef DBUG_OFF
        my_bitmap_map *tmp_map = dbug_tmp_use_all_columns(table,
          table->read_set);
#endif
        if (
          spider_db_oracle_utility.
            append_column_value(spider, str, *fields, NULL,
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
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
int spider_oracle_handler::append_direct_update_set_part()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_direct_update_set_part");
  DBUG_PRINT("info",("spider this=%p", this));
  update_set_pos = update_sql.length();
  error_num = append_direct_update_set(&update_sql);
  where_pos = update_sql.length();
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_direct_update_set(
  spider_string *str
) {
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint field_name_length;
  SPIDER_SHARE *share = spider->share;
#ifndef DBUG_OFF
  TABLE *table = spider->get_table();
#endif
#endif
  DBUG_ENTER("spider_oracle_handler::append_direct_update_set");
  if (
    spider->direct_update_kinds == SPIDER_SQL_KIND_SQL &&
    spider->direct_update_fields
  ) {
    if (str->reserve(SPIDER_SQL_SET_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SET_STR, SPIDER_SQL_SET_LEN);
    DBUG_RETURN(append_update_columns(str, NULL, 0));
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
        oracle_share->column_name_str[field->field_index].length();
      if (top_table_field->is_null())
      {
        if (str->reserve(field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */
          2 + SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_NULL_LEN +
          SPIDER_SQL_COMMA_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        oracle_share->append_column_name(str, field->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
        str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
      } else {
        if (str->reserve(field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */
          2 + SPIDER_SQL_EQUAL_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        oracle_share->append_column_name(str, field->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
#ifndef DBUG_OFF
        my_bitmap_map *tmp_map = dbug_tmp_use_all_columns(table,
          table->read_set);
#endif
        if (
          spider_db_oracle_utility.
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

int spider_oracle_handler::append_dup_update_pushdown_part(
  const char *alias,
  uint alias_length
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_dup_update_pushdown_part");
  DBUG_PRINT("info",("spider this=%p", this));
  dup_update_sql.length(0);
  error_num = append_update_columns(&dup_update_sql, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_update_columns_part(
  const char *alias,
  uint alias_length
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_update_columns_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_update_columns(&update_sql, alias, alias_length);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::check_update_columns_part()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::check_update_columns_part");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = append_update_columns(NULL, NULL, 0);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_update_columns(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  List_iterator_fast<Item> fi(*spider->direct_update_fields),
    vi(*spider->direct_update_values);
  Item *field, *value;
  DBUG_ENTER("spider_oracle_handler::append_update_columns");
  while ((field = fi++))
  {
    value = vi++;
    if ((error_num = spider_db_print_item_type(
      (Item *) field, spider, str, alias, alias_length,
      spider_dbton_oracle.dbton_id)))
    {
      if (
        error_num == ER_SPIDER_COND_SKIP_NUM &&
        field->type() == Item::FIELD_ITEM &&
        ((Item_field *) field)->field
      )
        continue;
      DBUG_RETURN(error_num);
    }
    if (str)
    {
      if (str->reserve(SPIDER_SQL_EQUAL_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
    }
    if ((error_num = spider_db_print_item_type(
      (Item *) value, spider, str, alias, alias_length,
      spider_dbton_oracle.dbton_id)))
      DBUG_RETURN(error_num);
    if (str)
    {
      if (str->reserve(SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
  }
  if (str)
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
/*
  error_num = spider_db_append_update_columns(spider, str,
    alias, alias_length, spider_dbton_oracle.dbton_id);
  DBUG_RETURN(error_num);
*/
}
#endif

int spider_oracle_handler::append_select_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_select_part");
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

int spider_oracle_handler::append_select(
  spider_string *str,
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::append_select");
  if (sql_type == SPIDER_SQL_TYPE_HANDLER)
  {
    if (str->reserve(SPIDER_SQL_HANDLER_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_HANDLER_STR, SPIDER_SQL_HANDLER_LEN);
  } else {
    if (str->reserve(SPIDER_SQL_SELECT_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_SELECT_STR, SPIDER_SQL_SELECT_LEN);
    if (spider->result_list.direct_distinct)
    {
      if (str->reserve(SPIDER_SQL_DISTINCT_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_DISTINCT_STR, SPIDER_SQL_DISTINCT_LEN);
    }
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_table_select_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_table_select_part");
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

int spider_oracle_handler::append_table_select(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_table_select");
  table_name_pos = str->length() + oracle_share->table_select_pos;
  if (str->append(*(oracle_share->table_select)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_key_select_part(
  ulong sql_type,
  uint idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_key_select_part");
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

int spider_oracle_handler::append_key_select(
  spider_string *str,
  uint idx
) {
  DBUG_ENTER("spider_oracle_handler::append_key_select");
  table_name_pos = str->length() + oracle_share->key_select_pos[idx];
  if (str->append(oracle_share->key_select[idx]))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_minimum_select_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_minimum_select_part");
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

int spider_oracle_handler::append_minimum_select(
  spider_string *str,
  ulong sql_type
) {
  TABLE *table = spider->get_table();
  Field **field;
  int field_length;
  bool appended = FALSE;
  DBUG_ENTER("spider_oracle_handler::append_minimum_select");
  minimum_select_bitmap_create();
  for (field = table->field; *field; field++)
  {
    if (minimum_select_bit_is_set((*field)->field_index))
    {
/*
      spider_set_bit(minimum_select_bitmap, (*field)->field_index);
*/
      field_length =
        oracle_share->column_name_str[(*field)->field_index].length();
      if (str->reserve(field_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      oracle_share->append_column_name(str, (*field)->field_index);
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

int spider_oracle_handler::append_table_select_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  TABLE *table = spider->get_table();
  Field **field;
  int field_length;
  DBUG_ENTER("spider_oracle_handler::append_table_select_with_alias");
  for (field = table->field; *field; field++)
  {
    field_length =
      oracle_share->column_name_str[(*field)->field_index].length();
    if (str->reserve(alias_length + field_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(alias, alias_length);
    oracle_share->append_column_name(str, (*field)->field_index);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_key_select_with_alias(
  spider_string *str,
  const KEY *key_info,
  const char *alias,
  uint alias_length
) {
  KEY_PART_INFO *key_part;
  Field *field;
  uint part_num;
  int field_length;
  DBUG_ENTER("spider_oracle_handler::append_key_select_with_alias");
  for (key_part = key_info->key_part, part_num = 0;
    part_num < spider_user_defined_key_parts(key_info); key_part++, part_num++)
  {
    field = key_part->field;
    field_length = oracle_share->column_name_str[field->field_index].length();
    if (str->reserve(alias_length + field_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(alias, alias_length);
    oracle_share->append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_minimum_select_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  TABLE *table = spider->get_table();
  Field **field;
  int field_length;
  bool appended = FALSE;
  DBUG_ENTER("spider_oracle_handler::append_minimum_select_with_alias");
  minimum_select_bitmap_create();
  for (field = table->field; *field; field++)
  {
    if (minimum_select_bit_is_set((*field)->field_index))
    {
/*
      spider_set_bit(minimum_select_bitmap, (*field)->field_index);
*/
      field_length =
        oracle_share->column_name_str[(*field)->field_index].length();
      if (str->reserve(alias_length + field_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(alias, alias_length);
      oracle_share->append_column_name(str, (*field)->field_index);
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

int spider_oracle_handler::append_select_columns_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_oracle_handler::append_select_columns_with_alias");
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

int spider_oracle_handler::append_hint_after_table_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_hint_after_table_part");
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

int spider_oracle_handler::append_hint_after_table(
  spider_string *str
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_hint_after_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    oracle_share->key_hint &&
    (error_num = spider_db_append_hint_after_table(spider,
      str, &oracle_share->key_hint[spider->active_index]))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  DBUG_RETURN(0);
}

void spider_oracle_handler::set_where_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::set_where_pos");
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

void spider_oracle_handler::set_where_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::set_where_to_pos");
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

int spider_oracle_handler::check_item_type(
  Item *item
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::check_item_type");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = spider_db_print_item_type(item, spider, NULL, NULL, 0,
    spider_dbton_oracle.dbton_id);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_values_connector_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_values_connector_part");
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

int spider_oracle_handler::append_values_connector(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_values_connector");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN +
    SPIDER_SQL_COMMA_LEN + SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_values_terminator_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_values_terminator_part");
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

int spider_oracle_handler::append_values_terminator(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_values_terminator");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(str->length() -
    SPIDER_SQL_COMMA_LEN - SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_union_table_connector_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_union_table_connector_part");
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

int spider_oracle_handler::append_union_table_connector(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_union_table_connector");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve((SPIDER_SQL_SPACE_LEN * 2) + SPIDER_SQL_UNION_ALL_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
  str->q_append(SPIDER_SQL_UNION_ALL_STR, SPIDER_SQL_UNION_ALL_LEN);
  str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_union_table_terminator_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_union_table_terminator_part");
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

int spider_oracle_handler::append_union_table_terminator(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_union_table_terminator");
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

int spider_oracle_handler::append_key_column_values_part(
  const key_range *start_key,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_key_column_values_part");
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

int spider_oracle_handler::append_key_column_values(
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
  DBUG_ENTER("spider_oracle_handler::append_key_column_values");
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
      if (spider_db_oracle_utility.append_column_value(spider, str, field, ptr,
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

int spider_oracle_handler::append_key_column_values_with_name_part(
  const key_range *start_key,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_key_column_values_with_name_part");
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

int spider_oracle_handler::append_key_column_values_with_name(
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
  DBUG_ENTER("spider_oracle_handler::append_key_column_values_with_name");
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
      if (spider_db_oracle_utility.append_column_value(spider, str, field, ptr,
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

int spider_oracle_handler::append_key_where_part(
  const key_range *start_key,
  const key_range *end_key,
  ulong sql_type
) {
  int error_num;
  spider_string *str, *str_part = NULL, *str_part2 = NULL;
  bool set_order;
  DBUG_ENTER("spider_oracle_handler::append_key_where_part");
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

int spider_oracle_handler::append_key_where(
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  const key_range *start_key,
  const key_range *end_key,
  ulong sql_type,
  bool set_order
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_key_where");
  error_num = spider_db_append_key_where_internal(str, str_part, str_part2,
    start_key, end_key, spider, set_order, sql_type,
    spider_dbton_oracle.dbton_id);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_is_null_part(
  ulong sql_type,
  KEY_PART_INFO *key_part,
  const key_range *key,
  const uchar **ptr,
  bool key_eq,
  bool tgt_final
) {
  int error_num;
  spider_string *str, *str_part = NULL, *str_part2 = NULL;
  DBUG_ENTER("spider_oracle_handler::append_is_null_part");
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

int spider_oracle_handler::append_is_null(
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
  DBUG_ENTER("spider_oracle_handler::append_is_null");
  DBUG_PRINT("info",("spider this=%p", this));
  if (key_part->null_bit)
  {
    if (*(*ptr)++)
    {
      if (sql_type == SPIDER_SQL_TYPE_HANDLER)
      {
        str = str_part;
        if (
          key_eq ||
          key->flag == HA_READ_KEY_EXACT ||
          key->flag == HA_READ_KEY_OR_NEXT
        ) {
          if (str->reserve(SPIDER_SQL_IS_NULL_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_IS_NULL_STR, SPIDER_SQL_IS_NULL_LEN);
        } else {
          str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          ha_next_pos = str->length();
          if (str->reserve(SPIDER_SQL_FIRST_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_FIRST_STR, SPIDER_SQL_FIRST_LEN);
          spider->result_list.ha_read_kind = 1;
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
          oracle_share->column_name_str[key_part->field->field_index].length()))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        oracle_share->append_column_name(str, key_part->field->field_index);
        str->q_append(SPIDER_SQL_IS_NULL_STR, SPIDER_SQL_IS_NULL_LEN);
      } else {
        if (str->reserve(SPIDER_SQL_IS_NOT_NULL_LEN +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
          oracle_share->column_name_str[key_part->field->field_index].length()))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        oracle_share->append_column_name(str, key_part->field->field_index);
        str->q_append(SPIDER_SQL_IS_NOT_NULL_STR, SPIDER_SQL_IS_NOT_NULL_LEN);
      }
      DBUG_RETURN(-1);
    }
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_where_terminator_part(
  ulong sql_type,
  bool set_order,
  int key_count
) {
  int error_num;
  spider_string *str, *str_part = NULL, *str_part2 = NULL;
  DBUG_ENTER("spider_oracle_handler::append_where_terminator_part");
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

int spider_oracle_handler::append_where_terminator(
  ulong sql_type,
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  bool set_order,
  int key_count
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_oracle_handler::append_where_terminator");
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

int spider_oracle_handler::append_match_where_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_match_where_part");
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

int spider_oracle_handler::append_match_where(
  spider_string *str
) {
  int error_num;
  bool first = TRUE;
  st_spider_ft_info *ft_info = spider->ft_first;
  DBUG_ENTER("spider_oracle_handler::append_match_where");
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

int spider_oracle_handler::append_update_where(
  spider_string *str,
  const TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  uint field_name_length;
  Field **field;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_oracle_handler::append_update_where");
  if (str->reserve(SPIDER_SQL_WHERE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
  for (field = table->field; *field; field++)
  {
    if (
      table->s->primary_key == MAX_KEY ||
      bitmap_is_set(table->read_set, (*field)->field_index)
    ) {
      field_name_length =
        oracle_share->column_name_str[(*field)->field_index].length();
      if ((*field)->is_null(ptr_diff))
      {
        if (str->reserve(field_name_length +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
          SPIDER_SQL_IS_NULL_LEN + SPIDER_SQL_AND_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        oracle_share->append_column_name(str, (*field)->field_index);
        str->q_append(SPIDER_SQL_IS_NULL_STR, SPIDER_SQL_IS_NULL_LEN);
      } else {
        if (str->reserve(field_name_length +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
          SPIDER_SQL_EQUAL_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        oracle_share->append_column_name(str, (*field)->field_index);
        str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
        (*field)->move_field_offset(ptr_diff);
        if (
          spider_db_oracle_utility.
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
/*
  str->length(str->length() - SPIDER_SQL_AND_LEN);
*/
  if (str->reserve(SPIDER_SQL_LIMIT1_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_LIMIT1_STR, SPIDER_SQL_LIMIT1_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_condition_part(
  const char *alias,
  uint alias_length,
  ulong sql_type,
  bool test_flg
) {
  int error_num;
  spider_string *str;
  bool start_where = FALSE;
  DBUG_ENTER("spider_oracle_handler::append_condition_part");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      DBUG_PRINT("info",("spider case1 sql_type=%lu", sql_type));
      if (test_flg)
      {
        str = NULL;
      } else {
        str = &sql;
        start_where = ((int) str->length() == where_pos);
      }
      break;
    case SPIDER_SQL_TYPE_TMP_SQL:
      DBUG_PRINT("info",("spider case1 sql_type=%lu", sql_type));
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
      DBUG_PRINT("info",("spider case2 sql_type=%lu", sql_type));
      if (test_flg)
      {
        str = NULL;
      } else {
        str = &update_sql;
        start_where = ((int) str->length() == where_pos);
      }
      break;
    case SPIDER_SQL_TYPE_HANDLER:
      DBUG_PRINT("info",("spider case3 sql_type=%lu", sql_type));
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

        if (sql_part2.length())
        {
          str->append(sql_part2);
          start_where = FALSE;
        }
      }
      break;
    default:
      DBUG_PRINT("info",("spider default sql_type=%lu", sql_type));
      DBUG_RETURN(0);
  }
  error_num = append_condition(str, alias, alias_length, start_where,
    sql_type);
  DBUG_PRINT("info",("spider str=%s", str ? str->c_ptr_safe() : "NULL"));
  DBUG_PRINT("info",("spider length=%u", str ? str->length() : 0));
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_condition(
  spider_string *str,
  const char *alias,
  uint alias_length,
  bool start_where,
  ulong sql_type
) {
  int error_num, restart_pos = 0, start_where_pos;
  SPIDER_CONDITION *tmp_cond = spider->condition;
  DBUG_ENTER("spider_oracle_handler::append_condition");
  DBUG_PRINT("info",("spider str=%p", str));
  DBUG_PRINT("info",("spider alias=%p", alias));
  DBUG_PRINT("info",("spider alias_length=%u", alias_length));
  DBUG_PRINT("info",("spider start_where=%s", start_where ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
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
      spider_dbton_oracle.dbton_id)))
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

int spider_oracle_handler::append_match_against_part(
  ulong sql_type,
  st_spider_ft_info *ft_info,
  const char *alias,
  uint alias_length
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_match_against_part");
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

int spider_oracle_handler::append_match_against(
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
  DBUG_ENTER("spider_oracle_handler::append_match_against");
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
      oracle_share->column_name_str[field->field_index].length();
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
    oracle_share->append_column_name(str, field->field_index);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  if (str->reserve(SPIDER_SQL_AGAINST_LEN + SPIDER_SQL_VALUE_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_AGAINST_STR, SPIDER_SQL_AGAINST_LEN);
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);

  char buf[MAX_FIELD_WIDTH];
  spider_string tmp_str(buf, MAX_FIELD_WIDTH, share->access_charset);
  tmp_str.init_calc_mem(211);
  tmp_str.length(0);
  if (
    tmp_str.append(ft_init_key->ptr(), ft_init_key->length(),
      ft_init_key->charset()) ||
    str->reserve(tmp_str.length() * 2) ||
    spider_db_oracle_utility.append_escaped_util(str, tmp_str.get_str())
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

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

int spider_oracle_handler::append_match_select_part(
  ulong sql_type,
  const char *alias,
  uint alias_length
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_match_select_part");
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

int spider_oracle_handler::append_match_select(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_match_select");
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
int spider_oracle_handler::append_sum_select_part(
  ulong sql_type,
  const char *alias,
  uint alias_length
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_sum_select_part");
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

int spider_oracle_handler::append_sum_select(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  st_select_lex *select_lex;
  DBUG_ENTER("spider_oracle_handler::append_sum_select");
  DBUG_PRINT("info",("spider this=%p", this));
  select_lex = spider_get_select_lex(spider);
  JOIN *join = select_lex->join;
  Item_sum **item_sum_ptr;
  for (item_sum_ptr = join->sum_funcs; *item_sum_ptr; ++item_sum_ptr)
  {
    if ((error_num = spider_db_oracle_utility.open_item_sum_func(*item_sum_ptr,
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

void spider_oracle_handler::set_order_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::set_order_pos");
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

void spider_oracle_handler::set_order_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::set_order_to_pos");
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
int spider_oracle_handler::append_group_by_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_group_by_part");
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

int spider_oracle_handler::append_group_by(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  st_select_lex *select_lex;
  DBUG_ENTER("spider_oracle_handler::append_group_by");
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
        alias, alias_length, spider_dbton_oracle.dbton_id)))
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

int spider_oracle_handler::append_key_order_for_merge_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_key_order_for_merge_with_alias_part");
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

int spider_oracle_handler::append_key_order_for_merge_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  /* sort for index merge */
  TABLE *table = spider->get_table();
  int length;
  Field *field;
  uint key_name_length;
  DBUG_ENTER("spider_oracle_handler::append_key_order_for_merge_with_alias");
  DBUG_PRINT("info",("spider this=%p", this));
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  if (spider->result_list.direct_aggregate)
  {
    int error_num;
    if ((error_num = append_group_by(str, alias, alias_length)))
      DBUG_RETURN(error_num);
  }
#endif
  if (
    spider->result_list.direct_order_limit ||
    spider->result_list.internal_limit < 9223372036854775807LL ||
    spider->result_list.split_read < 9223372036854775807LL ||
    spider->result_list.internal_offset
  ) {
    if (update_rownum_appended || select_rownum_appended)
    {
      if (str->reserve(SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      order_pos = str->length();
      limit_pos = str->length();
      DBUG_RETURN(0);
    }
    sql_part.length(0);
    if (str == &update_sql)
    {
      if (sql_part.reserve(str->length() + SPIDER_SQL_UPDATE_WRAPPER_HEAD_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(str->ptr(), where_pos);
      sql_part.q_append(SPIDER_SQL_UPDATE_WRAPPER_HEAD_STR,
        SPIDER_SQL_UPDATE_WRAPPER_HEAD_LEN);
    } else {
      if (sql_part.reserve(str->length() + SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN +
        SPIDER_SQL_ROW_NUMBER_HEAD_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_HEAD_STR,
        SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN);
      sql_part.q_append(str->ptr(), table_name_pos - SPIDER_SQL_FROM_LEN);
      sql_part.q_append(SPIDER_SQL_ROW_NUMBER_HEAD_STR,
        SPIDER_SQL_ROW_NUMBER_HEAD_LEN);
    }
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
          oracle_share->column_name_str[field->field_index].length();
        if (sql_part.reserve(alias_length + key_name_length +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(alias, alias_length);
        oracle_share->append_column_name(&sql_part, field->field_index);
        sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
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
          oracle_share->column_name_str[(*fieldp)->field_index].length();
        if (sql_part.reserve(alias_length + key_name_length +
          /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(alias, alias_length);
        oracle_share->append_column_name(&sql_part, (*fieldp)->field_index);
        sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      }
    }
    uint pos_diff;
    if (str == &update_sql)
    {
      uint table_name_size = (update_set_pos ? update_set_pos : where_pos) -
        table_name_pos;
      if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_TAIL_LEN +
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - where_pos +
        SPIDER_SQL_FROM_LEN + table_name_size))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(SPIDER_SQL_ROW_NUMBER_TAIL_STR,
        SPIDER_SQL_ROW_NUMBER_TAIL_LEN);
      sql_part.q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
      sql_part.q_append(str->ptr() + table_name_pos, table_name_size);
      pos_diff = sql_part.length() - where_pos;
      sql_part.q_append(str->ptr() + where_pos, str->length() - where_pos);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      update_rownum_appended = TRUE;
    } else {
      if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_TAIL_LEN +
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - table_name_pos +
        SPIDER_SQL_FROM_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(SPIDER_SQL_ROW_NUMBER_TAIL_STR,
        SPIDER_SQL_ROW_NUMBER_TAIL_LEN);
      pos_diff = sql_part.length() + SPIDER_SQL_FROM_LEN - table_name_pos;
      sql_part.q_append(str->ptr() + table_name_pos - SPIDER_SQL_FROM_LEN,
        str->length() - table_name_pos + SPIDER_SQL_FROM_LEN);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      select_rownum_appended = TRUE;
      table_name_pos = table_name_pos + pos_diff;
    }
    if (str->copy(sql_part))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    where_pos = where_pos + pos_diff;
    order_pos = str->length();
    limit_pos = str->length();
    DBUG_RETURN(0);
  }
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
        oracle_share->column_name_str[field->field_index].length();
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
      oracle_share->append_column_name(str, field->field_index);
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
        oracle_share->column_name_str[(*fieldp)->field_index].length();
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
      oracle_share->append_column_name(str, (*fieldp)->field_index);
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

int spider_oracle_handler::append_key_order_for_direct_order_limit_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_key_order_for_direct_order_limit_with_alias_part");
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

int spider_oracle_handler::append_key_order_for_direct_order_limit_with_alias(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  int error_num;
  ORDER *order;
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  DBUG_ENTER("spider_oracle_handler::append_key_order_for_direct_order_limit_with_alias");
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
  if (
    spider->result_list.direct_order_limit ||
    spider->result_list.internal_limit < 9223372036854775807LL ||
    spider->result_list.split_read < 9223372036854775807LL ||
    spider->result_list.internal_offset
  ) {
    if (update_rownum_appended || select_rownum_appended)
    {
      if (str->reserve(SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      order_pos = str->length();
      limit_pos = str->length();
      DBUG_RETURN(0);
    }
    sql_part.length(0);
    if (str == &update_sql)
    {
      if (sql_part.reserve(str->length() + SPIDER_SQL_UPDATE_WRAPPER_HEAD_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(str->ptr(), where_pos);
      sql_part.q_append(SPIDER_SQL_UPDATE_WRAPPER_HEAD_STR,
        SPIDER_SQL_UPDATE_WRAPPER_HEAD_LEN);
    } else {
      if (sql_part.reserve(str->length() + SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN +
        SPIDER_SQL_ROW_NUMBER_HEAD_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_HEAD_STR,
        SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN);
      sql_part.q_append(str->ptr(), table_name_pos - SPIDER_SQL_FROM_LEN);
      sql_part.q_append(SPIDER_SQL_ROW_NUMBER_HEAD_STR,
        SPIDER_SQL_ROW_NUMBER_HEAD_LEN);
    }
    bool all_desc = TRUE;
    if (select_lex->order_list.first)
    {
      for (order = (ORDER *) select_lex->order_list.first; order;
        order = order->next)
      {
        if ((error_num =
          spider_db_print_item_type((*order->item), spider, &sql_part, alias,
            alias_length, spider_dbton_oracle.dbton_id)))
        {
          DBUG_PRINT("info",("spider error=%d", error_num));
          DBUG_RETURN(error_num);
        }
        if (order->asc)
        {
          if (sql_part.reserve(SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          all_desc = FALSE;
        } else {
          if (sql_part.reserve(SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql_part.q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        }
      }
    } else {
      all_desc = FALSE;
    }
    uint pos_diff;
    if (str == &update_sql)
    {
      uint table_name_size = (update_set_pos ? update_set_pos : where_pos) -
        table_name_pos;
      if (all_desc)
      {
        if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN +
          SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - where_pos +
          SPIDER_SQL_FROM_LEN + table_name_size))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_STR,
          SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN);
      } else {
        if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_TAIL_LEN +
          SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - where_pos +
          SPIDER_SQL_FROM_LEN + table_name_size))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_ROW_NUMBER_TAIL_STR,
          SPIDER_SQL_ROW_NUMBER_TAIL_LEN);
      }
      sql_part.q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
      sql_part.q_append(str->ptr() + table_name_pos, table_name_size);
      pos_diff = sql_part.length() - where_pos;
      sql_part.q_append(str->ptr() + where_pos, str->length() - where_pos);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      update_rownum_appended = TRUE;
    } else {
      if (all_desc)
      {
        if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN +
          SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - table_name_pos +
          SPIDER_SQL_FROM_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_STR,
          SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN);
      } else {
        if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_TAIL_LEN +
          SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - table_name_pos +
          SPIDER_SQL_FROM_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_ROW_NUMBER_TAIL_STR,
          SPIDER_SQL_ROW_NUMBER_TAIL_LEN);
      }
      pos_diff = sql_part.length() + SPIDER_SQL_FROM_LEN - table_name_pos;
      sql_part.q_append(str->ptr() + table_name_pos - SPIDER_SQL_FROM_LEN,
        str->length() - table_name_pos + SPIDER_SQL_FROM_LEN);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      select_rownum_appended = TRUE;
      table_name_pos = table_name_pos + pos_diff;
    }
    if (str->copy(sql_part))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    where_pos = where_pos + pos_diff;
    order_pos = str->length();
    limit_pos = str->length();
    DBUG_RETURN(0);
  }
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
          alias_length, spider_dbton_oracle.dbton_id)))
      {
        DBUG_PRINT("info",("spider error=%d", error_num));
        DBUG_RETURN(error_num);
      }
      if (order->asc)
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

int spider_oracle_handler::append_key_order_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_key_order_with_alias_part");
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

int spider_oracle_handler::append_key_order_for_handler(
  spider_string *str,
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_oracle_handler::append_key_order_for_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider ha_next_pos=%d", ha_next_pos));
  DBUG_PRINT("info",("spider ha_where_pos=%d", ha_where_pos));
  str->q_append(alias, alias_length);
  memset((char *) str->ptr() + str->length(), ' ',
    ha_where_pos - ha_next_pos - alias_length);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_key_order_with_alias(
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
  DBUG_ENTER("spider_oracle_handler::append_key_order_with_alias");
  DBUG_PRINT("info",("spider this=%p", this));
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  if (spider->result_list.direct_aggregate)
  {
    int error_num;
    if ((error_num = append_group_by(str, alias, alias_length)))
      DBUG_RETURN(error_num);
  }
#endif
  if (
    spider->result_list.direct_order_limit ||
    spider->result_list.internal_limit < 9223372036854775807LL ||
    spider->result_list.split_read < 9223372036854775807LL ||
    spider->result_list.internal_offset
  ) {
    if (update_rownum_appended || select_rownum_appended)
    {
      if (str->reserve(SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      order_pos = str->length();
      limit_pos = str->length();
      DBUG_RETURN(0);
    }
    sql_part.length(0);
    if (str == &update_sql)
    {
      if (sql_part.reserve(str->length() + SPIDER_SQL_UPDATE_WRAPPER_HEAD_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(str->ptr(), where_pos);
      sql_part.q_append(SPIDER_SQL_UPDATE_WRAPPER_HEAD_STR,
        SPIDER_SQL_UPDATE_WRAPPER_HEAD_LEN);
    } else {
      if (sql_part.reserve(str->length() + SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN +
        SPIDER_SQL_ROW_NUMBER_HEAD_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_HEAD_STR,
        SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN);
      sql_part.q_append(str->ptr(), table_name_pos - SPIDER_SQL_FROM_LEN);
      sql_part.q_append(SPIDER_SQL_ROW_NUMBER_HEAD_STR,
        SPIDER_SQL_ROW_NUMBER_HEAD_LEN);
    }
    if (result_list->sorted == TRUE)
    {
      if (result_list->desc_flg == TRUE)
      {
        for (
          key_part = key_info->key_part + result_list->key_order,
          length = 1;
          length + result_list->key_order <=
            (int) spider_user_defined_key_parts(key_info) &&
          length <= result_list->max_order;
          key_part++,
          length++
        ) {
          field = key_part->field;
          key_name_length =
            oracle_share->column_name_str[field->field_index].length();
          if (key_part->key_part_flag & HA_REVERSE_SORT)
          {
            if (sql_part.reserve(alias_length + key_name_length +
              /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            sql_part.q_append(alias, alias_length);
            oracle_share->append_column_name(&sql_part, field->field_index);
            sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          } else {
            if (sql_part.reserve(alias_length + key_name_length +
              /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
              SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            sql_part.q_append(alias, alias_length);
            oracle_share->append_column_name(&sql_part, field->field_index);
            sql_part.q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
            sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          }
        }
      } else {
        for (
          key_part = key_info->key_part + result_list->key_order,
          length = 1;
          length + result_list->key_order <=
            (int) spider_user_defined_key_parts(key_info) &&
          length <= result_list->max_order;
          key_part++,
          length++
        ) {
          field = key_part->field;
          key_name_length =
            oracle_share->column_name_str[field->field_index].length();
          if (key_part->key_part_flag & HA_REVERSE_SORT)
          {
            if (sql_part.reserve(alias_length + key_name_length +
              /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
              SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            sql_part.q_append(alias, alias_length);
            oracle_share->append_column_name(&sql_part, field->field_index);
            sql_part.q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
            sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          } else {
            if (sql_part.reserve(alias_length + key_name_length +
              /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            sql_part.q_append(alias, alias_length);
            oracle_share->append_column_name(&sql_part, field->field_index);
            sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          }
        }
      }
    }
    uint pos_diff;
    if (str == &update_sql)
    {
      uint table_name_size = (update_set_pos ? update_set_pos : where_pos) -
        table_name_pos;
      if (result_list->sorted == TRUE && result_list->desc_flg == TRUE)
      {
        if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN +
          SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - where_pos +
          SPIDER_SQL_FROM_LEN + table_name_size))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_STR,
          SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN);
      } else {
        if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_TAIL_LEN +
          SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - where_pos +
          SPIDER_SQL_FROM_LEN + table_name_size))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_ROW_NUMBER_TAIL_STR,
          SPIDER_SQL_ROW_NUMBER_TAIL_LEN);
      }
      sql_part.q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
      sql_part.q_append(str->ptr() + table_name_pos,
        table_name_size);
      pos_diff = sql_part.length() - where_pos;
      sql_part.q_append(str->ptr() + where_pos, str->length() - where_pos);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      update_rownum_appended = TRUE;
    } else {
      if (result_list->sorted == TRUE && result_list->desc_flg == TRUE)
      {
        if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN +
          SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - table_name_pos +
          SPIDER_SQL_FROM_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_STR,
          SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN);
      } else {
        if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_TAIL_LEN +
          SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + str->length() - table_name_pos +
          SPIDER_SQL_FROM_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_ROW_NUMBER_TAIL_STR,
          SPIDER_SQL_ROW_NUMBER_TAIL_LEN);
      }
      pos_diff = sql_part.length() + SPIDER_SQL_FROM_LEN - table_name_pos;
      sql_part.q_append(str->ptr() + table_name_pos - SPIDER_SQL_FROM_LEN,
        str->length() - table_name_pos + SPIDER_SQL_FROM_LEN);
      sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
        SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
      select_rownum_appended = TRUE;
      table_name_pos = table_name_pos + pos_diff;
    }
    if (str->copy(sql_part))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    where_pos = where_pos + pos_diff;
    order_pos = str->length();
    limit_pos = str->length();
    DBUG_RETURN(0);
  }
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
          oracle_share->column_name_str[field->field_index].length();
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
          oracle_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
            SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          oracle_share->append_column_name(str, field->field_index);
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
          oracle_share->column_name_str[field->field_index].length();
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
          oracle_share->append_column_name(str, field->field_index);
        } else {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_DESC_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          oracle_share->append_column_name(str, field->field_index);
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
          oracle_share->column_name_str[field->field_index].length();
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
          oracle_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          oracle_share->append_column_name(str, field->field_index);
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
          oracle_share->column_name_str[field->field_index].length();
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
          oracle_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
        } else {
          if (str->reserve(alias_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(alias, alias_length);
          oracle_share->append_column_name(str, field->field_index);
        }
      }
    }
  }
  limit_pos = str->length();
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_limit_part(
  longlong offset,
  longlong limit,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_limit_part");
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
  DBUG_PRINT("info",("spider str=%s", str->c_ptr_safe()));
  DBUG_PRINT("info",("spider length=%u", str->length()));
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::reappend_limit_part(
  longlong offset,
  longlong limit,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::reappend_limit_part");
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

int spider_oracle_handler::append_limit(
  spider_string *str,
  longlong offset,
  longlong limit
) {
  char buf[SPIDER_LONGLONG_LEN + 1];
  uint32 length;
  DBUG_ENTER("spider_oracle_handler::append_limit");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info", ("spider offset=%lld", offset));
  DBUG_PRINT("info", ("spider limit=%lld", limit));
  if (offset || limit < 9223372036854775807LL)
  {
    if ((int) str->length() == where_pos)
    {
      if (offset)
      {
        int error_num;
        if ((error_num = append_key_order_for_direct_order_limit_with_alias(
          str, NULL, 0)))
          DBUG_RETURN(error_num);
      } else {
        if (str->reserve(SPIDER_SQL_WHERE_LEN + SPIDER_SQL_ROWNUM_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
        str->q_append(SPIDER_SQL_ROWNUM_STR, SPIDER_SQL_ROWNUM_LEN);
      }
    }
    if (offset)
    {
      if (str->reserve(SPIDER_SQL_BETWEEN_LEN + SPIDER_SQL_AND_LEN +
        ((SPIDER_LONGLONG_LEN) * 2)))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_BETWEEN_STR, SPIDER_SQL_BETWEEN_LEN);
      length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
        &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, offset + 1);
      str->q_append(buf, length);
      str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
      length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
        &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, limit + offset);
      str->q_append(buf, length);
    } else {
      if (str->reserve(SPIDER_SQL_HS_LTEQUAL_LEN +
        (SPIDER_LONGLONG_LEN)))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_HS_LTEQUAL_STR, SPIDER_SQL_HS_LTEQUAL_LEN);
      length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
        &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, limit);
      str->q_append(buf, length);
    }
    if (update_rownum_appended)
    {
      if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
    }
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_select_lock_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_select_lock_part");
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
  DBUG_PRINT("info",("spider str=%s", str->c_ptr_safe()));
  DBUG_PRINT("info",("spider length=%u", str->length()));
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::append_select_lock(
  spider_string *str
) {
  int lock_mode = spider_conn_lock_mode(spider);
  DBUG_ENTER("spider_oracle_handler::append_select_lock");
  DBUG_PRINT("info",("spider this=%p", this));
  if (select_rownum_appended)
  {
    table_lock_mode = lock_mode;
  } else {
    if (lock_mode == SPIDER_LOCK_MODE_EXCLUSIVE)
    {
      if (str->reserve(SPIDER_SQL_FOR_UPDATE_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_FOR_UPDATE_STR, SPIDER_SQL_FOR_UPDATE_LEN);
    } else if (lock_mode == SPIDER_LOCK_MODE_SHARED)
    {
      if (str->reserve(SPIDER_SQL_FOR_UPDATE_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_FOR_UPDATE_STR, SPIDER_SQL_FOR_UPDATE_LEN);
    }
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_union_all_start_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_union_all_start_part");
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

int spider_oracle_handler::append_union_all_start(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_union_all_start");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_union_all_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_union_all_part");
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

int spider_oracle_handler::append_union_all(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_union_all");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_UNION_ALL_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_UNION_ALL_STR, SPIDER_SQL_UNION_ALL_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_union_all_end_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_union_all_end_part");
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

int spider_oracle_handler::append_union_all_end(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_union_all_end");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(str->length() -
    SPIDER_SQL_UNION_ALL_LEN + SPIDER_SQL_CLOSE_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_multi_range_cnt_part(
  ulong sql_type,
  uint multi_range_cnt,
  bool with_comma
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_multi_range_cnt_part");
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

int spider_oracle_handler::append_multi_range_cnt(
  spider_string *str,
  uint multi_range_cnt,
  bool with_comma
) {
  int range_cnt_length;
  char range_cnt_str[SPIDER_SQL_INT_LEN];
  DBUG_ENTER("spider_oracle_handler::append_multi_range_cnt");
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

int spider_oracle_handler::append_multi_range_cnt_with_name_part(
  ulong sql_type,
  uint multi_range_cnt
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_multi_range_cnt_with_name_part");
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

int spider_oracle_handler::append_multi_range_cnt_with_name(
  spider_string *str,
  uint multi_range_cnt
) {
  int range_cnt_length;
  char range_cnt_str[SPIDER_SQL_INT_LEN];
  DBUG_ENTER("spider_oracle_handler::append_multi_range_cnt_with_name");
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

int spider_oracle_handler::append_open_handler_part(
  ulong sql_type,
  uint handler_id,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_open_handler_part");
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

int spider_oracle_handler::append_open_handler(
  spider_string *str,
  uint handler_id,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_open_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider link_idx=%d", link_idx));
  DBUG_PRINT("info",("spider m_handler_cid=%s",
    spider->m_handler_cid[link_idx]));
  if (str->reserve(SPIDER_SQL_HANDLER_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_HANDLER_STR, SPIDER_SQL_HANDLER_LEN);
  if ((error_num = oracle_share->append_table_name(str,
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

int spider_oracle_handler::append_close_handler_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_close_handler_part");
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

int spider_oracle_handler::append_close_handler(
  spider_string *str,
  int link_idx
) {
  DBUG_ENTER("spider_oracle_handler::append_close_handler");
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

int spider_oracle_handler::append_insert_terminator_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_insert_terminator_part");
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

int spider_oracle_handler::append_insert_terminator(
  spider_string *str
) {
  DBUG_ENTER("spider_oracle_handler::append_insert_terminator");
  DBUG_PRINT("info",("spider this=%p", this));
  if (spider->result_list.insert_dup_update_pushdown)
  {
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

int spider_oracle_handler::append_insert_values_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_insert_values_part");
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

int spider_oracle_handler::append_insert_values(
  spider_string *str
) {
  SPIDER_SHARE *share = spider->share;
  TABLE *table = spider->get_table();
  Field **field;
  bool add_value = FALSE;
  DBUG_ENTER("spider_oracle_handler::append_insert_values");
  DBUG_PRINT("info",("spider this=%p", this));
  nextval_pos = 0;
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
      my_bitmap_map *tmp_map =
        dbug_tmp_use_all_columns(table, table->read_set);
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
        table->next_number_field == *field &&
        !table->auto_increment_field_not_null &&
        !spider->force_auto_increment
      ) {
        nextval_pos = str->length();
        if (str->reserve(oracle_share->nextval_max_length +
          SPIDER_SQL_COMMA_LEN))
        {
#ifndef DBUG_OFF
          dbug_tmp_restore_column_map(table->read_set, tmp_map);
#endif
          str->length(0);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->length(str->length() + oracle_share->nextval_max_length);
      } else if ((*field)->is_null())
      {
        if (str->reserve(SPIDER_SQL_NULL_LEN + SPIDER_SQL_COMMA_LEN))
        {
#ifndef DBUG_OFF
          dbug_tmp_restore_column_map(table->read_set, tmp_map);
#endif
          str->length(0);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
      } else {
        if (
          spider_db_oracle_utility.
            append_column_value(spider, str, *field, NULL,
              share->access_charset) ||
          str->reserve(SPIDER_SQL_COMMA_LEN)
        ) {
#ifndef DBUG_OFF
          dbug_tmp_restore_column_map(table->read_set, tmp_map);
#endif
          str->length(0);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
      }
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(table->read_set, tmp_map);
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

int spider_oracle_handler::append_into_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_into_part");
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

int spider_oracle_handler::append_into(
  spider_string *str
) {
  const TABLE *table = spider->get_table();
  Field **field;
  uint field_name_length = 0;
  DBUG_ENTER("spider_oracle_handler::append_into");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_INTO_LEN + oracle_share->db_nm_max_length +
    SPIDER_SQL_DOT_LEN + oracle_share->table_nm_max_length +
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
        oracle_share->column_name_str[(*field)->field_index].length();
      if (str->reserve(field_name_length +
        /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      oracle_share->append_column_name(str, (*field)->field_index);
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

void spider_oracle_handler::set_insert_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::set_insert_to_pos");
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

int spider_oracle_handler::append_from_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_from_part");
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

int spider_oracle_handler::append_from(
  spider_string *str,
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_oracle_handler::append_from");
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
    if (str->reserve(SPIDER_SQL_FROM_LEN + oracle_share->db_nm_max_length +
      SPIDER_SQL_DOT_LEN + oracle_share->table_nm_max_length +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_OPEN_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
    table_name_pos = str->length();
    append_table_name_with_adjusting(str, link_idx, sql_type);
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_flush_tables_part(
  ulong sql_type,
  int link_idx,
  bool lock
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_flush_tables_part");
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

int spider_oracle_handler::append_flush_tables(
  spider_string *str,
  int link_idx,
  bool lock
) {
  DBUG_ENTER("spider_oracle_handler::append_flush_tables");
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

int spider_oracle_handler::append_optimize_table_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_optimize_table_part");
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

int spider_oracle_handler::append_optimize_table(
  spider_string *str,
  int link_idx
) {
  SPIDER_SHARE *share = spider->share;
  int conn_link_idx = spider->conn_link_idx[link_idx];
  int local_length = spider_param_internal_optimize_local(spider->trx->thd,
    share->internal_optimize_local) * SPIDER_SQL_SQL_LOCAL_LEN;
  DBUG_ENTER("spider_oracle_handler::append_optimize_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_OPTIMIZE_LEN + SPIDER_SQL_SQL_TABLE_LEN +
    local_length +
    oracle_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    oracle_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_OPTIMIZE_STR, SPIDER_SQL_SQL_OPTIMIZE_LEN);
  if (local_length)
    str->q_append(SPIDER_SQL_SQL_LOCAL_STR, SPIDER_SQL_SQL_LOCAL_LEN);
  str->q_append(SPIDER_SQL_SQL_TABLE_STR, SPIDER_SQL_SQL_TABLE_LEN);
  oracle_share->append_table_name(str, conn_link_idx);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_analyze_table_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_analyze_table_part");
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

int spider_oracle_handler::append_analyze_table(
  spider_string *str,
  int link_idx
) {
  SPIDER_SHARE *share = spider->share;
  int conn_link_idx = spider->conn_link_idx[link_idx];
  int local_length = spider_param_internal_optimize_local(spider->trx->thd,
    share->internal_optimize_local) * SPIDER_SQL_SQL_LOCAL_LEN;
  DBUG_ENTER("spider_oracle_handler::append_analyze_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_ANALYZE_LEN + SPIDER_SQL_SQL_TABLE_LEN +
    local_length +
    oracle_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    oracle_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_ANALYZE_STR, SPIDER_SQL_SQL_ANALYZE_LEN);
  if (local_length)
    str->q_append(SPIDER_SQL_SQL_LOCAL_STR, SPIDER_SQL_SQL_LOCAL_LEN);
  str->q_append(SPIDER_SQL_SQL_TABLE_STR, SPIDER_SQL_SQL_TABLE_LEN);
  oracle_share->append_table_name(str, conn_link_idx);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_repair_table_part(
  ulong sql_type,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_repair_table_part");
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

int spider_oracle_handler::append_repair_table(
  spider_string *str,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  SPIDER_SHARE *share = spider->share;
  int conn_link_idx = spider->conn_link_idx[link_idx];
  int local_length = spider_param_internal_optimize_local(spider->trx->thd,
    share->internal_optimize_local) * SPIDER_SQL_SQL_LOCAL_LEN;
  DBUG_ENTER("spider_oracle_handler::append_repair_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_REPAIR_LEN + SPIDER_SQL_SQL_TABLE_LEN +
    local_length +
    oracle_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    oracle_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_REPAIR_STR, SPIDER_SQL_SQL_REPAIR_LEN);
  if (local_length)
    str->q_append(SPIDER_SQL_SQL_LOCAL_STR, SPIDER_SQL_SQL_LOCAL_LEN);
  str->q_append(SPIDER_SQL_SQL_TABLE_STR, SPIDER_SQL_SQL_TABLE_LEN);
  oracle_share->append_table_name(str, conn_link_idx);
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

int spider_oracle_handler::append_check_table_part(
  ulong sql_type,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_check_table_part");
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

int spider_oracle_handler::append_check_table(
  spider_string *str,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int conn_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_oracle_handler::append_check_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_CHECK_TABLE_LEN +
    oracle_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    oracle_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_CHECK_TABLE_STR,
    SPIDER_SQL_SQL_CHECK_TABLE_LEN);
  oracle_share->append_table_name(str, conn_link_idx);
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

int spider_oracle_handler::append_enable_keys_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_enable_keys_part");
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

int spider_oracle_handler::append_enable_keys(
  spider_string *str,
  int link_idx
) {
  int conn_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_oracle_handler::append_enable_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_ALTER_TABLE_LEN +
    oracle_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    oracle_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_SQL_ENABLE_KEYS_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_ALTER_TABLE_STR,
    SPIDER_SQL_SQL_ALTER_TABLE_LEN);
  oracle_share->append_table_name(str, conn_link_idx);
  str->q_append(SPIDER_SQL_SQL_ENABLE_KEYS_STR,
    SPIDER_SQL_SQL_ENABLE_KEYS_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_disable_keys_part(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_disable_keys_part");
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

int spider_oracle_handler::append_disable_keys(
  spider_string *str,
  int link_idx
) {
  int conn_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_oracle_handler::append_disable_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(SPIDER_SQL_SQL_ALTER_TABLE_LEN +
    oracle_share->db_names_str[conn_link_idx].length() +
    SPIDER_SQL_DOT_LEN +
    oracle_share->table_names_str[conn_link_idx].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_SQL_DISABLE_KEYS_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SQL_ALTER_TABLE_STR,
    SPIDER_SQL_SQL_ALTER_TABLE_LEN);
  oracle_share->append_table_name(str, conn_link_idx);
  str->q_append(SPIDER_SQL_SQL_DISABLE_KEYS_STR,
    SPIDER_SQL_SQL_DISABLE_KEYS_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_delete_all_rows_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_delete_all_rows_part");
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

int spider_oracle_handler::append_delete_all_rows(
  spider_string *str,
  ulong sql_type
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_delete_all_rows");
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

int spider_oracle_handler::append_truncate(
  spider_string *str,
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_oracle_handler::append_truncate");
  if (str->reserve(SPIDER_SQL_TRUNCATE_TABLE_LEN +
    oracle_share->db_nm_max_length +
    SPIDER_SQL_DOT_LEN + oracle_share->table_nm_max_length +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_TRUNCATE_TABLE_STR, SPIDER_SQL_TRUNCATE_TABLE_LEN);
  table_name_pos = str->length();
  append_table_name_with_adjusting(str, link_idx, sql_type);
  DBUG_RETURN(0);
}

int spider_oracle_handler::append_explain_select_part(
  key_range *start_key,
  key_range *end_key,
  ulong sql_type,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_oracle_handler::append_explain_select_part");
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

int spider_oracle_handler::append_explain_select(
  spider_string *str,
  key_range *start_key,
  key_range *end_key,
  ulong sql_type,
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::append_explain_select");
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
bool spider_oracle_handler::is_sole_projection_field( uint16 field_index )
{
    // Determine whether the projection list consists solely of the field of interest
    bool            is_field_in_projection_list = FALSE;
    TABLE*          table                       = spider->get_table();
    uint16          projection_field_count      = 0;
    uint16          projection_field_index;
    Field**         field;
    DBUG_ENTER( "spider_oracle_handler::is_sole_projection_field" );

    for ( field = table->field; *field; field++ )
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
            if (field_index == projection_field_index)
            {
                // Field of interest is in the projection list
                is_field_in_projection_list     = TRUE;
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

bool spider_oracle_handler::is_bulk_insert_exec_period(
  bool bulk_end
) {
  DBUG_ENTER("spider_oracle_handler::is_bulk_insert_exec_period");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider insert_sql.length=%u", insert_sql.length()));
  DBUG_PRINT("info",("spider insert_pos=%d", insert_pos));
  DBUG_PRINT("info",("spider insert_sql=%s", insert_sql.c_ptr_safe()));
  if (
/*
    (bulk_end || (int) insert_sql.length() >= spider->bulk_size) &&
*/
    (int) insert_sql.length() > insert_pos
  ) {
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

bool spider_oracle_handler::sql_is_filled_up(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::sql_is_filled_up");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(filled_up);
}

bool spider_oracle_handler::sql_is_empty(
  ulong sql_type
) {
  bool is_empty;
  DBUG_ENTER("spider_oracle_handler::sql_is_empty");
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

bool spider_oracle_handler::support_multi_split_read()
{
  DBUG_ENTER("spider_oracle_handler::support_multi_split_read");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

bool spider_oracle_handler::support_bulk_update()
{
  DBUG_ENTER("spider_oracle_handler::support_bulk_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_oracle_handler::bulk_tmp_table_insert()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::bulk_tmp_table_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = store_sql_to_bulk_tmp_table(&update_sql, upd_tmp_tbl);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::bulk_tmp_table_insert(
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::bulk_tmp_table_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = store_sql_to_bulk_tmp_table(
    &spider->result_list.update_sqls[link_idx],
    spider->result_list.upd_tmp_tbls[link_idx]);
  DBUG_RETURN(error_num);
}

int spider_oracle_handler::bulk_tmp_table_end_bulk_insert()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::bulk_tmp_table_end_bulk_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = upd_tmp_tbl->file->ha_end_bulk_insert()))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::bulk_tmp_table_rnd_init()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::bulk_tmp_table_rnd_init");
  DBUG_PRINT("info",("spider this=%p", this));
  upd_tmp_tbl->file->extra(HA_EXTRA_CACHE);
  if ((error_num = upd_tmp_tbl->file->ha_rnd_init(TRUE)))
  {
    DBUG_RETURN(error_num);
  }
  reading_from_bulk_tmp_table = TRUE;
  DBUG_RETURN(0);
}

int spider_oracle_handler::bulk_tmp_table_rnd_next()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::bulk_tmp_table_rnd_next");
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

int spider_oracle_handler::bulk_tmp_table_rnd_end()
{
  int error_num;
  DBUG_ENTER("spider_oracle_handler::bulk_tmp_table_rnd_end");
  DBUG_PRINT("info",("spider this=%p", this));
  reading_from_bulk_tmp_table = FALSE;
  if ((error_num = upd_tmp_tbl->file->ha_rnd_end()))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

bool spider_oracle_handler::need_copy_for_update(
  int link_idx
) {
  int all_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_oracle_handler::need_copy_for_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(!oracle_share->same_db_table_name ||
    spider->share->link_statuses[all_link_idx] == SPIDER_LINK_STATUS_RECOVERY);
}

bool spider_oracle_handler::bulk_tmp_table_created()
{
  DBUG_ENTER("spider_oracle_handler::bulk_tmp_table_created");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(upd_tmp_tbl);
}

int spider_oracle_handler::mk_bulk_tmp_table_and_bulk_start()
{
  THD *thd = spider->trx->thd;
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_oracle_handler::mk_bulk_tmp_table_and_bulk_start");
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

void spider_oracle_handler::rm_bulk_tmp_table()
{
  DBUG_ENTER("spider_oracle_handler::rm_bulk_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (upd_tmp_tbl)
  {
    spider_rm_sys_tmp_table(spider->trx->thd, upd_tmp_tbl, &upd_tmp_tbl_prm);
    upd_tmp_tbl = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_handler::store_sql_to_bulk_tmp_table(
  spider_string *str,
  TABLE *tmp_table
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::store_sql_to_bulk_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_table->field[0]->set_notnull();
  tmp_table->field[0]->store(str->ptr(), str->length(), str->charset());
  if ((error_num = tmp_table->file->ha_write_row(tmp_table->record[0])))
    DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}

int spider_oracle_handler::restore_sql_from_bulk_tmp_table(
  spider_string *str,
  TABLE *tmp_table
) {
  DBUG_ENTER("spider_oracle_handler::restore_sql_from_bulk_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_table->field[0]->val_str(str->get_str());
  str->mem_calc();
  DBUG_RETURN(0);
}

int spider_oracle_handler::insert_lock_tables_list(
  SPIDER_CONN *conn,
  int link_idx
) {
  spider_db_oracle *db_conn = (spider_db_oracle *) conn->db_conn;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash2 = &link_for_hash[link_idx];
  DBUG_ENTER("spider_oracle_handler::insert_lock_tables_list");
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

int spider_oracle_handler::append_lock_tables_list(
  SPIDER_CONN *conn,
  int link_idx,
  int *appended
) {
  int error_num;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash, *tmp_link_for_hash2;
  int conn_link_idx = spider->conn_link_idx[link_idx];
  spider_db_oracle *db_conn = (spider_db_oracle *) conn->db_conn;
  DBUG_ENTER("spider_oracle_handler::append_lock_tables_list");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_link_for_hash2 = &link_for_hash[link_idx];
  tmp_link_for_hash2->db_table_str =
    &oracle_share->db_table_str[conn_link_idx];
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  tmp_link_for_hash2->db_table_str_hash_value =
    oracle_share->db_table_str_hash_value[conn_link_idx];
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

int spider_oracle_handler::realloc_sql(
  ulong *realloced
) {
  THD *thd = spider->trx->thd;
  st_spider_share *share = spider->share;
  int init_sql_alloc_size =
    spider_param_init_sql_alloc_size(thd, share->init_sql_alloc_size);
  DBUG_ENTER("spider_oracle_handler::realloc_sql");
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

int spider_oracle_handler::reset_sql(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::reset_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & SPIDER_SQL_TYPE_SELECT_SQL)
  {
    table_lock_mode = 0;
    select_rownum_appended = FALSE;
    sql.length(0);
  }
  if (sql_type & SPIDER_SQL_TYPE_INSERT_SQL)
  {
    insert_sql.length(0);
  }
  if (sql_type & (SPIDER_SQL_TYPE_UPDATE_SQL | SPIDER_SQL_TYPE_DELETE_SQL |
    SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
  {
    update_rownum_appended = FALSE;
    update_set_pos = 0;
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
int spider_oracle_handler::reset_keys(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::reset_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_oracle_handler::reset_upds(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::reset_upds");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_upds.clear();
  DBUG_RETURN(0);
}

int spider_oracle_handler::reset_strs(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::reset_strs");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_oracle_handler::reset_strs_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::reset_strs_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_oracle_handler::push_back_upds(
  SPIDER_HS_STRING_REF &info
) {
  int error_num;
  DBUG_ENTER("spider_oracle_handler::push_back_upds");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = hs_upds.push_back(info);
  DBUG_RETURN(error_num);
}
#endif

bool spider_oracle_handler::need_lock_before_set_sql_for_exec(
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::need_lock_before_set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_oracle_handler::set_sql_for_exec(
  ulong sql_type,
  int link_idx
) {
  int error_num;
  uint tmp_pos;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  int all_link_idx = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_oracle_handler::set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & (SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_TMP_SQL))
  {
    if (table_lock_mode)
    {
      spider_string *str = &result_list->insert_sqls[link_idx];
      str->length(0);
      if (str->reserve(SPIDER_SQL_LOCK_TABLE_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_LOCK_TABLE_STR, SPIDER_SQL_LOCK_TABLE_LEN);
      if ((error_num = oracle_share->append_table_name(str, all_link_idx)))
        DBUG_RETURN(error_num);
      if (table_lock_mode == SPIDER_LOCK_MODE_EXCLUSIVE)
      {
        if (str->reserve(SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_STR,
          SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_LEN);
      } else if (table_lock_mode == SPIDER_LOCK_MODE_SHARED)
      {
        if (str->reserve(SPIDER_SQL_LOCK_TABLE_SHARE_MODE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_LOCK_TABLE_SHARE_MODE_STR,
          SPIDER_SQL_LOCK_TABLE_SHARE_MODE_LEN);
      }
      exec_lock_sql = str;
    }

    if (oracle_share->same_db_table_name || link_idx == first_link_idx)
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
        oracle_share->db_names_str[link_idx].charset());
      const char *table_names[2], *table_aliases[2];
      uint table_name_lengths[2], table_alias_lengths[2];
      tgt_table_name_str.init_calc_mem(212);
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
            if ((error_num = spider_db_oracle_utility.append_from_with_alias(
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
    if (oracle_share->same_db_table_name || link_idx == first_link_idx)
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
    if (nextval_pos)
    {
      memcpy((uchar *) exec_insert_sql->ptr() + nextval_pos,
        oracle_share->nextval_str[all_link_idx].ptr(),
        oracle_share->nextval_max_length);
    }
  }
  if (sql_type & SPIDER_SQL_TYPE_BULK_UPDATE_SQL)
  {
    if (reading_from_bulk_tmp_table)
    {
      if (
        oracle_share->same_db_table_name &&
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
        oracle_share->same_db_table_name &&
        share->link_statuses[all_link_idx] != SPIDER_LINK_STATUS_RECOVERY
      ) {
        exec_update_sql = &update_sql;
      } else {
        exec_update_sql = &spider->result_list.update_sqls[link_idx];
      }
    }
    DBUG_PRINT("info",("spider exec_update_sql=%s",
      exec_update_sql->c_ptr_safe()));
  } else if (sql_type &
    (SPIDER_SQL_TYPE_UPDATE_SQL | SPIDER_SQL_TYPE_DELETE_SQL))
  {
    if (oracle_share->same_db_table_name || link_idx == first_link_idx)
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
    DBUG_PRINT("info",("spider exec_update_sql=%s",
      exec_update_sql->c_ptr_safe()));
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
    DBUG_PRINT("info",("spider exec_ha_sql=%s",
      exec_ha_sql->c_ptr_safe()));
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::set_sql_for_exec(
  spider_db_copy_table *tgt_ct,
  ulong sql_type
) {
  spider_oracle_copy_table *oracle_ct = (spider_oracle_copy_table *) tgt_ct;
  DBUG_ENTER("spider_oracle_handler::set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_INSERT_SQL:
      exec_insert_sql = &oracle_ct->sql;
      break;
    default:
      DBUG_ASSERT(0);
      break;
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::execute_sql(
  ulong sql_type,
  SPIDER_CONN *conn,
  int quick_mode,
  int *need_mon
) {
  spider_string *tgt_sql;
  uint tgt_length;
  DBUG_ENTER("spider_oracle_handler::execute_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_SQL:
      DBUG_PRINT("info",("spider SPIDER_SQL_TYPE_SELECT_SQL"));
      tgt_sql = exec_sql;
      tgt_length = tgt_sql->length();
      if (table_lock_mode)
      {
        DBUG_PRINT("info",("spider table_lock_mode=%d", table_lock_mode));
        spider_db_oracle *db_conn = (spider_db_oracle *) conn->db_conn;
        db_conn->table_lock_mode = table_lock_mode;
        db_conn->exec_lock_sql = exec_lock_sql;
        table_lock_mode = 0;
      }
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

int spider_oracle_handler::reset()
{
  DBUG_ENTER("spider_oracle_handler::reset");
  DBUG_PRINT("info",("spider this=%p", this));
  update_sql.length(0);
  DBUG_RETURN(0);
}

int spider_oracle_handler::sts_mode_exchange(
  int sts_mode
) {
  DBUG_ENTER("spider_oracle_handler::sts_mode_exchange");
  DBUG_PRINT("info",("spider sts_mode=%d", sts_mode));
  DBUG_RETURN(1);
}

int spider_oracle_handler::show_table_status(
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
  DBUG_ENTER("spider_oracle_handler::show_table_status");
  DBUG_PRINT("info",("spider sts_mode=%d", sts_mode));
  if (
    (flag & HA_STATUS_AUTO) &&
    (error_num = show_autoinc(link_idx))
  ) {
    DBUG_RETURN(error_num);
  }

  if (sts_mode == 1)
  {
    if (!share->records)
      share->records = 10000;
    share->mean_rec_length = 65535;
    share->data_file_length = 65535;
    share->max_data_file_length = 65535;
    share->index_file_length = 65535;
    share->create_time = (time_t) 0;
    share->update_time = (time_t) 0;
    share->check_time = (time_t) 0;
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
          oracle_share->show_table_status[1 + pos].ptr(),
          oracle_share->show_table_status[1 + pos].length(),
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
          oracle_share->show_table_status[1 + pos].ptr(),
          oracle_share->show_table_status[1 + pos].length(),
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
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      if (error_num || (error_num = spider_db_errorno(conn)))
        DBUG_RETURN(error_num);
      else
        DBUG_RETURN(ER_QUERY_ON_FOREIGN_DATA_SOURCE);
    }
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
      DBUG_RETURN(error_num);
  }
  if (auto_increment_value > share->lgtm_tblhnd_share->auto_increment_value)
  {
    share->lgtm_tblhnd_share->auto_increment_value = auto_increment_value;
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      share->lgtm_tblhnd_share->auto_increment_value));
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::crd_mode_exchange(
  int crd_mode
) {
  DBUG_ENTER("spider_oracle_handler::crd_mode_exchange");
  DBUG_PRINT("info",("spider crd_mode=%d", crd_mode));
  DBUG_RETURN(1);
}

int spider_oracle_handler::show_index(
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
  DBUG_ENTER("spider_oracle_handler::show_index");
  DBUG_PRINT("info",("spider crd_mode=%d", crd_mode));
  if (crd_mode == 1)
  {
    for (roop_count = 0, tmp_cardinality = share->cardinality;
      roop_count < (int) table->s->fields;
      roop_count++, tmp_cardinality++)
    {
      if (!spider_bit_is_set(share->cardinality_upd, roop_count))
      {
        DBUG_PRINT("info",
          ("spider init column cardinality id=%d", roop_count));
        *tmp_cardinality = 1;
      }
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
          oracle_share->show_index[1 + pos].ptr(),
          oracle_share->show_index[1 + pos].length(),
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
          oracle_share->show_index[1 + pos].ptr(),
          oracle_share->show_index[1 + pos].length(),
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
          ("spider init column cardinality id=%d", roop_count));
        *tmp_cardinality = 1;
      }
    }
    if (res)
    {
      res->free_result();
      delete res;
    }
    if (error_num)
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::show_records(
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  SPIDER_DB_RESULT *res;
  SPIDER_SHARE *share = spider->share;
  uint pos = spider->conn_link_idx[link_idx];
  DBUG_ENTER("spider_oracle_handler::show_records");
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
        oracle_share->show_records[pos].ptr(),
        oracle_share->show_records[pos].length(),
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
        oracle_share->show_records[pos].ptr(),
        oracle_share->show_records[pos].length(),
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
    else if (error_num || (error_num = spider_db_errorno(conn)))
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

int spider_oracle_handler::show_autoinc(
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  SPIDER_DB_RESULT *res;
  SPIDER_SHARE *share = spider->share;
  uint pos = spider->conn_link_idx[link_idx];
  ulonglong auto_increment_value;
  DBUG_ENTER("spider_oracle_handler::show_autoinc");
  if (!oracle_share->show_autoinc)
    DBUG_RETURN(0);

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
        oracle_share->show_autoinc[pos].ptr(),
        oracle_share->show_autoinc[pos].length(),
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
        oracle_share->show_records[pos].ptr(),
        oracle_share->show_records[pos].length(),
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
    auto_increment_value
  );
  res->free_result();
  delete res;
  if (error_num)
  {
    DBUG_PRINT("info", ("spider error_num=%d 7", error_num));
    DBUG_RETURN(error_num);
  }
  if (auto_increment_value >=
    share->lgtm_tblhnd_share->auto_increment_value)
  {
    share->lgtm_tblhnd_share->auto_increment_value =
      auto_increment_value + 1;
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      share->lgtm_tblhnd_share->auto_increment_value));
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::show_last_insert_id(
  int link_idx,
  ulonglong &last_insert_id
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  SPIDER_DB_RESULT *res;
  uint pos = spider->conn_link_idx[link_idx];
  spider_db_oracle *db_oracle = (spider_db_oracle *) conn->db_conn;
  DBUG_ENTER("spider_oracle_handler::show_last_insert_id");
  if (!oracle_share->show_last_insert_id)
  {
    DBUG_ASSERT(0);
    last_insert_id = 0;
    db_oracle->stored_last_insert_id = 0;
    DBUG_RETURN(0);
  }

  if (
    spider_db_query(
      conn,
      oracle_share->show_last_insert_id[pos].ptr(),
      oracle_share->show_last_insert_id[pos].length(),
      -1,
      &spider->need_mons[link_idx]) &&
    (error_num = spider_db_errorno(conn))
  ) {
    DBUG_PRINT("info", ("spider error_num=%d 4", error_num));
    DBUG_RETURN(error_num);
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
      DBUG_PRINT("info", ("spider error_num=%d 5", error_num));
      DBUG_RETURN(error_num);
    } else {
      DBUG_PRINT("info", ("spider error_num=%d 6",
        ER_QUERY_ON_FOREIGN_DATA_SOURCE));
      DBUG_RETURN(ER_QUERY_ON_FOREIGN_DATA_SOURCE);
    }
  }
  error_num = res->fetch_table_records(
    1,
    last_insert_id
  );
  res->free_result();
  delete res;
  if (error_num)
  {
    DBUG_PRINT("info", ("spider error_num=%d 7", error_num));
    DBUG_RETURN(error_num);
  }
  db_oracle->stored_last_insert_id = last_insert_id;
  DBUG_RETURN(0);
}

ha_rows spider_oracle_handler::explain_select(
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
  DBUG_ENTER("spider_oracle_handler::explain_select");
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

int spider_oracle_handler::lock_tables(
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  spider_string *str = &sql;
  DBUG_ENTER("spider_oracle_handler::lock_tables");
  do {
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
  } while (str->length());
  DBUG_RETURN(0);
}

int spider_oracle_handler::unlock_tables(
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  DBUG_ENTER("spider_oracle_handler::unlock_tables");
  if (conn->table_locked)
  {
    if ((error_num = conn->db_conn->commit(&spider->need_mons[link_idx])))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_oracle_handler::disable_keys(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::disable_keys");
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

int spider_oracle_handler::enable_keys(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::enable_keys");
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

int spider_oracle_handler::check_table(
  SPIDER_CONN *conn,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::check_table");
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

int spider_oracle_handler::repair_table(
  SPIDER_CONN *conn,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::repair_table");
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

int spider_oracle_handler::analyze_table(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::analyze_table");
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

int spider_oracle_handler::optimize_table(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::optimize_table");
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

int spider_oracle_handler::flush_tables(
  SPIDER_CONN *conn,
  int link_idx,
  bool lock
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_string *str = &spider->result_list.sqls[link_idx];
  DBUG_ENTER("spider_oracle_handler::flush_tables");
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

int spider_oracle_handler::flush_logs(
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_oracle_handler::flush_logs");
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

int spider_oracle_handler::insert_opened_handler(
  SPIDER_CONN *conn,
  int link_idx
) {
  spider_db_oracle *db_conn = (spider_db_oracle *) conn->db_conn;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash = &link_for_hash[link_idx];
  DBUG_ASSERT(tmp_link_for_hash->spider == spider);
  DBUG_ASSERT(tmp_link_for_hash->link_idx == link_idx);
  uint old_elements = db_conn->handler_open_array.max_element;
  DBUG_ENTER("spider_oracle_handler::insert_opened_handler");
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

int spider_oracle_handler::delete_opened_handler(
  SPIDER_CONN *conn,
  int link_idx
) {
  spider_db_oracle *db_conn = (spider_db_oracle *) conn->db_conn;
  uint roop_count, elements = db_conn->handler_open_array.elements;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash;
  DBUG_ENTER("spider_oracle_handler::delete_opened_handler");
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

int spider_oracle_handler::sync_from_clone_source(
  spider_db_handler *dbton_hdl
) {
  DBUG_ENTER("spider_oracle_handler::sync_from_clone_source");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

bool spider_oracle_handler::support_use_handler(
  int use_handler
) {
  DBUG_ENTER("spider_oracle_handler::support_use_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

void spider_oracle_handler::minimum_select_bitmap_create()
{
  TABLE *table = spider->get_table();
  Field **field_p;
  DBUG_ENTER("spider_oracle_handler::minimum_select_bitmap_create");
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
  for (field_p = table->field; *field_p; field_p++)
  {
    uint field_index = (*field_p)->field_index;
    if (
      spider_bit_is_set(spider->searched_bitmap, field_index) |
      bitmap_is_set(table->read_set, field_index) |
      bitmap_is_set(table->write_set, field_index)
    ) {
      spider_set_bit(minimum_select_bitmap, field_index);
    }
  }
  DBUG_VOID_RETURN;
}

bool spider_oracle_handler::minimum_select_bit_is_set(
  uint field_index
) {
  DBUG_ENTER("spider_oracle_handler::minimum_select_bit_is_set");
  DBUG_PRINT("info",("spider field_index=%u", field_index));
  DBUG_PRINT("info",("spider minimum_select_bitmap=%s",
    spider_bit_is_set(minimum_select_bitmap, field_index) ?
      "TRUE" : "FALSE"));
  DBUG_RETURN(spider_bit_is_set(minimum_select_bitmap, field_index));
}

void spider_oracle_handler::copy_minimum_select_bitmap(
  uchar *bitmap
) {
  int roop_count;
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_oracle_handler::copy_minimum_select_bitmap");
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

int spider_oracle_handler::init_union_table_name_pos()
{
  DBUG_ENTER("spider_oracle_handler::init_union_table_name_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!union_table_name_pos_first)
  {
    if (!spider_bulk_malloc(spider_current_trx, 238, MYF(MY_WME),
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

int spider_oracle_handler::set_union_table_name_pos()
{
  DBUG_ENTER("spider_oracle_handler::set_union_table_name_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  if (union_table_name_pos_current->tgt_num >= SPIDER_INT_HLD_TGT_SIZE)
  {
    if (!union_table_name_pos_current->next)
    {
      if (!spider_bulk_malloc(spider_current_trx, 239, MYF(MY_WME),
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

int spider_oracle_handler::reset_union_table_name(
  spider_string *str,
  int link_idx,
  ulong sql_type
) {
  DBUG_ENTER("spider_oracle_handler::reset_union_table_name");
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

spider_oracle_copy_table::spider_oracle_copy_table(
  spider_oracle_share *db_share
) : spider_db_copy_table(
  db_share
),
  oracle_share(db_share),
  pos(0),
  table_name_pos(0),
  pos_diff(0),
  table_lock_mode(0),
  select_rownum_appended(FALSE),
  first_str(NULL),
  current_str(NULL)
{
  DBUG_ENTER("spider_oracle_copy_table::spider_oracle_copy_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_oracle_copy_table::~spider_oracle_copy_table()
{
  DBUG_ENTER("spider_oracle_copy_table::~spider_oracle_copy_table");
  DBUG_PRINT("info",("spider this=%p", this));
  while (first_str)
  {
    current_str = first_str;
    first_str = first_str->next;
    delete [] current_str;
  }
  DBUG_VOID_RETURN;
}

int spider_oracle_copy_table::init()
{
  DBUG_ENTER("spider_oracle_copy_table::init");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.init_calc_mem(213);
  sql_part.init_calc_mem(215);
  DBUG_RETURN(0);
}

void spider_oracle_copy_table::set_sql_charset(
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_oracle_copy_table::set_sql_charset");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.set_charset(cs);
  DBUG_VOID_RETURN;
}

int spider_oracle_copy_table::append_select_str()
{
  DBUG_ENTER("spider_oracle_copy_table::append_select_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_SELECT_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_SELECT_STR, SPIDER_SQL_SELECT_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_insert_str(
  int insert_flg
) {
  DBUG_ENTER("spider_oracle_copy_table::append_insert_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_INSERT_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_INSERT_STR, SPIDER_SQL_INSERT_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_table_columns(
  TABLE_SHARE *table_share
) {
  int error_num;
  Field **field;
  DBUG_ENTER("spider_oracle_copy_table::append_table_columns");
  DBUG_PRINT("info",("spider this=%p", this));
  for (field = table_share->field; *field; field++)
  {
    if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    if ((error_num = spider_db_append_name_with_quote_str(&sql,
      (char *) (*field)->field_name, spider_dbton_oracle.dbton_id)))
      DBUG_RETURN(error_num);
    if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
    sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  sql.length(sql.length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_from_str()
{
  DBUG_ENTER("spider_oracle_copy_table::append_from_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_FROM_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_table_name(
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_oracle_copy_table::append_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  table_name_pos = sql.length();
  error_num = oracle_share->append_table_name(&sql, link_idx);
  store_link_idx = link_idx;
  DBUG_RETURN(error_num);
}

void spider_oracle_copy_table::set_sql_pos()
{
  DBUG_ENTER("spider_oracle_copy_table::set_sql_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  pos = sql.length();
  DBUG_VOID_RETURN;
}

void spider_oracle_copy_table::set_sql_to_pos()
{
  DBUG_ENTER("spider_oracle_copy_table::set_sql_to_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  sql.length(pos);
  DBUG_VOID_RETURN;
}

int spider_oracle_copy_table::append_copy_where(
  spider_db_copy_table *source_ct,
  KEY *key_info,
  ulong *last_row_pos,
  ulong *last_lengths
) {
  int error_num, roop_count, roop_count2;
  DBUG_ENTER("spider_oracle_copy_table::append_copy_where");
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

int spider_oracle_copy_table::append_key_order_str(
  KEY *key_info,
  int start_pos,
  bool desc_flg
) {
  int length, error_num;
  KEY_PART_INFO *key_part;
  Field *field;
  DBUG_ENTER("spider_oracle_copy_table::append_key_order_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (select_rownum_appended)
  {
    if (sql.reserve(SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql.q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
      SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);
    DBUG_RETURN(0);
  }
  sql_part.length(0);
  if (sql_part.reserve(sql.length() + SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN +
    SPIDER_SQL_ROW_NUMBER_HEAD_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_HEAD_STR,
    SPIDER_SQL_SELECT_WRAPPER_HEAD_LEN);
  sql_part.q_append(sql.ptr(), table_name_pos - SPIDER_SQL_FROM_LEN);
  sql_part.q_append(SPIDER_SQL_ROW_NUMBER_HEAD_STR,
    SPIDER_SQL_ROW_NUMBER_HEAD_LEN);
  if ((int) spider_user_defined_key_parts(key_info) > start_pos)
  {
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
        if (sql_part.reserve(SPIDER_SQL_NAME_QUOTE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_NAME_QUOTE_STR,
          SPIDER_SQL_NAME_QUOTE_LEN);
        if ((error_num = spider_db_append_name_with_quote_str(&sql_part,
          (char *) field->field_name, spider_dbton_oracle.dbton_id)))
          DBUG_RETURN(error_num);
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (sql_part.reserve(SPIDER_SQL_NAME_QUOTE_LEN +
            SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql_part.q_append(SPIDER_SQL_NAME_QUOTE_STR,
            SPIDER_SQL_NAME_QUOTE_LEN);
          sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (sql_part.reserve(SPIDER_SQL_NAME_QUOTE_LEN +
            SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql_part.q_append(SPIDER_SQL_NAME_QUOTE_STR,
            SPIDER_SQL_NAME_QUOTE_LEN);
          sql_part.q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
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
        if (sql_part.reserve(SPIDER_SQL_NAME_QUOTE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        sql_part.q_append(SPIDER_SQL_NAME_QUOTE_STR,
          SPIDER_SQL_NAME_QUOTE_LEN);
        if ((error_num = spider_db_append_name_with_quote_str(&sql_part,
          (char *) field->field_name, spider_dbton_oracle.dbton_id)))
          DBUG_RETURN(error_num);
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (sql_part.reserve(SPIDER_SQL_NAME_QUOTE_LEN +
            SPIDER_SQL_DESC_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql_part.q_append(SPIDER_SQL_NAME_QUOTE_STR,
            SPIDER_SQL_NAME_QUOTE_LEN);
          sql_part.q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (sql_part.reserve(SPIDER_SQL_NAME_QUOTE_LEN +
            SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql_part.q_append(SPIDER_SQL_NAME_QUOTE_STR,
            SPIDER_SQL_NAME_QUOTE_LEN);
          sql_part.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        }
      }
    }
  }
  if (desc_flg == TRUE)
  {
    if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN +
      SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + sql.length() - table_name_pos +
      SPIDER_SQL_FROM_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql_part.q_append(SPIDER_SQL_ROW_NUMBER_DESC_TAIL_STR,
      SPIDER_SQL_ROW_NUMBER_DESC_TAIL_LEN);
  } else {
    if (sql_part.reserve(SPIDER_SQL_ROW_NUMBER_TAIL_LEN +
      SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN + sql.length() - table_name_pos +
      SPIDER_SQL_FROM_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql_part.q_append(SPIDER_SQL_ROW_NUMBER_TAIL_STR,
      SPIDER_SQL_ROW_NUMBER_TAIL_LEN);
  }
  pos_diff = sql_part.length() + SPIDER_SQL_FROM_LEN - table_name_pos;
  sql_part.q_append(sql.ptr() + table_name_pos - SPIDER_SQL_FROM_LEN,
    sql.length() - table_name_pos + SPIDER_SQL_FROM_LEN);
  sql_part.q_append(SPIDER_SQL_SELECT_WRAPPER_TAIL_STR,
    SPIDER_SQL_SELECT_WRAPPER_TAIL_LEN);

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
        sql.q_append(SPIDER_SQL_NAME_QUOTE_STR,
          SPIDER_SQL_NAME_QUOTE_LEN);
        if ((error_num = spider_db_append_name_with_quote_str(&sql,
          (char *) field->field_name, spider_dbton_oracle.dbton_id)))
          DBUG_RETURN(error_num);
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql.q_append(SPIDER_SQL_NAME_QUOTE_STR,
            SPIDER_SQL_NAME_QUOTE_LEN);
          sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_DESC_LEN +
            SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql.q_append(SPIDER_SQL_NAME_QUOTE_STR,
            SPIDER_SQL_NAME_QUOTE_LEN);
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
        sql.q_append(SPIDER_SQL_NAME_QUOTE_STR,
          SPIDER_SQL_NAME_QUOTE_LEN);
        if ((error_num = spider_db_append_name_with_quote_str(&sql,
          (char *) field->field_name, spider_dbton_oracle.dbton_id)))
          DBUG_RETURN(error_num);
        if (key_part->key_part_flag & HA_REVERSE_SORT)
        {
          if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_DESC_LEN +
            SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql.q_append(SPIDER_SQL_NAME_QUOTE_STR,
            SPIDER_SQL_NAME_QUOTE_LEN);
          sql.q_append(SPIDER_SQL_DESC_STR, SPIDER_SQL_DESC_LEN);
          sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        } else {
          if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN + SPIDER_SQL_COMMA_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          sql.q_append(SPIDER_SQL_NAME_QUOTE_STR,
            SPIDER_SQL_NAME_QUOTE_LEN);
          sql.q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
        }
      }
    }
    sql.length(sql.length() - SPIDER_SQL_COMMA_LEN);
  }
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_limit(
  longlong offset,
  longlong limit
) {
  char buf[SPIDER_LONGLONG_LEN + 1];
  uint32 length;
  DBUG_ENTER("spider_oracle_copy_table::append_limit");
  DBUG_PRINT("info",("spider this=%p", this));
  if (offset || limit < 9223372036854775807LL)
  {
    if (!select_rownum_appended)
    {
      select_rownum_appended = TRUE;
      table_name_pos = table_name_pos + pos_diff;
      if (sql.copy(sql_part))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      pos = pos + pos_diff;
    }
    if (offset)
    {
      if (sql.reserve(SPIDER_SQL_BETWEEN_LEN + SPIDER_SQL_AND_LEN +
        ((SPIDER_LONGLONG_LEN) * 2)))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql.q_append(SPIDER_SQL_BETWEEN_STR, SPIDER_SQL_BETWEEN_LEN);
      length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
        &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, offset);
      sql.q_append(buf, length);
      sql.q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
      length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
        &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, limit);
      sql.q_append(buf, length);
    } else {
      if (sql.reserve(SPIDER_SQL_HS_LTEQUAL_LEN +
        (SPIDER_LONGLONG_LEN)))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql.q_append(SPIDER_SQL_HS_LTEQUAL_STR, SPIDER_SQL_HS_LTEQUAL_LEN);
      length = (uint32) (my_charset_bin.cset->longlong10_to_str)(
        &my_charset_bin, buf, SPIDER_LONGLONG_LEN + 1, -10, limit);
      sql.q_append(buf, length);
    }
  }
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_into_str()
{
  DBUG_ENTER("spider_oracle_copy_table::append_into_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_INTO_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_INTO_STR, SPIDER_SQL_INTO_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_open_paren_str()
{
  DBUG_ENTER("spider_oracle_copy_table::append_open_paren_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_values_str()
{
  DBUG_ENTER("spider_oracle_copy_table::append_values_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_VALUES_LEN +
    SPIDER_SQL_OPEN_PAREN_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  sql.q_append(SPIDER_SQL_VALUES_STR, SPIDER_SQL_VALUES_LEN);
  sql.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_select_lock_str(
  int lock_mode
) {
  DBUG_ENTER("spider_oracle_copy_table::append_select_lock_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (select_rownum_appended)
  {
    int error_num;
    table_lock_mode = lock_mode;
    sql_part.length(0);
    if (sql_part.reserve(SPIDER_SQL_LOCK_TABLE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    sql_part.q_append(SPIDER_SQL_LOCK_TABLE_STR, SPIDER_SQL_LOCK_TABLE_LEN);
    if ((error_num = oracle_share->append_table_name(&sql_part,
      store_link_idx)))
      DBUG_RETURN(error_num);
    if (lock_mode == SPIDER_LOCK_MODE_EXCLUSIVE)
    {
      if (sql_part.reserve(SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_STR,
        SPIDER_SQL_LOCK_TABLE_EXCLUSIVE_MODE_LEN);
    } else if (lock_mode == SPIDER_LOCK_MODE_SHARED)
    {
      if (sql_part.reserve(SPIDER_SQL_LOCK_TABLE_SHARE_MODE_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql_part.q_append(SPIDER_SQL_LOCK_TABLE_SHARE_MODE_STR,
        SPIDER_SQL_LOCK_TABLE_SHARE_MODE_LEN);
    }
  } else {
    if (lock_mode == SPIDER_LOCK_MODE_EXCLUSIVE)
    {
      if (sql.reserve(SPIDER_SQL_FOR_UPDATE_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql.q_append(SPIDER_SQL_FOR_UPDATE_STR, SPIDER_SQL_FOR_UPDATE_LEN);
    } else if (lock_mode == SPIDER_LOCK_MODE_SHARED)
    {
      if (sql.reserve(SPIDER_SQL_FOR_UPDATE_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      sql.q_append(SPIDER_SQL_FOR_UPDATE_STR, SPIDER_SQL_FOR_UPDATE_LEN);
    }
  }
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::exec_query(
  SPIDER_CONN *conn,
  int quick_mode,
  int *need_mon
) {
  int error_num = 0;
  DBUG_ENTER("spider_oracle_copy_table::exec_query");
  DBUG_PRINT("info",("spider this=%p", this));
  if (current_str)
  {
    spider_string *tmp_str = first_str;
    while (tmp_str && tmp_str != current_str)
    {
      if (
        (error_num = spider_db_query(conn, tmp_str->ptr(), tmp_str->length(),
          quick_mode, need_mon)) &&
        error_num != HA_ERR_FOUND_DUPP_KEY
      ) {
        break;
      }
      tmp_str = tmp_str->next;
    }
    if (tmp_str == current_str)
    {
      error_num = spider_db_query(conn, tmp_str->ptr(), tmp_str->length(),
        quick_mode, need_mon);
    }
    if (error_num == HA_ERR_FOUND_DUPP_KEY)
      error_num = 0;
    current_str = NULL;
  } else {
    if (table_lock_mode)
    {
      DBUG_PRINT("info",("spider table_lock_mode=%d", table_lock_mode));
      spider_db_oracle *db_conn = (spider_db_oracle *) conn->db_conn;
      db_conn->table_lock_mode = table_lock_mode;
      db_conn->exec_lock_sql = &sql_part;
      table_lock_mode = 0;
    }
    error_num = spider_db_query(conn, sql.ptr(), sql.length(), quick_mode,
      need_mon);
  }
  DBUG_RETURN(error_num);
}

int spider_oracle_copy_table::copy_key_row(
  spider_db_copy_table *source_ct,
  Field *field,
  ulong *row_pos,
  ulong *length,
  const char *joint_str,
  const int joint_length
) {
  int error_num;
  spider_string *source_str = &((spider_oracle_copy_table *) source_ct)->sql;
  DBUG_ENTER("spider_oracle_copy_table::copy_key_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(SPIDER_SQL_NAME_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  sql.q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  if ((error_num = spider_db_append_name_with_quote_str(&sql,
    (char *) field->field_name, spider_dbton_oracle.dbton_id)))
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

int spider_oracle_copy_table::copy_row(
  Field *field,
  SPIDER_DB_ROW *row
) {
  int error_num;
  DBUG_ENTER("spider_oracle_copy_table::copy_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (row->is_null())
  {
    DBUG_PRINT("info",("spider column is null"));
    if (current_str->reserve(SPIDER_SQL_NULL_LEN + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    current_str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
  } else if (field->str_needs_quotes())
  {
    DBUG_PRINT("info",("spider str_needs_quotes"));
    if (current_str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    current_str->q_append(SPIDER_SQL_VALUE_QUOTE_STR,
      SPIDER_SQL_VALUE_QUOTE_LEN);
    if ((error_num = row->append_escaped_to_str(current_str,
      spider_dbton_oracle.dbton_id)))
      DBUG_RETURN(error_num);
    if (current_str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN +
      SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    current_str->q_append(SPIDER_SQL_VALUE_QUOTE_STR,
      SPIDER_SQL_VALUE_QUOTE_LEN);
  } else {
    DBUG_PRINT("info",("spider without_quotes"));
    if ((error_num = row->append_to_str(current_str)))
      DBUG_RETURN(error_num);
    if (current_str->reserve(SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  current_str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::copy_rows(
  TABLE *table,
  SPIDER_DB_ROW *row,
  ulong **last_row_pos,
  ulong **last_lengths
) {
  int error_num;
  Field **field;
  ulong *lengths2, *row_pos2;
  DBUG_ENTER("spider_oracle_copy_table::copy_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!current_str)
  {
    if (!first_str)
    {
      if (!(first_str = new spider_string[1]))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      first_str->init_calc_mem(216);
      first_str->set_charset(sql.charset());
      if (first_str->reserve(sql.length()))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      first_str->q_append(sql.ptr(), sql.length());
    } else {
      first_str->length(sql.length());
    }
    current_str = first_str;
  } else {
    if (!current_str->next)
    {
      if (!(current_str->next = new spider_string[1]))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      current_str->next->init_calc_mem(217);
      current_str->next->set_charset(sql.charset());
      if (current_str->next->reserve(sql.length()))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      current_str->next->q_append(sql.ptr(), sql.length());
    } else {
      current_str->next->length(sql.length());
    }
    current_str = current_str->next;
  }
  row_pos2 = *last_row_pos;
  lengths2 = *last_lengths;

  for (
    field = table->field;
    *field;
    field++,
    lengths2++
  ) {
    *row_pos2 = current_str->length();
    if ((error_num =
      copy_row(*field, row)))
      DBUG_RETURN(error_num);
    *lengths2 = current_str->length() - *row_pos2 - SPIDER_SQL_COMMA_LEN;
    row->next();
    row_pos2++;
  }
  current_str->length(current_str->length() - SPIDER_SQL_COMMA_LEN);
  if (current_str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  current_str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
    SPIDER_SQL_CLOSE_PAREN_LEN);
  DBUG_PRINT("info",("spider current_str=%s", current_str->c_ptr_safe()));
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::copy_rows(
  TABLE *table,
  SPIDER_DB_ROW *row
) {
  int error_num;
  Field **field;
  DBUG_ENTER("spider_oracle_copy_table::copy_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!current_str)
  {
    if (!first_str)
    {
      if (!(first_str = new spider_string[1]))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      first_str->init_calc_mem(218);
      first_str->set_charset(sql.charset());
      if (first_str->reserve(sql.length()))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      first_str->q_append(sql.ptr(), sql.length());
    } else {
      first_str->length(sql.length());
    }
    current_str = first_str;
  } else {
    if (!current_str->next)
    {
      if (!(current_str->next = new spider_string[1]))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      current_str->next->init_calc_mem(219);
      current_str->next->set_charset(sql.charset());
      if (current_str->next->reserve(sql.length()))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      current_str->next->q_append(sql.ptr(), sql.length());
    } else {
      current_str->next->length(sql.length());
    }
    current_str = current_str->next;
  }

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
  current_str->length(current_str->length() - SPIDER_SQL_COMMA_LEN);
  if (current_str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  current_str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
    SPIDER_SQL_CLOSE_PAREN_LEN);
  DBUG_PRINT("info",("spider current_str=%s", current_str->c_ptr_safe()));
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::append_insert_terminator()
{
  DBUG_ENTER("spider_oracle_copy_table::append_insert_terminator");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_oracle_copy_table::copy_insert_values(
  spider_db_copy_table *source_ct
) {
  spider_oracle_copy_table *tmp_ct = (spider_oracle_copy_table *) source_ct;
  spider_string *source_str = &tmp_ct->sql;
  int values_length = source_str->length() - tmp_ct->pos;
  const char *values_ptr = source_str->ptr() + tmp_ct->pos;
  DBUG_ENTER("spider_oracle_copy_table::copy_insert_values");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql.reserve(values_length))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  sql.q_append(values_ptr, values_length);
  DBUG_RETURN(0);
}
#endif
