/* Copyright (C) 2008-2020 Kentoku Shiba
   Copyright (C) 2019, 2020, MariaDB Corporation.

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
#include "sql_table.h"
#include "tztime.h"
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "ha_spider.h"
#include "spd_db_conn.h"
#include "spd_trx.h"
#include "spd_conn.h"
#include "spd_table.h"
#include "spd_direct_sql.h"
#include "spd_ping_table.h"
#include "spd_malloc.h"
#include "spd_err.h"

#ifdef SPIDER_HAS_NEXT_THREAD_ID
#define SPIDER_set_next_thread_id(A)
#else
extern ulong *spd_db_att_thread_id;
inline void SPIDER_set_next_thread_id(THD *A)
{
  pthread_mutex_lock(&LOCK_thread_count);
  A->thread_id = (*spd_db_att_thread_id)++;
  pthread_mutex_unlock(&LOCK_thread_count);
}
#endif

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
extern struct charset_info_st *spd_charset_utf8mb3_bin;
extern LEX_CSTRING spider_unique_id;
pthread_mutex_t spider_conn_id_mutex;
pthread_mutex_t spider_ipport_conn_mutex;
ulonglong spider_conn_id = 1;

extern pthread_attr_t spider_pt_attr;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key spd_key_mutex_mta_conn;
extern PSI_mutex_key spd_key_mutex_conn_i;
extern PSI_mutex_key spd_key_mutex_conn_loop_check;
extern PSI_cond_key spd_key_cond_conn_i;
extern PSI_mutex_key spd_key_mutex_bg_conn_chain;
extern PSI_mutex_key spd_key_mutex_bg_conn_sync;
extern PSI_mutex_key spd_key_mutex_bg_conn;
extern PSI_mutex_key spd_key_mutex_bg_job_stack;
extern PSI_mutex_key spd_key_mutex_bg_mon;
extern PSI_cond_key spd_key_cond_bg_conn_sync;
extern PSI_cond_key spd_key_cond_bg_conn;
extern PSI_cond_key spd_key_cond_bg_sts;
extern PSI_cond_key spd_key_cond_bg_sts_sync;
extern PSI_cond_key spd_key_cond_bg_crd;
extern PSI_cond_key spd_key_cond_bg_crd_sync;
extern PSI_cond_key spd_key_cond_bg_mon;
extern PSI_cond_key spd_key_cond_bg_mon_sleep;
extern PSI_thread_key spd_key_thd_bg;
extern PSI_thread_key spd_key_thd_bg_sts;
extern PSI_thread_key spd_key_thd_bg_crd;
extern PSI_thread_key spd_key_thd_bg_mon;
#endif

/* UTC time zone for timestamp columns */
extern Time_zone *UTC;

extern sql_mode_t full_sql_mode;
extern sql_mode_t pushdown_sql_mode;

HASH spider_open_connections;
uint spider_open_connections_id;
HASH spider_ipport_conns;
long spider_conn_mutex_id = 0;

const char *spider_open_connections_func_name;
const char *spider_open_connections_file_name;
ulong spider_open_connections_line_no;
pthread_mutex_t spider_conn_mutex;

/* for spider_open_connections and trx_conn_hash */
uchar *spider_conn_get_key(
  SPIDER_CONN *conn,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_conn_get_key");
  *length = conn->conn_key_length;
  DBUG_PRINT("info",("spider conn_kind=%u", conn->conn_kind));
#ifndef DBUG_OFF
  spider_print_keys(conn->conn_key, conn->conn_key_length);
#endif
  DBUG_RETURN((uchar*) conn->conn_key);
}

uchar *spider_ipport_conn_get_key(
   SPIDER_IP_PORT_CONN *ip_port,
   size_t *length,
   my_bool not_used __attribute__ ((unused))
)
{
  DBUG_ENTER("spider_ipport_conn_get_key");
  *length = ip_port->key_len;
  DBUG_RETURN((uchar*) ip_port->key);
}

static uchar *spider_loop_check_full_get_key(
  SPIDER_CONN_LOOP_CHECK *ptr,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_loop_check_full_get_key");
  *length = ptr->full_name.length;
  DBUG_RETURN((uchar*) ptr->full_name.str);
}

static uchar *spider_loop_check_to_get_key(
  SPIDER_CONN_LOOP_CHECK *ptr,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_loop_check_to_get_key");
  *length = ptr->to_name.length;
  DBUG_RETURN((uchar*) ptr->to_name.str);
}

int spider_conn_init(
  SPIDER_CONN *conn
) {
  int error_num = HA_ERR_OUT_OF_MEM;
  DBUG_ENTER("spider_conn_init");
  if (mysql_mutex_init(spd_key_mutex_conn_loop_check, &conn->loop_check_mutex,
    MY_MUTEX_INIT_FAST))
  {
    goto error_loop_check_mutex_init;
  }
  if (
    my_hash_init(PSI_INSTRUMENT_ME, &conn->loop_checked, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_loop_check_full_get_key, 0, 0)
  ) {
    goto error_loop_checked_hash_init;
  }
  spider_alloc_calc_mem_init(conn->loop_checked, 268);
  spider_alloc_calc_mem(spider_current_trx,
    conn->loop_checked,
    conn->loop_checked.array.max_element *
    conn->loop_checked.array.size_of_element);
  if (
    my_hash_init(PSI_INSTRUMENT_ME, &conn->loop_check_queue, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_loop_check_to_get_key, 0, 0)
  ) {
    goto error_loop_check_queue_hash_init;
  }
  spider_alloc_calc_mem_init(conn->loop_check_queue, 269);
  spider_alloc_calc_mem(spider_current_trx,
    conn->loop_check_queue,
    conn->loop_check_queue.array.max_element *
    conn->loop_check_queue.array.size_of_element);
  DBUG_RETURN(0);

error_loop_check_queue_hash_init:
  spider_free_mem_calc(spider_current_trx,
    conn->loop_checked_id,
    conn->loop_checked.array.max_element *
    conn->loop_checked.array.size_of_element);
  my_hash_free(&conn->loop_checked);
error_loop_checked_hash_init:
  pthread_mutex_destroy(&conn->loop_check_mutex);
error_loop_check_mutex_init:
  DBUG_RETURN(error_num);
}

void spider_conn_done(
  SPIDER_CONN *conn
) {
  SPIDER_CONN_LOOP_CHECK *lcptr;
  DBUG_ENTER("spider_conn_done");
  uint l = 0;
  while ((lcptr = (SPIDER_CONN_LOOP_CHECK *) my_hash_element(
    &conn->loop_checked, l)))
  {
    spider_free(spider_current_trx, lcptr, MYF(0));
    ++l;
  }
  spider_free_mem_calc(spider_current_trx,
    conn->loop_check_queue_id,
    conn->loop_check_queue.array.max_element *
    conn->loop_check_queue.array.size_of_element);
  my_hash_free(&conn->loop_check_queue);
  spider_free_mem_calc(spider_current_trx,
    conn->loop_checked_id,
    conn->loop_checked.array.max_element *
    conn->loop_checked.array.size_of_element);
  my_hash_free(&conn->loop_checked);
  pthread_mutex_destroy(&conn->loop_check_mutex);
  DBUG_VOID_RETURN;
}

int spider_reset_conn_setted_parameter(
  SPIDER_CONN *conn,
  THD *thd
) {
  DBUG_ENTER("spider_reset_conn_setted_parameter");
  conn->autocommit = spider_param_remote_autocommit();
  conn->sql_log_off = spider_param_remote_sql_log_off();
  conn->wait_timeout = spider_param_remote_wait_timeout(thd);
  conn->sql_mode = full_sql_mode + 1;
  myf utf8_flag= thd->get_utf8_flag();
  if (thd && spider_param_remote_time_zone())
  {
    int tz_length = strlen(spider_param_remote_time_zone());
    String tz_str(spider_param_remote_time_zone(), tz_length,
      &my_charset_latin1);
    conn->time_zone = my_tz_find(thd, &tz_str);
  } else
    conn->time_zone = NULL;
  conn->trx_isolation = spider_param_remote_trx_isolation();
  DBUG_PRINT("info",("spider conn->trx_isolation=%d", conn->trx_isolation));
  if (spider_param_remote_access_charset())
  {
    if (!(conn->access_charset =
        get_charset_by_csname(spider_param_remote_access_charset(),
        MY_CS_PRIMARY, MYF(utf8_flag | MY_WME))))
      DBUG_RETURN(ER_UNKNOWN_CHARACTER_SET);
  } else
    conn->access_charset = NULL;
  char *default_database = spider_param_remote_default_database();
  if (default_database)
  {
    uint default_database_length = strlen(default_database);
    if (conn->default_database.reserve(default_database_length + 1))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    conn->default_database.q_append(default_database,
      default_database_length + 1);
    conn->default_database.length(default_database_length);
  } else
    conn->default_database.length(0);
  DBUG_RETURN(spider_conn_reset_queue_loop_check(conn));
}

int spider_free_conn_alloc(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_free_conn_alloc");
  spider_free_conn_thread(conn);
  spider_db_disconnect(conn);
  if (conn->db_conn)
  {
    delete conn->db_conn;
    conn->db_conn = NULL;
  }
  spider_conn_done(conn);
  DBUG_ASSERT(!conn->mta_conn_mutex_file_pos.file_name);
  pthread_mutex_destroy(&conn->mta_conn_mutex);
  conn->default_database.free();
  DBUG_RETURN(0);
}

void spider_free_conn_from_trx(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn,
  bool another,
  bool trx_free,
  int *roop_count
) {
  ha_spider *spider;
  SPIDER_IP_PORT_CONN *ip_port_conn = conn->ip_port_conn;
  DBUG_ENTER("spider_free_conn_from_trx");
  spider_conn_clear_queue(conn);
  conn->use_for_active_standby = FALSE;
  conn->error_mode = 1;
    if (
      trx_free ||
      (
        (
          conn->server_lost ||
          spider_param_conn_recycle_mode(trx->thd) != 2
        ) &&
        !conn->opened_handlers
      )
    ) {
      conn->thd = NULL;
      if (another)
      {
        ha_spider *next_spider;
        my_hash_delete(&trx->trx_another_conn_hash, (uchar*) conn);
        spider = (ha_spider*) conn->another_ha_first;
        while (spider)
        {
          next_spider = spider->next;
          spider_free_tmp_dbton_handler(spider);
          spider_free_tmp_dbton_share(spider->share);
          spider_free_tmp_share_alloc(spider->share);
          spider_free(spider_current_trx, spider->share, MYF(0));
          delete spider;
          spider = next_spider;
        }
        conn->another_ha_first = NULL;
        conn->another_ha_last = NULL;
      } else {
        my_hash_delete(&trx->trx_conn_hash, (uchar*) conn);
      }

      if (
        !trx_free &&
        !conn->server_lost &&
        !conn->queued_connect &&
        spider_param_conn_recycle_mode(trx->thd) == 1
      ) {
        /* conn_recycle_mode == 1 */
        *conn->conn_key = '0';
        conn->casual_read_base_conn = NULL;
        if (
          conn->quick_target &&
          spider_db_free_result((ha_spider *) conn->quick_target, FALSE)
        ) {
          spider_free_conn(conn);
        } else {
          pthread_mutex_lock(&spider_conn_mutex);
          uint old_elements = spider_open_connections.array.max_element;
          if (my_hash_insert(&spider_open_connections, (uchar*) conn))
          {
            pthread_mutex_unlock(&spider_conn_mutex);
            spider_free_conn(conn);
          } else {
            if (ip_port_conn)
            { /* exists */
              if (ip_port_conn->waiting_count)
              {
                pthread_mutex_lock(&ip_port_conn->mutex);
                pthread_cond_signal(&ip_port_conn->cond);
                pthread_mutex_unlock(&ip_port_conn->mutex);
              }
            }
            if (spider_open_connections.array.max_element > old_elements)
            {
              spider_alloc_calc_mem(spider_current_trx,
                spider_open_connections,
                (spider_open_connections.array.max_element - old_elements) *
                spider_open_connections.array.size_of_element);
            }
            pthread_mutex_unlock(&spider_conn_mutex);
          }
        }
      } else {
        /* conn_recycle_mode == 0 */
        if (conn->quick_target)
        {
          spider_db_free_result((ha_spider *) conn->quick_target, TRUE);
        }
        spider_free_conn(conn);
      }
    } else if (roop_count)
      (*roop_count)++;
  DBUG_VOID_RETURN;
}

