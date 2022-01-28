/* Copyright (C) 2009-2020 Kentoku Shiba
   Copyright (C) 2019-2020 MariaDB corp

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
#include "sql_partition.h"
#include "sql_base.h"
#include "sql_servers.h"
#include "tztime.h"
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_sys_table.h"
#include "ha_spider.h"
#include "spd_db_conn.h"
#include "spd_trx.h"
#include "spd_conn.h"
#include "spd_table.h"
#include "spd_direct_sql.h"
#include "spd_udf.h"
#include "spd_malloc.h"

#define SPIDER_NEED_INIT_ONE_TABLE_FOR_FIND_TEMPORARY_TABLE

extern const char **spd_defaults_extra_file;
extern const char **spd_defaults_file;

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key spd_key_mutex_mta_conn;
extern PSI_mutex_key spd_key_mutex_bg_direct_sql;
extern PSI_cond_key spd_key_cond_bg_direct_sql;
#endif

extern HASH spider_open_connections;
extern HASH spider_ipport_conns;
extern pthread_mutex_t spider_conn_mutex;
extern pthread_mutex_t spider_conn_id_mutex;
extern pthread_mutex_t spider_ipport_conn_mutex;
extern ulonglong spider_conn_id;

/* UTC time zone for timestamp columns */
extern Time_zone *UTC;

uint spider_udf_calc_hash(
  char *key,
  uint mod
) {
  uint sum = 0;
  DBUG_ENTER("spider_udf_calc_hash");
  while (*key != '\0')
  {
    sum += *key;
    key++;
  }
  DBUG_PRINT("info",("spider calc hash = %u", sum % mod));
  DBUG_RETURN(sum % mod);
}

int spider_udf_direct_sql_create_table_list(
  SPIDER_DIRECT_SQL *direct_sql,
  char *table_name_list,
  uint table_name_list_length
) {
  int table_count, roop_count, length;
  char *tmp_ptr, *tmp_ptr2, *tmp_ptr3, *tmp_name_ptr;
  THD *thd = direct_sql->trx->thd;
  DBUG_ENTER("spider_udf_direct_sql_create_table_list");
  tmp_ptr = table_name_list;
  while (*tmp_ptr == ' ')
    tmp_ptr++;
  if (*tmp_ptr)
    table_count = 1;
  else {
    direct_sql->table_count = 0;
    DBUG_RETURN(0);
  }

  while (TRUE)
  {
    if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
    {
      table_count++;
      tmp_ptr = tmp_ptr2 + 1;
      while (*tmp_ptr == ' ')
        tmp_ptr++;
    } else
      break;
  }
  if (!(direct_sql->db_names = (char**)
    spider_bulk_malloc(spider_current_trx, 31, MYF(MY_WME | MY_ZEROFILL),
      &direct_sql->db_names, (uint) (sizeof(char*) * table_count),
      &direct_sql->table_names, (uint) (sizeof(char*) * table_count),
      &direct_sql->tables, (uint) (sizeof(TABLE*) * table_count),
      &tmp_name_ptr, (uint) (sizeof(char) * (
        table_name_list_length +
        SPIDER_THD_db_length(thd) * table_count +
        2 * table_count
      )),
      &direct_sql->iop, (uint) (sizeof(int) * table_count),
      &direct_sql->table_list, (uint) (sizeof(TABLE_LIST) * table_count),
      &direct_sql->real_table_bitmap,
        (uint) (sizeof(uchar) * ((table_count + 7) / 8)),
      NullS))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  tmp_ptr = table_name_list;
  while (*tmp_ptr == ' ')
    tmp_ptr++;
  roop_count = 0;
  while (TRUE)
  {
    if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
      *tmp_ptr2 = '\0';

    direct_sql->db_names[roop_count] = tmp_name_ptr;

    if ((tmp_ptr3 = strchr(tmp_ptr, '.')))
    {
      /* exist database name */
      *tmp_ptr3 = '\0';
      length = strlen(tmp_ptr);
      memcpy(tmp_name_ptr, tmp_ptr, length + 1);
      tmp_name_ptr += length + 1;
      tmp_ptr = tmp_ptr3 + 1;
    } else {
      if (SPIDER_THD_db_str(thd))
      {
        memcpy(tmp_name_ptr, SPIDER_THD_db_str(thd),
          SPIDER_THD_db_length(thd) + 1);
        tmp_name_ptr += SPIDER_THD_db_length(thd) + 1;
      } else {
        direct_sql->db_names[roop_count] = (char *) "";
      }
    }

    direct_sql->table_names[roop_count] = tmp_name_ptr;
    length = strlen(tmp_ptr);
    memcpy(tmp_name_ptr, tmp_ptr, length + 1);
    tmp_name_ptr += length + 1;

    DBUG_PRINT("info",("spider db=%s",
      direct_sql->db_names[roop_count]));
    DBUG_PRINT("info",("spider table_name=%s",
      direct_sql->table_names[roop_count]));

    if (!tmp_ptr2)
      break;
    tmp_ptr = tmp_ptr2 + 1;
    while (*tmp_ptr == ' ')
      tmp_ptr++;
    roop_count++;
  }
  direct_sql->table_count = table_count;
  DBUG_RETURN(0);
}

int spider_udf_direct_sql_create_conn_key(
  SPIDER_DIRECT_SQL *direct_sql
) {
  char *tmp_name, port_str[6];
  DBUG_ENTER("spider_udf_direct_sql_create_conn_key");

  uint roop_count2;
  bool tables_on_different_db_are_joinable = TRUE;
  direct_sql->dbton_id = SPIDER_DBTON_SIZE;
  DBUG_PRINT("info",("spider direct_sql->tgt_wrapper=%s",
    direct_sql->tgt_wrapper));
  for (roop_count2 = 0; roop_count2 < SPIDER_DBTON_SIZE; roop_count2++)
  {
    DBUG_PRINT("info",("spider spider_dbton[%d].wrapper=%s", roop_count2,
      spider_dbton[roop_count2].wrapper ?
        spider_dbton[roop_count2].wrapper : "NULL"));
    if (
      spider_dbton[roop_count2].wrapper &&
      !strcmp(direct_sql->tgt_wrapper, spider_dbton[roop_count2].wrapper)
    ) {
        if (spider_dbton[roop_count2].db_access_type ==
          SPIDER_DB_ACCESS_TYPE_SQL)
        {
          direct_sql->dbton_id = roop_count2;
          break;
        }
    }
  }
  if (direct_sql->dbton_id == SPIDER_DBTON_SIZE)
  {
      my_printf_error(
        ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM,
        ER_SPIDER_SQL_WRAPPER_IS_INVALID_STR,
        MYF(0), direct_sql->tgt_wrapper);
      DBUG_RETURN(ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM);
  }
    tables_on_different_db_are_joinable =
      spider_dbton[direct_sql->dbton_id].db_util->
        tables_on_different_db_are_joinable();
    direct_sql->conn_key_length
      = 1
      + direct_sql->tgt_wrapper_length + 1
      + direct_sql->tgt_host_length + 1
      + 5 + 1
      + direct_sql->tgt_socket_length + 1
      + (tables_on_different_db_are_joinable ?
        0 : direct_sql->tgt_default_db_name_length + 1)
      + direct_sql->tgt_username_length + 1
      + direct_sql->tgt_password_length + 1
      + direct_sql->tgt_ssl_ca_length + 1
      + direct_sql->tgt_ssl_capath_length + 1
      + direct_sql->tgt_ssl_cert_length + 1
      + direct_sql->tgt_ssl_cipher_length + 1
      + direct_sql->tgt_ssl_key_length + 1
      + 1 + 1
      + direct_sql->tgt_default_file_length + 1
      + direct_sql->tgt_default_group_length + 1
      + direct_sql->tgt_dsn_length + 1
      + direct_sql->tgt_filedsn_length + 1
      + direct_sql->tgt_driver_length;
  if (!(direct_sql->conn_key = (char *)
    spider_malloc(spider_current_trx, 9, direct_sql->conn_key_length + 1,
      MYF(MY_WME | MY_ZEROFILL)))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (direct_sql->connection_channel > 48)
    *direct_sql->conn_key = '0' + 48 - direct_sql->connection_channel;
  else
    *direct_sql->conn_key = '0' + direct_sql->connection_channel;
  DBUG_PRINT("info",("spider tgt_wrapper=%s", direct_sql->tgt_wrapper));
  tmp_name = strmov(direct_sql->conn_key + 1, direct_sql->tgt_wrapper);
  if (direct_sql->tgt_host)
  {
    DBUG_PRINT("info",("spider tgt_host=%s", direct_sql->tgt_host));
    tmp_name = strmov(tmp_name + 1, direct_sql->tgt_host);
  } else {
    DBUG_PRINT("info",("spider tgt_host=NULL"));
    tmp_name++;
  }
  my_sprintf(port_str, (port_str, "%05ld", direct_sql->tgt_port));
  DBUG_PRINT("info",("spider port_str=%s", port_str));
  tmp_name = strmov(tmp_name + 1, port_str);
  if (direct_sql->tgt_socket)
  {
    DBUG_PRINT("info",("spider tgt_socket=%s", direct_sql->tgt_socket));
    tmp_name = strmov(tmp_name + 1, direct_sql->tgt_socket);
  }
  else
  {
    tmp_name++;
  }
    if (!tables_on_different_db_are_joinable)
    {
      if (direct_sql->tgt_default_db_name)
      {
        DBUG_PRINT("info",("spider tgt_default_db_name=%s",
          direct_sql->tgt_default_db_name));
        tmp_name = strmov(tmp_name + 1, direct_sql->tgt_default_db_name);
      } else
        tmp_name++;
    }
    if (direct_sql->tgt_username)
    {
      DBUG_PRINT("info",("spider tgt_username=%s", direct_sql->tgt_username));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_username);
    } else
      tmp_name++;
    if (direct_sql->tgt_password)
    {
      DBUG_PRINT("info",("spider tgt_password=%s", direct_sql->tgt_password));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_password);
    } else
      tmp_name++;
    if (direct_sql->tgt_ssl_ca)
    {
      DBUG_PRINT("info",("spider tgt_ssl_ca=%s", direct_sql->tgt_ssl_ca));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_ssl_ca);
    } else
      tmp_name++;
    if (direct_sql->tgt_ssl_capath)
    {
      DBUG_PRINT("info",("spider tgt_ssl_capath=%s",
        direct_sql->tgt_ssl_capath));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_ssl_capath);
    } else
      tmp_name++;
    if (direct_sql->tgt_ssl_cert)
    {
      DBUG_PRINT("info",("spider tgt_ssl_cert=%s", direct_sql->tgt_ssl_cert));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_ssl_cert);
    } else
      tmp_name++;
    if (direct_sql->tgt_ssl_cipher)
    {
      DBUG_PRINT("info",("spider tgt_ssl_cipher=%s",
        direct_sql->tgt_ssl_cipher));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_ssl_cipher);
    } else
      tmp_name++;
    if (direct_sql->tgt_ssl_key)
    {
      DBUG_PRINT("info",("spider tgt_ssl_key=%s", direct_sql->tgt_ssl_key));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_ssl_key);
    } else
      tmp_name++;
    tmp_name++;
    *tmp_name = '0' + ((char) direct_sql->tgt_ssl_vsc);
    if (direct_sql->tgt_default_file)
    {
      DBUG_PRINT("info",("spider tgt_default_file=%s",
        direct_sql->tgt_default_file));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_default_file);
    } else
      tmp_name++;
    if (direct_sql->tgt_default_group)
    {
      DBUG_PRINT("info",("spider tgt_default_group=%s",
        direct_sql->tgt_default_group));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_default_group);
    } else
      tmp_name++;
    if (direct_sql->tgt_dsn)
    {
      DBUG_PRINT("info",("spider tgt_dsn=%s",
        direct_sql->tgt_dsn));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_dsn);
    } else
      tmp_name++;
    if (direct_sql->tgt_filedsn)
    {
      DBUG_PRINT("info",("spider tgt_filedsn=%s",
        direct_sql->tgt_filedsn));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_filedsn);
    } else
      tmp_name++;
    if (direct_sql->tgt_driver)
    {
      DBUG_PRINT("info",("spider tgt_driver=%s",
        direct_sql->tgt_driver));
      tmp_name = strmov(tmp_name + 1, direct_sql->tgt_driver);
    } else
      tmp_name++;
  direct_sql->conn_key_hash_value = my_calc_hash(&spider_open_connections,
    (uchar*) direct_sql->conn_key, direct_sql->conn_key_length);
  DBUG_RETURN(0);
}

