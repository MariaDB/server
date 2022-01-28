/* Copyright (C) 2008-2018 Kentoku Shiba

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
#include "key.h"
#include "sql_base.h"
#include "tztime.h"
#include "sql_select.h"
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_sys_table.h"
#include "spd_malloc.h"

extern handlerton *spider_hton_ptr;
extern Time_zone *spd_tz_system;

#define SPIDER_XA_FORMAT_ID_POS                               0
#define SPIDER_XA_GTRID_LENGTH_POS                            1
#define SPIDER_XA_BQUAL_LENGTH_POS                            2
#define SPIDER_XA_DATA_POS                                    3
#define SPIDER_XA_STATUS_POS                                  4

#define SPIDER_XA_MEMBER_FORMAT_ID_POS                        0
#define SPIDER_XA_MEMBER_GTRID_LENGTH_POS                     1
#define SPIDER_XA_MEMBER_BQUAL_LENGTH_POS                     2
#define SPIDER_XA_MEMBER_DATA_POS                             3
#define SPIDER_XA_MEMBER_SCHEME_POS                           4
#define SPIDER_XA_MEMBER_HOST_POS                             5
#define SPIDER_XA_MEMBER_PORT_POS                             6
#define SPIDER_XA_MEMBER_SOCKET_POS                           7
#define SPIDER_XA_MEMBER_USERNAME_POS                         8
#define SPIDER_XA_MEMBER_PASSWORD_POS                         9
#define SPIDER_XA_MEMBER_SSL_CA_POS                          10
#define SPIDER_XA_MEMBER_SSL_CAPATH_POS                      11
#define SPIDER_XA_MEMBER_SSL_CERT_POS                        12
#define SPIDER_XA_MEMBER_SSL_CIPHER_POS                      13
#define SPIDER_XA_MEMBER_SSL_KEY_POS                         14
#define SPIDER_XA_MEMBER_SSL_VERIFY_SERVER_CERT_POS          15
#define SPIDER_XA_MEMBER_DEFAULT_FILE_POS                    16
#define SPIDER_XA_MEMBER_DEFAULT_GROUP_POS                   17
#define SPIDER_XA_MEMBER_DSN_POS                             18
#define SPIDER_XA_MEMBER_FILEDSN_POS                         19
#define SPIDER_XA_MEMBER_DRIVER_POS                          20
#define SPIDER_XA_FAILED_LOG_THREAD_ID_POS                   21
#define SPIDER_XA_FAILED_LOG_STATUS_POS                      22
#define SPIDER_XA_FAILED_LOG_FAILED_TIME_POS                 23

#define SPIDER_TABLES_DB_NAME_POS                             0
#define SPIDER_TABLES_TABLE_NAME_POS                          1
#define SPIDER_TABLES_LINK_ID_POS                             2
#define SPIDER_TABLES_PRIORITY_POS                            3
#define SPIDER_TABLES_SERVER_POS                              4
#define SPIDER_TABLES_SCHEME_POS                              5
#define SPIDER_TABLES_HOST_POS                                6
#define SPIDER_TABLES_PORT_POS                                7
#define SPIDER_TABLES_SOCKET_POS                              8
#define SPIDER_TABLES_USERNAME_POS                            9
#define SPIDER_TABLES_PASSWORD_POS                           10
#define SPIDER_TABLES_SSL_CA_POS                             11
#define SPIDER_TABLES_SSL_CAPATH_POS                         12
#define SPIDER_TABLES_SSL_CERT_POS                           13
#define SPIDER_TABLES_SSL_CIPHER_POS                         14
#define SPIDER_TABLES_SSL_KEY_POS                            15
#define SPIDER_TABLES_SSL_VERIFY_SERVER_CERT_POS             16
#define SPIDER_TABLES_MONITORING_BINLOG_POS_AT_FAILING_POS   17
#define SPIDER_TABLES_DEFAULT_FILE_POS                       18
#define SPIDER_TABLES_DEFAULT_GROUP_POS                      19
#define SPIDER_TABLES_DSN_POS                                20
#define SPIDER_TABLES_FILEDSN_POS                            21
#define SPIDER_TABLES_DRIVER_POS                             22
#define SPIDER_TABLES_TGT_DB_NAME_POS                        23
#define SPIDER_TABLES_TGT_TABLE_NAME_POS                     24
#define SPIDER_TABLES_LINK_STATUS_POS                        25
#define SPIDER_TABLES_BLOCK_STATUS_POS                       26
#define SPIDER_TABLES_STATIC_LINK_ID_POS                     27

#define SPIDER_LINK_MON_SERVERS_DB_NAME_POS                   0
#define SPIDER_LINK_MON_SERVERS_TABLE_NAME_POS                1
#define SPIDER_LINK_MON_SERVERS_LINK_ID_POS                   2
#define SPIDER_LINK_MON_SERVERS_SID_POS                       3
#define SPIDER_LINK_MON_SERVERS_SERVER_POS                    4
#define SPIDER_LINK_MON_SERVERS_SCHEME_POS                    5
#define SPIDER_LINK_MON_SERVERS_HOST_POS                      6
#define SPIDER_LINK_MON_SERVERS_PORT_POS                      7
#define SPIDER_LINK_MON_SERVERS_SOCKET_POS                    8
#define SPIDER_LINK_MON_SERVERS_USERNAME_POS                  9
#define SPIDER_LINK_MON_SERVERS_PASSWORD_POS                 10
#define SPIDER_LINK_MON_SERVERS_SSL_CA_POS                   11
#define SPIDER_LINK_MON_SERVERS_SSL_CAPATH_POS               12
#define SPIDER_LINK_MON_SERVERS_SSL_CERT_POS                 13
#define SPIDER_LINK_MON_SERVERS_SSL_CIPHER_POS               14
#define SPIDER_LINK_MON_SERVERS_SSL_KEY_POS                  15
#define SPIDER_LINK_MON_SERVERS_SSL_VERIFY_SERVER_CERT_POS   16
#define SPIDER_LINK_MON_SERVERS_DEFAULT_FILE_POS             17
#define SPIDER_LINK_MON_SERVERS_DEFAULT_GROUP_POS            18
#define SPIDER_LINK_MON_SERVERS_DSN_POS                      19
#define SPIDER_LINK_MON_SERVERS_FILEDSN_POS                  20
#define SPIDER_LINK_MON_SERVERS_DRIVER_POS                   21

#define SPIDER_LINK_FAILED_LOG_DB_NAME_POS                    0
#define SPIDER_LINK_FAILED_LOG_TABLE_NAME_POS                 1
#define SPIDER_LINK_FAILED_LOG_LINK_ID_POS                    2
#define SPIDER_LINK_FAILED_LOG_FAILED_TIME_POS                3

#define SPIDER_TABLE_POSITION_FOR_RECOVERY_DB_NAME_POS        0
#define SPIDER_TABLE_POSITION_FOR_RECOVERY_TABLE_NAME_POS     1
#define SPIDER_TABLE_POSITION_FOR_RECOVERY_FAILED_LINK_ID_POS 2
#define SPIDER_TABLE_POSITION_FOR_RECOVERY_SOURCE_LINK_ID_POS 3
#define SPIDER_TABLE_POSITION_FOR_RECOVERY_FILE_POS           4
#define SPIDER_TABLE_POSITION_FOR_RECOVERY_POSITION_POS       5
#define SPIDER_TABLE_POSITION_FOR_RECOVERY_GTID_POS           6

#define SPIDER_TABLE_STS_DB_NAME_POS                          0
#define SPIDER_TABLE_STS_TABLE_NAME_POS                       1
#define SPIDER_TABLE_STS_DATA_FILE_LENGTH_POS                 2
#define SPIDER_TABLE_STS_MAX_DATA_FILE_LENGTH_POS             3
#define SPIDER_TABLE_STS_INDEX_FILE_LENGTH_POS                4
#define SPIDER_TABLE_STS_RECORDS_POS                          5
#define SPIDER_TABLE_STS_MEAN_REC_LENGTH_POS                  6
#define SPIDER_TABLE_STS_CHECK_TIME_POS                       7
#define SPIDER_TABLE_STS_CREATE_TIME_POS                      8
#define SPIDER_TABLE_STS_UPDATE_TIME_POS                      9
#define SPIDER_TABLE_STS_CHECKSUM_POS                        10

#define SPIDER_TABLE_CRD_DB_NAME_POS                          0
#define SPIDER_TABLE_CRD_TABLE_NAME_POS                       1
#define SPIDER_TABLE_CRD_KEY_SEQ_POS                          2
#define SPIDER_TABLE_CRD_CARDINALITY_POS                      3

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
  @param  do_handle_error   TRUE if an error message should be printed
                            before returning.

  @return                   Error code returned by the update.
*/