SPIDER_CONN *spider_create_conn(
  SPIDER_SHARE *share,
  ha_spider *spider,
  int link_idx,
  int base_link_idx,
  uint conn_kind,
  int *error_num
) {
  int *need_mon;
  SPIDER_CONN *conn;
  SPIDER_IP_PORT_CONN *ip_port_conn;
  char *tmp_name, *tmp_host, *tmp_username, *tmp_password, *tmp_socket;
  char *tmp_wrapper, *tmp_db, *tmp_ssl_ca, *tmp_ssl_capath, *tmp_ssl_cert;
  char *tmp_ssl_cipher, *tmp_ssl_key, *tmp_default_file, *tmp_default_group;
  char *tmp_dsn, *tmp_filedsn, *tmp_driver;
  DBUG_ENTER("spider_create_conn");

  if (unlikely(!UTC))
  {
    /* UTC time zone for timestamp columns */
    String tz_00_name(STRING_WITH_LEN("+00:00"), &my_charset_bin);
    UTC = my_tz_find(current_thd, &tz_00_name);
  }

    bool tables_on_different_db_are_joinable;
    if (share->sql_dbton_ids[link_idx] != SPIDER_DBTON_SIZE)
    {
      tables_on_different_db_are_joinable =
        spider_dbton[share->sql_dbton_ids[link_idx]].db_util->
          tables_on_different_db_are_joinable();
    } else {
      tables_on_different_db_are_joinable = TRUE;
    }
    if (!(conn = (SPIDER_CONN *)
      spider_bulk_malloc(spider_current_trx, 18, MYF(MY_WME | MY_ZEROFILL),
        &conn, (uint) (sizeof(*conn)),
        &tmp_name, (uint) (share->conn_keys_lengths[link_idx] + 1),
        &tmp_host, (uint) (share->tgt_hosts_lengths[link_idx] + 1),
        &tmp_username,
          (uint) (share->tgt_usernames_lengths[link_idx] + 1),
        &tmp_password,
          (uint) (share->tgt_passwords_lengths[link_idx] + 1),
        &tmp_socket, (uint) (share->tgt_sockets_lengths[link_idx] + 1),
        &tmp_wrapper,
          (uint) (share->tgt_wrappers_lengths[link_idx] + 1),
        &tmp_db, (uint) (tables_on_different_db_are_joinable ?
          0 : share->tgt_dbs_lengths[link_idx] + 1),
        &tmp_ssl_ca, (uint) (share->tgt_ssl_cas_lengths[link_idx] + 1),
        &tmp_ssl_capath,
          (uint) (share->tgt_ssl_capaths_lengths[link_idx] + 1),
        &tmp_ssl_cert,
          (uint) (share->tgt_ssl_certs_lengths[link_idx] + 1),
        &tmp_ssl_cipher,
          (uint) (share->tgt_ssl_ciphers_lengths[link_idx] + 1),
        &tmp_ssl_key,
          (uint) (share->tgt_ssl_keys_lengths[link_idx] + 1),
        &tmp_default_file,
          (uint) (share->tgt_default_files_lengths[link_idx] + 1),
        &tmp_default_group,
          (uint) (share->tgt_default_groups_lengths[link_idx] + 1),
        &tmp_dsn,
          (uint) (share->tgt_dsns_lengths[link_idx] + 1),
        &tmp_filedsn,
          (uint) (share->tgt_filedsns_lengths[link_idx] + 1),
        &tmp_driver,
          (uint) (share->tgt_drivers_lengths[link_idx] + 1),
        &need_mon, (uint) (sizeof(int)),
        NullS))
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc_conn;
    }

    conn->default_database.init_calc_mem(75);
    conn->conn_key_length = share->conn_keys_lengths[link_idx];
    conn->conn_key = tmp_name;
    memcpy(conn->conn_key, share->conn_keys[link_idx],
      share->conn_keys_lengths[link_idx]);
    conn->conn_key_hash_value = share->conn_keys_hash_value[link_idx];
    conn->tgt_host_length = share->tgt_hosts_lengths[link_idx];
    conn->tgt_host = tmp_host;
    memcpy(conn->tgt_host, share->tgt_hosts[link_idx],
      share->tgt_hosts_lengths[link_idx]);
    conn->tgt_username_length = share->tgt_usernames_lengths[link_idx];
    conn->tgt_username = tmp_username;
    memcpy(conn->tgt_username, share->tgt_usernames[link_idx],
      share->tgt_usernames_lengths[link_idx]);
    conn->tgt_password_length = share->tgt_passwords_lengths[link_idx];
    conn->tgt_password = tmp_password;
    memcpy(conn->tgt_password, share->tgt_passwords[link_idx],
      share->tgt_passwords_lengths[link_idx]);
    conn->tgt_socket_length = share->tgt_sockets_lengths[link_idx];
    conn->tgt_socket = tmp_socket;
    memcpy(conn->tgt_socket, share->tgt_sockets[link_idx],
      share->tgt_sockets_lengths[link_idx]);
    conn->tgt_wrapper_length = share->tgt_wrappers_lengths[link_idx];
    conn->tgt_wrapper = tmp_wrapper;
    memcpy(conn->tgt_wrapper, share->tgt_wrappers[link_idx],
      share->tgt_wrappers_lengths[link_idx]);
    if (!tables_on_different_db_are_joinable)
    {
      conn->tgt_db_length = share->tgt_dbs_lengths[link_idx];
      conn->tgt_db = tmp_db;
      memcpy(conn->tgt_db, share->tgt_dbs[link_idx],
        share->tgt_dbs_lengths[link_idx]);
    }
    conn->tgt_ssl_ca_length = share->tgt_ssl_cas_lengths[link_idx];
    if (conn->tgt_ssl_ca_length)
    {
      conn->tgt_ssl_ca = tmp_ssl_ca;
      memcpy(conn->tgt_ssl_ca, share->tgt_ssl_cas[link_idx],
        share->tgt_ssl_cas_lengths[link_idx]);
    } else
      conn->tgt_ssl_ca = NULL;
    conn->tgt_ssl_capath_length = share->tgt_ssl_capaths_lengths[link_idx];
    if (conn->tgt_ssl_capath_length)
    {
      conn->tgt_ssl_capath = tmp_ssl_capath;
      memcpy(conn->tgt_ssl_capath, share->tgt_ssl_capaths[link_idx],
        share->tgt_ssl_capaths_lengths[link_idx]);
    } else
      conn->tgt_ssl_capath = NULL;
    conn->tgt_ssl_cert_length = share->tgt_ssl_certs_lengths[link_idx];
    if (conn->tgt_ssl_cert_length)
    {
      conn->tgt_ssl_cert = tmp_ssl_cert;
      memcpy(conn->tgt_ssl_cert, share->tgt_ssl_certs[link_idx],
        share->tgt_ssl_certs_lengths[link_idx]);
    } else
      conn->tgt_ssl_cert = NULL;
    conn->tgt_ssl_cipher_length = share->tgt_ssl_ciphers_lengths[link_idx];
    if (conn->tgt_ssl_cipher_length)
    {
      conn->tgt_ssl_cipher = tmp_ssl_cipher;
      memcpy(conn->tgt_ssl_cipher, share->tgt_ssl_ciphers[link_idx],
        share->tgt_ssl_ciphers_lengths[link_idx]);
    } else
      conn->tgt_ssl_cipher = NULL;
    conn->tgt_ssl_key_length = share->tgt_ssl_keys_lengths[link_idx];
    if (conn->tgt_ssl_key_length)
    {
      conn->tgt_ssl_key = tmp_ssl_key;
      memcpy(conn->tgt_ssl_key, share->tgt_ssl_keys[link_idx],
        share->tgt_ssl_keys_lengths[link_idx]);
    } else
      conn->tgt_ssl_key = NULL;
    conn->tgt_default_file_length = share->tgt_default_files_lengths[link_idx];
    if (conn->tgt_default_file_length)
    {
      conn->tgt_default_file = tmp_default_file;
      memcpy(conn->tgt_default_file, share->tgt_default_files[link_idx],
        share->tgt_default_files_lengths[link_idx]);
    } else
      conn->tgt_default_file = NULL;
    conn->tgt_default_group_length =
      share->tgt_default_groups_lengths[link_idx];
    if (conn->tgt_default_group_length)
    {
      conn->tgt_default_group = tmp_default_group;
      memcpy(conn->tgt_default_group, share->tgt_default_groups[link_idx],
        share->tgt_default_groups_lengths[link_idx]);
    } else
      conn->tgt_default_group = NULL;
    conn->tgt_dsn_length =
      share->tgt_dsns_lengths[link_idx];
    if (conn->tgt_dsn_length)
    {
      conn->tgt_dsn = tmp_dsn;
      memcpy(conn->tgt_dsn, share->tgt_dsns[link_idx],
        share->tgt_dsns_lengths[link_idx]);
    } else
      conn->tgt_dsn = NULL;
    conn->tgt_filedsn_length =
      share->tgt_filedsns_lengths[link_idx];
    if (conn->tgt_filedsn_length)
    {
      conn->tgt_filedsn = tmp_filedsn;
      memcpy(conn->tgt_filedsn, share->tgt_filedsns[link_idx],
        share->tgt_filedsns_lengths[link_idx]);
    } else
      conn->tgt_filedsn = NULL;
    conn->tgt_driver_length =
      share->tgt_drivers_lengths[link_idx];
    if (conn->tgt_driver_length)
    {
      conn->tgt_driver = tmp_driver;
      memcpy(conn->tgt_driver, share->tgt_drivers[link_idx],
        share->tgt_drivers_lengths[link_idx]);
    } else
      conn->tgt_driver = NULL;
    conn->tgt_port = share->tgt_ports[link_idx];
    conn->tgt_ssl_vsc = share->tgt_ssl_vscs[link_idx];
    conn->dbton_id = share->sql_dbton_ids[link_idx];
  if (conn->dbton_id == SPIDER_DBTON_SIZE)
  {
      my_printf_error(
        ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM,
        ER_SPIDER_SQL_WRAPPER_IS_INVALID_STR,
        MYF(0), conn->tgt_wrapper);
      *error_num = ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM;
    goto error_invalid_wrapper;
  }
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
  conn->link_idx = base_link_idx;
  conn->conn_kind = conn_kind;
  conn->conn_need_mon = need_mon;
  if (spider)
    conn->need_mon = &spider->need_mons[base_link_idx];
  else
    conn->need_mon = need_mon;

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

  spider_conn_queue_connect(share, conn, link_idx);
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
        *error_num = ER_SPIDER_CON_COUNT_ERROR;
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

error_too_many_ipport_count:
  spider_conn_done(conn);
error_conn_init:
  pthread_mutex_destroy(&conn->mta_conn_mutex);
error_mta_conn_mutex_init:
error_db_conn_init:
  delete conn->db_conn;
error_db_conn_create:
error_invalid_wrapper:
  spider_free(spider_current_trx, conn, MYF(0));
error_alloc_conn:
  DBUG_RETURN(NULL);
}

SPIDER_CONN *spider_get_conn(
  SPIDER_SHARE *share,
  int link_idx,
  char *conn_key,
  SPIDER_TRX *trx,
  ha_spider *spider,
  bool another,
  bool thd_chg,
  uint conn_kind,
  int *error_num
) {
  SPIDER_CONN *conn = NULL;
  int base_link_idx = link_idx;
  DBUG_ENTER("spider_get_conn");
  DBUG_PRINT("info",("spider conn_kind=%u", conn_kind));

  if (spider)
    link_idx = spider->conn_link_idx[base_link_idx];
  DBUG_PRINT("info",("spider link_idx=%u", link_idx));
  DBUG_PRINT("info",("spider base_link_idx=%u", base_link_idx));

#ifndef DBUG_OFF
    spider_print_keys(conn_key, share->conn_keys_lengths[link_idx]);
#endif
  if (
        (another &&
          !(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
            &trx->trx_another_conn_hash,
            share->conn_keys_hash_value[link_idx],
            (uchar*) conn_key, share->conn_keys_lengths[link_idx]))) ||
        (!another &&
          !(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
            &trx->trx_conn_hash,
            share->conn_keys_hash_value[link_idx],
            (uchar*) conn_key, share->conn_keys_lengths[link_idx])))
  )
  {
    if (
      !trx->thd ||
        (
          (spider_param_conn_recycle_mode(trx->thd) & 1) ||
          spider_param_conn_recycle_strict(trx->thd)
        )
    ) {
        pthread_mutex_lock(&spider_conn_mutex);
        if (!(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
          &spider_open_connections, share->conn_keys_hash_value[link_idx],
          (uchar*) share->conn_keys[link_idx],
          share->conn_keys_lengths[link_idx])))
        {
          pthread_mutex_unlock(&spider_conn_mutex);
          if (spider_param_max_connections())
          { /* enable connection pool */
            conn = spider_get_conn_from_idle_connection(share, link_idx, conn_key, spider, conn_kind, base_link_idx, error_num);
            /* failed get conn, goto error */
            if (!conn)
              goto error;

          }
          else
          { /* did not enable conncetion pool , create_conn */
            DBUG_PRINT("info",("spider create new conn"));
            if (!(conn = spider_create_conn(share, spider, link_idx,
              base_link_idx, conn_kind, error_num)))
              goto error;
            *conn->conn_key = *conn_key;
            if (spider)
            {
              spider->conns[base_link_idx] = conn;
              if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
                conn->use_for_active_standby = TRUE;
            }
          }
        } else {
          my_hash_delete(&spider_open_connections, (uchar*) conn);
          pthread_mutex_unlock(&spider_conn_mutex);
          DBUG_PRINT("info",("spider get global conn"));
          if (spider)
          {
            spider->conns[base_link_idx] = conn;
            if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
              conn->use_for_active_standby = TRUE;
          }
        }
    } else {
      DBUG_PRINT("info",("spider create new conn"));
      /* conn_recycle_strict = 0 and conn_recycle_mode = 0 or 2 */
      if (!(conn = spider_create_conn(share, spider, link_idx, base_link_idx,
        conn_kind, error_num)))
        goto error;
      *conn->conn_key = *conn_key;
      if (spider)
      {
          spider->conns[base_link_idx] = conn;
        if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
          conn->use_for_active_standby = TRUE;
      }
    }
    conn->thd = trx->thd;
    conn->priority = share->priority;

      if (another)
      {
        uint old_elements = trx->trx_another_conn_hash.array.max_element;
        if (my_hash_insert(&trx->trx_another_conn_hash, (uchar*) conn))
        {
          spider_free_conn(conn);
          *error_num = HA_ERR_OUT_OF_MEM;
          goto error;
        }
        if (trx->trx_another_conn_hash.array.max_element > old_elements)
        {
          spider_alloc_calc_mem(spider_current_trx,
            trx->trx_another_conn_hash,
            (trx->trx_another_conn_hash.array.max_element - old_elements) *
            trx->trx_another_conn_hash.array.size_of_element);
        }
      } else {
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
  } else if (spider)
  {
      spider->conns[base_link_idx] = conn;
    if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
      conn->use_for_active_standby = TRUE;
  }
  conn->link_idx = base_link_idx;

  if (conn->queued_connect)
    spider_conn_queue_connect_rewrite(share, conn, link_idx);

  if (conn->queued_ping)
  {
    if (spider)
      spider_conn_queue_ping_rewrite(spider, conn, base_link_idx);
    else
      conn->queued_ping = FALSE;
  }

    if (unlikely(spider && spider->wide_handler->top_share &&
      (*error_num = spider_conn_queue_loop_check(
        conn, spider, base_link_idx))))
    {
      goto error;
    }

  DBUG_PRINT("info",("spider conn=%p", conn));
  DBUG_RETURN(conn);

error:
  DBUG_RETURN(NULL);
}

int spider_free_conn(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_free_conn");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  SPIDER_IP_PORT_CONN* ip_port_conn = conn->ip_port_conn;
  if (ip_port_conn)
  { /* free conn, ip_port_count-- */
    pthread_mutex_lock(&ip_port_conn->mutex);
    if (ip_port_conn->ip_port_count > 0)
      ip_port_conn->ip_port_count--;
    pthread_mutex_unlock(&ip_port_conn->mutex);
  }
  spider_free_conn_alloc(conn);
  spider_free(spider_current_trx, conn, MYF(0));
  DBUG_RETURN(0);
}

int spider_check_and_get_casual_read_conn(
  THD *thd,
  ha_spider *spider,
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_check_and_get_casual_read_conn");
  if (spider->result_list.casual_read[link_idx])
  {
    SPIDER_CONN *conn = spider->conns[link_idx];
    if (conn->casual_read_query_id != thd->query_id)
    {
      conn->casual_read_query_id = thd->query_id;
      conn->casual_read_current_id = 2;
    }
    if (spider->result_list.casual_read[link_idx] == 1)
    {
      spider->result_list.casual_read[link_idx] = conn->casual_read_current_id;
      ++conn->casual_read_current_id;
      if (conn->casual_read_current_id > 63)
      {
        conn->casual_read_current_id = 2;
      }
    }
    char first_byte_bak = *spider->conn_keys[link_idx];
    *spider->conn_keys[link_idx] =
      '0' + spider->result_list.casual_read[link_idx];
    if (
      !(spider->conns[link_idx] =
        spider_get_conn(spider->share, link_idx,
          spider->conn_keys[link_idx], spider->wide_handler->trx,
          spider, FALSE, TRUE, SPIDER_CONN_KIND_MYSQL,
          &error_num))
    ) {
      *spider->conn_keys[link_idx] = first_byte_bak;
      DBUG_RETURN(error_num);
    }
    *spider->conn_keys[link_idx] = first_byte_bak;
    spider->conns[link_idx]->casual_read_base_conn = conn;
    conn = spider->conns[link_idx];
    spider_check_and_set_autocommit(thd, conn, NULL);
  }
  DBUG_RETURN(0);
}