SPIDER_CONN *spider_udf_direct_sql_create_conn(
  const SPIDER_DIRECT_SQL *direct_sql,
  int *error_num
) {
  SPIDER_CONN *conn;
  SPIDER_IP_PORT_CONN *ip_port_conn;
  char *tmp_name, *tmp_host, *tmp_username, *tmp_password, *tmp_socket;
  char *tmp_wrapper, *tmp_db, *tmp_ssl_ca, *tmp_ssl_capath, *tmp_ssl_cert;
  char *tmp_ssl_cipher, *tmp_ssl_key, *tmp_default_file, *tmp_default_group;
  char *tmp_dsn, *tmp_filedsn, *tmp_driver;
  int *need_mon;
  bool tables_on_different_db_are_joinable = TRUE;
  DBUG_ENTER("spider_udf_direct_sql_create_conn");

  if (unlikely(!UTC))
  {
    /* UTC time zone for timestamp columns */
    String tz_00_name(STRING_WITH_LEN("+00:00"), &my_charset_bin);
    UTC = my_tz_find(current_thd, &tz_00_name);
  }

    tables_on_different_db_are_joinable =
      spider_dbton[direct_sql->dbton_id].db_util->
        tables_on_different_db_are_joinable();
    if (!(conn = (SPIDER_CONN *)
      spider_bulk_malloc(spider_current_trx, 32, MYF(MY_WME | MY_ZEROFILL),
        &conn, (uint) (sizeof(*conn)),
        &tmp_name, (uint) (direct_sql->conn_key_length + 1),
        &tmp_host, (uint) (direct_sql->tgt_host_length + 1),
        &tmp_username, (uint) (direct_sql->tgt_username_length + 1),
        &tmp_password, (uint) (direct_sql->tgt_password_length + 1),
        &tmp_socket, (uint) (direct_sql->tgt_socket_length + 1),
        &tmp_wrapper, (uint) (direct_sql->tgt_wrapper_length + 1),
        &tmp_db, (uint) (tables_on_different_db_are_joinable ?
          0 : direct_sql->tgt_default_db_name_length + 1),
        &tmp_ssl_ca, (uint) (direct_sql->tgt_ssl_ca_length + 1),
        &tmp_ssl_capath, (uint) (direct_sql->tgt_ssl_capath_length + 1),
        &tmp_ssl_cert, (uint) (direct_sql->tgt_ssl_cert_length + 1),
        &tmp_ssl_cipher, (uint) (direct_sql->tgt_ssl_cipher_length + 1),
        &tmp_ssl_key, (uint) (direct_sql->tgt_ssl_key_length + 1),
        &tmp_default_file,
          (uint) (direct_sql->tgt_default_file_length + 1),
        &tmp_default_group,
          (uint) (direct_sql->tgt_default_group_length + 1),
        &tmp_dsn,
          (uint) (direct_sql->tgt_dsn_length + 1),
        &tmp_filedsn,
          (uint) (direct_sql->tgt_filedsn_length + 1),
        &tmp_driver,
          (uint) (direct_sql->tgt_driver_length + 1),
        &need_mon, (uint) (sizeof(int)),
        NullS))
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc_conn;
    }
    conn->default_database.init_calc_mem(138);

  conn->conn_key_length = direct_sql->conn_key_length;
  conn->conn_key = tmp_name;
  memcpy(conn->conn_key, direct_sql->conn_key, direct_sql->conn_key_length);
  conn->tgt_wrapper_length = direct_sql->tgt_wrapper_length;
  conn->tgt_wrapper = tmp_wrapper;
  memcpy(conn->tgt_wrapper, direct_sql->tgt_wrapper,
    direct_sql->tgt_wrapper_length);
  conn->tgt_host_length = direct_sql->tgt_host_length;
  conn->tgt_host = tmp_host;
  memcpy(conn->tgt_host, direct_sql->tgt_host, direct_sql->tgt_host_length);
    conn->tgt_port = direct_sql->tgt_port;
    conn->tgt_socket_length = direct_sql->tgt_socket_length;
    conn->tgt_socket = tmp_socket;
    memcpy(conn->tgt_socket, direct_sql->tgt_socket,
      direct_sql->tgt_socket_length);
    if (!tables_on_different_db_are_joinable)
    {
      conn->tgt_db_length = direct_sql->tgt_default_db_name_length;
      conn->tgt_db = tmp_db;
      memcpy(conn->tgt_db, direct_sql->tgt_default_db_name,
        direct_sql->tgt_default_db_name_length);
    }
    conn->tgt_username_length = direct_sql->tgt_username_length;
    conn->tgt_username = tmp_username;
    memcpy(conn->tgt_username, direct_sql->tgt_username,
      direct_sql->tgt_username_length);
    conn->tgt_password_length = direct_sql->tgt_password_length;
    conn->tgt_password = tmp_password;
    memcpy(conn->tgt_password, direct_sql->tgt_password,
      direct_sql->tgt_password_length);
    conn->tgt_ssl_ca_length = direct_sql->tgt_ssl_ca_length;
    if (conn->tgt_ssl_ca_length)
    {
      conn->tgt_ssl_ca = tmp_ssl_ca;
      memcpy(conn->tgt_ssl_ca, direct_sql->tgt_ssl_ca,
        direct_sql->tgt_ssl_ca_length);
    } else
      conn->tgt_ssl_ca = NULL;
    conn->tgt_ssl_capath_length = direct_sql->tgt_ssl_capath_length;
    if (conn->tgt_ssl_capath_length)
    {
      conn->tgt_ssl_capath = tmp_ssl_capath;
      memcpy(conn->tgt_ssl_capath, direct_sql->tgt_ssl_capath,
        direct_sql->tgt_ssl_capath_length);
    } else
      conn->tgt_ssl_capath = NULL;
    conn->tgt_ssl_cert_length = direct_sql->tgt_ssl_cert_length;
    if (conn->tgt_ssl_cert_length)
    {
      conn->tgt_ssl_cert = tmp_ssl_cert;
      memcpy(conn->tgt_ssl_cert, direct_sql->tgt_ssl_cert,
        direct_sql->tgt_ssl_cert_length);
    } else
      conn->tgt_ssl_cert = NULL;
    conn->tgt_ssl_cipher_length = direct_sql->tgt_ssl_cipher_length;
    if (conn->tgt_ssl_cipher_length)
    {
      conn->tgt_ssl_cipher = tmp_ssl_cipher;
      memcpy(conn->tgt_ssl_cipher, direct_sql->tgt_ssl_cipher,
        direct_sql->tgt_ssl_cipher_length);
    } else
      conn->tgt_ssl_cipher = NULL;
    conn->tgt_ssl_key_length = direct_sql->tgt_ssl_key_length;
    if (conn->tgt_ssl_key_length)
    {
      conn->tgt_ssl_key = tmp_ssl_key;
      memcpy(conn->tgt_ssl_key, direct_sql->tgt_ssl_key,
        direct_sql->tgt_ssl_key_length);
    } else
      conn->tgt_ssl_key = NULL;
    conn->tgt_default_file_length = direct_sql->tgt_default_file_length;
    if (conn->tgt_default_file_length)
    {
      conn->tgt_default_file = tmp_default_file;
      memcpy(conn->tgt_default_file, direct_sql->tgt_default_file,
        direct_sql->tgt_default_file_length);
    } else
      conn->tgt_default_file = NULL;
    conn->tgt_default_group_length = direct_sql->tgt_default_group_length;
    if (conn->tgt_default_group_length)
    {
      conn->tgt_default_group = tmp_default_group;
      memcpy(conn->tgt_default_group, direct_sql->tgt_default_group,
        direct_sql->tgt_default_group_length);
    } else
      conn->tgt_default_group = NULL;
    conn->tgt_dsn_length = direct_sql->tgt_dsn_length;
    if (conn->tgt_dsn_length)
    {
      conn->tgt_dsn = tmp_dsn;
      memcpy(conn->tgt_dsn, direct_sql->tgt_dsn,
        direct_sql->tgt_dsn_length);
    } else
      conn->tgt_dsn = NULL;
    conn->tgt_filedsn_length = direct_sql->tgt_filedsn_length;
    if (conn->tgt_filedsn_length)
    {
      conn->tgt_filedsn = tmp_filedsn;
      memcpy(conn->tgt_filedsn, direct_sql->tgt_filedsn,
        direct_sql->tgt_filedsn_length);
    } else
      conn->tgt_filedsn = NULL;
    conn->tgt_driver_length = direct_sql->tgt_driver_length;
    if (conn->tgt_driver_length)
    {
      conn->tgt_driver = tmp_driver;
      memcpy(conn->tgt_driver, direct_sql->tgt_driver,
        direct_sql->tgt_driver_length);
    } else
      conn->tgt_driver = NULL;
    conn->tgt_ssl_vsc = direct_sql->tgt_ssl_vsc;
  conn->dbton_id = direct_sql->dbton_id;
  conn->conn_need_mon = need_mon;
  conn->need_mon = need_mon;
  if (!(conn->db_conn = spider_dbton[conn->dbton_id].create_db_conn(conn)))
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_db_conn_create;
  }
  if ((*error_num = conn->db_conn->init()))
  {
    goto error_db_conn_init;
  }
  conn->join_trx = 0;
  conn->thd = NULL;
  conn->table_lock = 0;
  conn->semi_trx_isolation = -2;
  conn->semi_trx_isolation_chk = FALSE;
  conn->semi_trx_chk = FALSE;
    conn->conn_kind = SPIDER_CONN_KIND_MYSQL;

  if (mysql_mutex_init(spd_key_mutex_mta_conn, &conn->mta_conn_mutex,
    MY_MUTEX_INIT_FAST))
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_mta_conn_mutex_init;
  }

  if (unlikely((*error_num = spider_conn_init(conn))))
  {
    goto error_conn_init;
  }

  if ((*error_num = spider_db_udf_direct_sql_connect(direct_sql, conn)))
    goto error;
  conn->ping_time = (time_t) time((time_t*) 0);
  conn->connect_error_time = conn->ping_time;
  pthread_mutex_lock(&spider_conn_id_mutex);
  conn->conn_id = spider_conn_id;
  ++spider_conn_id;
  pthread_mutex_unlock(&spider_conn_id_mutex);

  pthread_mutex_lock(&spider_ipport_conn_mutex);
  if ((ip_port_conn = (SPIDER_IP_PORT_CONN*) my_hash_search_using_hash_value(
    &spider_ipport_conns, conn->conn_key_hash_value,
    (uchar*)conn->conn_key, conn->conn_key_length)))
  { /* exists, +1 */
    pthread_mutex_unlock(&spider_ipport_conn_mutex);
    pthread_mutex_lock(&ip_port_conn->mutex);
    if (spider_param_max_connections())
    { /* enable conncetion pool */
      if (ip_port_conn->ip_port_count >= spider_param_max_connections())
      { /* bigger than the max num of connections, free conn and return NULL */
        pthread_mutex_unlock(&ip_port_conn->mutex);
        goto error_too_many_ipport_count;
      }
    }
    ip_port_conn->ip_port_count++;
    pthread_mutex_unlock(&ip_port_conn->mutex);
  }
  else
  {// do not exist
    ip_port_conn = spider_create_ipport_conn(conn);
    if (!ip_port_conn) {
      /* failed, always do not effect 'create conn' */
      pthread_mutex_unlock(&spider_ipport_conn_mutex);
      DBUG_RETURN(conn);
    }
    if (my_hash_insert(&spider_ipport_conns, (uchar *)ip_port_conn)) {
      /* insert failed, always do not effect 'create conn' */
      pthread_mutex_unlock(&spider_ipport_conn_mutex);
      DBUG_RETURN(conn);
    }
    pthread_mutex_unlock(&spider_ipport_conn_mutex);
  }
  conn->ip_port_conn = ip_port_conn;

  DBUG_RETURN(conn);