inline int spider_update_sys_table_row(TABLE *table, bool do_handle_error = TRUE)
{
  int error_num;
  THD *thd = table->in_use;

  tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
  error_num = table->file->ha_update_row(table->record[1], table->record[0]);
  reenable_binlog(thd);

  if (error_num && do_handle_error)
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

  @return                   Error code returned by the delete.
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

TABLE *spider_open_sys_table(
  THD *thd,
  const char *table_name,
  int table_name_length,
  bool write,
  SPIDER_Open_tables_backup *open_tables_backup,
  bool need_lock,
  int *error_num
) {
  TABLE *table;
  TABLE_LIST tables;
  DBUG_ENTER("spider_open_sys_table");


#ifdef SPIDER_use_LEX_CSTRING_for_database_tablename_alias
  LEX_CSTRING db_name =
  {
    "mysql",
    sizeof("mysql") - 1
  };
  LEX_CSTRING tbl_name =
  {
    table_name,
    (size_t) table_name_length
  };
  tables.init_one_table(&db_name, &tbl_name, 0, (write ? TL_WRITE : TL_READ));
#else
  tables.init_one_table(
    "mysql", sizeof("mysql") - 1, table_name, table_name_length, table_name,
    (write ? TL_WRITE : TL_READ));
#endif
    if (!(table = spider_sys_open_table(thd, &tables, open_tables_backup)))
    {
      my_printf_error(ER_SPIDER_CANT_OPEN_SYS_TABLE_NUM,
        ER_SPIDER_CANT_OPEN_SYS_TABLE_STR, MYF(0),
        "mysql", table_name);
      *error_num = ER_SPIDER_CANT_OPEN_SYS_TABLE_NUM;
      DBUG_RETURN(NULL);
    }
  switch (table_name_length)
  {
    case 9:
      if (!memcmp(table_name, SPIDER_SYS_XA_TABLE_NAME_STR,
        SPIDER_SYS_XA_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_XA"));
        if (table->s->fields != SPIDER_SYS_XA_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_XA_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    case 13:
      if (!memcmp(table_name, SPIDER_SYS_TABLES_TABLE_NAME_STR,
        SPIDER_SYS_TABLES_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_TABLES"));
        if (table->s->fields != SPIDER_SYS_TABLES_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_TABLES_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    case 16:
      if (!memcmp(table_name, SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR,
        SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_XA_MEMBER"));
        if (table->s->fields != SPIDER_SYS_XA_MEMBER_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      if (!memcmp(table_name, SPIDER_SYS_TABLE_STS_TABLE_NAME_STR,
        SPIDER_SYS_TABLE_STS_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_TABLE_STS"));
        if (table->s->fields != SPIDER_SYS_TABLE_STS_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_TABLE_STS_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      if (!memcmp(table_name, SPIDER_SYS_TABLE_CRD_TABLE_NAME_STR,
        SPIDER_SYS_TABLE_CRD_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_TABLE_CRD"));
        if (table->s->fields != SPIDER_SYS_TABLE_CRD_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_TABLE_CRD_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    case 20:
      if (!memcmp(table_name, SPIDER_SYS_XA_FAILED_TABLE_NAME_STR,
        SPIDER_SYS_XA_FAILED_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_XA_FAILED"));
        if (table->s->fields != SPIDER_SYS_XA_FAILED_TABLE_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_XA_FAILED_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    case 21:
      if (!memcmp(table_name, SPIDER_SYS_RW_TBLS_TABLE_NAME_STR,
        SPIDER_SYS_RW_TBLS_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_RW_TBLS"));
        if (table->s->fields != SPIDER_SYS_RW_TBLS_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_RW_TBLS_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
      }
      DBUG_ASSERT(0);
      break;
    case 22:
      if (!memcmp(table_name, SPIDER_SYS_LINK_FAILED_TABLE_NAME_STR,
        SPIDER_SYS_LINK_FAILED_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_LINK_FAILED"));
        if (table->s->fields != SPIDER_SYS_LINK_FAILED_TABLE_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_LINK_FAILED_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    case 23:
      if (!memcmp(table_name, SPIDER_SYS_LINK_MON_TABLE_NAME_STR,
        SPIDER_SYS_LINK_MON_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_LINK_MON"));
        if (table->s->fields != SPIDER_SYS_LINK_MON_TABLE_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_LINK_MON_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      if (!memcmp(table_name, SPIDER_SYS_RWN_TBLS_TABLE_NAME_STR,
        SPIDER_SYS_RWN_TBLS_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_RWN_TBLS"));
        if (table->s->fields != SPIDER_SYS_RWN_TBLS_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_RWN_TBLS_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    case 27:
      if (!memcmp(table_name, SPIDER_SYS_RW_TBL_TBLS_TABLE_NAME_STR,
        SPIDER_SYS_RW_TBL_TBLS_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_RW_TBL_TBLS"));
        if (table->s->fields != SPIDER_SYS_RW_TBL_TBLS_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_RW_TBL_TBLS_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    case 31:
      if (!memcmp(table_name, SPIDER_SYS_RW_TBL_PTTS_TABLE_NAME_STR,
        SPIDER_SYS_RW_TBL_PTTS_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_RW_TBL_PTTS"));
        if (table->s->fields != SPIDER_SYS_RW_TBL_PTTS_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_RW_TBL_PTTS_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    case 34:
      if (!memcmp(table_name, SPIDER_SYS_POS_FOR_RECOVERY_TABLE_NAME_STR,
        SPIDER_SYS_POS_FOR_RECOVERY_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_POS_FOR_RECOVERY"));
        if (table->s->fields != SPIDER_SYS_POS_FOR_RECOVERY_TABLE_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_POS_FOR_RECOVERY_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      if (!memcmp(table_name, SPIDER_SYS_RW_TBL_SPTTS_TABLE_NAME_STR,
        SPIDER_SYS_RW_TBL_SPTTS_TABLE_NAME_LEN))
      {
        DBUG_PRINT("info",("spider checking for SYS_RW_TBL_SPTTS"));
        if (table->s->fields != SPIDER_SYS_RW_TBL_SPTTS_COL_CNT)
        {
          spider_close_sys_table(thd, table, open_tables_backup, need_lock);
          table = NULL;
          my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
            ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
            SPIDER_SYS_RW_TBL_SPTTS_TABLE_NAME_STR);
          *error_num = ER_SPIDER_SYS_TABLE_VERSION_NUM;
          goto error_col_num_chk;
        }
        break;
      }
      DBUG_ASSERT(0);
      break;
    default:
      DBUG_ASSERT(0);
      break;
  }
  DBUG_RETURN(table);
error_col_num_chk:
  DBUG_RETURN(NULL);
}

void spider_close_sys_table(
  THD *thd,
  TABLE *table,
  SPIDER_Open_tables_backup *open_tables_backup,
  bool need_lock
) {
  DBUG_ENTER("spider_close_sys_table");
  spider_sys_close_table(thd, open_tables_backup);
  DBUG_VOID_RETURN;
}

bool spider_sys_open_and_lock_tables(
  THD *thd,
  TABLE_LIST **tables,
  SPIDER_Open_tables_backup *open_tables_backup
) {
  uint counter;
  uint flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
    MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY | MYSQL_OPEN_IGNORE_FLUSH |
    MYSQL_LOCK_IGNORE_TIMEOUT | MYSQL_LOCK_LOG_TABLE;
  ulonglong utime_after_lock_backup = thd->utime_after_lock;
  DBUG_ENTER("spider_sys_open_and_lock_tables");
  SPIDER_reset_n_backup_open_tables_state(thd, open_tables_backup, TRUE);
  if (open_tables(thd, tables, &counter, flags))
  {
    SPIDER_restore_backup_open_tables_state(thd, open_tables_backup);
    thd->utime_after_lock = utime_after_lock_backup;
    DBUG_RETURN(TRUE);
  }
  if (lock_tables(thd, *tables, counter, flags))
  {
    SPIDER_sys_close_thread_tables(thd);
    SPIDER_restore_backup_open_tables_state(thd, open_tables_backup);
    thd->utime_after_lock = utime_after_lock_backup;
    DBUG_RETURN(TRUE);
  }
  thd->utime_after_lock = utime_after_lock_backup;
  DBUG_RETURN(FALSE);
}

TABLE *spider_sys_open_table(
  THD *thd,
  TABLE_LIST *tables,
  SPIDER_Open_tables_backup *open_tables_backup
) {
  TABLE *table;
  ulonglong utime_after_lock_backup = thd->utime_after_lock;
  DBUG_ENTER("spider_sys_open_table");
  if (open_tables_backup)
  {
    SPIDER_reset_n_backup_open_tables_state(thd, open_tables_backup, NULL);
  }
  if ((table = open_ltable(thd, tables, tables->lock_type,
    MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
    MYSQL_OPEN_IGNORE_FLUSH | MYSQL_LOCK_IGNORE_TIMEOUT | MYSQL_LOCK_LOG_TABLE
  ))) {
    table->use_all_columns();
    table->s->no_replicate = 1;
  } else if (open_tables_backup)
  {
    SPIDER_restore_backup_open_tables_state(thd, open_tables_backup);
  }
  thd->utime_after_lock = utime_after_lock_backup;
  DBUG_RETURN(table);
}

void spider_sys_close_table(
  THD *thd,
  SPIDER_Open_tables_backup *open_tables_backup
) {
  DBUG_ENTER("spider_sys_close_table");
  if (open_tables_backup)
  {
    SPIDER_sys_close_thread_tables(thd);
    SPIDER_restore_backup_open_tables_state(thd, open_tables_backup);
  }
  DBUG_VOID_RETURN;
}

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

  DBUG_RETURN(table->file->ha_index_read_idx_map(
    table->record[0], 0, (uchar *) table_key,
    HA_WHOLE_KEY, HA_READ_KEY_EXACT));
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

  DBUG_RETURN(table->file->ha_index_read_idx_map(
    table->record[0], 0, (uchar *) table_key,
    HA_WHOLE_KEY, find_flag));
}

int spider_check_sys_table_for_update_all_columns(
  TABLE *table,
  char *table_key
) {
  DBUG_ENTER("spider_check_sys_table_for_update_all_columns");

  key_copy(
    (uchar *) table_key,
    table->record[0],
    table->key_info,
    table->key_info->key_length);

  DBUG_RETURN(table->file->ha_index_read_idx_map(
    table->record[1], 0, (uchar *) table_key,
    HA_WHOLE_KEY, HA_READ_KEY_EXACT));
}

int spider_get_sys_table_by_idx(
  TABLE *table,
  char *table_key,
  const int idx,
  const int col_count
) {
  int error_num;
  uint key_length;
  KEY *key_info = table->key_info + idx;
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
    (error_num = table->file->ha_index_read_map(
      table->record[0], (uchar *) table_key,
      make_prev_keypart_map(col_count), HA_READ_KEY_EXACT))
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
  DBUG_RETURN(table->file->ha_index_next_same(
    table->record[0],
    (const uchar*) table_key,
    table->key_info->key_length));
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
    (error_num = table->file->ha_index_first(table->record[0]))
  ) {
    spider_sys_index_end(table);
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_sys_index_last(
  TABLE *table,
  const int idx
) {
  int error_num;
  DBUG_ENTER("spider_sys_index_last");
  if ((error_num = spider_sys_index_init(table, idx, FALSE)))
    DBUG_RETURN(error_num);

  if (
    (error_num = table->file->ha_index_last(table->record[0]))
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
  DBUG_RETURN(table->file->ha_index_next(table->record[0]));
}

void spider_store_xa_pk(
  TABLE *table,
  XID *xid
) {
  DBUG_ENTER("spider_store_xa_pk");
  table->field[SPIDER_XA_FORMAT_ID_POS]->store(xid->formatID);
  table->field[SPIDER_XA_GTRID_LENGTH_POS]->store(xid->gtrid_length);
  table->field[SPIDER_XA_DATA_POS]->store(
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
  table->field[SPIDER_XA_BQUAL_LENGTH_POS]->store(xid->bqual_length);
  DBUG_VOID_RETURN;
}

void spider_store_xa_status(
  TABLE *table,
  const char *status
) {
  DBUG_ENTER("spider_store_xa_status");
  table->field[SPIDER_XA_STATUS_POS]->store(
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
  table->field[SPIDER_XA_MEMBER_FORMAT_ID_POS]->store(xid->formatID);
  table->field[SPIDER_XA_MEMBER_GTRID_LENGTH_POS]->store(xid->gtrid_length);
  table->field[SPIDER_XA_MEMBER_DATA_POS]->store(
    xid->data,
    (uint) xid->gtrid_length + xid->bqual_length,
    system_charset_info);
  table->field[SPIDER_XA_MEMBER_HOST_POS]->store(
    conn->tgt_host,
    (uint) conn->tgt_host_length,
    system_charset_info);
  table->field[SPIDER_XA_MEMBER_PORT_POS]->store(
    conn->tgt_port);
  table->field[SPIDER_XA_MEMBER_SOCKET_POS]->store(
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
  table->field[SPIDER_XA_MEMBER_BQUAL_LENGTH_POS]->store(xid->bqual_length);
  table->field[SPIDER_XA_MEMBER_SCHEME_POS]->store(
    conn->tgt_wrapper,
    (uint) conn->tgt_wrapper_length,
    system_charset_info);
  table->field[SPIDER_XA_MEMBER_USERNAME_POS]->store(
    conn->tgt_username,
    (uint) conn->tgt_username_length,
    system_charset_info);
  table->field[SPIDER_XA_MEMBER_PASSWORD_POS]->store(
    conn->tgt_password,
    (uint) conn->tgt_password_length,
    system_charset_info);
  if (conn->tgt_ssl_ca)
  {
    table->field[SPIDER_XA_MEMBER_SSL_CA_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_SSL_CA_POS]->store(
      conn->tgt_ssl_ca,
      (uint) conn->tgt_ssl_ca_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_SSL_CA_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_SSL_CA_POS]->reset();
  }
  if (conn->tgt_ssl_capath)
  {
    table->field[SPIDER_XA_MEMBER_SSL_CAPATH_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_SSL_CAPATH_POS]->store(
      conn->tgt_ssl_capath,
      (uint) conn->tgt_ssl_capath_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_SSL_CAPATH_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_SSL_CAPATH_POS]->reset();
  }
  if (conn->tgt_ssl_cert)
  {
    table->field[SPIDER_XA_MEMBER_SSL_CERT_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_SSL_CERT_POS]->store(
      conn->tgt_ssl_cert,
      (uint) conn->tgt_ssl_cert_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_SSL_CERT_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_SSL_CERT_POS]->reset();
  }
  if (conn->tgt_ssl_cipher)
  {
    table->field[SPIDER_XA_MEMBER_SSL_CIPHER_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_SSL_CIPHER_POS]->store(
      conn->tgt_ssl_cipher,
      (uint) conn->tgt_ssl_cipher_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_SSL_CIPHER_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_SSL_CIPHER_POS]->reset();
  }
  if (conn->tgt_ssl_key)
  {
    table->field[SPIDER_XA_MEMBER_SSL_KEY_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_SSL_KEY_POS]->store(
      conn->tgt_ssl_key,
      (uint) conn->tgt_ssl_key_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_SSL_KEY_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_SSL_KEY_POS]->reset();
  }
  if (conn->tgt_ssl_vsc >= 0)
  {
    table->field[SPIDER_XA_MEMBER_SSL_VERIFY_SERVER_CERT_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_SSL_VERIFY_SERVER_CERT_POS]->store(
      conn->tgt_ssl_vsc);
  } else {
    table->field[SPIDER_XA_MEMBER_SSL_VERIFY_SERVER_CERT_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_SSL_VERIFY_SERVER_CERT_POS]->reset();
  }
  if (conn->tgt_default_file)
  {
    table->field[SPIDER_XA_MEMBER_DEFAULT_FILE_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_DEFAULT_FILE_POS]->store(
      conn->tgt_default_file,
      (uint) conn->tgt_default_file_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_DEFAULT_FILE_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_DEFAULT_FILE_POS]->reset();
  }
  if (conn->tgt_default_group)
  {
    table->field[SPIDER_XA_MEMBER_DEFAULT_GROUP_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_DEFAULT_GROUP_POS]->store(
      conn->tgt_default_group,
      (uint) conn->tgt_default_group_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_DEFAULT_GROUP_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_DEFAULT_GROUP_POS]->reset();
  }
  if (conn->tgt_dsn)
  {
    table->field[SPIDER_XA_MEMBER_DSN_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_DSN_POS]->store(
      conn->tgt_dsn,
      (uint) conn->tgt_dsn_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_DSN_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_DSN_POS]->reset();
  }
  if (conn->tgt_filedsn)
  {
    table->field[SPIDER_XA_MEMBER_FILEDSN_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_FILEDSN_POS]->store(
      conn->tgt_filedsn,
      (uint) conn->tgt_filedsn_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_FILEDSN_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_FILEDSN_POS]->reset();
  }
  if (conn->tgt_driver)
  {
    table->field[SPIDER_XA_MEMBER_DRIVER_POS]->set_notnull();
    table->field[SPIDER_XA_MEMBER_DRIVER_POS]->store(
      conn->tgt_driver,
      (uint) conn->tgt_driver_length,
      system_charset_info);
  } else {
    table->field[SPIDER_XA_MEMBER_DRIVER_POS]->set_null();
    table->field[SPIDER_XA_MEMBER_DRIVER_POS]->reset();
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
  table->field[SPIDER_TABLES_DB_NAME_POS]->store(
    ptr_db,
    (uint)(ptr_diff_table - 1),
    system_charset_info);
  DBUG_PRINT("info",("spider field[%u]->null_bit = %d",
    SPIDER_TABLES_DB_NAME_POS,
    table->field[SPIDER_TABLES_DB_NAME_POS]->null_bit));
  table->field[SPIDER_TABLES_TABLE_NAME_POS]->store(
    ptr_table,
    (uint) ((my_ptrdiff_t) name_length - ptr_diff_db - ptr_diff_table),
    system_charset_info);
  DBUG_PRINT("info",("spider field[%u]->null_bit = %d",
    SPIDER_TABLES_TABLE_NAME_POS,
    table->field[SPIDER_TABLES_TABLE_NAME_POS]->null_bit));
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
  table->field[SPIDER_TABLES_DB_NAME_POS]->store(
    db_name,
    db_name_length,
    system_charset_info);
  DBUG_PRINT("info",("spider field[%u]->null_bit = %d",
    SPIDER_TABLES_DB_NAME_POS,
    table->field[SPIDER_TABLES_DB_NAME_POS]->null_bit));
  table->field[SPIDER_TABLES_TABLE_NAME_POS]->store(
    table_name,
    table_name_length,
    system_charset_info);
  DBUG_PRINT("info",("spider field[%u]->null_bit = %d",
    SPIDER_TABLES_TABLE_NAME_POS,
    table->field[SPIDER_TABLES_TABLE_NAME_POS]->null_bit));
  DBUG_VOID_RETURN;
}

void spider_store_tables_link_idx(
  TABLE *table,
  int link_idx
) {
  DBUG_ENTER("spider_store_tables_link_idx");
  table->field[SPIDER_TABLES_LINK_ID_POS]->set_notnull();
  table->field[SPIDER_TABLES_LINK_ID_POS]->store(link_idx);
  DBUG_VOID_RETURN;
}

void spider_store_tables_link_idx_str(
  TABLE *table,
  const char *link_idx,
  const uint link_idx_length
) {
  DBUG_ENTER("spider_store_tables_link_idx_str");
  table->field[SPIDER_TABLES_LINK_ID_POS]->store(
    link_idx,
    link_idx_length,
    system_charset_info);
  DBUG_PRINT("info",("spider field[%u]->null_bit = %d",
    SPIDER_TABLES_LINK_ID_POS,
    table->field[SPIDER_TABLES_LINK_ID_POS]->null_bit));
  DBUG_VOID_RETURN;
}

void spider_store_tables_static_link_id(
  TABLE *table,
  const char *static_link_id,
  const uint static_link_id_length
) {
  DBUG_ENTER("spider_store_tables_static_link_id");
  if (static_link_id)
  {
    table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->set_notnull();
    table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->store(
      static_link_id,
      static_link_id_length,
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->set_null();
    table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->reset();
  }
  DBUG_VOID_RETURN;
}

void spider_store_tables_priority(
  TABLE *table,
  longlong priority
) {
  DBUG_ENTER("spider_store_tables_priority");
  DBUG_PRINT("info",("spider priority = %lld", priority));
  table->field[SPIDER_TABLES_PRIORITY_POS]->store(priority, FALSE);
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
    table->field[SPIDER_TABLES_SERVER_POS]->set_notnull();
    table->field[SPIDER_TABLES_SERVER_POS]->store(
      alter_table->tmp_server_names[link_idx],
      (uint) alter_table->tmp_server_names_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_SERVER_POS]->set_null();
    table->field[SPIDER_TABLES_SERVER_POS]->reset();
  }
  if (alter_table->tmp_tgt_wrappers[link_idx])
  {
    table->field[SPIDER_TABLES_SCHEME_POS]->set_notnull();
    table->field[SPIDER_TABLES_SCHEME_POS]->store(
      alter_table->tmp_tgt_wrappers[link_idx],
      (uint) alter_table->tmp_tgt_wrappers_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_SCHEME_POS]->set_null();
    table->field[SPIDER_TABLES_SCHEME_POS]->reset();
  }
  if (alter_table->tmp_tgt_hosts[link_idx])
  {
    table->field[SPIDER_TABLES_HOST_POS]->set_notnull();
    table->field[SPIDER_TABLES_HOST_POS]->store(
      alter_table->tmp_tgt_hosts[link_idx],
      (uint) alter_table->tmp_tgt_hosts_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_HOST_POS]->set_null();
    table->field[SPIDER_TABLES_HOST_POS]->reset();
  }
  if (alter_table->tmp_tgt_ports[link_idx] >= 0)
  {
    table->field[SPIDER_TABLES_PORT_POS]->set_notnull();
    table->field[SPIDER_TABLES_PORT_POS]->store(
      alter_table->tmp_tgt_ports[link_idx]);
  } else {
    table->field[SPIDER_TABLES_PORT_POS]->set_null();
    table->field[SPIDER_TABLES_PORT_POS]->reset();
  }
  if (alter_table->tmp_tgt_sockets[link_idx])
  {
    table->field[SPIDER_TABLES_SOCKET_POS]->set_notnull();
    table->field[SPIDER_TABLES_SOCKET_POS]->store(
      alter_table->tmp_tgt_sockets[link_idx],
      (uint) alter_table->tmp_tgt_sockets_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_SOCKET_POS]->set_null();
    table->field[SPIDER_TABLES_SOCKET_POS]->reset();
  }
  if (alter_table->tmp_tgt_usernames[link_idx])
  {
    table->field[SPIDER_TABLES_USERNAME_POS]->set_notnull();
    table->field[SPIDER_TABLES_USERNAME_POS]->store(
      alter_table->tmp_tgt_usernames[link_idx],
      (uint) alter_table->tmp_tgt_usernames_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_USERNAME_POS]->set_null();
    table->field[SPIDER_TABLES_USERNAME_POS]->reset();
  }
  if (alter_table->tmp_tgt_passwords[link_idx])
  {
    table->field[SPIDER_TABLES_PASSWORD_POS]->set_notnull();
    table->field[SPIDER_TABLES_PASSWORD_POS]->store(
      alter_table->tmp_tgt_passwords[link_idx],
      (uint) alter_table->tmp_tgt_passwords_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_PASSWORD_POS]->set_null();
    table->field[SPIDER_TABLES_PASSWORD_POS]->reset();
  }
  if (alter_table->tmp_tgt_ssl_cas[link_idx])
  {
    table->field[SPIDER_TABLES_SSL_CA_POS]->set_notnull();
    table->field[SPIDER_TABLES_SSL_CA_POS]->store(
      alter_table->tmp_tgt_ssl_cas[link_idx],
      (uint) alter_table->tmp_tgt_ssl_cas_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_SSL_CA_POS]->set_null();
    table->field[SPIDER_TABLES_SSL_CA_POS]->reset();
  }
  if (alter_table->tmp_tgt_ssl_capaths[link_idx])
  {
    table->field[SPIDER_TABLES_SSL_CAPATH_POS]->set_notnull();
    table->field[SPIDER_TABLES_SSL_CAPATH_POS]->store(
      alter_table->tmp_tgt_ssl_capaths[link_idx],
      (uint) alter_table->tmp_tgt_ssl_capaths_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_SSL_CAPATH_POS]->set_null();
    table->field[SPIDER_TABLES_SSL_CAPATH_POS]->reset();
  }
  if (alter_table->tmp_tgt_ssl_certs[link_idx])
  {
    table->field[SPIDER_TABLES_SSL_CERT_POS]->set_notnull();
    table->field[SPIDER_TABLES_SSL_CERT_POS]->store(
      alter_table->tmp_tgt_ssl_certs[link_idx],
      (uint) alter_table->tmp_tgt_ssl_certs_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_SSL_CERT_POS]->set_null();
    table->field[SPIDER_TABLES_SSL_CERT_POS]->reset();
  }
  if (alter_table->tmp_tgt_ssl_ciphers[link_idx])
  {
    table->field[SPIDER_TABLES_SSL_CIPHER_POS]->set_notnull();
    table->field[SPIDER_TABLES_SSL_CIPHER_POS]->store(
      alter_table->tmp_tgt_ssl_ciphers[link_idx],
      (uint) alter_table->tmp_tgt_ssl_ciphers_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_SSL_CIPHER_POS]->set_null();
    table->field[SPIDER_TABLES_SSL_CIPHER_POS]->reset();
  }
  if (alter_table->tmp_tgt_ssl_keys[link_idx])
  {
    table->field[SPIDER_TABLES_SSL_KEY_POS]->set_notnull();
    table->field[SPIDER_TABLES_SSL_KEY_POS]->store(
      alter_table->tmp_tgt_ssl_keys[link_idx],
      (uint) alter_table->tmp_tgt_ssl_keys_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_SSL_KEY_POS]->set_null();
    table->field[SPIDER_TABLES_SSL_KEY_POS]->reset();
  }
  if (alter_table->tmp_tgt_ssl_vscs[link_idx] >= 0)
  {
    table->field[SPIDER_TABLES_SSL_VERIFY_SERVER_CERT_POS]->set_notnull();
    table->field[SPIDER_TABLES_SSL_VERIFY_SERVER_CERT_POS]->store(
      alter_table->tmp_tgt_ssl_vscs[link_idx]);
  } else {
    table->field[SPIDER_TABLES_SSL_VERIFY_SERVER_CERT_POS]->set_null();
    table->field[SPIDER_TABLES_SSL_VERIFY_SERVER_CERT_POS]->reset();
  }
  table->field[SPIDER_TABLES_MONITORING_BINLOG_POS_AT_FAILING_POS]->
    set_notnull();
  if (alter_table->tmp_monitoring_binlog_pos_at_failing[link_idx] >= 0)
  {
    table->field[SPIDER_TABLES_MONITORING_BINLOG_POS_AT_FAILING_POS]->store(
      alter_table->tmp_monitoring_binlog_pos_at_failing[link_idx]);
  } else {
    table->field[SPIDER_TABLES_MONITORING_BINLOG_POS_AT_FAILING_POS]->store(0);
  }
  if (alter_table->tmp_tgt_default_files[link_idx])
  {
    table->field[SPIDER_TABLES_DEFAULT_FILE_POS]->set_notnull();
    table->field[SPIDER_TABLES_DEFAULT_FILE_POS]->store(
      alter_table->tmp_tgt_default_files[link_idx],
      (uint) alter_table->tmp_tgt_default_files_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_DEFAULT_FILE_POS]->set_null();
    table->field[SPIDER_TABLES_DEFAULT_FILE_POS]->reset();
  }
  if (alter_table->tmp_tgt_default_groups[link_idx])
  {
    table->field[SPIDER_TABLES_DEFAULT_GROUP_POS]->set_notnull();
    table->field[SPIDER_TABLES_DEFAULT_GROUP_POS]->store(
      alter_table->tmp_tgt_default_groups[link_idx],
      (uint) alter_table->tmp_tgt_default_groups_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_DEFAULT_GROUP_POS]->set_null();
    table->field[SPIDER_TABLES_DEFAULT_GROUP_POS]->reset();
  }
  if (alter_table->tmp_tgt_dsns[link_idx])
  {
    table->field[SPIDER_TABLES_DSN_POS]->set_notnull();
    table->field[SPIDER_TABLES_DSN_POS]->store(
      alter_table->tmp_tgt_dsns[link_idx],
      (uint) alter_table->tmp_tgt_dsns_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_DSN_POS]->set_null();
    table->field[SPIDER_TABLES_DSN_POS]->reset();
  }
  if (alter_table->tmp_tgt_filedsns[link_idx])
  {
    table->field[SPIDER_TABLES_FILEDSN_POS]->set_notnull();
    table->field[SPIDER_TABLES_FILEDSN_POS]->store(
      alter_table->tmp_tgt_filedsns[link_idx],
      (uint) alter_table->tmp_tgt_filedsns_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_FILEDSN_POS]->set_null();
    table->field[SPIDER_TABLES_FILEDSN_POS]->reset();
  }
  if (alter_table->tmp_tgt_drivers[link_idx])
  {
    table->field[SPIDER_TABLES_DRIVER_POS]->set_notnull();
    table->field[SPIDER_TABLES_DRIVER_POS]->store(
      alter_table->tmp_tgt_drivers[link_idx],
      (uint) alter_table->tmp_tgt_drivers_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_DRIVER_POS]->set_null();
    table->field[SPIDER_TABLES_DRIVER_POS]->reset();
  }
  if (alter_table->tmp_tgt_dbs[link_idx])
  {
    table->field[SPIDER_TABLES_TGT_DB_NAME_POS]->set_notnull();
    table->field[SPIDER_TABLES_TGT_DB_NAME_POS]->store(
      alter_table->tmp_tgt_dbs[link_idx],
      (uint) alter_table->tmp_tgt_dbs_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_TGT_DB_NAME_POS]->set_null();
    table->field[SPIDER_TABLES_TGT_DB_NAME_POS]->reset();
  }
  if (alter_table->tmp_tgt_table_names[link_idx])
  {
    table->field[SPIDER_TABLES_TGT_TABLE_NAME_POS]->set_notnull();
    table->field[SPIDER_TABLES_TGT_TABLE_NAME_POS]->store(
      alter_table->tmp_tgt_table_names[link_idx],
      (uint) alter_table->tmp_tgt_table_names_lengths[link_idx],
      system_charset_info);
  } else {
    table->field[SPIDER_TABLES_TGT_TABLE_NAME_POS]->set_null();
    table->field[SPIDER_TABLES_TGT_TABLE_NAME_POS]->reset();
  }
  table->field[SPIDER_TABLES_BLOCK_STATUS_POS]->store((longlong) 0, FALSE);
  if (alter_table->tmp_static_link_ids[link_idx])
  {
    DBUG_PRINT("info",("spider static_link_id[%d] = %s",
      link_idx, alter_table->tmp_static_link_ids[link_idx]));
    table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->set_notnull();
    table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->store(
      alter_table->tmp_static_link_ids[link_idx],
      (uint) alter_table->tmp_static_link_ids_lengths[link_idx],
      system_charset_info);
  } else {
    DBUG_PRINT("info",("spider static_link_id[%d] = NULL", link_idx));
    table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->set_null();
    table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->reset();
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
    table->field[SPIDER_TABLES_LINK_STATUS_POS]->store(link_status, FALSE);
  DBUG_VOID_RETURN;
}

void spider_store_binlog_pos_failed_link_idx(
  TABLE *table,
  int failed_link_idx
) {
  DBUG_ENTER("spider_store_binlog_pos_failed_link_idx");
  table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_FAILED_LINK_ID_POS]->
    set_notnull();
  table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_FAILED_LINK_ID_POS]->
    store(failed_link_idx);
  DBUG_VOID_RETURN;
}

void spider_store_binlog_pos_source_link_idx(
  TABLE *table,
  int source_link_idx
) {
  DBUG_ENTER("spider_store_binlog_pos_source_link_idx");
  table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_SOURCE_LINK_ID_POS]->
    set_notnull();
  table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_SOURCE_LINK_ID_POS]->
    store(source_link_idx);
  DBUG_VOID_RETURN;
}

void spider_store_binlog_pos_binlog_file(
  TABLE *table,
  const char *file_name,
  int file_name_length,
  const char *position,
  int position_length,
  CHARSET_INFO *binlog_pos_cs
) {
  DBUG_ENTER("spider_store_binlog_pos_binlog_file");
  if (!file_name)
  {
    DBUG_PRINT("info",("spider file_name is NULL"));
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_FILE_POS]->set_null();
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_FILE_POS]->reset();
  } else {
    DBUG_PRINT("info",("spider file_name = %s", file_name));
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_FILE_POS]->set_notnull();
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_FILE_POS]->store(
      file_name, file_name_length, binlog_pos_cs);
  }
  if (!position)
  {
    DBUG_PRINT("info",("spider position is NULL"));
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_POSITION_POS]->set_null();
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_POSITION_POS]->reset();
  } else {
    DBUG_PRINT("info",("spider position = %s", position));
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_POSITION_POS]->
      set_notnull();
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_POSITION_POS]->store(
      position, position_length, binlog_pos_cs);
  }
  DBUG_VOID_RETURN;
}

void spider_store_binlog_pos_gtid(
  TABLE *table,
  const char *gtid,
  int gtid_length,
  CHARSET_INFO *binlog_pos_cs
) {
  DBUG_ENTER("spider_store_binlog_pos_gtid");
  if (!gtid)
  {
    DBUG_PRINT("info",("spider gtid is NULL"));
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_GTID_POS]->set_null();
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_GTID_POS]->reset();
  } else {
    DBUG_PRINT("info",("spider gtid = %s", gtid));
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_GTID_POS]->set_notnull();
    table->field[SPIDER_TABLE_POSITION_FOR_RECOVERY_GTID_POS]->store(
      gtid, gtid_length, binlog_pos_cs);
  }
  DBUG_VOID_RETURN;
}

void spider_store_table_sts_info(
  TABLE *table,
  ha_statistics *stat
) {
  MYSQL_TIME mysql_time;
  DBUG_ENTER("spider_store_table_sts_info");
  table->field[SPIDER_TABLE_STS_DATA_FILE_LENGTH_POS]->store(
    (longlong) stat->data_file_length, TRUE);
  table->field[SPIDER_TABLE_STS_MAX_DATA_FILE_LENGTH_POS]->store(
    (longlong) stat->max_data_file_length, TRUE);
  table->field[SPIDER_TABLE_STS_INDEX_FILE_LENGTH_POS]->store(
    (longlong) stat->index_file_length, TRUE);
  table->field[SPIDER_TABLE_STS_RECORDS_POS]->store(
    (longlong) stat->records, TRUE);
  table->field[SPIDER_TABLE_STS_MEAN_REC_LENGTH_POS]->store(
    (longlong) stat->mean_rec_length, TRUE);
  spd_tz_system->gmt_sec_to_TIME(&mysql_time, (my_time_t) stat->check_time);
  table->field[SPIDER_TABLE_STS_CHECK_TIME_POS]->store_time(&mysql_time);
  spd_tz_system->gmt_sec_to_TIME(&mysql_time, (my_time_t) stat->create_time);
  table->field[SPIDER_TABLE_STS_CREATE_TIME_POS]->store_time(&mysql_time);
  spd_tz_system->gmt_sec_to_TIME(&mysql_time, (my_time_t) stat->update_time);
  table->field[SPIDER_TABLE_STS_UPDATE_TIME_POS]->store_time(&mysql_time);
  if (stat->checksum_null)
  {
    table->field[SPIDER_TABLE_STS_CHECKSUM_POS]->set_null();
    table->field[SPIDER_TABLE_STS_CHECKSUM_POS]->reset();
  } else {
    table->field[SPIDER_TABLE_STS_CHECKSUM_POS]->set_notnull();
    table->field[SPIDER_TABLE_STS_CHECKSUM_POS]->store(
      (longlong) stat->checksum, TRUE);
  }
  DBUG_VOID_RETURN;
}

void spider_store_table_crd_info(
  TABLE *table,
  uint *seq,
  longlong *cardinality
) {
  DBUG_ENTER("spider_store_table_crd_info");
  table->field[SPIDER_TABLE_CRD_KEY_SEQ_POS]->store((longlong) *seq, TRUE);
  table->field[SPIDER_TABLE_CRD_CARDINALITY_POS]->store(
    (longlong) *cardinality, FALSE);
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
    {
      DBUG_RETURN(error_num);
    }
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
    {
      DBUG_RETURN(error_num);
    }
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
    {
      DBUG_RETURN(error_num);
    }
  }

  DBUG_RETURN(0);
}

int spider_insert_sys_table(
  TABLE *table
) {
  int error_num;
  DBUG_ENTER("spider_insert_sys_table");
  error_num = spider_write_sys_table_row(table);
  DBUG_RETURN(error_num);
}

int spider_insert_or_update_table_sts(
  TABLE *table,
  const char *name,
  uint name_length,
  ha_statistics *stat
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_insert_or_update_table_sts");
  table->use_all_columns();
  spider_store_tables_name(table, name, name_length);
  spider_store_table_sts_info(
    table,
    stat
  );

  if ((error_num = spider_check_sys_table_for_update_all_columns(table, table_key)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
    if ((error_num = spider_write_sys_table_row(table)))
    {
      DBUG_RETURN(error_num);
    }
  } else {
    if ((error_num = spider_update_sys_table_row(table, FALSE)))
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
  }

  DBUG_RETURN(0);
}

int spider_insert_or_update_table_crd(
  TABLE *table,
  const char *name,
  uint name_length,
  longlong *cardinality,
  uint number_of_keys
) {
  int error_num;
  uint roop_count;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_insert_or_update_table_crd");
  table->use_all_columns();
  spider_store_tables_name(table, name, name_length);

  for (roop_count = 0; roop_count < number_of_keys; ++roop_count)
  {
    spider_store_table_crd_info(table, &roop_count, &cardinality[roop_count]);
    if ((error_num = spider_check_sys_table_for_update_all_columns(table, table_key)))
    {
      if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
      {
        table->file->print_error(error_num, MYF(0));
        DBUG_RETURN(error_num);
      }
      if ((error_num = spider_write_sys_table_row(table)))
      {
        DBUG_RETURN(error_num);
      }
    } else {
      if ((error_num = spider_update_sys_table_row(table, FALSE)))
      {
        table->file->print_error(error_num, MYF(0));
        DBUG_RETURN(error_num);
      }
    }
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
  if ((error_num = spider_write_sys_table_row(table)))
  {
    DBUG_RETURN(error_num);
  }
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
    table->field[SPIDER_XA_FAILED_LOG_THREAD_ID_POS]->set_notnull();
    table->field[SPIDER_XA_FAILED_LOG_THREAD_ID_POS]->store(
      thd->thread_id, TRUE);
  } else {
    table->field[SPIDER_XA_FAILED_LOG_THREAD_ID_POS]->set_null();
    table->field[SPIDER_XA_FAILED_LOG_THREAD_ID_POS]->reset();
  }
  table->field[SPIDER_XA_FAILED_LOG_STATUS_POS]->store(
    status,
    (uint) strlen(status),
    system_charset_info);

  if ((error_num = spider_write_sys_table_row(table)))
  {
    DBUG_RETURN(error_num);
  }
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
    {
      DBUG_RETURN(error_num);
    }
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
      {
        DBUG_RETURN(error_num);
      }
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
          {
            DBUG_RETURN(error_num);
          }
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
      {
        DBUG_RETURN(error_num);
      }
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
      {
        DBUG_RETURN(error_num);
      }
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
    {
      DBUG_RETURN(error_num);
    }
  }

  DBUG_RETURN(0);
}

int spider_update_sys_table(
  TABLE *table
) {
  int error_num;
  DBUG_ENTER("spider_update_sys_table");
  error_num = spider_update_sys_table_row(table);
  DBUG_RETURN(error_num);
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
    {
      DBUG_RETURN(error_num);
    }
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
      {
        DBUG_RETURN(error_num);
      }
    }
    roop_count++;
  }

  *old_link_count = roop_count;
  DBUG_RETURN(0);
}

int spider_delete_table_sts(
  TABLE *table,
  const char *name,
  uint name_length
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_delete_table_sts");
  table->use_all_columns();
  spider_store_tables_name(table, name, name_length);

  if ((error_num = spider_check_sys_table(table, table_key)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
    /* no record is ok */
    DBUG_RETURN(0);
  } else {
    if ((error_num = spider_delete_sys_table_row(table)))
    {
      DBUG_RETURN(error_num);
    }
  }

  DBUG_RETURN(0);
}

int spider_delete_table_crd(
  TABLE *table,
  const char *name,
  uint name_length
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  DBUG_ENTER("spider_delete_table_crd");
  table->use_all_columns();
  spider_store_tables_name(table, name, name_length);

  if ((error_num = spider_get_sys_table_by_idx(table, table_key, 0,
    SPIDER_SYS_TABLE_CRD_PK_COL_CNT - 1)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error_num, MYF(0));
      DBUG_RETURN(error_num);
    }
    /* no record is ok */
    DBUG_RETURN(0);
  } else {
    do {
      if ((error_num = spider_delete_sys_table_row(table)))
      {
        spider_sys_index_end(table);
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

int spider_get_sys_xid(
  TABLE *table,
  XID *xid,
  MEM_ROOT *mem_root
) {
  char *ptr;
  DBUG_ENTER("spider_get_sys_xid");
  ptr = get_field(mem_root, table->field[SPIDER_XA_FORMAT_ID_POS]);
  if (ptr)
  {
    xid->formatID = atoi(ptr);
  } else
    xid->formatID = 0;
  ptr = get_field(mem_root, table->field[SPIDER_XA_GTRID_LENGTH_POS]);
  if (ptr)
  {
    xid->gtrid_length = atoi(ptr);
  } else
    xid->gtrid_length = 0;
  ptr = get_field(mem_root, table->field[SPIDER_XA_BQUAL_LENGTH_POS]);
  if (ptr)
  {
    xid->bqual_length = atoi(ptr);
  } else
    xid->bqual_length = 0;
  ptr = get_field(mem_root, table->field[SPIDER_XA_DATA_POS]);
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
  if ((ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_SCHEME_POS])))
  {
    share->tgt_wrappers_lengths[link_idx] = strlen(ptr);
    share->tgt_wrappers[link_idx] = spider_create_string(ptr,
      share->tgt_wrappers_lengths[link_idx]);
  } else {
    share->tgt_wrappers_lengths[link_idx] = 0;
    share->tgt_wrappers[link_idx] = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_HOST_POS])))
  {
    share->tgt_hosts_lengths[link_idx] = strlen(ptr);
    share->tgt_hosts[link_idx] = spider_create_string(ptr,
      share->tgt_hosts_lengths[link_idx]);
  } else {
    share->tgt_hosts_lengths[link_idx] = 0;
    share->tgt_hosts[link_idx] = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_PORT_POS])))
  {
    share->tgt_ports[link_idx] = atol(ptr);
  } else
    share->tgt_ports[link_idx] = MYSQL_PORT;
  if ((ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_SOCKET_POS])))
  {
    share->tgt_sockets_lengths[link_idx] = strlen(ptr);
    share->tgt_sockets[link_idx] = spider_create_string(ptr,
      share->tgt_sockets_lengths[link_idx]);
  } else {
    share->tgt_sockets_lengths[link_idx] = 0;
    share->tgt_sockets[link_idx] = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_USERNAME_POS])))
  {
    share->tgt_usernames_lengths[link_idx] = strlen(ptr);
    share->tgt_usernames[link_idx] =
      spider_create_string(ptr, share->tgt_usernames_lengths[link_idx]);
  } else {
    share->tgt_usernames_lengths[link_idx] = 0;
    share->tgt_usernames[link_idx] = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_PASSWORD_POS])))
  {
    share->tgt_passwords_lengths[link_idx] = strlen(ptr);
    share->tgt_passwords[link_idx] =
      spider_create_string(ptr, share->tgt_passwords_lengths[link_idx]);
  } else {
    share->tgt_passwords_lengths[link_idx] = 0;
    share->tgt_passwords[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_SSL_CA_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_SSL_CA_POS]))
  ) {
    share->tgt_ssl_cas_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_cas[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_cas_lengths[link_idx]);
  } else {
    share->tgt_ssl_cas_lengths[link_idx] = 0;
    share->tgt_ssl_cas[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_SSL_CAPATH_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_SSL_CAPATH_POS]))
  ) {
    share->tgt_ssl_capaths_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_capaths[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_capaths_lengths[link_idx]);
  } else {
    share->tgt_ssl_capaths_lengths[link_idx] = 0;
    share->tgt_ssl_capaths[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_SSL_CERT_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_SSL_CERT_POS]))
  ) {
    share->tgt_ssl_certs_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_certs[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_certs_lengths[link_idx]);
  } else {
    share->tgt_ssl_certs_lengths[link_idx] = 0;
    share->tgt_ssl_certs[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_SSL_CIPHER_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_SSL_CIPHER_POS]))
  ) {
    share->tgt_ssl_ciphers_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_ciphers[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_ciphers_lengths[link_idx]);
  } else {
    share->tgt_ssl_ciphers_lengths[link_idx] = 0;
    share->tgt_ssl_ciphers[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_SSL_KEY_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_SSL_KEY_POS]))
  ) {
    share->tgt_ssl_keys_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_keys[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_keys_lengths[link_idx]);
  } else {
    share->tgt_ssl_keys_lengths[link_idx] = 0;
    share->tgt_ssl_keys[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_SSL_VERIFY_SERVER_CERT_POS]->is_null() &&
    (ptr = get_field(mem_root, table->
      field[SPIDER_XA_MEMBER_SSL_VERIFY_SERVER_CERT_POS]))
  ) {
    share->tgt_ssl_vscs[link_idx] = atol(ptr);
  } else
    share->tgt_ssl_vscs[link_idx] = 0;
  if (
    !table->field[SPIDER_XA_MEMBER_DEFAULT_FILE_POS]->is_null() &&
    (ptr = get_field(mem_root, table->
      field[SPIDER_XA_MEMBER_DEFAULT_FILE_POS]))
  ) {
    share->tgt_default_files_lengths[link_idx] = strlen(ptr);
    share->tgt_default_files[link_idx] =
      spider_create_string(ptr, share->tgt_default_files_lengths[link_idx]);
  } else {
    share->tgt_default_files_lengths[link_idx] = 0;
    share->tgt_default_files[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_DEFAULT_GROUP_POS]->is_null() &&
    (ptr = get_field(mem_root, table->
      field[SPIDER_XA_MEMBER_DEFAULT_GROUP_POS]))
  ) {
    share->tgt_default_groups_lengths[link_idx] = strlen(ptr);
    share->tgt_default_groups[link_idx] =
      spider_create_string(ptr, share->tgt_default_groups_lengths[link_idx]);
  } else {
    share->tgt_default_groups_lengths[link_idx] = 0;
    share->tgt_default_groups[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_DSN_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_DSN_POS]))
  ) {
    share->tgt_dsns_lengths[link_idx] = strlen(ptr);
    share->tgt_dsns[link_idx] =
      spider_create_string(ptr, share->tgt_dsns_lengths[link_idx]);
  } else {
    share->tgt_dsns_lengths[link_idx] = 0;
    share->tgt_dsns[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_FILEDSN_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_FILEDSN_POS]))
  ) {
    share->tgt_filedsns_lengths[link_idx] = strlen(ptr);
    share->tgt_filedsns[link_idx] =
      spider_create_string(ptr, share->tgt_filedsns_lengths[link_idx]);
  } else {
    share->tgt_filedsns_lengths[link_idx] = 0;
    share->tgt_filedsns[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_XA_MEMBER_DRIVER_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_XA_MEMBER_DRIVER_POS]))
  ) {
    share->tgt_drivers_lengths[link_idx] = strlen(ptr);
    share->tgt_drivers[link_idx] =
      spider_create_string(ptr, share->tgt_drivers_lengths[link_idx]);
  } else {
    share->tgt_drivers_lengths[link_idx] = 0;
    share->tgt_drivers[link_idx] = NULL;
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
  ptr = get_field(mem_root, table->field[SPIDER_XA_STATUS_POS]);
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
  if ((ptr = get_field(mem_root, table->field[SPIDER_TABLES_DB_NAME_POS])))
  {
    *db_name = spider_create_string(ptr, strlen(ptr));
  } else {
    *db_name = NULL;
  }
  if ((ptr = get_field(mem_root, table->field[SPIDER_TABLES_TABLE_NAME_POS])))
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
  DBUG_PRINT("info",("spider link_idx:%d", link_idx));
  if ((ptr = get_field(mem_root, table->field[SPIDER_TABLES_PRIORITY_POS])))
  {
    share->priority = my_strtoll10(ptr, (char**) NULL, &error_num);
  } else
    share->priority = 1000000;
  DBUG_PRINT("info",("spider priority:%lld", share->priority));
  if (
    !table->field[SPIDER_TABLES_SERVER_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_SERVER_POS]))
  ) {
    share->server_names_lengths[link_idx] = strlen(ptr);
    share->server_names[link_idx] =
      spider_create_string(ptr, share->server_names_lengths[link_idx]);
    DBUG_PRINT("info",("spider server_name:%s",
      share->server_names[link_idx]));
  } else {
    share->server_names_lengths[link_idx] = 0;
    share->server_names[link_idx] = NULL;
    DBUG_PRINT("info",("spider server_name is NULL"));
  }
  if (
    !table->field[SPIDER_TABLES_SCHEME_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_SCHEME_POS]))
  ) {
    share->tgt_wrappers_lengths[link_idx] = strlen(ptr);
    share->tgt_wrappers[link_idx] =
      spider_create_string(ptr, share->tgt_wrappers_lengths[link_idx]);
    DBUG_PRINT("info",("spider tgt_wrapper:%s",
      share->tgt_wrappers[link_idx]));
  } else {
    share->tgt_wrappers_lengths[link_idx] = 0;
    share->tgt_wrappers[link_idx] = NULL;
    DBUG_PRINT("info",("spider tgt_wrapper is NULL"));
  }
  if (
    !table->field[SPIDER_TABLES_HOST_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_HOST_POS]))
  ) {
    share->tgt_hosts_lengths[link_idx] = strlen(ptr);
    share->tgt_hosts[link_idx] =
      spider_create_string(ptr, share->tgt_hosts_lengths[link_idx]);
    DBUG_PRINT("info",("spider tgt_host:%s",
      share->tgt_hosts[link_idx]));
  } else {
    share->tgt_hosts_lengths[link_idx] = 0;
    share->tgt_hosts[link_idx] = NULL;
    DBUG_PRINT("info",("spider tgt_host is NULL"));
  }
  if (
    !table->field[SPIDER_TABLES_PORT_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_PORT_POS]))
  ) {
    share->tgt_ports[link_idx] = atol(ptr);
  } else {
    share->tgt_ports[link_idx] = -1;
  }
  DBUG_PRINT("info",("spider port:%ld", share->tgt_ports[link_idx]));
  if (
    !table->field[SPIDER_TABLES_SOCKET_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_SOCKET_POS]))
  ) {
    share->tgt_sockets_lengths[link_idx] = strlen(ptr);
    share->tgt_sockets[link_idx] =
      spider_create_string(ptr, share->tgt_sockets_lengths[link_idx]);
  } else {
    share->tgt_sockets_lengths[link_idx] = 0;
    share->tgt_sockets[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_USERNAME_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_USERNAME_POS]))
  ) {
    share->tgt_usernames_lengths[link_idx] = strlen(ptr);
    share->tgt_usernames[link_idx] =
      spider_create_string(ptr, share->tgt_usernames_lengths[link_idx]);
  } else {
    share->tgt_usernames_lengths[link_idx] = 0;
    share->tgt_usernames[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_PASSWORD_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_PASSWORD_POS]))
  ) {
    share->tgt_passwords_lengths[link_idx] = strlen(ptr);
    share->tgt_passwords[link_idx] =
      spider_create_string(ptr, share->tgt_passwords_lengths[link_idx]);
  } else {
    share->tgt_passwords_lengths[link_idx] = 0;
    share->tgt_passwords[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_SSL_CA_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_SSL_CA_POS]))
  ) {
    share->tgt_ssl_cas_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_cas[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_cas_lengths[link_idx]);
  } else {
    share->tgt_ssl_cas_lengths[link_idx] = 0;
    share->tgt_ssl_cas[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_SSL_CAPATH_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_SSL_CAPATH_POS]))
  ) {
    share->tgt_ssl_capaths_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_capaths[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_capaths_lengths[link_idx]);
  } else {
    share->tgt_ssl_capaths_lengths[link_idx] = 0;
    share->tgt_ssl_capaths[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_SSL_CERT_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_SSL_CERT_POS]))
  ) {
    share->tgt_ssl_certs_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_certs[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_certs_lengths[link_idx]);
  } else {
    share->tgt_ssl_certs_lengths[link_idx] = 0;
    share->tgt_ssl_certs[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_SSL_CIPHER_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_SSL_CIPHER_POS]))
  ) {
    share->tgt_ssl_ciphers_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_ciphers[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_ciphers_lengths[link_idx]);
  } else {
    share->tgt_ssl_ciphers_lengths[link_idx] = 0;
    share->tgt_ssl_ciphers[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_SSL_KEY_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_SSL_KEY_POS]))
  ) {
    share->tgt_ssl_keys_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_keys[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_keys_lengths[link_idx]);
  } else {
    share->tgt_ssl_keys_lengths[link_idx] = 0;
    share->tgt_ssl_keys[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_SSL_VERIFY_SERVER_CERT_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_TABLES_SSL_VERIFY_SERVER_CERT_POS]))
  ) {
    share->tgt_ssl_vscs[link_idx] = atol(ptr);
  } else
    share->tgt_ssl_vscs[link_idx] = -1;
  if (
    !table->field[SPIDER_TABLES_MONITORING_BINLOG_POS_AT_FAILING_POS]->
      is_null() &&
    (ptr = get_field(mem_root, table->
      field[SPIDER_TABLES_MONITORING_BINLOG_POS_AT_FAILING_POS]))
  ) {
    share->monitoring_binlog_pos_at_failing[link_idx] = atol(ptr);
  } else
    share->monitoring_binlog_pos_at_failing[link_idx] = 0;
  if (
    !table->field[SPIDER_TABLES_DEFAULT_FILE_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_DEFAULT_FILE_POS]))
  ) {
    share->tgt_default_files_lengths[link_idx] = strlen(ptr);
    share->tgt_default_files[link_idx] =
      spider_create_string(ptr, share->tgt_default_files_lengths[link_idx]);
  } else {
    share->tgt_default_files_lengths[link_idx] = 0;
    share->tgt_default_files[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_DEFAULT_GROUP_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_DEFAULT_GROUP_POS]))
  ) {
    share->tgt_default_groups_lengths[link_idx] = strlen(ptr);
    share->tgt_default_groups[link_idx] =
      spider_create_string(ptr, share->tgt_default_groups_lengths[link_idx]);
  } else {
    share->tgt_default_groups_lengths[link_idx] = 0;
    share->tgt_default_groups[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_DSN_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_DSN_POS]))
  ) {
    share->tgt_dsns_lengths[link_idx] = strlen(ptr);
    share->tgt_dsns[link_idx] =
      spider_create_string(ptr, share->tgt_dsns_lengths[link_idx]);
  } else {
    share->tgt_dsns_lengths[link_idx] = 0;
    share->tgt_dsns[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_FILEDSN_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_FILEDSN_POS]))
  ) {
    share->tgt_filedsns_lengths[link_idx] = strlen(ptr);
    share->tgt_filedsns[link_idx] =
      spider_create_string(ptr, share->tgt_filedsns_lengths[link_idx]);
  } else {
    share->tgt_filedsns_lengths[link_idx] = 0;
    share->tgt_filedsns[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_DRIVER_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_DRIVER_POS]))
  ) {
    share->tgt_drivers_lengths[link_idx] = strlen(ptr);
    share->tgt_drivers[link_idx] =
      spider_create_string(ptr, share->tgt_drivers_lengths[link_idx]);
  } else {
    share->tgt_drivers_lengths[link_idx] = 0;
    share->tgt_drivers[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_TGT_DB_NAME_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_TGT_DB_NAME_POS]))
  ) {
    share->tgt_dbs_lengths[link_idx] = strlen(ptr);
    share->tgt_dbs[link_idx] =
      spider_create_string(ptr, share->tgt_dbs_lengths[link_idx]);
  } else {
    share->tgt_dbs_lengths[link_idx] = 0;
    share->tgt_dbs[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_TGT_TABLE_NAME_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_TGT_TABLE_NAME_POS]))
  ) {
    share->tgt_table_names_lengths[link_idx] = strlen(ptr);
    share->tgt_table_names[link_idx] =
      spider_create_string(ptr, share->tgt_table_names_lengths[link_idx]);
  } else {
    share->tgt_table_names_lengths[link_idx] = 0;
    share->tgt_table_names[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]))
  ) {
    share->static_link_ids_lengths[link_idx] = strlen(ptr);
    share->static_link_ids[link_idx] =
      spider_create_string(ptr, share->static_link_ids_lengths[link_idx]);
  } else {
    share->static_link_ids_lengths[link_idx] = 0;
    share->static_link_ids[link_idx] = NULL;
  }
  DBUG_RETURN(error_num);
}