int spider_check_and_init_casual_read(
  THD *thd,
  ha_spider *spider,
  int link_idx
) {
  int error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_check_and_init_casual_read");
  if (
    spider_param_sync_autocommit(thd) &&
    (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) &&
    (
      result_list->direct_order_limit
      || result_list->direct_aggregate
    )
  ) {
    if (!result_list->casual_read[link_idx])
    {
      result_list->casual_read[link_idx] =
        spider_param_casual_read(thd, share->casual_read);
    }
    if ((error_num = spider_check_and_get_casual_read_conn(thd, spider,
      link_idx)))
    {
      DBUG_RETURN(error_num);
    }
    SPIDER_CONN *conn = spider->conns[link_idx];
    if (
      conn->casual_read_base_conn &&
      (error_num = spider_create_conn_thread(conn))
    ) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

void spider_conn_queue_connect(
  SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_conn_queue_connect");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_connect = TRUE;
/*
  conn->queued_connect_share = share;
  conn->queued_connect_link_idx = link_idx;
*/
  DBUG_VOID_RETURN;
}

void spider_conn_queue_connect_rewrite(
  SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_conn_queue_connect_rewrite");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_connect_share = share;
  conn->queued_connect_link_idx = link_idx;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_ping(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_conn_queue_ping");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_ping = TRUE;
  conn->queued_ping_spider = spider;
  conn->queued_ping_link_idx = link_idx;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_ping_rewrite(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_conn_queue_ping_rewrite");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_ping_spider = spider;
  conn->queued_ping_link_idx = link_idx;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_trx_isolation(
  SPIDER_CONN *conn,
  int trx_isolation
) {
  DBUG_ENTER("spider_conn_queue_trx_isolation");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_trx_isolation = TRUE;
  conn->queued_trx_isolation_val = trx_isolation;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_semi_trx_isolation(
  SPIDER_CONN *conn,
  int trx_isolation
) {
  DBUG_ENTER("spider_conn_queue_semi_trx_isolation");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_semi_trx_isolation = TRUE;
  conn->queued_semi_trx_isolation_val = trx_isolation;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_autocommit(
  SPIDER_CONN *conn,
  bool autocommit
) {
  DBUG_ENTER("spider_conn_queue_autocommit");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_autocommit = TRUE;
  conn->queued_autocommit_val = autocommit;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_sql_log_off(
  SPIDER_CONN *conn,
  bool sql_log_off
) {
  DBUG_ENTER("spider_conn_queue_sql_log_off");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_sql_log_off = TRUE;
  conn->queued_sql_log_off_val = sql_log_off;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_wait_timeout(
  SPIDER_CONN *conn,
  int wait_timeout
) {
  DBUG_ENTER("spider_conn_queue_wait_timeout");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  if (wait_timeout > 0)
  {
    conn->queued_wait_timeout = TRUE;
    conn->queued_wait_timeout_val = wait_timeout;
  }
  DBUG_VOID_RETURN;
}

void spider_conn_queue_sql_mode(
  SPIDER_CONN *conn,
  sql_mode_t sql_mode
) {
  DBUG_ENTER("spider_conn_queue_sql_mode");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  DBUG_ASSERT(!(sql_mode & ~full_sql_mode));
  conn->queued_sql_mode = TRUE;
  conn->queued_sql_mode_val = (sql_mode & pushdown_sql_mode);
  DBUG_VOID_RETURN;
}

void spider_conn_queue_time_zone(
  SPIDER_CONN *conn,
  Time_zone *time_zone
) {
  DBUG_ENTER("spider_conn_queue_time_zone");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_time_zone = TRUE;
  conn->queued_time_zone_val = time_zone;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_UTC_time_zone(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_conn_queue_UTC_time_zone");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  spider_conn_queue_time_zone(conn, UTC);
  DBUG_VOID_RETURN;
}

int spider_conn_queue_and_merge_loop_check(
  SPIDER_CONN *conn,
  SPIDER_CONN_LOOP_CHECK *lcptr
) {
  int error_num = HA_ERR_OUT_OF_MEM;
  char *tmp_name, *from_name, *cur_name, *to_name, *full_name, *from_value,
    *merged_value;
  SPIDER_CONN_LOOP_CHECK *lcqptr, *lcrptr;
  DBUG_ENTER("spider_conn_queue_and_merge_loop_check");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  if (unlikely(!(lcqptr = (SPIDER_CONN_LOOP_CHECK *)
    my_hash_search_using_hash_value(&conn->loop_check_queue,
    lcptr->hash_value_to,
    (uchar *) lcptr->to_name.str, lcptr->to_name.length))))
  {
    DBUG_PRINT("info", ("spider create merged_value and insert"));
    lcptr->merged_value.length = spider_unique_id.length +
      lcptr->cur_name.length + lcptr->from_value.length + 1;
    tmp_name = (char *) lcptr->merged_value.str;
    memcpy(tmp_name, spider_unique_id.str, spider_unique_id.length);
    tmp_name += spider_unique_id.length;
    memcpy(tmp_name, lcptr->cur_name.str, lcptr->cur_name.length);
    tmp_name += lcptr->cur_name.length;
    *tmp_name = '-';
    ++tmp_name;
    memcpy(tmp_name, lcptr->from_value.str, lcptr->from_value.length + 1);
    if (unlikely(my_hash_insert(&conn->loop_check_queue, (uchar *) lcptr)))
    {
      goto error_hash_insert_queue;
    }
    lcptr->flag |= SPIDER_LOP_CHK_QUEUED;
  } else {
    DBUG_PRINT("info", ("spider append merged_value and replace"));
    if (unlikely(!spider_bulk_malloc(spider_current_trx, 271, MYF(MY_WME),
      &lcrptr, (uint) (sizeof(SPIDER_CONN_LOOP_CHECK)),
      &from_name, (uint) (lcqptr->from_name.length + 1),
      &cur_name, (uint) (lcqptr->cur_name.length + 1),
      &to_name, (uint) (lcqptr->to_name.length + 1),
      &full_name, (uint) (lcqptr->full_name.length + 1),
      &from_value, (uint) (lcqptr->from_value.length + 1),
      &merged_value, (uint) (lcqptr->merged_value.length +
        spider_unique_id.length + lcptr->cur_name.length +
        lcptr->from_value.length + 2),
      NullS)
    )) {
      goto error_alloc_loop_check_replace;
    }
    lcrptr->hash_value_to = lcqptr->hash_value_to;
    lcrptr->hash_value_full = lcqptr->hash_value_full;
    lcrptr->from_name.str = from_name;
    lcrptr->from_name.length = lcqptr->from_name.length;
    memcpy(from_name, lcqptr->from_name.str, lcqptr->from_name.length + 1);
    lcrptr->cur_name.str = cur_name;
    lcrptr->cur_name.length = lcqptr->cur_name.length;
    memcpy(cur_name, lcqptr->cur_name.str, lcqptr->cur_name.length + 1);
    lcrptr->to_name.str = to_name;
    lcrptr->to_name.length = lcqptr->to_name.length;
    memcpy(to_name, lcqptr->to_name.str, lcqptr->to_name.length + 1);
    lcrptr->full_name.str = full_name;
    lcrptr->full_name.length = lcqptr->full_name.length;
    memcpy(full_name, lcqptr->full_name.str, lcqptr->full_name.length + 1);
    lcrptr->from_value.str = from_value;
    lcrptr->from_value.length = lcqptr->from_value.length;
    memcpy(from_value, lcqptr->from_value.str, lcqptr->from_value.length + 1);
    lcrptr->merged_value.str = merged_value;
    lcrptr->merged_value.length = lcqptr->merged_value.length;
    memcpy(merged_value,
      lcqptr->merged_value.str, lcqptr->merged_value.length);
    merged_value += lcqptr->merged_value.length;
    memcpy(merged_value, spider_unique_id.str, spider_unique_id.length);
    merged_value += spider_unique_id.length;
    memcpy(merged_value, lcptr->cur_name.str, lcptr->cur_name.length);
    merged_value += lcptr->cur_name.length;
    *merged_value = '-';
    ++merged_value;
    memcpy(merged_value, lcptr->from_value.str, lcptr->from_value.length + 1);

    DBUG_PRINT("info", ("spider free lcqptr"));
    my_hash_delete(&conn->loop_checked, (uchar*) lcqptr);
    my_hash_delete(&conn->loop_check_queue, (uchar*) lcqptr);
    spider_free(spider_current_trx, lcqptr, MYF(0));

    lcptr = lcrptr;
    if (unlikely(my_hash_insert(&conn->loop_checked, (uchar *) lcptr)))
    {
      goto error_hash_insert;
    }
    if (unlikely(my_hash_insert(&conn->loop_check_queue, (uchar *) lcptr)))
    {
      goto error_hash_insert_queue;
    }
    lcptr->flag = SPIDER_LOP_CHK_MERAGED;
    lcptr->next = NULL;
    if (!conn->loop_check_meraged_first)
    {
      conn->loop_check_meraged_first = lcptr;
      conn->loop_check_meraged_last = lcptr;
    } else {
      conn->loop_check_meraged_last->next = lcptr;
      conn->loop_check_meraged_last = lcptr;
    }
  }
  DBUG_RETURN(0);

error_alloc_loop_check_replace:
error_hash_insert_queue:
  my_hash_delete(&conn->loop_checked, (uchar*) lcptr);
error_hash_insert:
  spider_free(spider_current_trx, lcptr, MYF(0));
  pthread_mutex_unlock(&conn->loop_check_mutex);
  DBUG_RETURN(error_num);
}

int spider_conn_reset_queue_loop_check(
  SPIDER_CONN *conn
) {
  int error_num;
  SPIDER_CONN_LOOP_CHECK *lcptr;
  DBUG_ENTER("spider_conn_reset_queue_loop_check");
  uint l = 0;
  pthread_mutex_lock(&conn->loop_check_mutex);
  while ((lcptr = (SPIDER_CONN_LOOP_CHECK *) my_hash_element(
    &conn->loop_checked, l)))
  {
    if (!lcptr->flag)
    {
      DBUG_PRINT("info", ("spider free lcptr"));
      my_hash_delete(&conn->loop_checked, (uchar*) lcptr);
      spider_free(spider_current_trx, lcptr, MYF(0));
    }
    ++l;
  }

  lcptr = conn->loop_check_ignored_first;
  while (lcptr)
  {
    lcptr->flag = 0;
    if ((error_num = spider_conn_queue_and_merge_loop_check(conn, lcptr)))
    {
      goto error_queue_and_merge;
    }
    lcptr = lcptr->next;
  }
  conn->loop_check_meraged_first = NULL;
  pthread_mutex_unlock(&conn->loop_check_mutex);
  DBUG_RETURN(0);

error_queue_and_merge:
  lcptr = lcptr->next;
  while (lcptr)
  {
    lcptr->flag = 0;
    lcptr = lcptr->next;
  }
  conn->loop_check_meraged_first = NULL;
  pthread_mutex_unlock(&conn->loop_check_mutex);
  DBUG_RETURN(error_num);
}

int spider_conn_queue_loop_check(
  SPIDER_CONN *conn,
  ha_spider *spider,
  int link_idx
) {
  int error_num = HA_ERR_OUT_OF_MEM;
  uint conn_link_idx = spider->conn_link_idx[link_idx], buf_sz;
  char path[FN_REFLEN + 1];
  char *tmp_name, *from_name, *cur_name, *to_name, *full_name, *from_value,
    *merged_value;
  user_var_entry *loop_check;
  char *loop_check_buf;
  THD *thd = spider->wide_handler->trx->thd;
  TABLE_SHARE *top_share = spider->wide_handler->top_share;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN_LOOP_CHECK *lcptr;
  LEX_CSTRING lex_str, from_str, to_str;
  DBUG_ENTER("spider_conn_queue_loop_check");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  lex_str.length = top_share->path.length + SPIDER_SQL_LOP_CHK_PRM_PRF_LEN;
  buf_sz = lex_str.length + 2;
  loop_check_buf = (char *) my_alloca(buf_sz);
  if (unlikely(!loop_check_buf))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  lex_str.str = loop_check_buf;
  memcpy(loop_check_buf,
    SPIDER_SQL_LOP_CHK_PRM_PRF_STR, SPIDER_SQL_LOP_CHK_PRM_PRF_LEN);
  memcpy(loop_check_buf + SPIDER_SQL_LOP_CHK_PRM_PRF_LEN,
    top_share->path.str, top_share->path.length);
  loop_check_buf[lex_str.length] = '\0';
  DBUG_PRINT("info", ("spider param name=%s", lex_str.str));
  loop_check = get_variable(&thd->user_vars, &lex_str, FALSE);
  if (!loop_check || loop_check->type != STRING_RESULT)
  {
    DBUG_PRINT("info", ("spider client is not Spider"));
    lex_str.str = "";
    lex_str.length = 0;
    from_str.str = "";
    from_str.length = 0;
  } else {
    lex_str.str = loop_check->value;
    lex_str.length = loop_check->length;
    DBUG_PRINT("info", ("spider from_str=%s", lex_str.str));
    if (unlikely(!(tmp_name = strchr(loop_check->value, '-'))))
    {
      DBUG_PRINT("info", ("spider invalid value for loop checking 1"));
      from_str.str = "";
      from_str.length = 0;
    }
    else if (unlikely(!(tmp_name = strchr(tmp_name + 1, '-'))))
    {
      DBUG_PRINT("info", ("spider invalid value for loop checking 2"));
      from_str.str = "";
      from_str.length = 0;
    }
    else if (unlikely(!(tmp_name = strchr(tmp_name + 1, '-'))))
    {
      DBUG_PRINT("info", ("spider invalid value for loop checking 3"));
      from_str.str = "";
      from_str.length = 0;
    }
    else if (unlikely(!(tmp_name = strchr(tmp_name + 1, '-'))))
    {
      DBUG_PRINT("info", ("spider invalid value for loop checking 4"));
      from_str.str = "";
      from_str.length = 0;
    }
    else
    {
      from_str.str = lex_str.str;
      from_str.length = tmp_name - lex_str.str + 1;
    }
  }
  my_afree(loop_check_buf);

  to_str.length = build_table_filename(path, FN_REFLEN,
    share->tgt_dbs[conn_link_idx] ? share->tgt_dbs[conn_link_idx] : "",
    share->tgt_table_names[conn_link_idx], "", 0);
  to_str.str = path;
  DBUG_PRINT("info", ("spider to=%s", to_str.str));
  buf_sz = from_str.length + top_share->path.length + to_str.length + 3;
  loop_check_buf = (char *) my_alloca(buf_sz);
  if (unlikely(!loop_check_buf))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  DBUG_PRINT("info", ("spider top_share->path=%s", top_share->path.str));
  memcpy(loop_check_buf, from_str.str, from_str.length);
  tmp_name = loop_check_buf + from_str.length;
  *tmp_name = '-';
  ++tmp_name;
  memcpy(tmp_name, top_share->path.str, top_share->path.length);
  tmp_name += top_share->path.length;
  *tmp_name = '-';
  ++tmp_name;
  memcpy(tmp_name, to_str.str, to_str.length);
  tmp_name += to_str.length;
  *tmp_name = '\0';
  my_hash_value_type hash_value = my_calc_hash(&conn->loop_checked,
    (uchar *) loop_check_buf, buf_sz - 1);
  pthread_mutex_lock(&conn->loop_check_mutex);
  lcptr = (SPIDER_CONN_LOOP_CHECK *)
    my_hash_search_using_hash_value(&conn->loop_checked, hash_value,
    (uchar *) loop_check_buf, buf_sz - 1);
  if (unlikely(
    !lcptr ||
    (
      !lcptr->flag &&
      (
        lcptr->from_value.length != lex_str.length ||
        memcmp(lcptr->from_value.str, lex_str.str, lex_str.length)
      )
    )
  ))
  {
    if (unlikely(lcptr))
    {
      DBUG_PRINT("info", ("spider free lcptr"));
      my_hash_delete(&conn->loop_checked, (uchar*) lcptr);
      spider_free(spider_current_trx, lcptr, MYF(0));
    }
    DBUG_PRINT("info", ("spider alloc_lcptr"));
    if (unlikely(!spider_bulk_malloc(spider_current_trx, 272, MYF(MY_WME),
      &lcptr, (uint) (sizeof(SPIDER_CONN_LOOP_CHECK)),
      &from_name, (uint) (from_str.length + 1),
      &cur_name, (uint) (top_share->path.length + 1),
      &to_name, (uint) (to_str.length + 1),
      &full_name, (uint) (buf_sz),
      &from_value, (uint) (lex_str.length + 1),
      &merged_value, (uint) (spider_unique_id.length + top_share->path.length +
        lex_str.length + 2),
      NullS)
    )) {
      my_afree(loop_check_buf);
      goto error_alloc_loop_check;
    }
    lcptr->flag = 0;
    lcptr->from_name.str = from_name;
    lcptr->from_name.length = from_str.length;
    memcpy(from_name, from_str.str, from_str.length + 1);
    lcptr->cur_name.str = cur_name;
    lcptr->cur_name.length = top_share->path.length;
    memcpy(cur_name, top_share->path.str, top_share->path.length + 1);
    lcptr->to_name.str = to_name;
    lcptr->to_name.length = to_str.length;
    memcpy(to_name, to_str.str, to_str.length + 1);
    lcptr->full_name.str = full_name;
    lcptr->full_name.length = buf_sz - 1;
    memcpy(full_name, loop_check_buf, buf_sz);
    lcptr->from_value.str = from_value;
    lcptr->from_value.length = lex_str.length;
    memcpy(from_value, lex_str.str, lex_str.length + 1);
    lcptr->merged_value.str = merged_value;
    lcptr->hash_value_to = my_calc_hash(&conn->loop_checked,
      (uchar *) to_str.str, to_str.length);
    lcptr->hash_value_full = hash_value;
    if (unlikely(my_hash_insert(&conn->loop_checked, (uchar *) lcptr)))
    {
      my_afree(loop_check_buf);
      goto error_hash_insert;
    }
  } else {
    if (!lcptr->flag)
    {
      DBUG_PRINT("info", ("spider add to ignored list"));
      lcptr->flag |= SPIDER_LOP_CHK_IGNORED;
      lcptr->next = NULL;
      if (!conn->loop_check_ignored_first)
      {
        conn->loop_check_ignored_first = lcptr;
        conn->loop_check_ignored_last = lcptr;
      } else {
        conn->loop_check_ignored_last->next = lcptr;
        conn->loop_check_ignored_last = lcptr;
      }
    }
    pthread_mutex_unlock(&conn->loop_check_mutex);
    my_afree(loop_check_buf);
    DBUG_PRINT("info", ("spider be sent or queued already"));
    DBUG_RETURN(0);
  }
  my_afree(loop_check_buf);

  if ((error_num = spider_conn_queue_and_merge_loop_check(conn, lcptr)))
  {
    goto error_queue_and_merge;
  }
  pthread_mutex_unlock(&conn->loop_check_mutex);
  DBUG_RETURN(0);

error_hash_insert:
  spider_free(spider_current_trx, lcptr, MYF(0));
error_queue_and_merge:
  pthread_mutex_unlock(&conn->loop_check_mutex);
error_alloc_loop_check:
  DBUG_RETURN(error_num);
}

void spider_conn_queue_start_transaction(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_conn_queue_start_transaction");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  DBUG_ASSERT(!conn->trx_start);
  conn->queued_trx_start = TRUE;
  conn->trx_start = TRUE;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_xa_start(
  SPIDER_CONN *conn,
  XID *xid
) {
  DBUG_ENTER("spider_conn_queue_xa_start");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_xa_start = TRUE;
  conn->queued_xa_start_xid = xid;
  DBUG_VOID_RETURN;
}

void spider_conn_clear_queue(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_conn_clear_queue");
  DBUG_PRINT("info", ("spider conn=%p", conn));
/*
  conn->queued_connect = FALSE;
  conn->queued_ping = FALSE;
*/
  conn->queued_trx_isolation = FALSE;
  conn->queued_semi_trx_isolation = FALSE;
  conn->queued_autocommit = FALSE;
  conn->queued_sql_log_off = FALSE;
  conn->queued_wait_timeout = FALSE;
  conn->queued_sql_mode = FALSE;
  conn->queued_time_zone = FALSE;
  conn->queued_trx_start = FALSE;
  conn->queued_xa_start = FALSE;
  DBUG_VOID_RETURN;
}

void spider_conn_clear_queue_at_commit(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_conn_clear_queue_at_commit");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  if (conn->queued_trx_start)
  {
    conn->queued_trx_start = FALSE;
    conn->trx_start = FALSE;
  }
  conn->queued_xa_start = FALSE;
  DBUG_VOID_RETURN;
}

void spider_conn_set_timeout(
  SPIDER_CONN *conn,
  uint net_read_timeout,
  uint net_write_timeout
) {
  DBUG_ENTER("spider_conn_set_timeout");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  if (net_read_timeout != conn->net_read_timeout)
  {
    DBUG_PRINT("info",("spider net_read_timeout set from %u to %u",
      conn->net_read_timeout, net_read_timeout));
    conn->queued_net_timeout = TRUE;
    conn->net_read_timeout = net_read_timeout;
  }
  if (net_write_timeout != conn->net_write_timeout)
  {
    DBUG_PRINT("info",("spider net_write_timeout set from %u to %u",
      conn->net_write_timeout, net_write_timeout));
    conn->queued_net_timeout = TRUE;
    conn->net_write_timeout = net_write_timeout;
  }
  DBUG_VOID_RETURN;
}

void spider_conn_set_timeout_from_share(
  SPIDER_CONN *conn,
  int link_idx,
  THD *thd,
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_conn_set_timeout_from_share");
  spider_conn_set_timeout(
    conn,
    spider_param_net_read_timeout(thd, share->net_read_timeouts[link_idx]),
    spider_param_net_write_timeout(thd, share->net_write_timeouts[link_idx])
  );
  DBUG_VOID_RETURN;
}

void spider_conn_set_timeout_from_direct_sql(
  SPIDER_CONN *conn,
  THD *thd,
  SPIDER_DIRECT_SQL *direct_sql
) {
  DBUG_ENTER("spider_conn_set_timeout_from_direct_sql");
  spider_conn_set_timeout(
    conn,
    spider_param_net_read_timeout(thd, direct_sql->net_read_timeout),
    spider_param_net_write_timeout(thd, direct_sql->net_write_timeout)
  );
  DBUG_VOID_RETURN;
}

void spider_tree_insert(
  SPIDER_CONN *top,
  SPIDER_CONN *conn
) {
  SPIDER_CONN *current = top;
  longlong priority = conn->priority;
  DBUG_ENTER("spider_tree_insert");
  while (TRUE)
  {
    if (priority < current->priority)
    {
      if (current->c_small == NULL)
      {
        conn->p_small = NULL;
        conn->p_big = current;
        conn->c_small = NULL;
        conn->c_big = NULL;
        current->c_small = conn;
        break;
      } else
        current = current->c_small;
    } else {
      if (current->c_big == NULL)
      {
        conn->p_small = current;
        conn->p_big = NULL;
        conn->c_small = NULL;
        conn->c_big = NULL;
        current->c_big = conn;
        break;
      } else
        current = current->c_big;
    }
  }
  DBUG_VOID_RETURN;
}

SPIDER_CONN *spider_tree_first(
  SPIDER_CONN *top
) {
  SPIDER_CONN *current = top;
  DBUG_ENTER("spider_tree_first");
  while (current)
  {
    if (current->c_small == NULL)
      break;
    else
      current = current->c_small;
  }
  DBUG_RETURN(current);
}

SPIDER_CONN *spider_tree_last(
  SPIDER_CONN *top
) {
  SPIDER_CONN *current = top;
  DBUG_ENTER("spider_tree_last");
  while (TRUE)
  {
    if (current->c_big == NULL)
      break;
    else
      current = current->c_big;
  }
  DBUG_RETURN(current);
}

SPIDER_CONN *spider_tree_next(
  SPIDER_CONN *current
) {
  DBUG_ENTER("spider_tree_next");
  if (current->c_big)
    DBUG_RETURN(spider_tree_first(current->c_big));
  while (TRUE)
  {
    if (current->p_big)
      DBUG_RETURN(current->p_big);
    if (!current->p_small)
      DBUG_RETURN(NULL);
    current = current->p_small;
  }
}

SPIDER_CONN *spider_tree_delete(
  SPIDER_CONN *conn,
  SPIDER_CONN *top
) {
  DBUG_ENTER("spider_tree_delete");
  if (conn->p_small)
  {
    if (conn->c_small)
    {
      conn->c_small->p_big = NULL;
      conn->c_small->p_small = conn->p_small;
      conn->p_small->c_big = conn->c_small;
      if (conn->c_big)
      {
        SPIDER_CONN *last = spider_tree_last(conn->c_small);
        conn->c_big->p_small = last;
        last->c_big = conn->c_big;
      }
    } else if (conn->c_big)
    {
      conn->c_big->p_small = conn->p_small;
      conn->p_small->c_big = conn->c_big;
    } else
      conn->p_small->c_big = NULL;
  } else if (conn->p_big)
  {
    if (conn->c_small)
    {
      conn->c_small->p_big = conn->p_big;
      conn->p_big->c_small = conn->c_small;
      if (conn->c_big)
      {
        SPIDER_CONN *last = spider_tree_last(conn->c_small);
        conn->c_big->p_small = last;
        last->c_big = conn->c_big;
      }
    } else if (conn->c_big)
    {
      conn->c_big->p_big = conn->p_big;
      conn->c_big->p_small = NULL;
      conn->p_big->c_small = conn->c_big;
    } else
      conn->p_big->c_small = NULL;
  } else {
    if (conn->c_small)
    {
      conn->c_small->p_big = NULL;
      conn->c_small->p_small = NULL;
      if (conn->c_big)
      {
        SPIDER_CONN *last = spider_tree_last(conn->c_small);
        conn->c_big->p_small = last;
        last->c_big = conn->c_big;
      }
      DBUG_RETURN(conn->c_small);
    } else if (conn->c_big)
    {
      conn->c_big->p_small = NULL;
      DBUG_RETURN(conn->c_big);
    }
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(top);
}

int spider_set_conn_bg_param(
  ha_spider *spider
) {
  int error_num, roop_count, bgs_mode;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_WIDE_HANDLER *wide_handler = spider->wide_handler;
  THD *thd = wide_handler->trx->thd;
  DBUG_ENTER("spider_set_conn_bg_param");
  DBUG_PRINT("info",("spider spider=%p", spider));
  bgs_mode =
    spider_param_bgs_mode(thd, share->bgs_mode);
  if (bgs_mode == 0)
    result_list->bgs_phase = 0;
  else if (
    bgs_mode <= 2 &&
    (wide_handler->external_lock_type == F_WRLCK ||
      wide_handler->lock_mode == 2)
  )
    result_list->bgs_phase = 0;
  else if (bgs_mode <= 1 && wide_handler->lock_mode == 1)
    result_list->bgs_phase = 0;
  else {
    result_list->bgs_phase = 1;

    result_list->bgs_split_read = spider_bg_split_read_param(spider);
    if (spider->use_pre_call)
    {
      DBUG_PRINT("info",("spider use_pre_call=TRUE"));
      result_list->bgs_first_read = result_list->bgs_split_read;
      result_list->bgs_second_read = result_list->bgs_split_read;
    } else {
      DBUG_PRINT("info",("spider use_pre_call=FALSE"));
      result_list->bgs_first_read =
        spider_param_bgs_first_read(thd, share->bgs_first_read);
      result_list->bgs_second_read =
        spider_param_bgs_second_read(thd, share->bgs_second_read);
    }
    DBUG_PRINT("info",("spider bgs_split_read=%lld",
      result_list->bgs_split_read));
    DBUG_PRINT("info",("spider bgs_first_read=%lld", share->bgs_first_read));
    DBUG_PRINT("info",("spider bgs_second_read=%lld", share->bgs_second_read));

    result_list->split_read =
      result_list->bgs_first_read > 0 ?
      result_list->bgs_first_read :
      result_list->bgs_split_read;
  }

  if (result_list->bgs_phase > 0)
  {
    if (spider->use_fields)
    {
      SPIDER_LINK_IDX_CHAIN *link_idx_chain;
      spider_fields *fields = spider->fields;
      fields->set_pos_to_first_link_idx_chain();
      while ((link_idx_chain = fields->get_next_link_idx_chain()))
      {
        if ((error_num = spider_create_conn_thread(link_idx_chain->conn)))
          DBUG_RETURN(error_num);
      }
    } else {
      for (
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, -1, share->link_count,
          spider->wide_handler->lock_mode ?
          SPIDER_LINK_STATUS_RECOVERY : SPIDER_LINK_STATUS_OK);
        roop_count < (int) share->link_count;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, roop_count, share->link_count,
          spider->wide_handler->lock_mode ?
          SPIDER_LINK_STATUS_RECOVERY : SPIDER_LINK_STATUS_OK)
      ) {
        if ((error_num = spider_create_conn_thread(spider->conns[roop_count])))
          DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_create_conn_thread(
  SPIDER_CONN *conn
) {
  int error_num;
  DBUG_ENTER("spider_create_conn_thread");
  if (conn && !conn->bg_init)
  {
    if (mysql_mutex_init(spd_key_mutex_bg_conn_chain,
      &conn->bg_conn_chain_mutex, MY_MUTEX_INIT_FAST))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_chain_mutex_init;
    }
    conn->bg_conn_chain_mutex_ptr = NULL;
    if (mysql_mutex_init(spd_key_mutex_bg_conn_sync,
      &conn->bg_conn_sync_mutex, MY_MUTEX_INIT_FAST))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_mutex_init;
    }
    if (mysql_mutex_init(spd_key_mutex_bg_conn, &conn->bg_conn_mutex,
      MY_MUTEX_INIT_FAST))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_mutex_init;
    }
    if (mysql_mutex_init(spd_key_mutex_bg_job_stack, &conn->bg_job_stack_mutex,
      MY_MUTEX_INIT_FAST))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_job_stack_mutex_init;
    }
    if (SPD_INIT_DYNAMIC_ARRAY2(&conn->bg_job_stack, sizeof(void *), NULL, 16,
      16, MYF(MY_WME)))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_job_stack_init;
    }
    spider_alloc_calc_mem_init(conn->bg_job_stack, 163);
    spider_alloc_calc_mem(spider_current_trx,
      conn->bg_job_stack,
      conn->bg_job_stack.max_element *
      conn->bg_job_stack.size_of_element);
    conn->bg_job_stack_cur_pos = 0;
    if (mysql_cond_init(spd_key_cond_bg_conn_sync,
      &conn->bg_conn_sync_cond, NULL))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
    if (mysql_cond_init(spd_key_cond_bg_conn,
      &conn->bg_conn_cond, NULL))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
    pthread_mutex_lock(&conn->bg_conn_mutex);
    if (mysql_thread_create(spd_key_thd_bg, &conn->bg_thread,
      &spider_pt_attr, spider_bg_conn_action, (void *) conn)
    )
    {
      pthread_mutex_unlock(&conn->bg_conn_mutex);
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    if (!conn->bg_init)
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&conn->bg_conn_cond);
error_cond_init:
  pthread_cond_destroy(&conn->bg_conn_sync_cond);
error_sync_cond_init:
  spider_free_mem_calc(spider_current_trx,
    conn->bg_job_stack_id,
    conn->bg_job_stack.max_element *
    conn->bg_job_stack.size_of_element);
  delete_dynamic(&conn->bg_job_stack);
error_job_stack_init:
  pthread_mutex_destroy(&conn->bg_job_stack_mutex);
error_job_stack_mutex_init:
  pthread_mutex_destroy(&conn->bg_conn_mutex);
error_mutex_init:
  pthread_mutex_destroy(&conn->bg_conn_sync_mutex);
error_sync_mutex_init:
  pthread_mutex_destroy(&conn->bg_conn_chain_mutex);
error_chain_mutex_init:
  DBUG_RETURN(error_num);
}

void spider_free_conn_thread(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_free_conn_thread");
  if (conn->bg_init)
  {
    spider_bg_conn_break(conn, NULL);
    pthread_mutex_lock(&conn->bg_conn_mutex);
    conn->bg_kill = TRUE;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_cond_signal(&conn->bg_conn_cond);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    pthread_join(conn->bg_thread, NULL);
    pthread_cond_destroy(&conn->bg_conn_cond);
    pthread_cond_destroy(&conn->bg_conn_sync_cond);
    spider_free_mem_calc(spider_current_trx,
      conn->bg_job_stack_id,
      conn->bg_job_stack.max_element *
      conn->bg_job_stack.size_of_element);
    delete_dynamic(&conn->bg_job_stack);
    pthread_mutex_destroy(&conn->bg_job_stack_mutex);
    pthread_mutex_destroy(&conn->bg_conn_mutex);
    pthread_mutex_destroy(&conn->bg_conn_sync_mutex);
    pthread_mutex_destroy(&conn->bg_conn_chain_mutex);
    conn->bg_kill = FALSE;
    conn->bg_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void spider_bg_conn_wait(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_bg_conn_wait");
  if (conn->bg_init)
  {
    pthread_mutex_lock(&conn->bg_conn_mutex);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
  }
  DBUG_VOID_RETURN;
}

void spider_bg_all_conn_wait(
  ha_spider *spider
) {
  int roop_count;
  SPIDER_CONN *conn;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_bg_all_conn_wait");
  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    conn = spider->conns[roop_count];
    if (conn && result_list->bgs_working)
      spider_bg_conn_wait(conn);
  }
  DBUG_VOID_RETURN;
}

int spider_bg_all_conn_pre_next(
  ha_spider *spider,
  int link_idx
) {
  int roop_start, roop_end, roop_count, lock_mode, link_ok, error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_bg_all_conn_pre_next");
  if (result_list->bgs_phase > 0)
  {
    lock_mode = spider_conn_lock_mode(spider);
    if (lock_mode)
    {
      /* "for update" or "lock in share mode" */
      link_ok = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_OK);
      roop_start = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, -1, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY);
      roop_end = spider->share->link_count;
    } else {
      link_ok = link_idx;
      roop_start = link_idx;
      roop_end = link_idx + 1;
    }

    for (roop_count = roop_start; roop_count < roop_end;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, roop_count, share->link_count,
        SPIDER_LINK_STATUS_RECOVERY)
    ) {
      if ((error_num = spider_bg_conn_search(spider, roop_count, roop_start,
        TRUE, TRUE, (roop_count != link_ok))))
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

void spider_bg_conn_break(
  SPIDER_CONN *conn,
  ha_spider *spider
) {
  DBUG_ENTER("spider_bg_conn_break");
  if (
    conn->bg_init &&
    conn->bg_thd != current_thd &&
    (
      !spider ||
      (
        spider->result_list.bgs_working &&
        conn->bg_target == spider
      )
    )
  ) {
    conn->bg_break = TRUE;
    pthread_mutex_lock(&conn->bg_conn_mutex);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
    conn->bg_break = FALSE;
  }
  DBUG_VOID_RETURN;
}

void spider_bg_all_conn_break(
  ha_spider *spider
) {
  int roop_count;
  SPIDER_CONN *conn;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_bg_all_conn_break");
  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    conn = spider->conns[roop_count];
    if (conn && result_list->bgs_working)
      spider_bg_conn_break(conn, spider);
    if (spider->quick_targets[roop_count])
    {
      spider_db_free_one_quick_result((SPIDER_RESULT *) result_list->current);
      DBUG_ASSERT(spider->quick_targets[roop_count] == conn->quick_target);
      DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
      conn->quick_target = NULL;
      spider->quick_targets[roop_count] = NULL;
    }
  }
  DBUG_VOID_RETURN;
}

bool spider_bg_conn_get_job(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_bg_conn_get_job");
  pthread_mutex_lock(&conn->bg_job_stack_mutex);
  if (conn->bg_job_stack_cur_pos >= conn->bg_job_stack.elements)
  {
    DBUG_PRINT("info",("spider bg all jobs are completed"));
    conn->bg_get_job_stack_off = FALSE;
    pthread_mutex_unlock(&conn->bg_job_stack_mutex);
    DBUG_RETURN(FALSE);
  }
  DBUG_PRINT("info",("spider bg get job %u",
    conn->bg_job_stack_cur_pos));
  conn->bg_target = ((void **) (conn->bg_job_stack.buffer +
    conn->bg_job_stack.size_of_element * conn->bg_job_stack_cur_pos))[0];
  conn->bg_job_stack_cur_pos++;
  if (conn->bg_job_stack_cur_pos == conn->bg_job_stack.elements)
  {
    DBUG_PRINT("info",("spider bg shift job stack"));
    conn->bg_job_stack_cur_pos = 0;
    conn->bg_job_stack.elements = 0;
  }
  pthread_mutex_unlock(&conn->bg_job_stack_mutex);
  DBUG_RETURN(TRUE);
}

int spider_bg_conn_search(
  ha_spider *spider,
  int link_idx,
  int first_link_idx,
  bool first,
  bool pre_next,
  bool discard_result
) {
  int error_num;
  SPIDER_CONN *conn, *first_conn = NULL;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  bool with_lock = FALSE;
  DBUG_ENTER("spider_bg_conn_search");
  DBUG_PRINT("info",("spider spider=%p", spider));
    conn = spider->conns[link_idx];
    with_lock = (spider_conn_lock_mode(spider) != SPIDER_LOCK_MODE_NO_LOCK);
    first_conn = spider->conns[first_link_idx];
  if (first)
  {
    if (spider->use_pre_call)
    {
      DBUG_PRINT("info",("spider skip bg first search"));
    } else {
      DBUG_PRINT("info",("spider bg first search"));
      pthread_mutex_lock(&conn->bg_conn_mutex);
      result_list->bgs_working = TRUE;
      conn->bg_search = TRUE;
      conn->bg_caller_wait = TRUE;
      conn->bg_target = spider;
      conn->link_idx = link_idx;
      conn->bg_discard_result = discard_result;
      pthread_mutex_lock(&conn->bg_conn_sync_mutex);
      pthread_cond_signal(&conn->bg_conn_cond);
      pthread_mutex_unlock(&conn->bg_conn_mutex);
      pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
      pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      conn->bg_caller_wait = FALSE;
      if (result_list->bgs_error)
      {
        if (result_list->bgs_error_with_message)
          my_message(result_list->bgs_error,
            result_list->bgs_error_msg, MYF(0));
        DBUG_RETURN(result_list->bgs_error);
      }
    }
    if (result_list->bgs_working || !result_list->finish_flg)
    {
      pthread_mutex_lock(&conn->bg_conn_mutex);
      if (!result_list->finish_flg)
      {
        DBUG_PRINT("info",("spider bg second search"));
        if (!spider->use_pre_call || pre_next)
        {
          if (result_list->bgs_error)
          {
            pthread_mutex_unlock(&conn->bg_conn_mutex);
            DBUG_PRINT("info",("spider bg error"));
            if (result_list->bgs_error == HA_ERR_END_OF_FILE)
            {
              DBUG_PRINT("info",("spider bg current->finish_flg=%s",
                result_list->current ?
                (result_list->current->finish_flg ? "TRUE" : "FALSE") : "NULL"));
              DBUG_RETURN(0);
            }
            if (result_list->bgs_error_with_message)
              my_message(result_list->bgs_error,
                result_list->bgs_error_msg, MYF(0));
            DBUG_RETURN(result_list->bgs_error);
          }
          DBUG_PRINT("info",("spider result_list->quick_mode=%d",
            result_list->quick_mode));
          DBUG_PRINT("info",("spider result_list->bgs_current->result=%p",
            result_list->bgs_current->result));
          if (
            result_list->quick_mode == 0 ||
            !result_list->bgs_current->result
          ) {
            DBUG_PRINT("info",("spider result_list->bgs_second_read=%lld",
              result_list->bgs_second_read));
            DBUG_PRINT("info",("spider result_list->bgs_split_read=%lld",
              result_list->bgs_split_read));
            result_list->split_read =
              result_list->bgs_second_read > 0 ?
              result_list->bgs_second_read :
              result_list->bgs_split_read;
            result_list->limit_num =
              result_list->internal_limit - result_list->record_num >=
              result_list->split_read ?
              result_list->split_read :
              result_list->internal_limit - result_list->record_num;
            DBUG_PRINT("info",("spider sql_kinds=%u", spider->sql_kinds));
            if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
            {
              if ((error_num = spider->reappend_limit_sql_part(
                result_list->internal_offset + result_list->record_num,
                result_list->limit_num,
                SPIDER_SQL_TYPE_SELECT_SQL)))
              {
                pthread_mutex_unlock(&conn->bg_conn_mutex);
                DBUG_RETURN(error_num);
              }
              if (
                !result_list->use_union &&
                (error_num = spider->append_select_lock_sql_part(
                  SPIDER_SQL_TYPE_SELECT_SQL))
              ) {
                pthread_mutex_unlock(&conn->bg_conn_mutex);
                DBUG_RETURN(error_num);
              }
            }
            if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
            {
              spider_db_append_handler_next(spider);
              if ((error_num = spider->reappend_limit_sql_part(
                0, result_list->limit_num,
                SPIDER_SQL_TYPE_HANDLER)))
              {
                pthread_mutex_unlock(&conn->bg_conn_mutex);
                DBUG_RETURN(error_num);
              }
            }
          }
          result_list->bgs_phase = 2;
          if (conn->db_conn->limit_mode() == 1)
          {
            conn->db_conn->set_limit(result_list->limit_num);
            if (!discard_result)
            {
              if ((error_num = spider_db_store_result_for_reuse_cursor(
                spider, link_idx, result_list->table)))
              {
                pthread_mutex_unlock(&conn->bg_conn_mutex);
                DBUG_RETURN(error_num);
              }
            }
            pthread_mutex_unlock(&conn->bg_conn_mutex);
            DBUG_RETURN(0);
          }
        }
        result_list->bgs_working = TRUE;
        conn->bg_search = TRUE;
        if (with_lock)
          conn->bg_conn_chain_mutex_ptr = &first_conn->bg_conn_chain_mutex;
        conn->bg_caller_sync_wait = TRUE;
        conn->bg_target = spider;
        conn->link_idx = link_idx;
        conn->bg_discard_result = discard_result;
        conn->link_idx_chain = spider->link_idx_chain;
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_cond);
        pthread_mutex_unlock(&conn->bg_conn_mutex);
        pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
        conn->bg_caller_sync_wait = FALSE;
      } else {
        pthread_mutex_unlock(&conn->bg_conn_mutex);
        DBUG_PRINT("info",("spider bg current->finish_flg=%s",
          result_list->current ?
          (result_list->current->finish_flg ? "TRUE" : "FALSE") : "NULL"));
        if (result_list->bgs_error)
        {
          DBUG_PRINT("info",("spider bg error"));
          if (result_list->bgs_error != HA_ERR_END_OF_FILE)
          {
            if (result_list->bgs_error_with_message)
              my_message(result_list->bgs_error,
                result_list->bgs_error_msg, MYF(0));
            DBUG_RETURN(result_list->bgs_error);
          }
        }
      }
    } else {
      DBUG_PRINT("info",("spider bg current->finish_flg=%s",
        result_list->current ?
        (result_list->current->finish_flg ? "TRUE" : "FALSE") : "NULL"));
      if (result_list->bgs_error)
      {
        DBUG_PRINT("info",("spider bg error"));
        if (result_list->bgs_error != HA_ERR_END_OF_FILE)
        {
          if (result_list->bgs_error_with_message)
            my_message(result_list->bgs_error,
              result_list->bgs_error_msg, MYF(0));
          DBUG_RETURN(result_list->bgs_error);
        }
      }
    }
  } else {
    DBUG_PRINT("info",("spider bg search"));
    if (result_list->current->finish_flg)
    {
      DBUG_PRINT("info",("spider bg end of file"));
      result_list->table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    if (result_list->bgs_working)
    {
      /* wait */
      DBUG_PRINT("info",("spider bg working wait"));
      pthread_mutex_lock(&conn->bg_conn_mutex);
      pthread_mutex_unlock(&conn->bg_conn_mutex);
    }
    if (result_list->bgs_error)
    {
      DBUG_PRINT("info",("spider bg error"));
      if (result_list->bgs_error == HA_ERR_END_OF_FILE)
      {
        result_list->current = result_list->current->next;
        result_list->current_row_num = 0;
        result_list->table->status = STATUS_NOT_FOUND;
      }
      if (result_list->bgs_error_with_message)
        my_message(result_list->bgs_error,
          result_list->bgs_error_msg, MYF(0));
      DBUG_RETURN(result_list->bgs_error);
    }
    result_list->current = result_list->current->next;
    result_list->current_row_num = 0;
    if (result_list->current == result_list->bgs_current)
    {
      DBUG_PRINT("info",("spider bg next search"));
      if (!result_list->current->finish_flg)
      {
        DBUG_PRINT("info",("spider result_list->quick_mode=%d",
          result_list->quick_mode));
        DBUG_PRINT("info",("spider result_list->bgs_current->result=%p",
          result_list->bgs_current->result));
        pthread_mutex_lock(&conn->bg_conn_mutex);
        result_list->bgs_phase = 3;
        if (
          result_list->quick_mode == 0 ||
          !result_list->bgs_current->result
        ) {
          result_list->split_read = result_list->bgs_split_read;
          result_list->limit_num =
            result_list->internal_limit - result_list->record_num >=
            result_list->split_read ?
            result_list->split_read :
            result_list->internal_limit - result_list->record_num;
          DBUG_PRINT("info",("spider sql_kinds=%u", spider->sql_kinds));
          if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
          {
            if ((error_num = spider->reappend_limit_sql_part(
              result_list->internal_offset + result_list->record_num,
              result_list->limit_num,
              SPIDER_SQL_TYPE_SELECT_SQL)))
            {
              pthread_mutex_unlock(&conn->bg_conn_mutex);
              DBUG_RETURN(error_num);
            }
            if (
              !result_list->use_union &&
              (error_num = spider->append_select_lock_sql_part(
                SPIDER_SQL_TYPE_SELECT_SQL))
            ) {
              pthread_mutex_unlock(&conn->bg_conn_mutex);
              DBUG_RETURN(error_num);
            }
          }
          if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
          {
            spider_db_append_handler_next(spider);
            if ((error_num = spider->reappend_limit_sql_part(
              0, result_list->limit_num,
              SPIDER_SQL_TYPE_HANDLER)))
            {
              pthread_mutex_unlock(&conn->bg_conn_mutex);
              DBUG_RETURN(error_num);
            }
          }
          if (conn->db_conn->limit_mode() == 1)
          {
            conn->db_conn->set_limit(result_list->limit_num);
            if (!discard_result)
            {
              if ((error_num = spider_db_store_result_for_reuse_cursor(
                spider, link_idx, result_list->table)))
              {
                pthread_mutex_unlock(&conn->bg_conn_mutex);
                DBUG_RETURN(error_num);
              }
            }
            pthread_mutex_unlock(&conn->bg_conn_mutex);
            DBUG_RETURN(0);
          }
        }
        conn->bg_target = spider;
        conn->link_idx = link_idx;
        conn->bg_discard_result = discard_result;
        conn->link_idx_chain = spider->link_idx_chain;
        result_list->bgs_working = TRUE;
        conn->bg_search = TRUE;
        if (with_lock)
          conn->bg_conn_chain_mutex_ptr = &first_conn->bg_conn_chain_mutex;
        conn->bg_caller_sync_wait = TRUE;
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_cond);
        pthread_mutex_unlock(&conn->bg_conn_mutex);
        pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
        conn->bg_caller_sync_wait = FALSE;
      }
    }
  }
  DBUG_RETURN(0);
}

void spider_bg_conn_simple_action(
  SPIDER_CONN *conn,
  uint simple_action,
  bool caller_wait,
  void *target,
  uint link_idx,
  int *error_num
) {
  DBUG_ENTER("spider_bg_conn_simple_action");
  pthread_mutex_lock(&conn->bg_conn_mutex);
  conn->bg_target = target;
  conn->link_idx = link_idx;
  conn->bg_simple_action = simple_action;
  conn->bg_error_num = error_num;
  if (caller_wait)
  {
    conn->bg_caller_wait = TRUE;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
  } else {
    conn->bg_caller_sync_wait = TRUE;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
  }
  pthread_cond_signal(&conn->bg_conn_cond);
  pthread_mutex_unlock(&conn->bg_conn_mutex);
  if (caller_wait)
  {
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    conn->bg_caller_wait = FALSE;
  } else {
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    conn->bg_caller_sync_wait = FALSE;
  }
  DBUG_VOID_RETURN;
}

void *spider_bg_conn_action(
  void *arg
) {
  int error_num;
  SPIDER_CONN *conn = (SPIDER_CONN*) arg;
  SPIDER_TRX *trx;
  ha_spider *spider;
  SPIDER_RESULT_LIST *result_list;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_conn_action");
  /* init start */
  if (!(thd = SPIDER_new_THD(next_thread_id())))
  {
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_cond_signal(&conn->bg_conn_sync_cond);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char*) &thd;
  thd->store_globals();
  if (!(trx = spider_get_trx(thd, FALSE, &error_num)))
  {
    delete thd;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_cond_signal(&conn->bg_conn_sync_cond);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
    set_current_thd(nullptr);
#endif
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  /* lex_start(thd); */
  conn->bg_thd = thd;
  pthread_mutex_lock(&conn->bg_conn_mutex);
  pthread_mutex_lock(&conn->bg_conn_sync_mutex);
  pthread_cond_signal(&conn->bg_conn_sync_cond);
  conn->bg_init = TRUE;
  pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
  /* init end */

  while (TRUE)
  {
    if (conn->bg_conn_chain_mutex_ptr)
    {
      pthread_mutex_unlock(conn->bg_conn_chain_mutex_ptr);
      conn->bg_conn_chain_mutex_ptr = NULL;
    }
    thd->clear_error();
    pthread_cond_wait(&conn->bg_conn_cond, &conn->bg_conn_mutex);
    DBUG_PRINT("info",("spider bg roop start"));
#ifndef DBUG_OFF
    DBUG_PRINT("info",("spider conn->thd=%p", conn->thd));
    if (conn->thd)
    {
      DBUG_PRINT("info",("spider query_id=%lld", conn->thd->query_id));
    }
#endif
    if (conn->bg_caller_sync_wait)
    {
      pthread_mutex_lock(&conn->bg_conn_sync_mutex);
      if (conn->bg_direct_sql)
        conn->bg_get_job_stack_off = TRUE;
      pthread_cond_signal(&conn->bg_conn_sync_cond);
      pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      if (conn->bg_conn_chain_mutex_ptr)
      {
        pthread_mutex_lock(conn->bg_conn_chain_mutex_ptr);
        if ((&conn->bg_conn_chain_mutex) != conn->bg_conn_chain_mutex_ptr)
        {
          pthread_mutex_unlock(conn->bg_conn_chain_mutex_ptr);
          conn->bg_conn_chain_mutex_ptr = NULL;
        }
      }
    }
    if (conn->bg_kill)
    {
      DBUG_PRINT("info",("spider bg kill start"));
      if (conn->bg_conn_chain_mutex_ptr)
      {
        pthread_mutex_unlock(conn->bg_conn_chain_mutex_ptr);
        conn->bg_conn_chain_mutex_ptr = NULL;
      }
      spider_free_trx(trx, TRUE);
      /* lex_end(thd->lex); */
      delete thd;
      pthread_mutex_lock(&conn->bg_conn_sync_mutex);
      pthread_cond_signal(&conn->bg_conn_sync_cond);
      pthread_mutex_unlock(&conn->bg_conn_mutex);
      pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
      set_current_thd(nullptr);
#endif
      my_thread_end();
      DBUG_RETURN(NULL);
    }
    if (conn->bg_get_job_stack)
    {
      conn->bg_get_job_stack = FALSE;
      if (!spider_bg_conn_get_job(conn))
      {
        conn->bg_direct_sql = FALSE;
      }
    }
    if (conn->bg_search)
    {
      SPIDER_SHARE *share;
      spider_db_handler *dbton_handler;
      DBUG_PRINT("info",("spider bg search start"));
      spider = (ha_spider*) conn->bg_target;
      share = spider->share;
      dbton_handler = spider->dbton_handler[conn->dbton_id];
      result_list = &spider->result_list;
      result_list->bgs_error = 0;
      result_list->bgs_error_with_message = FALSE;
      if (
        result_list->quick_mode == 0 ||
        result_list->bgs_phase == 1 ||
        !result_list->bgs_current->result
      ) {
        ulong sql_type;
          if (spider->sql_kind[conn->link_idx] == SPIDER_SQL_KIND_SQL)
          {
            sql_type = SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_TMP_SQL;
          } else {
            sql_type = SPIDER_SQL_TYPE_HANDLER;
          }
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if (spider->use_fields)
        {
          if ((error_num = dbton_handler->set_sql_for_exec(sql_type,
            conn->link_idx, conn->link_idx_chain)))
          {
            result_list->bgs_error = error_num;
            if ((result_list->bgs_error_with_message = thd->is_error()))
              strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
          }
        } else {
          if ((error_num = dbton_handler->set_sql_for_exec(sql_type,
            conn->link_idx)))
          {
            result_list->bgs_error = error_num;
            if ((result_list->bgs_error_with_message = thd->is_error()))
              strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
          }
        }
        if (!dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        sql_type &= ~SPIDER_SQL_TYPE_TMP_SQL;
        DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
        if (!result_list->bgs_error)
        {
          conn->need_mon = &spider->need_mons[conn->link_idx];
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = TRUE;
          conn->mta_conn_mutex_unlock_later = TRUE;
            if (!(result_list->bgs_error =
              spider_db_set_names(spider, conn, conn->link_idx)))
            {
              if (
                result_list->tmp_table_join && spider->bka_mode != 2 &&
                spider_bit_is_set(result_list->tmp_table_join_first,
                  conn->link_idx)
              ) {
                spider_clear_bit(result_list->tmp_table_join_first,
                  conn->link_idx);
                spider_set_bit(result_list->tmp_table_created,
                  conn->link_idx);
                result_list->tmp_tables_created = TRUE;
                spider_conn_set_timeout_from_share(conn, conn->link_idx,
                  spider->wide_handler->trx->thd, share);
                if (dbton_handler->execute_sql(
                  SPIDER_SQL_TYPE_TMP_SQL,
                  conn,
                  -1,
                  &spider->need_mons[conn->link_idx])
                ) {
                  result_list->bgs_error = spider_db_errorno(conn);
                  if ((result_list->bgs_error_with_message = thd->is_error()))
                    strmov(result_list->bgs_error_msg,
                      spider_stmt_da_message(thd));
                } else
                  spider_db_discard_multiple_result(spider, conn->link_idx,
                    conn);
              }
              if (!result_list->bgs_error)
              {
                spider_conn_set_timeout_from_share(conn, conn->link_idx,
                  spider->wide_handler->trx->thd, share);
                if (dbton_handler->execute_sql(
                  sql_type,
                  conn,
                  result_list->quick_mode,
                  &spider->need_mons[conn->link_idx])
                ) {
                  result_list->bgs_error = spider_db_errorno(conn);
                  if ((result_list->bgs_error_with_message = thd->is_error()))
                    strmov(result_list->bgs_error_msg,
                      spider_stmt_da_message(thd));
                } else {
                  spider->connection_ids[conn->link_idx] = conn->connection_id;
                  if (!conn->bg_discard_result)
                  {
                    if (!(result_list->bgs_error =
                      spider_db_store_result(spider, conn->link_idx,
                        result_list->table)))
                      spider->result_link_idx = conn->link_idx;
                    else {
                      if ((result_list->bgs_error_with_message =
                        thd->is_error()))
                        strmov(result_list->bgs_error_msg,
                          spider_stmt_da_message(thd));
                    }
                  } else {
                    result_list->bgs_error = 0;
                    spider_db_discard_result(spider, conn->link_idx, conn);
                  }
                }
              }
            } else {
              if ((result_list->bgs_error_with_message = thd->is_error()))
                strmov(result_list->bgs_error_msg,
                  spider_stmt_da_message(thd));
            }
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        } else {
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
      } else {
        spider->connection_ids[conn->link_idx] = conn->connection_id;
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_unlock_later = TRUE;
        result_list->bgs_error =
          spider_db_store_result(spider, conn->link_idx, result_list->table);
        if ((result_list->bgs_error_with_message = thd->is_error()))
          strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_unlock_later = FALSE;
      }
      conn->bg_search = FALSE;
      result_list->bgs_working = FALSE;
      if (conn->bg_caller_wait)
      {
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_sync_cond);
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      }
      continue;
    }
    if (conn->bg_direct_sql)
    {
      bool is_error = FALSE;
      DBUG_PRINT("info",("spider bg direct sql start"));
      do {
        SPIDER_DIRECT_SQL *direct_sql = (SPIDER_DIRECT_SQL *) conn->bg_target;
        if (
          (error_num = spider_db_udf_direct_sql(direct_sql))
        ) {
          if (thd->is_error())
          {
            if (
              direct_sql->error_rw_mode &&
              spider_db_conn_is_network_error(error_num)
            ) {
              thd->clear_error();
            } else {
              SPIDER_BG_DIRECT_SQL *bg_direct_sql =
                (SPIDER_BG_DIRECT_SQL *) direct_sql->parent;
              pthread_mutex_lock(direct_sql->bg_mutex);
              bg_direct_sql->bg_error = spider_stmt_da_sql_errno(thd);
              strmov((char *) bg_direct_sql->bg_error_msg,
                spider_stmt_da_message(thd));
              pthread_mutex_unlock(direct_sql->bg_mutex);
              is_error = TRUE;
            }
          }
        }
        if (direct_sql->modified_non_trans_table)
        {
          SPIDER_BG_DIRECT_SQL *bg_direct_sql =
            (SPIDER_BG_DIRECT_SQL *) direct_sql->parent;
          pthread_mutex_lock(direct_sql->bg_mutex);
          bg_direct_sql->modified_non_trans_table = TRUE;
          pthread_mutex_unlock(direct_sql->bg_mutex);
        }
        spider_udf_free_direct_sql_alloc(direct_sql, TRUE);
      } while (!is_error && spider_bg_conn_get_job(conn));
      if (is_error)
      {
        while (spider_bg_conn_get_job(conn))
          spider_udf_free_direct_sql_alloc(
            (SPIDER_DIRECT_SQL *) conn->bg_target, TRUE);
      }
      conn->bg_direct_sql = FALSE;
      continue;
    }
    if (conn->bg_exec_sql)
    {
      DBUG_PRINT("info",("spider bg exec sql start"));
      spider = (ha_spider*) conn->bg_target;
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      conn->need_mon = &spider->need_mons[conn->link_idx];
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      *conn->bg_error_num = spider_db_query_with_set_names(
        conn->bg_sql_type,
        spider,
        conn,
        conn->link_idx
      );
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      conn->bg_exec_sql = FALSE;
      continue;
    }
    if (conn->bg_simple_action)
    {
      switch (conn->bg_simple_action)
      {
        case SPIDER_SIMPLE_CONNECT:
          conn->db_conn->bg_connect();
          break;
        case SPIDER_SIMPLE_DISCONNECT:
          conn->db_conn->bg_disconnect();
          break;
        default:
          spider = (ha_spider*) conn->bg_target;
          *conn->bg_error_num =
            spider_db_simple_action(conn->bg_simple_action,
              spider->dbton_handler[conn->dbton_id], conn->link_idx);
          break;
      }
      conn->bg_simple_action = SPIDER_SIMPLE_NO_ACTION;
      if (conn->bg_caller_wait)
      {
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_sync_cond);
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      }
      continue;
    }
    if (conn->bg_break)
    {
      DBUG_PRINT("info",("spider bg break start"));
      spider = (ha_spider*) conn->bg_target;
      result_list = &spider->result_list;
      result_list->bgs_working = FALSE;
      continue;
    }
  }
}

int spider_create_sts_thread(
  SPIDER_SHARE *share
) {
  int error_num;
  DBUG_ENTER("spider_create_sts_thread");
  if (!share->bg_sts_init)
  {
    if (mysql_cond_init(spd_key_cond_bg_sts,
      &share->bg_sts_cond, NULL))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
    if (mysql_cond_init(spd_key_cond_bg_sts_sync,
      &share->bg_sts_sync_cond, NULL))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
    if (mysql_thread_create(spd_key_thd_bg_sts, &share->bg_sts_thread,
      &spider_pt_attr, spider_bg_sts_action, (void *) share)
    )
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
    share->bg_sts_init = TRUE;
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&share->bg_sts_sync_cond);
error_sync_cond_init:
  pthread_cond_destroy(&share->bg_sts_cond);
error_cond_init:
  DBUG_RETURN(error_num);
}

void spider_free_sts_thread(
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_free_sts_thread");
  if (share->bg_sts_init)
  {
    pthread_mutex_lock(&share->sts_mutex);
    share->bg_sts_kill = TRUE;
    pthread_cond_signal(&share->bg_sts_cond);
    pthread_cond_wait(&share->bg_sts_sync_cond, &share->sts_mutex);
    pthread_mutex_unlock(&share->sts_mutex);
    pthread_join(share->bg_sts_thread, NULL);
    pthread_cond_destroy(&share->bg_sts_sync_cond);
    pthread_cond_destroy(&share->bg_sts_cond);
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void *spider_bg_sts_action(
  void *arg
) {
  SPIDER_SHARE *share = (SPIDER_SHARE*) arg;
  SPIDER_TRX *trx;
  int error_num = 0, roop_count;
  ha_spider spider;
  SPIDER_WIDE_HANDLER wide_handler;
  int *need_mons;
  SPIDER_CONN **conns;
  uint *conn_link_idx;
  uchar *conn_can_fo;
  char **conn_keys;
  spider_db_handler **dbton_hdl;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_sts_action");
  /* init start */
  char *ptr;
  ptr = (char *) my_alloca(
    (sizeof(int) * share->link_count) +
    (sizeof(SPIDER_CONN *) * share->link_count) +
    (sizeof(uint) * share->link_count) +
    (sizeof(uchar) * share->link_bitmap_size) +
    (sizeof(char *) * share->link_count) +
    (sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE));
  if (!ptr)
  {
    pthread_mutex_lock(&share->sts_mutex);
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  need_mons = (int *) ptr;
  ptr += (sizeof(int) * share->link_count);
  conns = (SPIDER_CONN **) ptr;
  ptr += (sizeof(SPIDER_CONN *) * share->link_count);
  conn_link_idx = (uint *) ptr;
  ptr += (sizeof(uint) * share->link_count);
  conn_can_fo = (uchar *) ptr;
  ptr += (sizeof(uchar) * share->link_bitmap_size);
  conn_keys = (char **) ptr;
  ptr += (sizeof(char *) * share->link_count);
  dbton_hdl = (spider_db_handler **) ptr;
  pthread_mutex_lock(&share->sts_mutex);
  if (!(thd = SPIDER_new_THD(next_thread_id())))
  {
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char*) &thd;
  thd->store_globals();
  if (!(trx = spider_get_trx(thd, FALSE, &error_num)))
  {
    delete thd;
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
    set_current_thd(nullptr);
#endif
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  share->bg_sts_thd = thd;
  spider.wide_handler = &wide_handler;
  wide_handler.trx = trx;
  spider.share = share;
  spider.conns = conns;
  spider.conn_link_idx = conn_link_idx;
  spider.conn_can_fo = conn_can_fo;
  spider.need_mons = need_mons;
  spider.conn_keys_first_ptr = share->conn_keys[0];
  spider.conn_keys = conn_keys;
  spider.dbton_handler = dbton_hdl;
  memset(conns, 0, sizeof(SPIDER_CONN *) * share->link_count);
  memset(need_mons, 0, sizeof(int) * share->link_count);
  memset(dbton_hdl, 0, sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE);
  spider_trx_set_link_idx_for_all(&spider);
  spider.search_link_idx = spider_conn_first_link_idx(thd,
    share->link_statuses, share->access_balances, spider.conn_link_idx,
    share->link_count, SPIDER_LINK_STATUS_OK);
  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++)
  {
    if (
      spider_bit_is_set(share->dbton_bitmap, roop_count) &&
      spider_dbton[roop_count].create_db_handler
    ) {
      if (!(dbton_hdl[roop_count] = spider_dbton[roop_count].create_db_handler(
        &spider, share->dbton_share[roop_count])))
        break;
      if (dbton_hdl[roop_count]->init())
        break;
    }
  }
  if (roop_count < SPIDER_DBTON_SIZE)
  {
    DBUG_PRINT("info",("spider handler init error"));
    for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count)
    {
      if (
        spider_bit_is_set(share->dbton_bitmap, roop_count) &&
        dbton_hdl[roop_count]
      ) {
        delete dbton_hdl[roop_count];
        dbton_hdl[roop_count] = NULL;
      }
    }
    spider_free_trx(trx, TRUE);
    delete thd;
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
    set_current_thd(nullptr);
#endif
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  /* init end */

  while (TRUE)
  {
    DBUG_PRINT("info",("spider bg sts roop start"));
    if (share->bg_sts_kill)
    {
      DBUG_PRINT("info",("spider bg sts kill start"));
      for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count)
      {
        if (
          spider_bit_is_set(share->dbton_bitmap, roop_count) &&
          dbton_hdl[roop_count]
        ) {
          delete dbton_hdl[roop_count];
          dbton_hdl[roop_count] = NULL;
        }
      }
      spider_free_trx(trx, TRUE);
      delete thd;
      pthread_cond_signal(&share->bg_sts_sync_cond);
      pthread_mutex_unlock(&share->sts_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
      set_current_thd(nullptr);
#endif
      my_thread_end();
      my_afree(need_mons);
      DBUG_RETURN(NULL);
    }
    if (spider.search_link_idx < 0)
    {
      spider_trx_set_link_idx_for_all(&spider);
/*
      spider.search_link_idx = spider_conn_next_link_idx(
        thd, share->link_statuses, share->access_balances,
        spider.conn_link_idx, spider.search_link_idx, share->link_count,
        SPIDER_LINK_STATUS_OK);
*/
      spider.search_link_idx = spider_conn_first_link_idx(thd,
        share->link_statuses, share->access_balances, spider.conn_link_idx,
        share->link_count, SPIDER_LINK_STATUS_OK);
    }
    if (spider.search_link_idx >= 0)
    {
      if (difftime(share->bg_sts_try_time, share->sts_get_time) >=
        share->bg_sts_interval)
      {
        if (!conns[spider.search_link_idx])
        {
          spider_get_conn(share, spider.search_link_idx,
            share->conn_keys[spider.search_link_idx],
            trx, &spider, FALSE, FALSE, SPIDER_CONN_KIND_MYSQL,
            &error_num);
          conns[spider.search_link_idx]->error_mode = 0;
/*
          if (
            error_num &&
            share->monitoring_kind[spider.search_link_idx] &&
            need_mons[spider.search_link_idx]
          ) {
            lex_start(thd);
            error_num = spider_ping_table_mon_from_table(
                trx,
                thd,
                share,
                spider.search_link_idx,
                (uint32) share->monitoring_sid[spider.search_link_idx],
                share->table_name,
                share->table_name_length,
                spider.conn_link_idx[spider.search_link_idx],
                NULL,
                0,
                share->monitoring_kind[spider.search_link_idx],
                share->monitoring_limit[spider.search_link_idx],
                share->monitoring_flag[spider.search_link_idx],
                TRUE
              );
            lex_end(thd->lex);
          }
*/
          spider.search_link_idx = -1;
        }
        if (spider.search_link_idx != -1 && conns[spider.search_link_idx])
        {
          if (spider_get_sts(share, spider.search_link_idx,
            share->bg_sts_try_time, &spider,
            share->bg_sts_interval, share->bg_sts_mode,
            share->bg_sts_sync,
            2, HA_STATUS_CONST | HA_STATUS_VARIABLE))
          {
/*
            if (
              share->monitoring_kind[spider.search_link_idx] &&
              need_mons[spider.search_link_idx]
            ) {
              lex_start(thd);
              error_num = spider_ping_table_mon_from_table(
                  trx,
                  thd,
                  share,
                  spider.search_link_idx,
                  (uint32) share->monitoring_sid[spider.search_link_idx],
                  share->table_name,
                  share->table_name_length,
                  spider.conn_link_idx[spider.search_link_idx],
                  NULL,
                  0,
                  share->monitoring_kind[spider.search_link_idx],
                  share->monitoring_limit[spider.search_link_idx],
                  share->monitoring_flag[spider.search_link_idx],
                  TRUE
                );
              lex_end(thd->lex);
            }
*/
            spider.search_link_idx = -1;
          }
        }
      }
    }
    memset(need_mons, 0, sizeof(int) * share->link_count);
    share->bg_sts_thd_wait = TRUE;
    pthread_cond_wait(&share->bg_sts_cond, &share->sts_mutex);
  }
}

int spider_create_crd_thread(
  SPIDER_SHARE *share
) {
  int error_num;
  DBUG_ENTER("spider_create_crd_thread");
  if (!share->bg_crd_init)
  {
    if (mysql_cond_init(spd_key_cond_bg_crd,
      &share->bg_crd_cond, NULL))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
    if (mysql_cond_init(spd_key_cond_bg_crd_sync,
      &share->bg_crd_sync_cond, NULL))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
    if (mysql_thread_create(spd_key_thd_bg_crd, &share->bg_crd_thread,
      &spider_pt_attr, spider_bg_crd_action, (void *) share)
    )
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
    share->bg_crd_init = TRUE;
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&share->bg_crd_sync_cond);
error_sync_cond_init:
  pthread_cond_destroy(&share->bg_crd_cond);
error_cond_init:
  DBUG_RETURN(error_num);
}

void spider_free_crd_thread(
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_free_crd_thread");
  if (share->bg_crd_init)
  {
    pthread_mutex_lock(&share->crd_mutex);
    share->bg_crd_kill = TRUE;
    pthread_cond_signal(&share->bg_crd_cond);
    pthread_cond_wait(&share->bg_crd_sync_cond, &share->crd_mutex);
    pthread_mutex_unlock(&share->crd_mutex);
    pthread_join(share->bg_crd_thread, NULL);
    pthread_cond_destroy(&share->bg_crd_sync_cond);
    pthread_cond_destroy(&share->bg_crd_cond);
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void *spider_bg_crd_action(
  void *arg
) {
  SPIDER_SHARE *share = (SPIDER_SHARE*) arg;
  SPIDER_TRX *trx;
  int error_num = 0, roop_count;
  ha_spider spider;
  SPIDER_WIDE_HANDLER wide_handler;
  TABLE table;
  int *need_mons;
  SPIDER_CONN **conns;
  uint *conn_link_idx;
  uchar *conn_can_fo;
  char **conn_keys;
  spider_db_handler **dbton_hdl;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_crd_action");
  /* init start */
  char *ptr;
  ptr = (char *) my_alloca(
    (sizeof(int) * share->link_count) +
    (sizeof(SPIDER_CONN *) * share->link_count) +
    (sizeof(uint) * share->link_count) +
    (sizeof(uchar) * share->link_bitmap_size) +
    (sizeof(char *) * share->link_count) +
    (sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE));
  if (!ptr)
  {
    pthread_mutex_lock(&share->crd_mutex);
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  need_mons = (int *) ptr;
  ptr += (sizeof(int) * share->link_count);
  conns = (SPIDER_CONN **) ptr;
  ptr += (sizeof(SPIDER_CONN *) * share->link_count);
  conn_link_idx = (uint *) ptr;
  ptr += (sizeof(uint) * share->link_count);
  conn_can_fo = (uchar *) ptr;
  ptr += (sizeof(uchar) * share->link_bitmap_size);
  conn_keys = (char **) ptr;
  ptr += (sizeof(char *) * share->link_count);
  dbton_hdl = (spider_db_handler **) ptr;
  pthread_mutex_lock(&share->crd_mutex);
  if (!(thd = SPIDER_new_THD(next_thread_id())))
  {
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char*) &thd;
  thd->store_globals();
  if (!(trx = spider_get_trx(thd, FALSE, &error_num)))
  {
    delete thd;
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
    set_current_thd(nullptr);
#endif
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  share->bg_crd_thd = thd;
  table.s = share->table_share;
  table.field = share->table_share->field;
  table.key_info = share->table_share->key_info;
  spider.wide_handler = &wide_handler;
  wide_handler.trx = trx;
  spider.change_table_ptr(&table, share->table_share);
  spider.share = share;
  spider.conns = conns;
  spider.conn_link_idx = conn_link_idx;
  spider.conn_can_fo = conn_can_fo;
  spider.need_mons = need_mons;
  spider.conn_keys_first_ptr = share->conn_keys[0];
  spider.conn_keys = conn_keys;
  spider.dbton_handler = dbton_hdl;
  memset(conns, 0, sizeof(SPIDER_CONN *) * share->link_count);
  memset(need_mons, 0, sizeof(int) * share->link_count);
  memset(dbton_hdl, 0, sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE);
  spider_trx_set_link_idx_for_all(&spider);
  spider.search_link_idx = spider_conn_first_link_idx(thd,
    share->link_statuses, share->access_balances, spider.conn_link_idx,
    share->link_count, SPIDER_LINK_STATUS_OK);
  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++)
  {
    if (
      spider_bit_is_set(share->dbton_bitmap, roop_count) &&
      spider_dbton[roop_count].create_db_handler
    ) {
      if (!(dbton_hdl[roop_count] = spider_dbton[roop_count].create_db_handler(
        &spider, share->dbton_share[roop_count])))
        break;
      if (dbton_hdl[roop_count]->init())
        break;
    }
  }
  if (roop_count < SPIDER_DBTON_SIZE)
  {
    DBUG_PRINT("info",("spider handler init error"));
    for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count)
    {
      if (
        spider_bit_is_set(share->dbton_bitmap, roop_count) &&
        dbton_hdl[roop_count]
      ) {
        delete dbton_hdl[roop_count];
        dbton_hdl[roop_count] = NULL;
      }
    }
    spider_free_trx(trx, TRUE);
    delete thd;
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
    set_current_thd(nullptr);
#endif
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  /* init end */

  while (TRUE)
  {
    DBUG_PRINT("info",("spider bg crd roop start"));
    if (share->bg_crd_kill)
    {
      DBUG_PRINT("info",("spider bg crd kill start"));
      for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count)
      {
        if (
          spider_bit_is_set(share->dbton_bitmap, roop_count) &&
          dbton_hdl[roop_count]
        ) {
          delete dbton_hdl[roop_count];
          dbton_hdl[roop_count] = NULL;
        }
      }
      spider_free_trx(trx, TRUE);
      delete thd;
      pthread_cond_signal(&share->bg_crd_sync_cond);
      pthread_mutex_unlock(&share->crd_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
      set_current_thd(nullptr);
#endif
      my_thread_end();
      my_afree(need_mons);
      DBUG_RETURN(NULL);
    }
    if (spider.search_link_idx < 0)
    {
      spider_trx_set_link_idx_for_all(&spider);
/*
      spider.search_link_idx = spider_conn_next_link_idx(
        thd, share->link_statuses, share->access_balances,
        spider.conn_link_idx, spider.search_link_idx, share->link_count,
        SPIDER_LINK_STATUS_OK);
*/
      spider.search_link_idx = spider_conn_first_link_idx(thd,
        share->link_statuses, share->access_balances, spider.conn_link_idx,
        share->link_count, SPIDER_LINK_STATUS_OK);
    }
    if (spider.search_link_idx >= 0)
    {
      if (difftime(share->bg_crd_try_time, share->crd_get_time) >=
        share->bg_crd_interval)
      {
        if (!conns[spider.search_link_idx])
        {
          spider_get_conn(share, spider.search_link_idx,
            share->conn_keys[spider.search_link_idx],
            trx, &spider, FALSE, FALSE, SPIDER_CONN_KIND_MYSQL,
            &error_num);
          conns[spider.search_link_idx]->error_mode = 0;
/*
          if (
            error_num &&
            share->monitoring_kind[spider.search_link_idx] &&
            need_mons[spider.search_link_idx]
          ) {
            lex_start(thd);
            error_num = spider_ping_table_mon_from_table(
                trx,
                thd,
                share,
                spider.search_link_idx,
                (uint32) share->monitoring_sid[spider.search_link_idx],
                share->table_name,
                share->table_name_length,
                spider.conn_link_idx[spider.search_link_idx],
                NULL,
                0,
                share->monitoring_kind[spider.search_link_idx],
                share->monitoring_limit[spider.search_link_idx],
                share->monitoring_flag[spider.search_link_idx],
                TRUE
              );
            lex_end(thd->lex);
          }
*/
          spider.search_link_idx = -1;
        }
        if (spider.search_link_idx != -1 && conns[spider.search_link_idx])
        {
          if (spider_get_crd(share, spider.search_link_idx,
            share->bg_crd_try_time, &spider, &table,
            share->bg_crd_interval, share->bg_crd_mode,
            share->bg_crd_sync,
            2))
          {
/*
            if (
              share->monitoring_kind[spider.search_link_idx] &&
              need_mons[spider.search_link_idx]
            ) {
              lex_start(thd);
              error_num = spider_ping_table_mon_from_table(
                  trx,
                  thd,
                  share,
                  spider.search_link_idx,
                  (uint32) share->monitoring_sid[spider.search_link_idx],
                  share->table_name,
                  share->table_name_length,
                  spider.conn_link_idx[spider.search_link_idx],
                  NULL,
                  0,
                  share->monitoring_kind[spider.search_link_idx],
                  share->monitoring_limit[spider.search_link_idx],
                  share->monitoring_flag[spider.search_link_idx],
                  TRUE
                );
              lex_end(thd->lex);
            }
*/
            spider.search_link_idx = -1;
          }
        }
      }
    }
    memset(need_mons, 0, sizeof(int) * share->link_count);
    share->bg_crd_thd_wait = TRUE;
    pthread_cond_wait(&share->bg_crd_cond, &share->crd_mutex);
  }
}

int spider_create_mon_threads(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share
) {
  bool create_bg_mons = FALSE;
  int error_num, roop_count, roop_count2;
  SPIDER_LINK_PACK link_pack;
  SPIDER_TABLE_MON_LIST *table_mon_list;
  DBUG_ENTER("spider_create_mon_threads");
  if (!share->bg_mon_init)
  {
    for (roop_count = 0; roop_count < (int) share->all_link_count;
      roop_count++)
    {
      if (share->monitoring_bg_kind[roop_count])
      {
        create_bg_mons = TRUE;
        break;
      }
    }
    if (create_bg_mons)
    {
      char link_idx_str[SPIDER_SQL_INT_LEN];
      int link_idx_str_length;
      char *buf = (char *) my_alloca(share->table_name_length + SPIDER_SQL_INT_LEN + 1);
      spider_string conv_name_str(buf, share->table_name_length +
        SPIDER_SQL_INT_LEN + 1, system_charset_info);
      conv_name_str.init_calc_mem(105);
      conv_name_str.length(0);
      conv_name_str.q_append(share->table_name, share->table_name_length);
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (share->monitoring_bg_kind[roop_count])
        {
          conv_name_str.length(share->table_name_length);
          if (share->static_link_ids[roop_count])
          {
            memcpy(link_idx_str, share->static_link_ids[roop_count],
              share->static_link_ids_lengths[roop_count] + 1);
            link_idx_str_length = share->static_link_ids_lengths[roop_count];
          } else {
            link_idx_str_length = my_sprintf(link_idx_str, (link_idx_str,
              "%010d", roop_count));
          }
          conv_name_str.q_append(link_idx_str, link_idx_str_length + 1);
          conv_name_str.length(conv_name_str.length() - 1);
          if (!(table_mon_list = spider_get_ping_table_mon_list(trx, trx->thd,
            &conv_name_str, share->table_name_length, roop_count,
            share->static_link_ids[roop_count],
            share->static_link_ids_lengths[roop_count],
            (uint32) share->monitoring_sid[roop_count], FALSE, &error_num)))
          {
            my_afree(buf);
            goto error_get_ping_table_mon_list;
          }
          spider_free_ping_table_mon_list(table_mon_list);
        }
      }
      if (!(share->bg_mon_thds = (THD **)
        spider_bulk_malloc(spider_current_trx, 23, MYF(MY_WME | MY_ZEROFILL),
          &share->bg_mon_thds,
            (uint) (sizeof(THD *) * share->all_link_count),
          &share->bg_mon_threads,
            (uint) (sizeof(pthread_t) * share->all_link_count),
          &share->bg_mon_mutexes,
            (uint) (sizeof(pthread_mutex_t) * share->all_link_count),
          &share->bg_mon_conds,
            (uint) (sizeof(pthread_cond_t) * share->all_link_count),
          &share->bg_mon_sleep_conds,
            (uint) (sizeof(pthread_cond_t) * share->all_link_count),
          NullS))
      ) {
        error_num = HA_ERR_OUT_OF_MEM;
        my_afree(buf);
        goto error_alloc_base;
      }
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (
          share->monitoring_bg_kind[roop_count] &&
          mysql_mutex_init(spd_key_mutex_bg_mon,
            &share->bg_mon_mutexes[roop_count], MY_MUTEX_INIT_FAST)
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          my_afree(buf);
          goto error_mutex_init;
        }
      }
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (
          share->monitoring_bg_kind[roop_count] &&
          mysql_cond_init(spd_key_cond_bg_mon,
            &share->bg_mon_conds[roop_count], NULL)
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          my_afree(buf);
          goto error_cond_init;
        }
      }
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (
          share->monitoring_bg_kind[roop_count] &&
          mysql_cond_init(spd_key_cond_bg_mon_sleep,
            &share->bg_mon_sleep_conds[roop_count], NULL)
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          my_afree(buf);
          goto error_sleep_cond_init;
        }
      }
      link_pack.share = share;
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (share->monitoring_bg_kind[roop_count])
        {
          link_pack.link_idx = roop_count;
          pthread_mutex_lock(&share->bg_mon_mutexes[roop_count]);
          if (mysql_thread_create(spd_key_thd_bg_mon,
            &share->bg_mon_threads[roop_count], &spider_pt_attr,
            spider_bg_mon_action, (void *) &link_pack)
          )
          {
            error_num = HA_ERR_OUT_OF_MEM;
            my_afree(buf);
            goto error_thread_create;
          }
          pthread_cond_wait(&share->bg_mon_conds[roop_count],
            &share->bg_mon_mutexes[roop_count]);
          pthread_mutex_unlock(&share->bg_mon_mutexes[roop_count]);
        }
      }
      share->bg_mon_init = TRUE;
      my_afree(buf);
    }
  }
  DBUG_RETURN(0);

error_thread_create:
  roop_count2 = roop_count;
  for (roop_count--; roop_count >= 0; roop_count--)
  {
    if (share->monitoring_bg_kind[roop_count])
      pthread_mutex_lock(&share->bg_mon_mutexes[roop_count]);
  }
  share->bg_mon_kill = TRUE;
  for (roop_count = roop_count2 - 1; roop_count >= 0; roop_count--)
  {
    if (share->monitoring_bg_kind[roop_count])
    {
      pthread_cond_wait(&share->bg_mon_conds[roop_count],
        &share->bg_mon_mutexes[roop_count]);
      pthread_mutex_unlock(&share->bg_mon_mutexes[roop_count]);
    }
  }
  share->bg_mon_kill = FALSE;
  roop_count = share->all_link_count;
error_sleep_cond_init:
  for (roop_count--; roop_count >= 0; roop_count--)
  {
    if (share->monitoring_bg_kind[roop_count])
      pthread_cond_destroy(&share->bg_mon_sleep_conds[roop_count]);
  }
  roop_count = share->all_link_count;
error_cond_init:
  for (roop_count--; roop_count >= 0; roop_count--)
  {
    if (share->monitoring_bg_kind[roop_count])
      pthread_cond_destroy(&share->bg_mon_conds[roop_count]);
  }
  roop_count = share->all_link_count;
error_mutex_init:
  for (roop_count--; roop_count >= 0; roop_count--)
  {
    if (share->monitoring_bg_kind[roop_count])
      pthread_mutex_destroy(&share->bg_mon_mutexes[roop_count]);
  }
  spider_free(spider_current_trx, share->bg_mon_thds, MYF(0));
error_alloc_base:
error_get_ping_table_mon_list:
  DBUG_RETURN(error_num);
}

void spider_free_mon_threads(
  SPIDER_SHARE *share
) {
  int roop_count;
  DBUG_ENTER("spider_free_mon_threads");
  if (share->bg_mon_init)
  {
    for (roop_count = 0; roop_count < (int) share->all_link_count;
      roop_count++)
    {
      if (
        share->monitoring_bg_kind[roop_count] &&
        share->bg_mon_thds[roop_count]
      ) {
        share->bg_mon_thds[roop_count]->killed = SPIDER_THD_KILL_CONNECTION;
      }
    }
    for (roop_count = 0; roop_count < (int) share->all_link_count;
      roop_count++)
    {
      if (share->monitoring_bg_kind[roop_count])
        pthread_mutex_lock(&share->bg_mon_mutexes[roop_count]);
    }
    share->bg_mon_kill = TRUE;
    for (roop_count = 0; roop_count < (int) share->all_link_count;
      roop_count++)
    {
      if (share->monitoring_bg_kind[roop_count])
      {
        pthread_cond_signal(&share->bg_mon_sleep_conds[roop_count]);
        pthread_cond_wait(&share->bg_mon_conds[roop_count],
          &share->bg_mon_mutexes[roop_count]);
        pthread_mutex_unlock(&share->bg_mon_mutexes[roop_count]);
        pthread_join(share->bg_mon_threads[roop_count], NULL);
        pthread_cond_destroy(&share->bg_mon_conds[roop_count]);
        pthread_cond_destroy(&share->bg_mon_sleep_conds[roop_count]);
        pthread_mutex_destroy(&share->bg_mon_mutexes[roop_count]);
      }
    }
    spider_free(spider_current_trx, share->bg_mon_thds, MYF(0));
    share->bg_mon_kill = FALSE;
    share->bg_mon_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void *spider_bg_mon_action(
  void *arg
) {
  SPIDER_LINK_PACK *link_pack = (SPIDER_LINK_PACK*) arg;
  SPIDER_SHARE *share = link_pack->share;
  SPIDER_TRX *trx;
  int error_num, link_idx = link_pack->link_idx;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_mon_action");
  /* init start */
  pthread_mutex_lock(&share->bg_mon_mutexes[link_idx]);
  if (!(thd = SPIDER_new_THD(next_thread_id())))
  {
    share->bg_mon_kill = FALSE;
    share->bg_mon_init = FALSE;
    pthread_cond_signal(&share->bg_mon_conds[link_idx]);
    pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char*) &thd;
  thd->store_globals();
  if (!(trx = spider_get_trx(thd, FALSE, &error_num)))
  {
    delete thd;
    share->bg_mon_kill = FALSE;
    share->bg_mon_init = FALSE;
    pthread_cond_signal(&share->bg_mon_conds[link_idx]);
    pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
    set_current_thd(nullptr);
#endif
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  share->bg_mon_thds[link_idx] = thd;
  pthread_cond_signal(&share->bg_mon_conds[link_idx]);
/*
  pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
*/
  /* init end */

  while (TRUE)
  {
    DBUG_PRINT("info",("spider bg mon sleep %lld",
      share->monitoring_bg_interval[link_idx]));
    if (!share->bg_mon_kill)
    {
      struct timespec abstime;
      set_timespec_nsec(abstime,
        share->monitoring_bg_interval[link_idx] * 1000);
      pthread_cond_timedwait(&share->bg_mon_sleep_conds[link_idx],
        &share->bg_mon_mutexes[link_idx], &abstime);
/*
      my_sleep((ulong) share->monitoring_bg_interval[link_idx]);
*/
    }
    DBUG_PRINT("info",("spider bg mon roop start"));
    if (share->bg_mon_kill)
    {
      DBUG_PRINT("info",("spider bg mon kill start"));
/*
      pthread_mutex_lock(&share->bg_mon_mutexes[link_idx]);
*/
      pthread_cond_signal(&share->bg_mon_conds[link_idx]);
      pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
      spider_free_trx(trx, TRUE);
      delete thd;
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
      set_current_thd(nullptr);
#endif
      my_thread_end();
      DBUG_RETURN(NULL);
    }
    if (share->monitoring_bg_kind[link_idx])
    {
      lex_start(thd);
      error_num = spider_ping_table_mon_from_table(
        trx,
        thd,
        share,
        link_idx,
        (uint32) share->monitoring_sid[link_idx],
        share->table_name,
        share->table_name_length,
        link_idx,
        NULL,
        0,
        share->monitoring_bg_kind[link_idx],
        share->monitoring_limit[link_idx],
        share->monitoring_bg_flag[link_idx],
        TRUE
      );
      lex_end(thd->lex);
    }
  }
}

int spider_conn_first_link_idx(
  THD *thd,
  long *link_statuses,
  long *access_balances,
  uint *conn_link_idx,
  int link_count,
  int link_status
) {
  int roop_count, active_links = 0;
  longlong balance_total = 0, balance_val;
  double rand_val;
  int *link_idxs, link_idx;
  long *balances;
  DBUG_ENTER("spider_conn_first_link_idx");
  char *ptr;
  ptr = (char *) my_alloca((sizeof(int) * link_count) + (sizeof(long) * link_count));
  if (!ptr)
  {
    DBUG_PRINT("info",("spider out of memory"));
    DBUG_RETURN(-2);
  }
  link_idxs = (int *) ptr;
  ptr += sizeof(int) * link_count;
  balances = (long *) ptr;
  for (roop_count = 0; roop_count < link_count; roop_count++)
  {
    DBUG_ASSERT((conn_link_idx[roop_count] - roop_count) % link_count == 0);
    if (link_statuses[conn_link_idx[roop_count]] <= link_status)
    {
      link_idxs[active_links] = roop_count;
      balances[active_links] = access_balances[roop_count];
      balance_total += access_balances[roop_count];
      active_links++;
    }
  }

  if (active_links == 0)
  {
    DBUG_PRINT("info",("spider all links are failed"));
    my_afree(link_idxs);
    DBUG_RETURN(-1);
  }
  DBUG_PRINT("info",("spider server_id=%lu", thd->variables.server_id));
  DBUG_PRINT("info",("spider thread_id=%lu", thd_get_thread_id(thd)));
  rand_val = spider_rand(thd->variables.server_id + thd_get_thread_id(thd));
  DBUG_PRINT("info",("spider rand_val=%f", rand_val));
  balance_val = (longlong) (rand_val * balance_total);
  DBUG_PRINT("info",("spider balance_val=%lld", balance_val));
  for (roop_count = 0; roop_count < active_links - 1; roop_count++)
  {
    DBUG_PRINT("info",("spider balances[%d]=%ld",
      roop_count, balances[roop_count]));
    if (balance_val < balances[roop_count])
      break;
    balance_val -= balances[roop_count];
  }

  DBUG_PRINT("info",("spider first link_idx=%d", link_idxs[roop_count]));
  link_idx = link_idxs[roop_count];
  my_afree(link_idxs);
  DBUG_RETURN(link_idx);
}

int spider_conn_next_link_idx(
  THD *thd,
  long *link_statuses,
  long *access_balances,
  uint *conn_link_idx,
  int link_idx,
  int link_count,
  int link_status
) {
  int tmp_link_idx;
  DBUG_ENTER("spider_conn_next_link_idx");
  DBUG_ASSERT((conn_link_idx[link_idx] - link_idx) % link_count == 0);
  tmp_link_idx = spider_conn_first_link_idx(thd, link_statuses,
    access_balances, conn_link_idx, link_count, link_status);
  if (
    tmp_link_idx >= 0 &&
    tmp_link_idx == link_idx
  ) {
    do {
      tmp_link_idx++;
      if (tmp_link_idx >= link_count)
        tmp_link_idx = 0;
      if (tmp_link_idx == link_idx)
        break;
    } while (link_statuses[conn_link_idx[tmp_link_idx]] > link_status);
    DBUG_PRINT("info",("spider next link_idx=%d", tmp_link_idx));
    DBUG_RETURN(tmp_link_idx);
  }
  DBUG_PRINT("info",("spider next link_idx=%d", tmp_link_idx));
  DBUG_RETURN(tmp_link_idx);
}

int spider_conn_link_idx_next(
  long *link_statuses,
  uint *conn_link_idx,
  int link_idx,
  int link_count,
  int link_status
) {
  DBUG_ENTER("spider_conn_link_idx_next");
  do {
    link_idx++;
    if (link_idx >= link_count)
      break;
    DBUG_ASSERT((conn_link_idx[link_idx] - link_idx) % link_count == 0);
  } while (link_statuses[conn_link_idx[link_idx]] > link_status);
  DBUG_PRINT("info",("spider link_idx=%d", link_idx));
  DBUG_RETURN(link_idx);
}

int spider_conn_get_link_status(
  long *link_statuses,
  uint *conn_link_idx,
  int link_idx
) {
  DBUG_ENTER("spider_conn_get_link_status");
  DBUG_PRINT("info",("spider link_status=%d",
    (int) link_statuses[conn_link_idx[link_idx]]));
  DBUG_RETURN((int) link_statuses[conn_link_idx[link_idx]]);
}

int spider_conn_lock_mode(
  ha_spider *spider
) {
  SPIDER_WIDE_HANDLER *wide_handler = spider->wide_handler;
  DBUG_ENTER("spider_conn_lock_mode");
  if (wide_handler->external_lock_type == F_WRLCK ||
    wide_handler->lock_mode == 2)
    DBUG_RETURN(SPIDER_LOCK_MODE_EXCLUSIVE);
  else if (wide_handler->lock_mode == 1)
    DBUG_RETURN(SPIDER_LOCK_MODE_SHARED);
  DBUG_RETURN(SPIDER_LOCK_MODE_NO_LOCK);
}

bool spider_conn_check_recovery_link(
  SPIDER_SHARE *share
) {
  int roop_count;
  DBUG_ENTER("spider_check_recovery_link");
  for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
  {
    if (share->link_statuses[roop_count] == SPIDER_LINK_STATUS_RECOVERY)
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

bool spider_conn_use_handler(
  ha_spider *spider,
  int lock_mode,
  int link_idx
) {
  THD *thd = spider->wide_handler->trx->thd;
  int use_handler = spider_param_use_handler(thd,
    spider->share->use_handlers[link_idx]);
  DBUG_ENTER("spider_conn_use_handler");
  DBUG_PRINT("info",("spider use_handler=%d", use_handler));
  DBUG_PRINT("info",("spider spider->conn_kind[link_idx]=%u",
    spider->conn_kind[link_idx]));
  if (spider->do_direct_update)
  {
    spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
      spider->direct_update_kinds |= SPIDER_SQL_KIND_SQL;
      DBUG_PRINT("info",("spider FALSE by using direct_update"));
      DBUG_RETURN(FALSE);
  }
  if (spider->use_spatial_index)
  {
    DBUG_PRINT("info",("spider FALSE by use_spatial_index"));
    spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
    DBUG_RETURN(FALSE);
  }
  uint dbton_id;
  spider_db_handler *dbton_hdl;
  dbton_id = spider->share->sql_dbton_ids[spider->conn_link_idx[link_idx]];
  dbton_hdl = spider->dbton_handler[dbton_id];
  if (!dbton_hdl->support_use_handler(use_handler))
  {
    DBUG_PRINT("info",("spider FALSE by dbton"));
    spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
    DBUG_RETURN(FALSE);
  }
  if (
    spider->wide_handler->sql_command == SQLCOM_HA_READ &&
    (
      !(use_handler & 2) ||
      (
        spider_param_sync_trx_isolation(thd) &&
        thd_tx_isolation(thd) == ISO_SERIALIZABLE
      )
    )
  ) {
    DBUG_PRINT("info",("spider TRUE by HA"));
    spider->sql_kinds |= SPIDER_SQL_KIND_HANDLER;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_HANDLER;
    DBUG_RETURN(TRUE);
  }
  if (
    spider->wide_handler->sql_command != SQLCOM_HA_READ &&
    lock_mode == SPIDER_LOCK_MODE_NO_LOCK &&
    spider_param_sync_trx_isolation(thd) &&
    thd_tx_isolation(thd) != ISO_SERIALIZABLE &&
    (use_handler & 1)
  ) {
    DBUG_PRINT("info",("spider TRUE by PARAM"));
    spider->sql_kinds |= SPIDER_SQL_KIND_HANDLER;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_HANDLER;
    DBUG_RETURN(TRUE);
  }
  spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
  spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
  DBUG_RETURN(FALSE);
}

bool spider_conn_need_open_handler(
  ha_spider *spider,
  uint idx,
  int link_idx
) {
  DBUG_ENTER("spider_conn_need_open_handler");
  DBUG_PRINT("info",("spider spider=%p", spider));
  if (spider->handler_opened(link_idx, spider->conn_kind[link_idx]))
  {
      DBUG_PRINT("info",("spider HA already opened"));
      DBUG_RETURN(FALSE);
  }
  DBUG_RETURN(TRUE);
}

SPIDER_CONN* spider_get_conn_from_idle_connection(
  SPIDER_SHARE *share,
  int link_idx,
  char *conn_key,
  ha_spider *spider,
  uint conn_kind,
  int base_link_idx,
  int *error_num
  )
{
  DBUG_ENTER("spider_get_conn_from_idle_connection");
  SPIDER_IP_PORT_CONN *ip_port_conn;
  SPIDER_CONN *conn = NULL;
  uint spider_max_connections = spider_param_max_connections();
  struct timespec abstime;
  ulonglong start, inter_val = 0;
  longlong last_ntime = 0;
  ulonglong wait_time = (ulonglong)spider_param_conn_wait_timeout()*1000*1000*1000; // default 10s

  unsigned long ip_port_count = 0; // init 0

  set_timespec(abstime, 0);

  pthread_mutex_lock(&spider_ipport_conn_mutex);
  if ((ip_port_conn = (SPIDER_IP_PORT_CONN*) my_hash_search_using_hash_value(
    &spider_ipport_conns, share->conn_keys_hash_value[link_idx],
    (uchar*) share->conn_keys[link_idx], share->conn_keys_lengths[link_idx])))
  { /* exists */
    pthread_mutex_unlock(&spider_ipport_conn_mutex);
    pthread_mutex_lock(&ip_port_conn->mutex);
    ip_port_count = ip_port_conn->ip_port_count;
  } else {
    pthread_mutex_unlock(&spider_ipport_conn_mutex);
  }

  if (
    ip_port_conn &&
    ip_port_count >= spider_max_connections &&
    spider_max_connections > 0
  ) { /* no idle conn && enable connection pool, wait */
    pthread_mutex_unlock(&ip_port_conn->mutex);
    start = my_hrtime().val;
    while(1)
    {
      int error;
      inter_val = my_hrtime().val - start; // us
      last_ntime = wait_time - inter_val*1000; // *1000, to ns
      if(last_ntime <= 0)
      {/* wait timeout */
        *error_num = ER_SPIDER_CON_COUNT_ERROR;
        DBUG_RETURN(NULL);
      }
      set_timespec_nsec(abstime, last_ntime);
      pthread_mutex_lock(&ip_port_conn->mutex);
      ++ip_port_conn->waiting_count;
      error = pthread_cond_timedwait(&ip_port_conn->cond, &ip_port_conn->mutex, &abstime);
      --ip_port_conn->waiting_count;
      pthread_mutex_unlock(&ip_port_conn->mutex);
      if (error == ETIMEDOUT || error == ETIME || error != 0 )
      {
        *error_num = ER_SPIDER_CON_COUNT_ERROR;
        DBUG_RETURN(NULL);
      }

      pthread_mutex_lock(&spider_conn_mutex);
      if ((conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
        &spider_open_connections, share->conn_keys_hash_value[link_idx],
        (uchar*) share->conn_keys[link_idx],
        share->conn_keys_lengths[link_idx])))
      {
        /* get conn from spider_open_connections, then delete conn in spider_open_connections */
        my_hash_delete(&spider_open_connections, (uchar*) conn);  
        pthread_mutex_unlock(&spider_conn_mutex);
        DBUG_PRINT("info",("spider get global conn"));
        if (spider)
        {
          spider->conns[base_link_idx] = conn;
          if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
            conn->use_for_active_standby = TRUE;
        }
        DBUG_RETURN(conn);
      }
      else
      {
        pthread_mutex_unlock(&spider_conn_mutex);
      }
    }
  }
  else
  { /* create conn */
    if (ip_port_conn)
      pthread_mutex_unlock(&ip_port_conn->mutex);
    DBUG_PRINT("info",("spider create new conn"));
    if (!(conn = spider_create_conn(share, spider, link_idx, base_link_idx, conn_kind, error_num)))
      DBUG_RETURN(conn);
    *conn->conn_key = *conn_key;
    if (spider)
    {
      spider->conns[base_link_idx] = conn;
      if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
        conn->use_for_active_standby = TRUE;
    }
  }

  DBUG_RETURN(conn);
}


SPIDER_IP_PORT_CONN* spider_create_ipport_conn(SPIDER_CONN *conn)
{
  DBUG_ENTER("spider_create_ipport_conn");
  if (conn)
  {
    SPIDER_IP_PORT_CONN *ret = (SPIDER_IP_PORT_CONN *)
      my_malloc(PSI_INSTRUMENT_ME, sizeof(*ret), MY_ZEROFILL | MY_WME);
    if (!ret)
    {
      goto err_return_direct;
    }

    if (mysql_mutex_init(spd_key_mutex_conn_i, &ret->mutex, MY_MUTEX_INIT_FAST))
    {
      //error
      goto err_malloc_key;
    }

    if (mysql_cond_init(spd_key_cond_conn_i, &ret->cond, NULL))
    {
      pthread_mutex_destroy(&ret->mutex);
      goto err_malloc_key;
      //error
    }

    ret->key_len = conn->conn_key_length;
    if (ret->key_len <= 0) {
      pthread_cond_destroy(&ret->cond);
      pthread_mutex_destroy(&ret->mutex);
      goto err_malloc_key;
    }

    ret->key = (char *) my_malloc(PSI_INSTRUMENT_ME, ret->key_len +
                             conn->tgt_host_length + 1, MY_ZEROFILL | MY_WME);
    if (!ret->key) {
      pthread_cond_destroy(&ret->cond);
      pthread_mutex_destroy(&ret->mutex);
      goto err_malloc_key;
    }
    ret->remote_ip_str = ret->key + ret->key_len;

    memcpy(ret->key, conn->conn_key, ret->key_len);

    memcpy(ret->remote_ip_str, conn->tgt_host, conn->tgt_host_length);
    ret->remote_port = conn->tgt_port;
    ret->conn_id = conn->conn_id;
    ret->ip_port_count = 1; // init

    ret->key_hash_value = conn->conn_key_hash_value;
    DBUG_RETURN(ret);
err_malloc_key:
    spider_my_free(ret, MYF(0));
err_return_direct:
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(NULL);
}


void spider_free_ipport_conn(void *info)
{
  DBUG_ENTER("spider_free_ipport_conn");
  if (info)
  {
    SPIDER_IP_PORT_CONN *p = (SPIDER_IP_PORT_CONN *)info;
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->mutex);
    spider_my_free(p->key, MYF(0));
    spider_my_free(p, MYF(0));
  }
  DBUG_VOID_RETURN;
}