error:
  DBUG_ASSERT(!conn->mta_conn_mutex_file_pos.file_name);
error_too_many_ipport_count:
  spider_conn_done(conn);
error_conn_init:
  pthread_mutex_destroy(&conn->mta_conn_mutex);
error_mta_conn_mutex_init:
error_db_conn_init:
  delete conn->db_conn;
error_db_conn_create:
  spider_free(spider_current_trx, conn, MYF(0));
error_alloc_conn:
  DBUG_RETURN(NULL);
}

SPIDER_CONN *spider_udf_direct_sql_get_conn(
  const SPIDER_DIRECT_SQL *direct_sql,
  SPIDER_TRX *trx,
  int *error_num
) {
  SPIDER_CONN *conn = NULL;
  DBUG_ENTER("spider_udf_direct_sql_get_conn");

  if (
      !(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
        &trx->trx_conn_hash, direct_sql->conn_key_hash_value,
        (uchar*) direct_sql->conn_key, direct_sql->conn_key_length))
  )
  {
    if (
        (
          (spider_param_conn_recycle_mode(trx->thd) & 1) ||
          spider_param_conn_recycle_strict(trx->thd)
        )
    ) {
        pthread_mutex_lock(&spider_conn_mutex);
        if (!(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
          &spider_open_connections, direct_sql->conn_key_hash_value,
          (uchar*) direct_sql->conn_key, direct_sql->conn_key_length)))
        {
          pthread_mutex_unlock(&spider_conn_mutex);
          DBUG_PRINT("info",("spider create new conn"));
          if(!(conn = spider_udf_direct_sql_create_conn(direct_sql,
            error_num)))
            goto error;
        } else {
          my_hash_delete(&spider_open_connections, (uchar*) conn);
          pthread_mutex_unlock(&spider_conn_mutex);
          DBUG_PRINT("info",("spider get global conn"));
        }
    } else {
      DBUG_PRINT("info",("spider create new conn"));
      /* conn_recycle_strict = 0 and conn_recycle_mode = 0 or 2 */
      if(!(conn = spider_udf_direct_sql_create_conn(direct_sql, error_num)))
        goto error;
    }
    conn->thd = trx->thd;
    conn->priority = direct_sql->priority;

      uint old_elements = trx->trx_conn_hash.array.max_element;
      if (my_hash_insert(&trx->trx_conn_hash, (uchar*) conn))
      {
        spider_free_conn(conn);
        *error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
      if (trx->trx_conn_hash.array.max_element > old_elements)
      {
        spider_alloc_calc_mem(spider_current_trx,
          trx->trx_conn_hash,
          (trx->trx_conn_hash.array.max_element - old_elements) *
          trx->trx_conn_hash.array.size_of_element);
      }
  }

  if (conn->queued_connect)
  {
    if ((*error_num = spider_db_udf_direct_sql_connect(direct_sql, conn)))
      goto error;
    conn->queued_connect = FALSE;
  }

  if (conn->queued_ping)
    conn->queued_ping = FALSE;

  DBUG_PRINT("info",("spider conn=%p", conn));
  DBUG_PRINT("info",("spider conn->conn_kind=%u", conn->conn_kind));
  DBUG_RETURN(conn);