int spider_get_sys_tables_monitoring_binlog_pos_at_failing(
  TABLE *table,
  long *monitoring_binlog_pos_at_failing,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_tables_monitoring_binlog_pos_at_failing");
  if ((ptr = get_field(mem_root, table->
    field[SPIDER_TABLES_MONITORING_BINLOG_POS_AT_FAILING_POS])))
    *monitoring_binlog_pos_at_failing = (long) my_strtoll10(ptr, (char**) NULL,
      &error_num);
  else
    *monitoring_binlog_pos_at_failing = 1;
  DBUG_PRINT("info",("spider monitoring_binlog_pos_at_failing=%ld",
    *monitoring_binlog_pos_at_failing));
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
  if ((ptr = get_field(mem_root, table->field[SPIDER_TABLES_LINK_STATUS_POS])))
  {
    share->link_statuses[link_idx] =
      (long) my_strtoll10(ptr, (char**) NULL, &error_num);
  } else
    share->link_statuses[link_idx] = 1;
  DBUG_PRINT("info",("spider link_statuses[%d]=%ld",
    link_idx, share->link_statuses[link_idx]));
  DBUG_RETURN(error_num);
}

int spider_get_sys_tables_link_status(
  TABLE *table,
  long *link_status,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_tables_link_status");
  if ((ptr = get_field(mem_root, table->field[SPIDER_TABLES_LINK_STATUS_POS])))
    *link_status = (long) my_strtoll10(ptr, (char**) NULL, &error_num);
  else
    *link_status = 1;
  DBUG_PRINT("info",("spider link_statuses=%ld", *link_status));
  DBUG_RETURN(error_num);
}

int spider_get_sys_tables_link_idx(
  TABLE *table,
  int *link_idx,
  MEM_ROOT *mem_root
) {
  char *ptr;
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_tables_link_idx");
  if ((ptr = get_field(mem_root, table->field[SPIDER_TABLES_LINK_ID_POS])))
    *link_idx = (int) my_strtoll10(ptr, (char**) NULL, &error_num);
  else
    *link_idx = 1;
  DBUG_PRINT("info",("spider link_idx=%d", *link_idx));
  DBUG_RETURN(error_num);
}

int spider_get_sys_tables_static_link_id(
  TABLE *table,
  char **static_link_id,
  uint *static_link_id_length,
  MEM_ROOT *mem_root
) {
  int error_num = 0;
  DBUG_ENTER("spider_get_sys_tables_static_link_id");
  *static_link_id = NULL;
  if (
    !table->field[SPIDER_TABLES_STATIC_LINK_ID_POS]->is_null() &&
    (*static_link_id = get_field(mem_root, table->
      field[SPIDER_TABLES_STATIC_LINK_ID_POS]))
  ) {
    *static_link_id_length = strlen(*static_link_id);
  } else {
    *static_link_id_length = 0;
  }
  DBUG_PRINT("info",("spider static_link_id=%s", *static_link_id ? *static_link_id : "NULL"));
  DBUG_RETURN(error_num);
}