error:
  DBUG_RETURN(NULL);
}

int spider_udf_direct_sql_get_server(
  SPIDER_DIRECT_SQL *direct_sql
) {
  MEM_ROOT mem_root;
  int error_num, length;
  FOREIGN_SERVER *server, server_buf;
  DBUG_ENTER("spider_udf_direct_sql_get_server");
  SPD_INIT_ALLOC_ROOT(&mem_root, 65, 0, MYF(MY_WME));

  if (!(server
       = get_server_by_name(&mem_root, direct_sql->server_name, &server_buf)))
  {
    error_num = ER_FOREIGN_SERVER_DOESNT_EXIST;
    goto error_get_server;
  }

  if (!direct_sql->tgt_wrapper && server->scheme)
  {
    direct_sql->tgt_wrapper_length = strlen(server->scheme);
    if (!(direct_sql->tgt_wrapper =
      spider_create_string(server->scheme, direct_sql->tgt_wrapper_length)))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_wrapper=%s", direct_sql->tgt_wrapper));
  }

  if (!direct_sql->tgt_host && server->host)
  {
    direct_sql->tgt_host_length = strlen(server->host);
    if (!(direct_sql->tgt_host =
      spider_create_string(server->host, direct_sql->tgt_host_length)))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_host=%s", direct_sql->tgt_host));
  }

  if (direct_sql->tgt_port == -1)
  {
    direct_sql->tgt_port = server->port;
    DBUG_PRINT("info",("spider tgt_port=%ld", direct_sql->tgt_port));
  }

  if (!direct_sql->tgt_socket && server->socket)
  {
    direct_sql->tgt_socket_length = strlen(server->socket);
    if (!(direct_sql->tgt_socket =
      spider_create_string(server->socket, direct_sql->tgt_socket_length)))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_socket=%s", direct_sql->tgt_socket));
  }

  if (!direct_sql->tgt_default_db_name && server->db &&
    (length = strlen(server->db)))
  {
    direct_sql->tgt_default_db_name_length = length;
    if (!(direct_sql->tgt_default_db_name =
      spider_create_string(server->db, length)))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_default_db_name=%s",
      direct_sql->tgt_default_db_name));
  }

  if (!direct_sql->tgt_username && server->username)
  {
    direct_sql->tgt_username_length = strlen(server->username);
    if (!(direct_sql->tgt_username =
      spider_create_string(server->username, direct_sql->tgt_username_length)))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_username=%s", direct_sql->tgt_username));
  }

  if (!direct_sql->tgt_password && server->password)
  {
    direct_sql->tgt_password_length = strlen(server->password);
    if (!(direct_sql->tgt_password =
      spider_create_string(server->password, direct_sql->tgt_password_length)))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_password=%s", direct_sql->tgt_password));
  }

  free_root(&mem_root, MYF(0));
  DBUG_RETURN(0);

error_get_server:
  my_error(error_num, MYF(0), direct_sql->server_name);
error:
  free_root(&mem_root, MYF(0));
  DBUG_RETURN(error_num);
}