void spider_get_sys_table_sts_info(
  TABLE *table,
  ha_statistics *stat
) {
  MYSQL_TIME mysql_time;
  uint not_used_uint;
  long not_used_long;
  DBUG_ENTER("spider_get_sys_table_sts_info");
  stat->data_file_length = (ulonglong) table->
    field[SPIDER_TABLE_STS_DATA_FILE_LENGTH_POS]->val_int();
  stat->max_data_file_length = (ulonglong) table->
    field[SPIDER_TABLE_STS_MAX_DATA_FILE_LENGTH_POS]->val_int();
  stat->index_file_length = (ulonglong) table->
    field[SPIDER_TABLE_STS_INDEX_FILE_LENGTH_POS]->val_int();
  stat->records = (ha_rows) table->
    field[SPIDER_TABLE_STS_RECORDS_POS]->val_int();
  stat->mean_rec_length = (ulong) table->
    field[SPIDER_TABLE_STS_MEAN_REC_LENGTH_POS]->val_int();
  table->field[SPIDER_TABLE_STS_CHECK_TIME_POS]->get_date(&mysql_time,
    SPIDER_date_mode_t(0));
  stat->check_time = (time_t) my_system_gmt_sec(&mysql_time,
    &not_used_long, &not_used_uint);
  table->field[SPIDER_TABLE_STS_CREATE_TIME_POS]->get_date(&mysql_time,
    SPIDER_date_mode_t(0));
  stat->create_time = (time_t) my_system_gmt_sec(&mysql_time,
    &not_used_long, &not_used_uint);
  table->field[SPIDER_TABLE_STS_UPDATE_TIME_POS]->get_date(&mysql_time,
    SPIDER_date_mode_t(0));
  stat->update_time = (time_t) my_system_gmt_sec(&mysql_time,
    &not_used_long, &not_used_uint);
  if (table->field[SPIDER_TABLE_STS_CHECKSUM_POS]->is_null())
  {
    stat->checksum_null = TRUE;
    stat->checksum = 0;
  } else {
    stat->checksum_null = FALSE;
    stat->checksum = (ha_checksum) table->
      field[SPIDER_TABLE_STS_CHECKSUM_POS]->val_int();
  }
  DBUG_VOID_RETURN;
}