#define SPIDER_PARAM_STR_LEN(name) name ## _length
#define SPIDER_PARAM_STR(title_name, param_name) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (!direct_sql->param_name) \
    { \
      if ((direct_sql->param_name = spider_get_string_between_quote( \
        start_ptr, TRUE, &param_string_parse))) \
        direct_sql->SPIDER_PARAM_STR_LEN(param_name) = \
          strlen(direct_sql->param_name); \
      else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%s", direct_sql->param_name)); \
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
    DBUG_PRINT("info",("spider direct_sql->param_name=%p", \
      direct_sql->param_name)); \
    if (direct_sql->param_name) \
    { \
      if (hint_num < 0 || hint_num >= max_size) \
      { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } else if (direct_sql->param_name[hint_num] != -1) \
        break; \
      char *hint_str = spider_get_string_between_quote(start_ptr, FALSE); \
      if (hint_str) \
      { \
        direct_sql->param_name[hint_num] = atoi(hint_str); \
        if (direct_sql->param_name[hint_num] < min_val) \
          direct_sql->param_name[hint_num] = min_val; \
        else if (direct_sql->param_name[hint_num] > max_val) \
          direct_sql->param_name[hint_num] = max_val; \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "[%d]=%d", hint_num, \
        direct_sql->param_name[hint_num])); \
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
    if (direct_sql->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        direct_sql->param_name = atoi(tmp_ptr2); \
        if (direct_sql->param_name < min_val) \
          direct_sql->param_name = min_val; \
        else if (direct_sql->param_name > max_val) \
          direct_sql->param_name = max_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                           tmp_ptr2 + \
                                             strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%d", \
        (int) direct_sql->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_INT(title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (direct_sql->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        direct_sql->param_name = atoi(tmp_ptr2); \
        if (direct_sql->param_name < min_val) \
          direct_sql->param_name = min_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                           tmp_ptr2 + \
                                             strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%d", direct_sql->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_LONGLONG(title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (direct_sql->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        direct_sql->param_name = \
          my_strtoll10(tmp_ptr2, (char**) NULL, &error_num); \
        if (direct_sql->param_name < min_val) \
          direct_sql->param_name = min_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                           tmp_ptr2 + \
                                             strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%lld", \
        direct_sql->param_name)); \
    } \
    break; \
  }

int spider_udf_parse_direct_sql_param(
  SPIDER_TRX *trx,
  SPIDER_DIRECT_SQL *direct_sql,
  const char *param,
  int param_length
) {
  int error_num = 0, roop_count;
  char *param_string = NULL;
  char *sprit_ptr;
  char *tmp_ptr, *tmp_ptr2, *start_ptr;
  int title_length;
  SPIDER_PARAM_STRING_PARSE param_string_parse;
  DBUG_ENTER("spider_udf_parse_direct_sql_param");
  direct_sql->tgt_port = -1;
  direct_sql->tgt_ssl_vsc = -1;
  direct_sql->table_loop_mode = -1;
  direct_sql->priority = -1;
  direct_sql->connect_timeout = -1;
  direct_sql->net_read_timeout = -1;
  direct_sql->net_write_timeout = -1;
  direct_sql->bulk_insert_rows = -1;
  direct_sql->connection_channel = -1;
  direct_sql->use_real_table = -1;
  direct_sql->error_rw_mode = -1;
  for (roop_count = 0; roop_count < direct_sql->table_count; roop_count++)
    direct_sql->iop[roop_count] = -1;

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
        SPIDER_PARAM_LONGLONG("bir", bulk_insert_rows, 0);
        SPIDER_PARAM_INT_WITH_MAX("cch", connection_channel, 0, 63);
        SPIDER_PARAM_INT("cto", connect_timeout, 0);
        SPIDER_PARAM_STR("dff", tgt_default_file);
        SPIDER_PARAM_STR("dfg", tgt_default_group);
        SPIDER_PARAM_STR("drv", tgt_driver);
        SPIDER_PARAM_STR("dsn", tgt_dsn);
        SPIDER_PARAM_STR("fds", tgt_filedsn);
        SPIDER_PARAM_LONGLONG("prt", priority, 0);
        SPIDER_PARAM_INT("rto", net_read_timeout, 0);
        SPIDER_PARAM_STR("sca", tgt_ssl_ca);
        SPIDER_PARAM_STR("sch", tgt_ssl_cipher);
        SPIDER_PARAM_STR("scp", tgt_ssl_capath);
        SPIDER_PARAM_STR("scr", tgt_ssl_cert);
        SPIDER_PARAM_STR("sky", tgt_ssl_key);
        SPIDER_PARAM_STR("srv", server_name);
        SPIDER_PARAM_INT_WITH_MAX("svc", tgt_ssl_vsc, 0, 1);
        SPIDER_PARAM_INT_WITH_MAX("tlm", table_loop_mode, 0, 2);
        SPIDER_PARAM_INT_WITH_MAX("urt", use_real_table, 0, 1);
        SPIDER_PARAM_INT("wto", net_write_timeout, 0);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 4:
        SPIDER_PARAM_INT_WITH_MAX("erwm", error_rw_mode, 0, 1);
        SPIDER_PARAM_STR("host", tgt_host);
        SPIDER_PARAM_INT_WITH_MAX("port", tgt_port, 0, 65535);
        SPIDER_PARAM_STR("user", tgt_username);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 6:
        SPIDER_PARAM_STR("driver", tgt_driver);
        SPIDER_PARAM_STR("server", server_name);
        SPIDER_PARAM_STR("socket", tgt_socket);
        SPIDER_PARAM_HINT_WITH_MAX("iop", iop, 3, direct_sql->table_count, 0, 2);
        SPIDER_PARAM_STR("ssl_ca", tgt_ssl_ca);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 7:
        SPIDER_PARAM_STR("filedsn", tgt_filedsn);
        SPIDER_PARAM_STR("wrapper", tgt_wrapper);
        SPIDER_PARAM_STR("ssl_key", tgt_ssl_key);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 8:
        SPIDER_PARAM_STR("database", tgt_default_db_name);
        SPIDER_PARAM_STR("password", tgt_password);
        SPIDER_PARAM_LONGLONG("priority", priority, 0);
        SPIDER_PARAM_STR("ssl_cert", tgt_ssl_cert);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 10:
        SPIDER_PARAM_STR("ssl_cipher", tgt_ssl_cipher);
        SPIDER_PARAM_STR("ssl_capath", tgt_ssl_capath);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 11:
        error_num = param_string_parse.print_param_error();
        goto error;
      case 12:
        SPIDER_PARAM_STR("default_file", tgt_default_file);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 13:
        SPIDER_PARAM_STR("default_group", tgt_default_group);
        SPIDER_PARAM_INT_WITH_MAX("error_rw_mode", error_rw_mode, 0, 1);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 14:
        SPIDER_PARAM_INT_WITH_MAX("use_real_table", use_real_table, 0, 1);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 15:
        SPIDER_PARAM_INT_WITH_MAX("table_loop_mode", table_loop_mode, 0, 2);
        SPIDER_PARAM_INT("connect_timeout", connect_timeout, 0);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 16:
        SPIDER_PARAM_LONGLONG("bulk_insert_rows", bulk_insert_rows, 1);
        SPIDER_PARAM_INT("net_read_timeout", net_read_timeout, 0);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 17:
        SPIDER_PARAM_INT("net_write_timeout", net_write_timeout, 0);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 18:
        SPIDER_PARAM_INT_WITH_MAX(
          "connection_channel", connection_channel, 0, 63);
        error_num = param_string_parse.print_param_error();
        goto error;
      case 22:
        SPIDER_PARAM_INT_WITH_MAX("ssl_verify_server_cert", tgt_ssl_vsc, 0, 1);
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
  if ((error_num = spider_udf_set_direct_sql_param_default(
    trx,
    direct_sql
  )))
    goto error;

  if (param_string)
  {
    spider_free(spider_current_trx, param_string, MYF(0));
  }
  DBUG_RETURN(0);

error:
  if (param_string)
  {
    spider_free(spider_current_trx, param_string, MYF(0));
  }
error_alloc_param_string:
  DBUG_RETURN(error_num);
}

int spider_udf_set_direct_sql_param_default(
  SPIDER_TRX *trx,
  SPIDER_DIRECT_SQL *direct_sql
) {
  bool check_socket;
  bool check_database;
  bool check_default_file;
  bool check_host;
  bool check_port;
  bool socket_has_default_value;
  bool database_has_default_value;
  bool default_file_has_default_value;
  bool host_has_default_value;
  bool port_has_default_value;
  int error_num, roop_count;
  DBUG_ENTER("spider_udf_set_direct_sql_param_default");
  if (direct_sql->server_name)
  {
    if ((error_num = spider_udf_direct_sql_get_server(direct_sql)))
      DBUG_RETURN(error_num);
  }

  if (
    !direct_sql->tgt_socket &&
    (!direct_sql->tgt_host || !strcmp(direct_sql->tgt_host, my_localhost))
  ) {
    check_socket = TRUE;
  } else {
    check_socket = FALSE;
  }
  if (!direct_sql->tgt_default_db_name)
  {
    check_database = TRUE;
  } else {
    check_database = FALSE;
  }
  if (
    !direct_sql->tgt_default_file &&
    direct_sql->tgt_default_group &&
    (*spd_defaults_file || *spd_defaults_extra_file)
  ) {
    check_default_file = TRUE;
  } else {
    check_default_file = FALSE;
  }
  if (!direct_sql->tgt_host)
  {
    check_host = TRUE;
  } else {
    check_host = FALSE;
  }
  if (direct_sql->tgt_port == -1)
  {
    check_port = TRUE;
  } else {
    check_port = FALSE;
  }
  if (check_socket || check_database || check_default_file || check_host ||
    check_port)
  {
    socket_has_default_value = check_socket;
    database_has_default_value = check_database;
    default_file_has_default_value = check_default_file;
    host_has_default_value = check_host;
    port_has_default_value = check_port;
    if (direct_sql->tgt_wrapper)
    {
      for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++)
      {
        DBUG_PRINT("info",("spider direct_sql->tgt_wrapper=%s",
          direct_sql->tgt_wrapper));
        DBUG_PRINT("info",("spider spider_dbton[%d].wrapper=%s", roop_count,
          spider_dbton[roop_count].wrapper ?
            spider_dbton[roop_count].wrapper : "NULL"));
        if (
          spider_dbton[roop_count].wrapper &&
          !strcmp(direct_sql->tgt_wrapper,
            spider_dbton[roop_count].wrapper)
        ) {
          if (spider_dbton[roop_count].db_access_type ==
            SPIDER_DB_ACCESS_TYPE_SQL)
          {
            if (check_socket)
            {
              socket_has_default_value = spider_dbton[roop_count].
                db_util->socket_has_default_value();
            }
            if (check_database)
            {
              database_has_default_value = spider_dbton[roop_count].
                db_util->database_has_default_value();
            }
            if (check_default_file)
            {
              default_file_has_default_value = spider_dbton[roop_count].
                db_util->default_file_has_default_value();
            }
            if (check_host)
            {
              host_has_default_value = spider_dbton[roop_count].
                db_util->host_has_default_value();
            }
            if (check_port)
            {
              port_has_default_value = spider_dbton[roop_count].
                db_util->port_has_default_value();
            }
            break;
          }
        }
      }
    }
  } else {
    socket_has_default_value = FALSE;
    database_has_default_value = FALSE;
    default_file_has_default_value = FALSE;
    host_has_default_value = FALSE;
    port_has_default_value = FALSE;
  }

  if (database_has_default_value)
  {
    DBUG_PRINT("info",("spider create default tgt_default_db_name"));
    direct_sql->tgt_default_db_name_length = SPIDER_THD_db_length(trx->thd);
    if (
      !(direct_sql->tgt_default_db_name = spider_create_string(
        SPIDER_THD_db_str(trx->thd),
        direct_sql->tgt_default_db_name_length))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (!direct_sql->tgt_wrapper)
  {
    DBUG_PRINT("info",("spider create default tgt_wrapper"));
    direct_sql->tgt_wrapper_length = SPIDER_DB_WRAPPER_LEN;
    if (
      !(direct_sql->tgt_wrapper = spider_create_string(
        SPIDER_DB_WRAPPER_STR,
        direct_sql->tgt_wrapper_length))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (host_has_default_value)
  {
    DBUG_PRINT("info",("spider create default tgt_host"));
    direct_sql->tgt_host_length = strlen(my_localhost);
    if (
      !(direct_sql->tgt_host = spider_create_string(
        my_localhost,
        direct_sql->tgt_host_length))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (default_file_has_default_value)
  {
    DBUG_PRINT("info",("spider create default tgt_default_file"));
    if (*spd_defaults_extra_file)
    {
      direct_sql->tgt_default_file_length = strlen(*spd_defaults_extra_file);
      if (
        !(direct_sql->tgt_default_file = spider_create_string(
          *spd_defaults_extra_file,
          direct_sql->tgt_default_file_length))
      ) {
        my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    } else {
      direct_sql->tgt_default_file_length = strlen(*spd_defaults_file);
      if (
        !(direct_sql->tgt_default_file = spider_create_string(
          *spd_defaults_file,
          direct_sql->tgt_default_file_length))
      ) {
        my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }
  }

  if (port_has_default_value)
  {
      direct_sql->tgt_port = MYSQL_PORT;
  }
  else if (direct_sql->tgt_port < 0)
    direct_sql->tgt_port = 0;
  else if (direct_sql->tgt_port > 65535)
    direct_sql->tgt_port = 65535;

  if (direct_sql->tgt_ssl_vsc == -1)
    direct_sql->tgt_ssl_vsc = 0;

  if (socket_has_default_value)
  {
    DBUG_PRINT("info",("spider create default tgt_socket"));
    direct_sql->tgt_socket_length = strlen((char *) MYSQL_UNIX_ADDR);
    if (
      !(direct_sql->tgt_socket = spider_create_string(
        (char *) MYSQL_UNIX_ADDR,
        direct_sql->tgt_socket_length))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (direct_sql->table_loop_mode == -1)
    direct_sql->table_loop_mode = 0;
  if (direct_sql->priority == -1)
    direct_sql->priority = 1000000;
  if (direct_sql->connect_timeout == -1)
    direct_sql->connect_timeout = 6;
  if (direct_sql->net_read_timeout == -1)
    direct_sql->net_read_timeout = 600;
  if (direct_sql->net_write_timeout == -1)
    direct_sql->net_write_timeout = 600;
  if (direct_sql->bulk_insert_rows == -1)
    direct_sql->bulk_insert_rows = 3000;
  if (direct_sql->connection_channel == -1)
    direct_sql->connection_channel = 0;
  if (direct_sql->use_real_table == -1)
    direct_sql->use_real_table = 0;
  if (direct_sql->error_rw_mode == -1)
    direct_sql->error_rw_mode = 0;
  for (roop_count = 0; roop_count < direct_sql->table_count; roop_count++)
  {
    if (direct_sql->iop[roop_count] == -1)
      direct_sql->iop[roop_count] = 0;
  }
  DBUG_RETURN(0);
}

void spider_udf_free_direct_sql_alloc(
  SPIDER_DIRECT_SQL *direct_sql,
  my_bool bg
) {
  SPIDER_BG_DIRECT_SQL *bg_direct_sql;
  DBUG_ENTER("spider_udf_free_direct_sql_alloc");
  if (bg)
  {
    pthread_mutex_lock(direct_sql->bg_mutex);
    bg_direct_sql = (SPIDER_BG_DIRECT_SQL *) direct_sql->parent;
    if (bg_direct_sql->direct_sql == direct_sql)
      bg_direct_sql->direct_sql = direct_sql->next;
    if (direct_sql->next)
      direct_sql->next->prev = direct_sql->prev;
    if (direct_sql->prev)
      direct_sql->prev->next = direct_sql->next;
    pthread_cond_signal(direct_sql->bg_cond);
    pthread_mutex_unlock(direct_sql->bg_mutex);
  }
  if (direct_sql->real_table_used && direct_sql->open_tables_thd)
  {
    spider_sys_close_table(direct_sql->open_tables_thd,
      &direct_sql->open_tables_backup);
  }
  if (direct_sql->server_name)
  {
    spider_free(spider_current_trx, direct_sql->server_name, MYF(0));
  }
  if (direct_sql->tgt_default_db_name)
  {
    spider_free(spider_current_trx, direct_sql->tgt_default_db_name, MYF(0));
  }
  if (direct_sql->tgt_host)
  {
    spider_free(spider_current_trx, direct_sql->tgt_host, MYF(0));
  }
  if (direct_sql->tgt_username)
  {
    spider_free(spider_current_trx, direct_sql->tgt_username, MYF(0));
  }
  if (direct_sql->tgt_password)
  {
    spider_free(spider_current_trx, direct_sql->tgt_password, MYF(0));
  }
  if (direct_sql->tgt_socket)
  {
    spider_free(spider_current_trx, direct_sql->tgt_socket, MYF(0));
  }
  if (direct_sql->tgt_wrapper)
  {
    spider_free(spider_current_trx, direct_sql->tgt_wrapper, MYF(0));
  }
  if (direct_sql->tgt_ssl_ca)
  {
    spider_free(spider_current_trx, direct_sql->tgt_ssl_ca, MYF(0));
  }
  if (direct_sql->tgt_ssl_capath)
  {
    spider_free(spider_current_trx, direct_sql->tgt_ssl_capath, MYF(0));
  }
  if (direct_sql->tgt_ssl_cert)
  {
    spider_free(spider_current_trx, direct_sql->tgt_ssl_cert, MYF(0));
  }
  if (direct_sql->tgt_ssl_cipher)
  {
    spider_free(spider_current_trx, direct_sql->tgt_ssl_cipher, MYF(0));
  }
  if (direct_sql->tgt_ssl_key)
  {
    spider_free(spider_current_trx, direct_sql->tgt_ssl_key, MYF(0));
  }
  if (direct_sql->tgt_default_file)
  {
    spider_free(spider_current_trx, direct_sql->tgt_default_file, MYF(0));
  }
  if (direct_sql->tgt_default_group)
  {
    spider_free(spider_current_trx, direct_sql->tgt_default_group, MYF(0));
  }
  if (direct_sql->tgt_dsn)
  {
    spider_free(spider_current_trx, direct_sql->tgt_dsn, MYF(0));
  }
  if (direct_sql->tgt_filedsn)
  {
    spider_free(spider_current_trx, direct_sql->tgt_filedsn, MYF(0));
  }
  if (direct_sql->tgt_driver)
  {
    spider_free(spider_current_trx, direct_sql->tgt_driver, MYF(0));
  }
  if (direct_sql->conn_key)
  {
    spider_free(spider_current_trx, direct_sql->conn_key, MYF(0));
  }
  if (direct_sql->db_names)
  {
    spider_free(spider_current_trx, direct_sql->db_names, MYF(0));
  }
  spider_free(spider_current_trx, direct_sql, MYF(0));
  DBUG_VOID_RETURN;
}

long long spider_direct_sql_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error,
  my_bool bg
) {
  int error_num, roop_count;
  SPIDER_DIRECT_SQL *direct_sql = NULL, *tmp_direct_sql;
  THD *thd = current_thd;
  SPIDER_TRX *trx;
  SPIDER_CONN *conn;
  char *sql;
  TABLE_LIST table_list;
  SPIDER_BG_DIRECT_SQL *bg_direct_sql;
  TABLE_LIST *real_table_list_last = NULL;
  uint use_real_table = 0;
  DBUG_ENTER("spider_direct_sql_body");
  SPIDER_BACKUP_DASTATUS;
  if (!(direct_sql = (SPIDER_DIRECT_SQL *)
    spider_bulk_malloc(spider_current_trx, 34, MYF(MY_WME | MY_ZEROFILL),
      &direct_sql, (uint) (sizeof(SPIDER_DIRECT_SQL)),
      &sql, (uint) (sizeof(char) * args->lengths[0]),
      NullS))
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  if (bg)
  {
    bg_direct_sql = (SPIDER_BG_DIRECT_SQL *) initid->ptr;
    pthread_mutex_lock(&bg_direct_sql->bg_mutex);
    tmp_direct_sql = (SPIDER_DIRECT_SQL *) bg_direct_sql->direct_sql;
    bg_direct_sql->direct_sql = direct_sql;
    if (tmp_direct_sql)
    {
      tmp_direct_sql->prev = direct_sql;
      direct_sql->next = tmp_direct_sql;
    }
    pthread_mutex_unlock(&bg_direct_sql->bg_mutex);
    direct_sql->bg_mutex = &bg_direct_sql->bg_mutex;
    direct_sql->bg_cond = &bg_direct_sql->bg_cond;
    direct_sql->parent = bg_direct_sql;
    bg_direct_sql->called_cnt++;
  }
  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
  {
    if (error_num == HA_ERR_OUT_OF_MEM)
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  direct_sql->trx = trx;

  if (args->args[1])
  {
    if ((error_num = spider_udf_direct_sql_create_table_list(
      direct_sql,
      args->args[1],
      args->lengths[1]
    ))) {
      if (error_num == HA_ERR_OUT_OF_MEM)
        my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
  } else {
    if ((error_num = spider_udf_direct_sql_create_table_list(
      direct_sql,
      (char *) "",
      0
    ))) {
      if (error_num == HA_ERR_OUT_OF_MEM)
        my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
  }
  if (args->args[2])
  {
    if ((error_num = spider_udf_parse_direct_sql_param(
      trx,
      direct_sql,
      args->args[2],
      args->lengths[2]
    ))) {
      goto error;
    }
  } else {
    if ((error_num = spider_udf_parse_direct_sql_param(
      trx,
      direct_sql,
      "",
      0
    ))) {
      goto error;
    }
  }
    trx->updated_in_this_trx = TRUE;
    DBUG_PRINT("info",("spider trx->updated_in_this_trx=TRUE"));
  use_real_table = spider_param_udf_ds_use_real_table(thd,
    direct_sql->use_real_table);
  for (roop_count = 0; roop_count < direct_sql->table_count; roop_count++)
  {
#ifdef SPIDER_NEED_INIT_ONE_TABLE_FOR_FIND_TEMPORARY_TABLE
#ifdef SPIDER_use_LEX_CSTRING_for_database_tablename_alias
    LEX_CSTRING db_name =
    {
      direct_sql->db_names[roop_count],
      strlen(direct_sql->db_names[roop_count])
    };
    LEX_CSTRING tbl_name =
    {
      direct_sql->table_names[roop_count],
      strlen(direct_sql->table_names[roop_count])
    };
    table_list.init_one_table(&db_name, &tbl_name, 0, TL_WRITE);
#else
    table_list.init_one_table(direct_sql->db_names[roop_count],
      strlen(direct_sql->db_names[roop_count]),
      direct_sql->table_names[roop_count],
      strlen(direct_sql->table_names[roop_count]),
      direct_sql->table_names[roop_count], TL_WRITE);
#endif
#else
    SPIDER_TABLE_LIST_db_str(&table_list) = direct_sql->db_names[roop_count];
    SPIDER_TABLE_LIST_table_name_str(&table_list) =
      direct_sql->table_names[roop_count];
#endif
    if (!(direct_sql->tables[roop_count] =
      spider_find_temporary_table(thd, &table_list)))
    {
      if (!use_real_table)
      {
        error_num = ER_SPIDER_UDF_TMP_TABLE_NOT_FOUND_NUM;
        my_printf_error(ER_SPIDER_UDF_TMP_TABLE_NOT_FOUND_NUM,
          ER_SPIDER_UDF_TMP_TABLE_NOT_FOUND_STR,
          MYF(0), SPIDER_TABLE_LIST_db_str(&table_list),
          SPIDER_TABLE_LIST_table_name_str(&table_list));
        goto error;
      }
      TABLE_LIST *tables = &direct_sql->table_list[roop_count];
#ifdef SPIDER_use_LEX_CSTRING_for_database_tablename_alias
      table_list.init_one_table(
        &table_list.db, &table_list.table_name, 0, TL_WRITE);
#else
      tables->init_one_table(
        SPIDER_TABLE_LIST_db_str(&table_list),
        SPIDER_TABLE_LIST_db_length(&table_list),
        SPIDER_TABLE_LIST_table_name_str(&table_list),
        SPIDER_TABLE_LIST_table_name_length(&table_list),
        SPIDER_TABLE_LIST_table_name_str(&table_list), TL_WRITE);
#endif
      MDL_REQUEST_INIT(&tables->mdl_request, MDL_key::TABLE,
        SPIDER_TABLE_LIST_db_str(&table_list),
        SPIDER_TABLE_LIST_table_name_str(&table_list),
        MDL_SHARED_WRITE, MDL_TRANSACTION);
      if (!direct_sql->table_list_first)
      {
        direct_sql->table_list_first = tables;
      } else {
        real_table_list_last->next_global = tables;
      }
      real_table_list_last = tables;
      spider_set_bit(direct_sql->real_table_bitmap, roop_count);
      direct_sql->real_table_used = TRUE;
    }
  }
  if ((error_num = spider_udf_direct_sql_create_conn_key(direct_sql)))
  {
    if (error_num == HA_ERR_OUT_OF_MEM)
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  if (!(conn = spider_udf_direct_sql_get_conn(direct_sql, trx, &error_num)))
  {
    if (error_num == HA_ERR_OUT_OF_MEM)
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  conn->error_mode = 0;
  direct_sql->conn = conn;
  if ((error_num = spider_db_udf_check_and_set_set_names(trx)))
  {
    if (error_num == HA_ERR_OUT_OF_MEM)
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  if (args->args[0])
  {
    direct_sql->sql_length = args->lengths[0];
    memcpy(sql, args->args[0], direct_sql->sql_length);
  } else
    direct_sql->sql_length = 0;
  direct_sql->sql = sql;

  if (bg)
  {
    if ((error_num = spider_udf_bg_direct_sql(direct_sql)))
    {
      if (error_num == HA_ERR_OUT_OF_MEM)
        my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
  } else {
    if (conn->bg_init)
      pthread_mutex_lock(&conn->bg_conn_mutex);
    if ((error_num = spider_db_udf_direct_sql(direct_sql)))
    {
      if (conn->bg_init)
        pthread_mutex_unlock(&conn->bg_conn_mutex);
      if (direct_sql->modified_non_trans_table)
        thd->transaction->stmt.modified_non_trans_table = TRUE;
      if (error_num == HA_ERR_OUT_OF_MEM)
        my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    if (conn->bg_init)
      pthread_mutex_unlock(&conn->bg_conn_mutex);
    if (direct_sql->modified_non_trans_table)
      thd->transaction->stmt.modified_non_trans_table = TRUE;
  }
  if (!bg)
  {
    spider_udf_free_direct_sql_alloc(direct_sql, FALSE);
  }
  DBUG_RETURN(1);

error:
  if (direct_sql)
  {
    if (
      direct_sql->error_rw_mode &&
      spider_db_conn_is_network_error(error_num)
    ) {
      SPIDER_RESTORE_DASTATUS;
      spider_udf_free_direct_sql_alloc(direct_sql, bg);
      DBUG_RETURN(1);
    }
    spider_udf_free_direct_sql_alloc(direct_sql, bg);
  }
  *error = 1;
  DBUG_RETURN(0);
}

my_bool spider_direct_sql_init_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message,
  my_bool bg
) {
  SPIDER_BG_DIRECT_SQL *bg_direct_sql;
  DBUG_ENTER("spider_direct_sql_init_body");
  if (args->arg_count != 3)
  {
    strcpy(message, "spider_(bg)_direct_sql() requires 3 arguments");
    goto error;
  }
  if (
    args->arg_type[0] != STRING_RESULT ||
    args->arg_type[1] != STRING_RESULT ||
    args->arg_type[2] != STRING_RESULT
  ) {
    strcpy(message, "spider_(bg)_direct_sql() requires string arguments");
    goto error;
  }
  if (bg)
  {
    if (!(bg_direct_sql = (SPIDER_BG_DIRECT_SQL *)
      spider_malloc(spider_current_trx, 10, sizeof(SPIDER_BG_DIRECT_SQL),
      MYF(MY_WME | MY_ZEROFILL)))
    ) {
      strcpy(message, "spider_bg_direct_sql() out of memory");
      goto error;
    }
    if (mysql_mutex_init(spd_key_mutex_bg_direct_sql,
      &bg_direct_sql->bg_mutex, MY_MUTEX_INIT_FAST))
    {
      strcpy(message, "spider_bg_direct_sql() out of memory");
      goto error_mutex_init;
    }
    if (mysql_cond_init(spd_key_cond_bg_direct_sql,
      &bg_direct_sql->bg_cond, NULL))
    {
      strcpy(message, "spider_bg_direct_sql() out of memory");
      goto error_cond_init;
    }
    initid->ptr = (char *) bg_direct_sql;
  }
  DBUG_RETURN(FALSE);

error_cond_init:
  pthread_mutex_destroy(&bg_direct_sql->bg_mutex);
error_mutex_init:
  spider_free(spider_current_trx, bg_direct_sql, MYF(0));
error:
  DBUG_RETURN(TRUE);
}

void spider_direct_sql_deinit_body(
  UDF_INIT *initid
) {
  SPIDER_BG_DIRECT_SQL *bg_direct_sql = (SPIDER_BG_DIRECT_SQL *) initid->ptr;
  DBUG_ENTER("spider_direct_sql_deinit_body");
  if (bg_direct_sql)
  {
    pthread_mutex_lock(&bg_direct_sql->bg_mutex);
    while (bg_direct_sql->direct_sql)
      pthread_cond_wait(&bg_direct_sql->bg_cond, &bg_direct_sql->bg_mutex);
    pthread_mutex_unlock(&bg_direct_sql->bg_mutex);
    if (bg_direct_sql->modified_non_trans_table)
    {
      THD *thd = current_thd;
      thd->transaction->stmt.modified_non_trans_table = TRUE;
    }
    pthread_cond_destroy(&bg_direct_sql->bg_cond);
    pthread_mutex_destroy(&bg_direct_sql->bg_mutex);
    spider_free(spider_current_trx, bg_direct_sql, MYF(0));
  }
  DBUG_VOID_RETURN;
}

void spider_direct_sql_bg_start(
  UDF_INIT *initid
) {
  SPIDER_BG_DIRECT_SQL *bg_direct_sql = (SPIDER_BG_DIRECT_SQL *) initid->ptr;
  DBUG_ENTER("spider_direct_sql_bg_start");
  bg_direct_sql->called_cnt = 0;
  bg_direct_sql->bg_error = 0;
  DBUG_VOID_RETURN;
}

long long spider_direct_sql_bg_end(
  UDF_INIT *initid
) {
  THD *thd = current_thd;
  SPIDER_BG_DIRECT_SQL *bg_direct_sql = (SPIDER_BG_DIRECT_SQL *) initid->ptr;
  DBUG_ENTER("spider_direct_sql_bg_end");
  pthread_mutex_lock(&bg_direct_sql->bg_mutex);
  while (bg_direct_sql->direct_sql)
    pthread_cond_wait(&bg_direct_sql->bg_cond, &bg_direct_sql->bg_mutex);
  pthread_mutex_unlock(&bg_direct_sql->bg_mutex);
  if (bg_direct_sql->modified_non_trans_table)
    thd->transaction->stmt.modified_non_trans_table = TRUE;
  if (bg_direct_sql->bg_error)
  {
    my_message(bg_direct_sql->bg_error, bg_direct_sql->bg_error_msg, MYF(0));
    DBUG_RETURN(0);
  }
  DBUG_RETURN(bg_direct_sql->called_cnt);
}

int spider_udf_bg_direct_sql(
  SPIDER_DIRECT_SQL *direct_sql
) {
  int error_num;
  SPIDER_CONN *conn = direct_sql->conn;
  DBUG_ENTER("spider_udf_bg_direct_sql");
  if ((error_num = spider_create_conn_thread(conn)))
    DBUG_RETURN(error_num);
  if (!pthread_mutex_trylock(&conn->bg_conn_mutex))
  {
    DBUG_PRINT("info",("spider get bg_conn_mutex"));
    conn->bg_target = direct_sql;
    conn->bg_direct_sql = TRUE;
    conn->bg_caller_sync_wait = TRUE;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_cond_signal(&conn->bg_conn_cond);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    conn->bg_caller_sync_wait = FALSE;
  } else {
    DBUG_PRINT("info",("spider get put job stack"));
    bool bg_get_job_stack = FALSE;
    pthread_mutex_lock(&conn->bg_job_stack_mutex);
    uint old_elements = conn->bg_job_stack.max_element;
    if (insert_dynamic(&conn->bg_job_stack, (uchar *) &direct_sql))
    {
      pthread_mutex_unlock(&conn->bg_job_stack_mutex);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    if (conn->bg_job_stack.max_element > old_elements)
    {
      spider_alloc_calc_mem(spider_current_trx,
        conn->bg_job_stack,
        (conn->bg_job_stack.max_element - old_elements) *
        conn->bg_job_stack.size_of_element);
    }
    if (!conn->bg_get_job_stack_off)
      bg_get_job_stack = TRUE;
    pthread_mutex_unlock(&conn->bg_job_stack_mutex);
    if (bg_get_job_stack)
    {
      DBUG_PRINT("info",("spider get bg_conn_mutex"));
      pthread_mutex_lock(&conn->bg_conn_mutex);
      conn->bg_target = NULL;
      conn->bg_get_job_stack = TRUE;
      conn->bg_direct_sql = TRUE;
      conn->bg_caller_sync_wait = TRUE;
      pthread_mutex_lock(&conn->bg_conn_sync_mutex);
      pthread_cond_signal(&conn->bg_conn_cond);
      pthread_mutex_unlock(&conn->bg_conn_mutex);
      pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
      pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      conn->bg_caller_sync_wait = FALSE;
    }
  }
  DBUG_RETURN(0);
}