void spider_get_sys_table_crd_info(
  TABLE *table,
  longlong *cardinality,
  uint number_of_keys
) {
  uint seq;
  DBUG_ENTER("spider_get_sys_table_crd_info");
  seq = (uint) table->field[SPIDER_TABLE_CRD_KEY_SEQ_POS]->val_int();
  if (seq < number_of_keys)
  {
    cardinality[seq] = (longlong) table->
      field[SPIDER_TABLE_CRD_CARDINALITY_POS]->val_int();
  }
  DBUG_VOID_RETURN;
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
  SPIDER_Open_tables_backup open_tables_backup;
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
  SPIDER_Open_tables_backup open_tables_backup;
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
  SPIDER_Open_tables_backup open_tables_backup;
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
    table->field[SPIDER_LINK_MON_SERVERS_DB_NAME_POS]->is_null() ||
    table->field[SPIDER_LINK_MON_SERVERS_TABLE_NAME_POS]->is_null() ||
    table->field[SPIDER_LINK_MON_SERVERS_LINK_ID_POS]->is_null()
  ) {
    my_printf_error(ER_SPIDER_SYS_TABLE_VERSION_NUM,
      ER_SPIDER_SYS_TABLE_VERSION_STR, MYF(0),
      SPIDER_SYS_LINK_MON_TABLE_NAME_STR);
    DBUG_RETURN(ER_SPIDER_SYS_TABLE_VERSION_NUM);
  }

  if (
    !(db_name = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_DB_NAME_POS])) ||
    !(table_name = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_TABLE_NAME_POS])) ||
    !(link_id = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_LINK_ID_POS]))
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
  if ((ptr = get_field(mem_root,
    table->field[SPIDER_LINK_MON_SERVERS_SID_POS])))
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
    !table->field[SPIDER_LINK_MON_SERVERS_SERVER_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SERVER_POS]))
  ) {
    share->server_names_lengths[link_idx] = strlen(ptr);
    share->server_names[link_idx] =
      spider_create_string(ptr, share->server_names_lengths[link_idx]);
  } else {
    share->server_names_lengths[link_idx] = 0;
    share->server_names[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_SCHEME_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SCHEME_POS]))
  ) {
    share->tgt_wrappers_lengths[link_idx] = strlen(ptr);
    share->tgt_wrappers[link_idx] =
      spider_create_string(ptr, share->tgt_wrappers_lengths[link_idx]);
  } else {
    share->tgt_wrappers_lengths[link_idx] = 0;
    share->tgt_wrappers[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_HOST_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_LINK_MON_SERVERS_HOST_POS]))
  ) {
    share->tgt_hosts_lengths[link_idx] = strlen(ptr);
    share->tgt_hosts[link_idx] =
      spider_create_string(ptr, share->tgt_hosts_lengths[link_idx]);
  } else {
    share->tgt_hosts_lengths[link_idx] = 0;
    share->tgt_hosts[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_PORT_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_LINK_MON_SERVERS_PORT_POS]))
  ) {
    share->tgt_ports[link_idx] = atol(ptr);
  } else {
    share->tgt_ports[link_idx] = -1;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_SOCKET_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SOCKET_POS]))
  ) {
    share->tgt_sockets_lengths[link_idx] = strlen(ptr);
    share->tgt_sockets[link_idx] =
      spider_create_string(ptr, share->tgt_sockets_lengths[link_idx]);
  } else {
    share->tgt_sockets_lengths[link_idx] = 0;
    share->tgt_sockets[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_USERNAME_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_USERNAME_POS]))
  ) {
    share->tgt_usernames_lengths[link_idx] = strlen(ptr);
    share->tgt_usernames[link_idx] =
      spider_create_string(ptr, share->tgt_usernames_lengths[link_idx]);
  } else {
    share->tgt_usernames_lengths[link_idx] = 0;
    share->tgt_usernames[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_PASSWORD_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_PASSWORD_POS]))
  ) {
    share->tgt_passwords_lengths[link_idx] = strlen(ptr);
    share->tgt_passwords[link_idx] =
      spider_create_string(ptr, share->tgt_passwords_lengths[link_idx]);
  } else {
    share->tgt_passwords_lengths[link_idx] = 0;
    share->tgt_passwords[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_SSL_CA_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SSL_CA_POS]))
  ) {
    share->tgt_ssl_cas_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_cas[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_cas_lengths[link_idx]);
  } else {
    share->tgt_ssl_cas_lengths[link_idx] = 0;
    share->tgt_ssl_cas[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_SSL_CAPATH_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SSL_CAPATH_POS]))
  ) {
    share->tgt_ssl_capaths_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_capaths[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_capaths_lengths[link_idx]);
  } else {
    share->tgt_ssl_capaths_lengths[link_idx] = 0;
    share->tgt_ssl_capaths[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_SSL_CERT_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SSL_CERT_POS]))
  ) {
    share->tgt_ssl_certs_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_certs[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_certs_lengths[link_idx]);
  } else {
    share->tgt_ssl_certs_lengths[link_idx] = 0;
    share->tgt_ssl_certs[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_SSL_CIPHER_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SSL_CIPHER_POS]))
  ) {
    share->tgt_ssl_ciphers_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_ciphers[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_ciphers_lengths[link_idx]);
  } else {
    share->tgt_ssl_ciphers_lengths[link_idx] = 0;
    share->tgt_ssl_ciphers[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_SSL_KEY_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SSL_KEY_POS]))
  ) {
    share->tgt_ssl_keys_lengths[link_idx] = strlen(ptr);
    share->tgt_ssl_keys[link_idx] =
      spider_create_string(ptr, share->tgt_ssl_keys_lengths[link_idx]);
  } else {
    share->tgt_ssl_keys_lengths[link_idx] = 0;
    share->tgt_ssl_keys[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_SSL_VERIFY_SERVER_CERT_POS]->
      is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_SSL_VERIFY_SERVER_CERT_POS]))
  ) {
    share->tgt_ssl_vscs[link_idx] = atol(ptr);
  } else
    share->tgt_ssl_vscs[link_idx] = -1;
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_DEFAULT_FILE_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_DEFAULT_FILE_POS]))
  ) {
    share->tgt_default_files_lengths[link_idx] = strlen(ptr);
    share->tgt_default_files[link_idx] =
      spider_create_string(ptr, share->tgt_default_files_lengths[link_idx]);
  } else {
    share->tgt_default_files_lengths[link_idx] = 0;
    share->tgt_default_files[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_DEFAULT_GROUP_POS]->is_null() &&
    (ptr = get_field(mem_root,
      table->field[SPIDER_LINK_MON_SERVERS_DEFAULT_GROUP_POS]))
  ) {
    share->tgt_default_groups_lengths[link_idx] = strlen(ptr);
    share->tgt_default_groups[link_idx] =
      spider_create_string(ptr, share->tgt_default_groups_lengths[link_idx]);
  } else {
    share->tgt_default_groups_lengths[link_idx] = 0;
    share->tgt_default_groups[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_DSN_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_LINK_MON_SERVERS_DSN_POS]))
  ) {
    share->tgt_dsns_lengths[link_idx] = strlen(ptr);
    share->tgt_dsns[link_idx] =
      spider_create_string(ptr, share->tgt_dsns_lengths[link_idx]);
  } else {
    share->tgt_dsns_lengths[link_idx] = 0;
    share->tgt_dsns[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_FILEDSN_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_LINK_MON_SERVERS_FILEDSN_POS]))
  ) {
    share->tgt_filedsns_lengths[link_idx] = strlen(ptr);
    share->tgt_filedsns[link_idx] =
      spider_create_string(ptr, share->tgt_filedsns_lengths[link_idx]);
  } else {
    share->tgt_filedsns_lengths[link_idx] = 0;
    share->tgt_filedsns[link_idx] = NULL;
  }
  if (
    !table->field[SPIDER_LINK_MON_SERVERS_DRIVER_POS]->is_null() &&
    (ptr = get_field(mem_root, table->field[SPIDER_LINK_MON_SERVERS_DRIVER_POS]))
  ) {
    share->tgt_drivers_lengths[link_idx] = strlen(ptr);
    share->tgt_drivers[link_idx] =
      spider_create_string(ptr, share->tgt_drivers_lengths[link_idx]);
  } else {
    share->tgt_drivers_lengths[link_idx] = 0;
    share->tgt_drivers[link_idx] = NULL;
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

int spider_sys_insert_or_update_table_sts(
  THD *thd,
  const char *name,
  uint name_length,
  ha_statistics *stat,
  bool need_lock
) {
  int error_num;
  TABLE *table_sts = NULL;
  SPIDER_Open_tables_backup open_tables_backup;
  DBUG_ENTER("spider_sys_insert_or_update_table_sts");
  if (
    !(table_sts = spider_open_sys_table(
      thd, SPIDER_SYS_TABLE_STS_TABLE_NAME_STR,
      SPIDER_SYS_TABLE_STS_TABLE_NAME_LEN, TRUE,
      &open_tables_backup, need_lock, &error_num))
  ) {
    goto error;
  }
  if ((error_num = spider_insert_or_update_table_sts(
    table_sts,
    name,
    name_length,
    stat
  )))
    goto error;
  spider_close_sys_table(thd, table_sts, &open_tables_backup, need_lock);
  table_sts = NULL;
  DBUG_RETURN(0);

error:
  if (table_sts)
    spider_close_sys_table(thd, table_sts, &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

int spider_sys_insert_or_update_table_crd(
  THD *thd,
  const char *name,
  uint name_length,
  longlong *cardinality,
  uint number_of_keys,
  bool need_lock
) {
  int error_num;
  TABLE *table_crd = NULL;
  SPIDER_Open_tables_backup open_tables_backup;
  DBUG_ENTER("spider_sys_insert_or_update_table_crd");
  if (
    !(table_crd = spider_open_sys_table(
      thd, SPIDER_SYS_TABLE_CRD_TABLE_NAME_STR,
      SPIDER_SYS_TABLE_CRD_TABLE_NAME_LEN, TRUE,
      &open_tables_backup, need_lock, &error_num))
  ) {
    goto error;
  }
  if ((error_num = spider_insert_or_update_table_crd(
    table_crd,
    name,
    name_length,
    cardinality,
    number_of_keys
  )))
    goto error;
  spider_close_sys_table(thd, table_crd, &open_tables_backup, need_lock);
  table_crd = NULL;
  DBUG_RETURN(0);

error:
  if (table_crd)
    spider_close_sys_table(thd, table_crd, &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

int spider_sys_delete_table_sts(
  THD *thd,
  const char *name,
  uint name_length,
  bool need_lock
) {
  int error_num;
  TABLE *table_sts = NULL;
  SPIDER_Open_tables_backup open_tables_backup;
  DBUG_ENTER("spider_sys_delete_table_sts");
  if (
    !(table_sts = spider_open_sys_table(
      thd, SPIDER_SYS_TABLE_STS_TABLE_NAME_STR,
      SPIDER_SYS_TABLE_STS_TABLE_NAME_LEN, TRUE,
      &open_tables_backup, need_lock, &error_num))
  ) {
    goto error;
  }
  if ((error_num = spider_delete_table_sts(
    table_sts,
    name,
    name_length
  )))
    goto error;
  spider_close_sys_table(thd, table_sts, &open_tables_backup, need_lock);
  table_sts = NULL;
  DBUG_RETURN(0);

error:
  if (table_sts)
    spider_close_sys_table(thd, table_sts, &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

int spider_sys_delete_table_crd(
  THD *thd,
  const char *name,
  uint name_length,
  bool need_lock
) {
  int error_num;
  TABLE *table_crd = NULL;
  SPIDER_Open_tables_backup open_tables_backup;
  DBUG_ENTER("spider_sys_delete_table_crd");
  if (
    !(table_crd = spider_open_sys_table(
      thd, SPIDER_SYS_TABLE_CRD_TABLE_NAME_STR,
      SPIDER_SYS_TABLE_CRD_TABLE_NAME_LEN, TRUE,
      &open_tables_backup, need_lock, &error_num))
  ) {
    goto error;
  }
  if ((error_num = spider_delete_table_crd(
    table_crd,
    name,
    name_length
  )))
    goto error;
  spider_close_sys_table(thd, table_crd, &open_tables_backup, need_lock);
  table_crd = NULL;
  DBUG_RETURN(0);

error:
  if (table_crd)
    spider_close_sys_table(thd, table_crd, &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

int spider_sys_get_table_sts(
  THD *thd,
  const char *name,
  uint name_length,
  ha_statistics *stat,
  bool need_lock
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  TABLE *table_sts = NULL;
  SPIDER_Open_tables_backup open_tables_backup;
  DBUG_ENTER("spider_sys_get_table_sts");
  if (
    !(table_sts = spider_open_sys_table(
      thd, SPIDER_SYS_TABLE_STS_TABLE_NAME_STR,
      SPIDER_SYS_TABLE_STS_TABLE_NAME_LEN, TRUE,
      &open_tables_backup, need_lock, &error_num))
  ) {
    goto error;
  }

  table_sts->use_all_columns();
  spider_store_tables_name(table_sts, name, name_length);
  if ((error_num = spider_check_sys_table(table_sts, table_key)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table_sts->file->print_error(error_num, MYF(0));
    }
    goto error;
  } else {
    spider_get_sys_table_sts_info(
      table_sts,
      stat
    );
  }

  spider_close_sys_table(thd, table_sts, &open_tables_backup, need_lock);
  table_sts = NULL;
  DBUG_RETURN(0);

error:
  if (table_sts)
    spider_close_sys_table(thd, table_sts, &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
}

int spider_sys_get_table_crd(
  THD *thd,
  const char *name,
  uint name_length,
  longlong *cardinality,
  uint number_of_keys,
  bool need_lock
) {
  int error_num;
  char table_key[MAX_KEY_LENGTH];
  bool index_inited = FALSE;
  TABLE *table_crd = NULL;
  SPIDER_Open_tables_backup open_tables_backup;
  DBUG_ENTER("spider_sys_get_table_crd");

  if (
    !(table_crd = spider_open_sys_table(
      thd, SPIDER_SYS_TABLE_CRD_TABLE_NAME_STR,
      SPIDER_SYS_TABLE_CRD_TABLE_NAME_LEN, TRUE,
      &open_tables_backup, need_lock, &error_num))
  ) {
    goto error;
  }

  table_crd->use_all_columns();
  spider_store_tables_name(table_crd, name, name_length);
  if ((error_num = spider_get_sys_table_by_idx(table_crd, table_key, 0,
    SPIDER_SYS_TABLE_CRD_PK_COL_CNT - 1)))
  {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table_crd->file->print_error(error_num, MYF(0));
    }
    goto error;
  } else {
    index_inited = TRUE;
    do {
      spider_get_sys_table_crd_info(
        table_crd,
        cardinality,
        number_of_keys
      );
      error_num = spider_sys_index_next_same(table_crd, table_key);
    } while (error_num == 0);
  }
  index_inited = FALSE;
  if ((error_num = spider_sys_index_end(table_crd)))
  {
    table_crd->file->print_error(error_num, MYF(0));
    goto error;
  }

  spider_close_sys_table(thd, table_crd, &open_tables_backup, need_lock);
  table_crd = NULL;
  DBUG_RETURN(0);

error:
  if (index_inited)
    spider_sys_index_end(table_crd);

  if (table_crd)
    spider_close_sys_table(thd, table_crd, &open_tables_backup, need_lock);
  DBUG_RETURN(error_num);
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
      error_num = table->file->ha_rnd_pos(table->record[1],
        table->file->dup_ref);
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
      error_num = table->file->ha_index_read_idx_map(table->record[1], key_num,
        (const uchar*)table_key, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
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

#ifdef SPIDER_use_LEX_CSTRING_for_Field_blob_constructor
TABLE *spider_mk_sys_tmp_table(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const LEX_CSTRING *field_name,
  CHARSET_INFO *cs
)
#else
TABLE *spider_mk_sys_tmp_table(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const char *field_name,
  CHARSET_INFO *cs
)
#endif
{
  Field_blob *field;
  Item_field *i_field;
  List<Item> i_list;
  TABLE *tmp_table;
  DBUG_ENTER("spider_mk_sys_tmp_table");

#ifdef SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
  if (!(field = new (thd->mem_root) Field_blob(
    4294967295U, FALSE, field_name, cs, TRUE)))
    goto error_alloc_field;
#else
  if (!(field = new Field_blob(
    4294967295U, FALSE, field_name, cs, TRUE)))
    goto error_alloc_field;
#endif
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
    i_list, (ORDER*) NULL, FALSE, FALSE,
    (TMP_TABLE_FORCE_MYISAM | TMP_TABLE_ALL_COLUMNS),
    HA_POS_ERROR, &SPIDER_empty_string)))
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

#ifdef SPIDER_use_LEX_CSTRING_for_Field_blob_constructor
TABLE *spider_mk_sys_tmp_table_for_result(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const LEX_CSTRING *field_name1,
  const LEX_CSTRING *field_name2,
  const LEX_CSTRING *field_name3,
  CHARSET_INFO *cs
)
#else
TABLE *spider_mk_sys_tmp_table_for_result(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const char *field_name1,
  const char *field_name2,
  const char *field_name3,
  CHARSET_INFO *cs
)
#endif
{
  Field_blob *field1, *field2, *field3;
  Item_field *i_field1, *i_field2, *i_field3;
  List<Item> i_list;
  TABLE *tmp_table;
  DBUG_ENTER("spider_mk_sys_tmp_table_for_result");

#ifdef SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
  if (!(field1 = new (thd->mem_root) Field_blob(
    4294967295U, FALSE, field_name1, cs, TRUE)))
    goto error_alloc_field1;
#else
  if (!(field1 = new Field_blob(
    4294967295U, FALSE, field_name1, cs, TRUE)))
    goto error_alloc_field1;
#endif
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

#ifdef SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
  if (!(field2 = new (thd->mem_root) Field_blob(
    4294967295U, FALSE, field_name2, cs, TRUE)))
    goto error_alloc_field2;
#else
  if (!(field2 = new Field_blob(
    4294967295U, FALSE, field_name2, cs, TRUE)))
    goto error_alloc_field2;
#endif
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

#ifdef SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
  if (!(field3 = new (thd->mem_root) Field_blob(
    4294967295U, FALSE, field_name3, cs, TRUE)))
    goto error_alloc_field3;
#else
  if (!(field3 = new Field_blob(
    4294967295U, FALSE, field_name3, cs, TRUE)))
    goto error_alloc_field3;
#endif
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
    i_list, (ORDER*) NULL, FALSE, FALSE,
    (TMP_TABLE_FORCE_MYISAM | TMP_TABLE_ALL_COLUMNS),
    HA_POS_ERROR, &SPIDER_empty_string)))
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

TABLE *spider_find_temporary_table(
  THD *thd,
  TABLE_LIST *table_list
) {
  DBUG_ENTER("spider_find_temporary_table");
#ifdef SPIDER_open_temporary_table
  if (!thd->has_temporary_tables())
  {
    DBUG_RETURN(NULL);
  }
  if (thd->open_temporary_table(table_list))
  {
    DBUG_RETURN(NULL);
  } else {
    DBUG_RETURN(table_list->table);
  }
#else
  DBUG_RETURN(find_temporary_table(A,B));
#endif
}
