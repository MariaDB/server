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
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

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
#include "tztime.h"
#endif
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

extern ulong *spd_db_att_thread_id;

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
pthread_mutex_t spider_conn_id_mutex;
ulonglong spider_conn_id = 1;

#ifndef WITHOUT_SPIDER_BG_SEARCH
extern pthread_attr_t spider_pt_attr;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key spd_key_mutex_mta_conn;
#ifndef WITHOUT_SPIDER_BG_SEARCH
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
#endif

extern pthread_mutex_t spider_global_trx_mutex;
extern SPIDER_TRX *spider_global_trx;
#endif

HASH spider_open_connections;
uint spider_open_connections_id;
const char *spider_open_connections_func_name;
const char *spider_open_connections_file_name;
ulong spider_open_connections_line_no;
pthread_mutex_t spider_conn_mutex;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
HASH spider_hs_r_conn_hash;
uint spider_hs_r_conn_hash_id;
const char *spider_hs_r_conn_hash_func_name;
const char *spider_hs_r_conn_hash_file_name;
ulong spider_hs_r_conn_hash_line_no;
pthread_mutex_t spider_hs_r_conn_mutex;
HASH spider_hs_w_conn_hash;
uint spider_hs_w_conn_hash_id;
const char *spider_hs_w_conn_hash_func_name;
const char *spider_hs_w_conn_hash_file_name;
ulong spider_hs_w_conn_hash_line_no;
pthread_mutex_t spider_hs_w_conn_mutex;
#endif

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

int spider_reset_conn_setted_parameter(
  SPIDER_CONN *conn,
  THD *thd
) {
  DBUG_ENTER("spider_reset_conn_setted_parameter");
  conn->autocommit = spider_param_remote_autocommit();
  conn->sql_log_off = spider_param_remote_sql_log_off();
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
        MY_CS_PRIMARY, MYF(MY_WME))))
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
  DBUG_RETURN(0);
}

int spider_free_conn_alloc(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_free_conn_alloc");
  spider_db_disconnect(conn);
#ifndef WITHOUT_SPIDER_BG_SEARCH
  spider_free_conn_thread(conn);
#endif
  if (conn->db_conn)
  {
    delete conn->db_conn;
    conn->db_conn = NULL;
  }
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
  DBUG_ENTER("spider_free_conn_from_trx");
  spider_conn_clear_queue(conn);
  conn->use_for_active_standby = FALSE;
  conn->error_mode = 1;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (conn->conn_kind == SPIDER_CONN_KIND_MYSQL)
  {
#endif
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
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        my_hash_delete_with_hash_value(&trx->trx_another_conn_hash,
          conn->conn_key_hash_value, (uchar*) conn);
#else
        my_hash_delete(&trx->trx_another_conn_hash, (uchar*) conn);
#endif
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
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        my_hash_delete_with_hash_value(&trx->trx_conn_hash,
          conn->conn_key_hash_value, (uchar*) conn);
#else
        my_hash_delete(&trx->trx_conn_hash, (uchar*) conn);
#endif
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
#ifdef HASH_UPDATE_WITH_HASH_VALUE
          if (my_hash_insert_with_hash_value(&spider_open_connections,
            conn->conn_key_hash_value, (uchar*) conn))
#else
          if (my_hash_insert(&spider_open_connections, (uchar*) conn))
#endif
          {
            pthread_mutex_unlock(&spider_conn_mutex);
            spider_free_conn(conn);
          } else {
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
        spider_free_conn(conn);
      }
    } else if (roop_count)
      (*roop_count)++;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  } else if (conn->conn_kind == SPIDER_CONN_KIND_HS_READ)
  {
    spider_db_hs_request_buf_reset(conn);
    if (
      trx_free ||
      (
        (
          conn->server_lost ||
          spider_param_hs_r_conn_recycle_mode(trx->thd) != 2
        ) &&
        !conn->opened_handlers
      )
    ) {
      conn->thd = NULL;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      my_hash_delete_with_hash_value(&trx->trx_hs_r_conn_hash,
        conn->conn_key_hash_value, (uchar*) conn);
#else
      my_hash_delete(&trx->trx_hs_r_conn_hash, (uchar*) conn);
#endif

      DBUG_ASSERT(conn->opened_handlers ==
        conn->db_conn->get_opened_handler_count());
      if (conn->db_conn->get_opened_handler_count())
      {
        conn->db_conn->reset_opened_handler();
      }

      if (
        !trx_free &&
        !conn->server_lost &&
        !conn->queued_connect &&
        spider_param_hs_r_conn_recycle_mode(trx->thd) == 1
      ) {
        /* conn_recycle_mode == 1 */
        *conn->conn_key = '0';
        pthread_mutex_lock(&spider_hs_r_conn_mutex);
        uint old_elements = spider_hs_r_conn_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        if (my_hash_insert_with_hash_value(&spider_hs_r_conn_hash,
          conn->conn_key_hash_value, (uchar*) conn))
#else
        if (my_hash_insert(&spider_hs_r_conn_hash, (uchar*) conn))
#endif
        {
          pthread_mutex_unlock(&spider_hs_r_conn_mutex);
          spider_free_conn(conn);
        } else {
          if (spider_hs_r_conn_hash.array.max_element > old_elements)
          {
            spider_alloc_calc_mem(spider_current_trx,
              spider_hs_r_conn_hash,
              (spider_hs_r_conn_hash.array.max_element - old_elements) *
              spider_hs_r_conn_hash.array.size_of_element);
          }
          pthread_mutex_unlock(&spider_hs_r_conn_mutex);
        }
      } else {
        /* conn_recycle_mode == 0 */
        spider_free_conn(conn);
      }
    } else if (roop_count)
      (*roop_count)++;
  } else {
    spider_db_hs_request_buf_reset(conn);
    if (
      trx_free ||
      (
        (
          conn->server_lost ||
          spider_param_hs_w_conn_recycle_mode(trx->thd) != 2
        ) &&
        !conn->opened_handlers
      )
    ) {
      conn->thd = NULL;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      my_hash_delete_with_hash_value(&trx->trx_hs_w_conn_hash,
        conn->conn_key_hash_value, (uchar*) conn);
#else
      my_hash_delete(&trx->trx_hs_w_conn_hash, (uchar*) conn);
#endif

      DBUG_ASSERT(conn->opened_handlers ==
        conn->db_conn->get_opened_handler_count());
      if (conn->db_conn->get_opened_handler_count())
      {
        conn->db_conn->reset_opened_handler();
      }

      if (
        !trx_free &&
        !conn->server_lost &&
        !conn->queued_connect &&
        spider_param_hs_w_conn_recycle_mode(trx->thd) == 1
      ) {
        /* conn_recycle_mode == 1 */
        *conn->conn_key = '0';
        pthread_mutex_lock(&spider_hs_w_conn_mutex);
        uint old_elements = spider_hs_w_conn_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        if (my_hash_insert_with_hash_value(&spider_hs_w_conn_hash,
          conn->conn_key_hash_value, (uchar*) conn))
#else
        if (my_hash_insert(&spider_hs_w_conn_hash, (uchar*) conn))
#endif
        {
          pthread_mutex_unlock(&spider_hs_w_conn_mutex);
          spider_free_conn(conn);
        } else {
          if (spider_hs_w_conn_hash.array.max_element > old_elements)
          {
            spider_alloc_calc_mem(spider_current_trx,
              spider_hs_w_conn_hash,
              (spider_hs_w_conn_hash.array.max_element - old_elements) *
              spider_hs_w_conn_hash.array.size_of_element);
          }
          pthread_mutex_unlock(&spider_hs_w_conn_mutex);
        }
      } else {
        /* conn_recycle_mode == 0 */
        spider_free_conn(conn);
      }
    } else if (roop_count)
      (*roop_count)++;
  }
#endif
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
  char *tmp_name, *tmp_host, *tmp_username, *tmp_password, *tmp_socket;
  char *tmp_wrapper, *tmp_ssl_ca, *tmp_ssl_capath, *tmp_ssl_cert;
  char *tmp_ssl_cipher, *tmp_ssl_key, *tmp_default_file, *tmp_default_group;
  DBUG_ENTER("spider_create_conn");

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (conn_kind == SPIDER_CONN_KIND_MYSQL)
  {
#endif
    if (!(conn = (SPIDER_CONN *)
      spider_bulk_malloc(spider_current_trx, 18, MYF(MY_WME | MY_ZEROFILL),
        &conn, sizeof(*conn),
        &tmp_name, share->conn_keys_lengths[link_idx] + 1,
        &tmp_host, share->tgt_hosts_lengths[link_idx] + 1,
        &tmp_username,
          share->tgt_usernames_lengths[link_idx] + 1,
        &tmp_password,
          share->tgt_passwords_lengths[link_idx] + 1,
        &tmp_socket, share->tgt_sockets_lengths[link_idx] + 1,
        &tmp_wrapper,
          share->tgt_wrappers_lengths[link_idx] + 1,
        &tmp_ssl_ca, share->tgt_ssl_cas_lengths[link_idx] + 1,
        &tmp_ssl_capath,
          share->tgt_ssl_capaths_lengths[link_idx] + 1,
        &tmp_ssl_cert,
          share->tgt_ssl_certs_lengths[link_idx] + 1,
        &tmp_ssl_cipher,
          share->tgt_ssl_ciphers_lengths[link_idx] + 1,
        &tmp_ssl_key,
          share->tgt_ssl_keys_lengths[link_idx] + 1,
        &tmp_default_file,
          share->tgt_default_files_lengths[link_idx] + 1,
        &tmp_default_group,
          share->tgt_default_groups_lengths[link_idx] + 1,
        &need_mon, sizeof(int),
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
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    conn->conn_key_hash_value = share->conn_keys_hash_value[link_idx];
#endif
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
    conn->tgt_port = share->tgt_ports[link_idx];
    conn->tgt_ssl_vsc = share->tgt_ssl_vscs[link_idx];
    conn->dbton_id = share->sql_dbton_ids[link_idx];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  } else if (conn_kind == SPIDER_CONN_KIND_HS_READ) {
    if (!(conn = (SPIDER_CONN *)
      spider_bulk_malloc(spider_current_trx, 19, MYF(MY_WME | MY_ZEROFILL),
        &conn, sizeof(*conn),
        &tmp_name, share->hs_read_conn_keys_lengths[link_idx] + 1,
        &tmp_host, share->tgt_hosts_lengths[link_idx] + 1,
        &tmp_socket, share->hs_read_socks_lengths[link_idx] + 1,
        &tmp_wrapper,
          share->tgt_wrappers_lengths[link_idx] + 1,
        &need_mon, sizeof(int),
        NullS))
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc_conn;
    }

    conn->default_database.init_calc_mem(76);
    conn->conn_key_length = share->hs_read_conn_keys_lengths[link_idx];
    conn->conn_key = tmp_name;
    memcpy(conn->conn_key, share->hs_read_conn_keys[link_idx],
      share->hs_read_conn_keys_lengths[link_idx]);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    conn->conn_key_hash_value = share->hs_read_conn_keys_hash_value[link_idx];
#endif
    conn->tgt_host_length = share->tgt_hosts_lengths[link_idx];
    conn->tgt_host = tmp_host;
    memcpy(conn->tgt_host, share->tgt_hosts[link_idx],
      share->tgt_hosts_lengths[link_idx]);
    conn->hs_sock_length = share->hs_read_socks_lengths[link_idx];
    if (conn->hs_sock_length)
    {
      conn->hs_sock = tmp_socket;
      memcpy(conn->hs_sock, share->hs_read_socks[link_idx],
        share->hs_read_socks_lengths[link_idx]);
    } else
      conn->hs_sock = NULL;
    conn->tgt_wrapper_length = share->tgt_wrappers_lengths[link_idx];
    conn->tgt_wrapper = tmp_wrapper;
    memcpy(conn->tgt_wrapper, share->tgt_wrappers[link_idx],
      share->tgt_wrappers_lengths[link_idx]);
    conn->hs_port = share->hs_read_ports[link_idx];
    conn->dbton_id = share->hs_dbton_ids[link_idx];
  } else {
    if (!(conn = (SPIDER_CONN *)
      spider_bulk_malloc(spider_current_trx, 20, MYF(MY_WME | MY_ZEROFILL),
        &conn, sizeof(*conn),
        &tmp_name, share->hs_write_conn_keys_lengths[link_idx] + 1,
        &tmp_host, share->tgt_hosts_lengths[link_idx] + 1,
        &tmp_socket, share->hs_write_socks_lengths[link_idx] + 1,
        &tmp_wrapper,
          share->tgt_wrappers_lengths[link_idx] + 1,
        &need_mon, sizeof(int),
        NullS))
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc_conn;
    }

    conn->default_database.init_calc_mem(77);
    conn->conn_key_length = share->hs_write_conn_keys_lengths[link_idx];
    conn->conn_key = tmp_name;
    memcpy(conn->conn_key, share->hs_write_conn_keys[link_idx],
      share->hs_write_conn_keys_lengths[link_idx]);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    conn->conn_key_hash_value = share->hs_write_conn_keys_hash_value[link_idx];
#endif
    conn->tgt_host_length = share->tgt_hosts_lengths[link_idx];
    conn->tgt_host = tmp_host;
    memcpy(conn->tgt_host, share->tgt_hosts[link_idx],
      share->tgt_hosts_lengths[link_idx]);
    conn->hs_sock_length = share->hs_write_socks_lengths[link_idx];
    if (conn->hs_sock_length)
    {
      conn->hs_sock = tmp_socket;
      memcpy(conn->hs_sock, share->hs_write_socks[link_idx],
        share->hs_write_socks_lengths[link_idx]);
    } else
      conn->hs_sock = NULL;
    conn->tgt_wrapper_length = share->tgt_wrappers_lengths[link_idx];
    conn->tgt_wrapper = tmp_wrapper;
    memcpy(conn->tgt_wrapper, share->tgt_wrappers[link_idx],
      share->tgt_wrappers_lengths[link_idx]);
    conn->hs_port = share->hs_write_ports[link_idx];
    conn->dbton_id = share->hs_dbton_ids[link_idx];
  }
#endif
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

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&conn->mta_conn_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_mta_conn, &conn->mta_conn_mutex,
    MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_mta_conn_mutex_init;
  }

  spider_conn_queue_connect(share, conn, link_idx);
  conn->ping_time = (time_t) time((time_t*) 0);
  conn->connect_error_time = conn->ping_time;
  pthread_mutex_lock(&spider_conn_id_mutex);
  conn->conn_id = spider_conn_id;
  ++spider_conn_id;
  pthread_mutex_unlock(&spider_conn_id_mutex);

  DBUG_RETURN(conn);

/*
error_init_lock_table_hash:
  DBUG_ASSERT(!conn->mta_conn_mutex_file_pos.file_name);
  pthread_mutex_destroy(&conn->mta_conn_mutex);
*/
error_mta_conn_mutex_init:
error_db_conn_init:
  delete conn->db_conn;
error_db_conn_create:
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

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (conn_kind == SPIDER_CONN_KIND_MYSQL)
  {
#endif
#ifndef DBUG_OFF
    spider_print_keys(conn_key, share->conn_keys_lengths[link_idx]);
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  } else if (conn_kind == SPIDER_CONN_KIND_HS_READ)
  {
    conn_key = share->hs_read_conn_keys[link_idx];
#ifndef DBUG_OFF
    spider_print_keys(conn_key, share->hs_read_conn_keys_lengths[link_idx]);
#endif
  } else {
    conn_key = share->hs_write_conn_keys[link_idx];
#ifndef DBUG_OFF
    spider_print_keys(conn_key, share->hs_write_conn_keys_lengths[link_idx]);
#endif
  }
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    (conn_kind == SPIDER_CONN_KIND_MYSQL &&
      (
#endif
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      )
    ) ||
    (conn_kind == SPIDER_CONN_KIND_HS_READ &&
      !(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
        &trx->trx_hs_r_conn_hash,
        share->hs_read_conn_keys_hash_value[link_idx],
        (uchar*) conn_key, share->hs_read_conn_keys_lengths[link_idx]))
    ) ||
    (conn_kind == SPIDER_CONN_KIND_HS_WRITE &&
      !(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
        &trx->trx_hs_w_conn_hash,
        share->hs_write_conn_keys_hash_value[link_idx],
        (uchar*) conn_key, share->hs_write_conn_keys_lengths[link_idx]))
    )
#endif
  )
#else
  if (
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    (conn_kind == SPIDER_CONN_KIND_MYSQL &&
      (
#endif
        (another &&
          !(conn = (SPIDER_CONN*) my_hash_search(&trx->trx_another_conn_hash,
            (uchar*) conn_key, share->conn_keys_lengths[link_idx]))) ||
        (!another &&
          !(conn = (SPIDER_CONN*) my_hash_search(&trx->trx_conn_hash,
            (uchar*) conn_key, share->conn_keys_lengths[link_idx])))
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      )
    ) ||
    (conn_kind == SPIDER_CONN_KIND_HS_READ &&
      !(conn = (SPIDER_CONN*) my_hash_search(&trx->trx_hs_r_conn_hash,
        (uchar*) conn_key, share->hs_read_conn_keys_lengths[link_idx]))
    ) ||
    (conn_kind == SPIDER_CONN_KIND_HS_WRITE &&
      !(conn = (SPIDER_CONN*) my_hash_search(&trx->trx_hs_w_conn_hash,
        (uchar*) conn_key, share->hs_write_conn_keys_lengths[link_idx]))
    )
#endif
  )
#endif
  {
    if (
      !trx->thd ||
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      (conn_kind == SPIDER_CONN_KIND_MYSQL &&
#endif
        (
          (spider_param_conn_recycle_mode(trx->thd) & 1) ||
          spider_param_conn_recycle_strict(trx->thd)
        )
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      ) ||
      (conn_kind == SPIDER_CONN_KIND_HS_READ &&
        (
          (spider_param_hs_r_conn_recycle_mode(trx->thd) & 1) ||
          spider_param_hs_r_conn_recycle_strict(trx->thd)
        )
      ) ||
      (conn_kind == SPIDER_CONN_KIND_HS_WRITE &&
        (
          (spider_param_hs_w_conn_recycle_mode(trx->thd) & 1) ||
          spider_param_hs_w_conn_recycle_strict(trx->thd)
        )
      )
#endif
    ) {
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      if (conn_kind == SPIDER_CONN_KIND_MYSQL)
      {
#endif
        pthread_mutex_lock(&spider_conn_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
        if (!(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
          &spider_open_connections, share->conn_keys_hash_value[link_idx],
          (uchar*) share->conn_keys[link_idx],
          share->conn_keys_lengths[link_idx])))
#else
        if (!(conn = (SPIDER_CONN*) my_hash_search(&spider_open_connections,
          (uchar*) share->conn_keys[link_idx],
          share->conn_keys_lengths[link_idx])))
#endif
        {
          pthread_mutex_unlock(&spider_conn_mutex);
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
        } else {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
          my_hash_delete_with_hash_value(&spider_open_connections,
            conn->conn_key_hash_value, (uchar*) conn);
#else
          my_hash_delete(&spider_open_connections, (uchar*) conn);
#endif
          pthread_mutex_unlock(&spider_conn_mutex);
          DBUG_PRINT("info",("spider get global conn"));
          if (spider)
          {
            spider->conns[base_link_idx] = conn;
            if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
              conn->use_for_active_standby = TRUE;
          }
        }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      } else if (conn_kind == SPIDER_CONN_KIND_HS_READ)
      {
        pthread_mutex_lock(&spider_hs_r_conn_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
        if (!(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
          &spider_hs_r_conn_hash,
          share->hs_read_conn_keys_hash_value[link_idx],
          (uchar*) share->hs_read_conn_keys[link_idx],
          share->hs_read_conn_keys_lengths[link_idx])))
#else
        if (!(conn = (SPIDER_CONN*) my_hash_search(&spider_hs_r_conn_hash,
          (uchar*) share->hs_read_conn_keys[link_idx],
          share->hs_read_conn_keys_lengths[link_idx])))
#endif
        {
          pthread_mutex_unlock(&spider_hs_r_conn_mutex);
          DBUG_PRINT("info",("spider create new hs r conn"));
          if (!(conn = spider_create_conn(share, spider, link_idx,
            base_link_idx, conn_kind, error_num)))
            goto error;
          *conn->conn_key = *conn_key;
          if (spider)
          {
            spider->hs_r_conns[base_link_idx] = conn;
            if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
              conn->use_for_active_standby = TRUE;
          }
        } else {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
          my_hash_delete_with_hash_value(&spider_hs_r_conn_hash,
            conn->conn_key_hash_value, (uchar*) conn);
#else
          my_hash_delete(&spider_hs_r_conn_hash, (uchar*) conn);
#endif
          pthread_mutex_unlock(&spider_hs_r_conn_mutex);
          DBUG_PRINT("info",("spider get global hs r conn"));
          if (spider)
          {
            spider->hs_r_conns[base_link_idx] = conn;
            if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
              conn->use_for_active_standby = TRUE;
          }
        }
      } else {
        pthread_mutex_lock(&spider_hs_w_conn_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
        if (!(conn = (SPIDER_CONN*) my_hash_search_using_hash_value(
          &spider_hs_w_conn_hash,
          share->hs_write_conn_keys_hash_value[link_idx],
          (uchar*) share->hs_write_conn_keys[link_idx],
          share->hs_write_conn_keys_lengths[link_idx])))
#else
        if (!(conn = (SPIDER_CONN*) my_hash_search(&spider_hs_w_conn_hash,
          (uchar*) share->hs_write_conn_keys[link_idx],
          share->hs_write_conn_keys_lengths[link_idx])))
#endif
        {
          pthread_mutex_unlock(&spider_hs_w_conn_mutex);
          DBUG_PRINT("info",("spider create new hs w conn"));
          if (!(conn = spider_create_conn(share, spider, link_idx,
            base_link_idx, conn_kind, error_num)))
            goto error;
          *conn->conn_key = *conn_key;
          if (spider)
          {
            spider->hs_w_conns[base_link_idx] = conn;
            if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
              conn->use_for_active_standby = TRUE;
          }
        } else {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
          my_hash_delete_with_hash_value(&spider_hs_w_conn_hash,
            conn->conn_key_hash_value, (uchar*) conn);
#else
          my_hash_delete(&spider_hs_w_conn_hash, (uchar*) conn);
#endif
          pthread_mutex_unlock(&spider_hs_w_conn_mutex);
          DBUG_PRINT("info",("spider get global hs w conn"));
          if (spider)
          {
            spider->hs_w_conns[base_link_idx] = conn;
            if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
              conn->use_for_active_standby = TRUE;
          }
        }
      }
#endif
    } else {
      DBUG_PRINT("info",("spider create new conn"));
      /* conn_recycle_strict = 0 and conn_recycle_mode = 0 or 2 */
      if (!(conn = spider_create_conn(share, spider, link_idx, base_link_idx,
        conn_kind, error_num)))
        goto error;
      *conn->conn_key = *conn_key;
      if (spider)
      {
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        if (conn_kind == SPIDER_CONN_KIND_MYSQL)
        {
#endif
          spider->conns[base_link_idx] = conn;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        } else if (conn_kind == SPIDER_CONN_KIND_HS_READ)
        {
          spider->hs_r_conns[base_link_idx] = conn;
        } else {
          spider->hs_w_conns[base_link_idx] = conn;
        }
#endif
        if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
          conn->use_for_active_standby = TRUE;
      }
    }
    conn->thd = trx->thd;
    conn->priority = share->priority;

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    if (conn_kind == SPIDER_CONN_KIND_MYSQL)
    {
#endif
      if (another)
      {
        uint old_elements = trx->trx_another_conn_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        if (my_hash_insert_with_hash_value(&trx->trx_another_conn_hash,
          share->conn_keys_hash_value[link_idx],
          (uchar*) conn))
#else
        if (my_hash_insert(&trx->trx_another_conn_hash, (uchar*) conn))
#endif
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
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        if (my_hash_insert_with_hash_value(&trx->trx_conn_hash,
          share->conn_keys_hash_value[link_idx],
          (uchar*) conn))
#else
        if (my_hash_insert(&trx->trx_conn_hash, (uchar*) conn))
#endif
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    } else if (conn_kind == SPIDER_CONN_KIND_HS_READ)
    {
      uint old_elements = trx->trx_hs_r_conn_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      if (my_hash_insert_with_hash_value(&trx->trx_hs_r_conn_hash,
        share->hs_read_conn_keys_hash_value[link_idx],
        (uchar*) conn))
#else
      if (my_hash_insert(&trx->trx_hs_r_conn_hash, (uchar*) conn))
#endif
      {
        spider_free_conn(conn);
        *error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
      if (trx->trx_hs_r_conn_hash.array.max_element > old_elements)
      {
        spider_alloc_calc_mem(spider_current_trx,
          trx->trx_hs_r_conn_hash,
          (trx->trx_hs_r_conn_hash.array.max_element - old_elements) *
          trx->trx_hs_r_conn_hash.array.size_of_element);
      }
    } else {
      uint old_elements = trx->trx_hs_w_conn_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      if (my_hash_insert_with_hash_value(&trx->trx_hs_w_conn_hash,
        share->hs_write_conn_keys_hash_value[link_idx],
        (uchar*) conn))
#else
      if (my_hash_insert(&trx->trx_hs_w_conn_hash, (uchar*) conn))
#endif
      {
        spider_free_conn(conn);
        *error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
      if (trx->trx_hs_w_conn_hash.array.max_element > old_elements)
      {
        spider_alloc_calc_mem(spider_current_trx,
          trx->trx_hs_w_conn_hash,
          (trx->trx_hs_w_conn_hash.array.max_element - old_elements) *
          trx->trx_hs_w_conn_hash.array.size_of_element);
      }
    }
#endif
  } else if (spider)
  {
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    if (conn_kind == SPIDER_CONN_KIND_MYSQL)
    {
#endif
      spider->conns[base_link_idx] = conn;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    } else if (conn_kind == SPIDER_CONN_KIND_HS_READ)
    {
      spider->hs_r_conns[base_link_idx] = conn;
    } else {
      spider->hs_w_conns[base_link_idx] = conn;
    }
#endif
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
          spider->conn_keys[link_idx], spider->trx,
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
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
      || result_list->direct_aggregate
#endif
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

#ifndef WITHOUT_SPIDER_BG_SEARCH
int spider_set_conn_bg_param(
  ha_spider *spider
) {
  int error_num, roop_count, bgs_mode;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  THD *thd = spider->trx->thd;
  DBUG_ENTER("spider_set_conn_bg_param");
  bgs_mode =
    spider_param_bgs_mode(thd, share->bgs_mode);
  if (bgs_mode == 0)
    result_list->bgs_phase = 0;
  else if (
    bgs_mode <= 2 &&
    (result_list->lock_type == F_WRLCK || spider->lock_mode == 2)
  )
    result_list->bgs_phase = 0;
  else if (bgs_mode <= 1 && spider->lock_mode == 1)
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
    for (
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, -1, share->link_count,
        spider->lock_mode ?
        SPIDER_LINK_STATUS_RECOVERY : SPIDER_LINK_STATUS_OK);
      roop_count < (int) share->link_count;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, roop_count, share->link_count,
        spider->lock_mode ?
        SPIDER_LINK_STATUS_RECOVERY : SPIDER_LINK_STATUS_OK)
    ) {
      if ((error_num = spider_create_conn_thread(spider->conns[roop_count])))
        DBUG_RETURN(error_num);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      if ((error_num = spider_create_conn_thread(
        spider->hs_r_conns[roop_count])))
        DBUG_RETURN(error_num);
      if ((error_num = spider_create_conn_thread(
        spider->hs_w_conns[roop_count])))
        DBUG_RETURN(error_num);
#endif
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
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&conn->bg_conn_chain_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_bg_conn_chain,
      &conn->bg_conn_chain_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_chain_mutex_init;
    }
    conn->bg_conn_chain_mutex_ptr = NULL;
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&conn->bg_conn_sync_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_bg_conn_sync,
      &conn->bg_conn_sync_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_mutex_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&conn->bg_conn_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_bg_conn, &conn->bg_conn_mutex,
      MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_mutex_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&conn->bg_job_stack_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_bg_job_stack, &conn->bg_job_stack_mutex,
      MY_MUTEX_INIT_FAST))
#endif
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
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&conn->bg_conn_sync_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_conn_sync,
      &conn->bg_conn_sync_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&conn->bg_conn_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_conn,
      &conn->bg_conn_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
    pthread_mutex_lock(&conn->bg_conn_mutex);
#if MYSQL_VERSION_ID < 50500
    if (pthread_create(&conn->bg_thread, &spider_pt_attr,
      spider_bg_conn_action, (void *) conn)
    )
#else
    if (mysql_thread_create(spd_key_thd_bg, &conn->bg_thread,
      &spider_pt_attr, spider_bg_conn_action, (void *) conn)
    )
#endif
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
#ifndef WITHOUT_SPIDER_BG_SEARCH
    if (conn && result_list->bgs_working)
      spider_bg_conn_wait(conn);
#endif
  }
  DBUG_VOID_RETURN;
}

int spider_bg_all_conn_pre_next(
  ha_spider *spider,
  int link_idx
) {
#ifndef WITHOUT_SPIDER_BG_SEARCH
  int roop_start, roop_end, roop_count, lock_mode, link_ok, error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
#endif
  DBUG_ENTER("spider_bg_all_conn_pre_next");
#ifndef WITHOUT_SPIDER_BG_SEARCH
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
#endif
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
#ifndef WITHOUT_SPIDER_BG_SEARCH
    if (conn && result_list->bgs_working)
      spider_bg_conn_break(conn, spider);
#endif
    if (spider->quick_targets[roop_count])
    {
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (spider->conn_kind[link_idx] == SPIDER_CONN_KIND_MYSQL)
  {
#endif
    conn = spider->conns[link_idx];
    with_lock = (spider_conn_lock_mode(spider) != SPIDER_LOCK_MODE_NO_LOCK);
    first_conn = spider->conns[first_link_idx];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  } else if (spider->conn_kind[link_idx] == SPIDER_CONN_KIND_HS_READ)
    conn = spider->hs_r_conns[link_idx];
  else
    conn = spider->hs_w_conns[link_idx];
#endif
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
    if (!result_list->finish_flg)
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
          if (
            result_list->quick_mode == 0 ||
            !result_list->bgs_current->result
          ) {
            result_list->split_read =
              result_list->bgs_second_read > 0 ?
              result_list->bgs_second_read :
              result_list->bgs_split_read;
            result_list->limit_num =
              result_list->internal_limit - result_list->record_num >=
              result_list->split_read ?
              result_list->split_read :
              result_list->internal_limit - result_list->record_num;
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
        }
        result_list->bgs_working = TRUE;
        conn->bg_search = TRUE;
        if (with_lock)
          conn->bg_conn_chain_mutex_ptr = &first_conn->bg_conn_chain_mutex;
        conn->bg_caller_sync_wait = TRUE;
        conn->bg_target = spider;
        conn->link_idx = link_idx;
        conn->bg_discard_result = discard_result;
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
      }
    } else {
      DBUG_PRINT("info",("spider bg current->finish_flg=%s",
        result_list->current ?
        (result_list->current->finish_flg ? "TRUE" : "FALSE") : "NULL"));
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
        conn->bg_target = spider;
        conn->link_idx = link_idx;
        conn->bg_discard_result = discard_result;
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
  if (!(thd = new THD()))
  {
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_cond_signal(&conn->bg_conn_sync_cond);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  pthread_mutex_lock(&LOCK_thread_count);
  thd->thread_id = (*spd_db_att_thread_id)++;
  pthread_mutex_unlock(&LOCK_thread_count);
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
    my_pthread_setspecific_ptr(THR_THD, NULL);
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
      my_pthread_setspecific_ptr(THR_THD, NULL);
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        if (conn->conn_kind == SPIDER_CONN_KIND_MYSQL)
        {
#endif
          if (spider->sql_kind[conn->link_idx] == SPIDER_SQL_KIND_SQL)
          {
            sql_type = SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_TMP_SQL;
          } else {
            sql_type = SPIDER_SQL_TYPE_HANDLER;
          }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        } else {
          sql_type = SPIDER_SQL_TYPE_SELECT_HS;
        }
#endif
        if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if ((error_num = dbton_handler->set_sql_for_exec(sql_type,
          conn->link_idx)))
        {
          result_list->bgs_error = error_num;
          if ((result_list->bgs_error_with_message = thd->is_error()))
            strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
        }
        if (!dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        sql_type &= ~SPIDER_SQL_TYPE_TMP_SQL;
        DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
#ifdef HA_CAN_BULK_ACCESS
        if (spider->is_bulk_access_clone)
        {
          spider->connection_ids[conn->link_idx] = conn->connection_id;
          spider_trx_add_bulk_access_conn(spider->trx, conn);
        }
#endif
        if (!result_list->bgs_error)
        {
          conn->need_mon = &spider->need_mons[conn->link_idx];
          conn->mta_conn_mutex_lock_already = TRUE;
          conn->mta_conn_mutex_unlock_later = TRUE;
#ifdef HA_CAN_BULK_ACCESS
          if (!spider->is_bulk_access_clone)
          {
#endif
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
                  spider->trx->thd, share);
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
                  spider->trx->thd, share);
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
#ifdef HA_CAN_BULK_ACCESS
          }
#endif
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
        conn->mta_conn_mutex_unlock_later = TRUE;
        result_list->bgs_error =
          spider_db_store_result(spider, conn->link_idx, result_list->table);
        if ((result_list->bgs_error_with_message = thd->is_error()))
          strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
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
      *conn->bg_error_num = spider_db_query_with_set_names(
        conn->bg_sql_type,
        spider,
        conn,
        conn->link_idx
      );
      conn->bg_exec_sql = FALSE;
      continue;
    }
    if (conn->bg_simple_action)
    {
      switch (conn->bg_simple_action)
      {
        case SPIDER_BG_SIMPLE_CONNECT:
          conn->db_conn->bg_connect();
          break;
        case SPIDER_BG_SIMPLE_DISCONNECT:
          conn->db_conn->bg_disconnect();
          break;
        case SPIDER_BG_SIMPLE_RECORDS:
          DBUG_PRINT("info",("spider bg simple records"));
          spider = (ha_spider*) conn->bg_target;
          *conn->bg_error_num =
            spider->dbton_handler[conn->dbton_id]->
              show_records(conn->link_idx);
          break;
        default:
          break;
      }
      conn->bg_simple_action = SPIDER_BG_SIMPLE_NO_ACTION;
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
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&share->bg_sts_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_sts,
      &share->bg_sts_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&share->bg_sts_sync_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_sts_sync,
      &share->bg_sts_sync_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_create(&share->bg_sts_thread, &spider_pt_attr,
      spider_bg_sts_action, (void *) share)
    )
#else
    if (mysql_thread_create(spd_key_thd_bg_sts, &share->bg_sts_thread,
      &spider_pt_attr, spider_bg_sts_action, (void *) share)
    )
#endif
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
#ifdef _MSC_VER
  int *need_mons;
  SPIDER_CONN **conns;
  uint *conn_link_idx;
  uchar *conn_can_fo;
  char **conn_keys;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char **hs_r_conn_keys;
  char **hs_w_conn_keys;
#endif
  spider_db_handler **dbton_hdl;
#else
  int need_mons[share->link_count];
  SPIDER_CONN *conns[share->link_count];
  uint conn_link_idx[share->link_count];
  uchar conn_can_fo[share->link_bitmap_size];
  char *conn_keys[share->link_count];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char *hs_r_conn_keys[share->link_count];
  char *hs_w_conn_keys[share->link_count];
#endif
  spider_db_handler *dbton_hdl[SPIDER_DBTON_SIZE];
#endif
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_sts_action");
  /* init start */
#ifdef _MSC_VER
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (!(need_mons = (int *)
    spider_bulk_malloc(spider_current_trx, 21, MYF(MY_WME),
      &need_mons, sizeof(int) * share->link_count,
      &conns, sizeof(SPIDER_CONN *) * share->link_count,
      &conn_link_idx, sizeof(uint) * share->link_count,
      &conn_can_fo, sizeof(uchar) * share->link_bitmap_size,
      &conn_keys, sizeof(char *) * share->link_count,
      &hs_r_conn_keys, sizeof(char *) * share->link_count,
      &hs_w_conn_keys, sizeof(char *) * share->link_count,
      &dbton_hdl, sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE,
      NullS))
  )
#else
  if (!(need_mons = (int *)
    spider_bulk_malloc(spider_current_trx, 21, MYF(MY_WME),
      &need_mons, sizeof(int) * share->link_count,
      &conns, sizeof(SPIDER_CONN *) * share->link_count,
      &conn_link_idx, sizeof(uint) * share->link_count,
      &conn_can_fo, sizeof(uchar) * share->link_bitmap_size,
      &conn_keys, sizeof(char *) * share->link_count,
      &dbton_hdl, sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE,
      NullS))
  )
#endif
  {
    pthread_mutex_lock(&share->sts_mutex);
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
#endif
  pthread_mutex_lock(&share->sts_mutex);
  if (!(thd = new THD()))
  {
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
    my_thread_end();
#ifdef _MSC_VER
    spider_free(NULL, need_mons, MYF(MY_WME));
#endif
    DBUG_RETURN(NULL);
  }
  pthread_mutex_lock(&LOCK_thread_count);
  thd->thread_id = (*spd_db_att_thread_id)++;
  pthread_mutex_unlock(&LOCK_thread_count);
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
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
#ifdef _MSC_VER
    spider_free(NULL, need_mons, MYF(MY_WME));
#endif
    DBUG_RETURN(NULL);
  }
  share->bg_sts_thd = thd;
/*
  spider.trx = spider_global_trx;
*/
  spider.trx = trx;
  spider.share = share;
  spider.conns = conns;
  spider.conn_link_idx = conn_link_idx;
  spider.conn_can_fo = conn_can_fo;
  spider.need_mons = need_mons;
  spider.conn_keys_first_ptr = share->conn_keys[0];
  spider.conn_keys = conn_keys;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider.hs_r_conn_keys = hs_r_conn_keys;
  spider.hs_w_conn_keys = hs_w_conn_keys;
#endif
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
      if ((dbton_hdl[roop_count] = spider_dbton[roop_count].create_db_handler(
        &spider, share->dbton_share[roop_count])))
        break;
      if (dbton_hdl[roop_count]->init())
        break;
    }
  }
  if (roop_count == SPIDER_DBTON_SIZE)
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
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
#ifdef _MSC_VER
    spider_free(NULL, need_mons, MYF(MY_WME));
#endif
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
      my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
      my_thread_end();
#ifdef _MSC_VER
      spider_free(NULL, need_mons, MYF(MY_WME));
#endif
      DBUG_RETURN(NULL);
    }
    if (spider.search_link_idx == -1)
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
          pthread_mutex_lock(&spider_global_trx_mutex);
          spider_get_conn(share, spider.search_link_idx,
            share->conn_keys[spider.search_link_idx],
            spider_global_trx, &spider, FALSE, FALSE, SPIDER_CONN_KIND_MYSQL,
            &error_num);
          conns[spider.search_link_idx]->error_mode = 0;
          pthread_mutex_unlock(&spider_global_trx_mutex);
/*
          if (
            error_num &&
            share->monitoring_kind[spider.search_link_idx] &&
            need_mons[spider.search_link_idx]
          ) {
            lex_start(thd);
            error_num = spider_ping_table_mon_from_table(
                spider_global_trx,
                thd,
                share,
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
          DBUG_ASSERT(!conns[spider.search_link_idx]->thd);
#ifdef WITH_PARTITION_STORAGE_ENGINE
          if (spider_get_sts(share, spider.search_link_idx,
            share->bg_sts_try_time, &spider,
            share->bg_sts_interval, share->bg_sts_mode,
            share->bg_sts_sync,
            2, HA_STATUS_CONST | HA_STATUS_VARIABLE))
#else
          if (spider_get_sts(share, spider.search_link_idx,
            share->bg_sts_try_time, &spider,
            share->bg_sts_interval, share->bg_sts_mode,
            2, HA_STATUS_CONST | HA_STATUS_VARIABLE))
#endif
          {
/*
            if (
              share->monitoring_kind[spider.search_link_idx] &&
              need_mons[spider.search_link_idx]
            ) {
              lex_start(thd);
              error_num = spider_ping_table_mon_from_table(
                  spider_global_trx,
                  thd,
                  share,
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
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&share->bg_crd_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_crd,
      &share->bg_crd_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&share->bg_crd_sync_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_crd_sync,
      &share->bg_crd_sync_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_create(&share->bg_crd_thread, &spider_pt_attr,
      spider_bg_crd_action, (void *) share)
    )
#else
    if (mysql_thread_create(spd_key_thd_bg_crd, &share->bg_crd_thread,
      &spider_pt_attr, spider_bg_crd_action, (void *) share)
    )
#endif
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
  TABLE table;
#ifdef _MSC_VER
  int *need_mons;
  SPIDER_CONN **conns;
  uint *conn_link_idx;
  uchar *conn_can_fo;
  char **conn_keys;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char **hs_r_conn_keys;
  char **hs_w_conn_keys;
#endif
  spider_db_handler **dbton_hdl;
#else
  int need_mons[share->link_count];
  SPIDER_CONN *conns[share->link_count];
  uint conn_link_idx[share->link_count];
  uchar conn_can_fo[share->link_bitmap_size];
  char *conn_keys[share->link_count];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char *hs_r_conn_keys[share->link_count];
  char *hs_w_conn_keys[share->link_count];
#endif
  spider_db_handler *dbton_hdl[SPIDER_DBTON_SIZE];
#endif
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_crd_action");
  /* init start */
#ifdef _MSC_VER
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (!(need_mons = (int *)
    spider_bulk_malloc(spider_current_trx, 22, MYF(MY_WME),
      &need_mons, sizeof(int) * share->link_count,
      &conns, sizeof(SPIDER_CONN *) * share->link_count,
      &conn_link_idx, sizeof(uint) * share->link_count,
      &conn_can_fo, sizeof(uchar) * share->link_bitmap_size,
      &conn_keys, sizeof(char *) * share->link_count,
      &hs_r_conn_keys, sizeof(char *) * share->link_count,
      &hs_w_conn_keys, sizeof(char *) * share->link_count,
      &dbton_hdl, sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE,
      NullS))
  )
#else
  if (!(need_mons = (int *)
    spider_bulk_malloc(spider_current_trx, 22, MYF(MY_WME),
      &need_mons, sizeof(int) * share->link_count,
      &conns, sizeof(SPIDER_CONN *) * share->link_count,
      &conn_link_idx, sizeof(uint) * share->link_count,
      &conn_can_fo, sizeof(uchar) * share->link_bitmap_size,
      &conn_keys, sizeof(char *) * share->link_count,
      &dbton_hdl, sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE,
      NullS))
  )
#endif
  {
    pthread_mutex_lock(&share->crd_mutex);
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
#endif
  pthread_mutex_lock(&share->crd_mutex);
  if (!(thd = new THD()))
  {
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
    my_thread_end();
#ifdef _MSC_VER
    spider_free(NULL, need_mons, MYF(MY_WME));
#endif
    DBUG_RETURN(NULL);
  }
  pthread_mutex_lock(&LOCK_thread_count);
  thd->thread_id = (*spd_db_att_thread_id)++;
  pthread_mutex_unlock(&LOCK_thread_count);
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
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
#ifdef _MSC_VER
    spider_free(NULL, need_mons, MYF(MY_WME));
#endif
    DBUG_RETURN(NULL);
  }
  share->bg_crd_thd = thd;
  table.s = share->table_share;
  table.field = share->table_share->field;
  table.key_info = share->table_share->key_info;
/*
  spider.trx = spider_global_trx;
*/
  spider.trx = trx;
  spider.change_table_ptr(&table, share->table_share);
  spider.share = share;
  spider.conns = conns;
  spider.conn_link_idx = conn_link_idx;
  spider.conn_can_fo = conn_can_fo;
  spider.need_mons = need_mons;
  spider.conn_keys_first_ptr = share->conn_keys[0];
  spider.conn_keys = conn_keys;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider.hs_r_conn_keys = hs_r_conn_keys;
  spider.hs_w_conn_keys = hs_w_conn_keys;
#endif
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
      if ((dbton_hdl[roop_count] = spider_dbton[roop_count].create_db_handler(
        &spider, share->dbton_share[roop_count])))
        break;
      if (dbton_hdl[roop_count]->init())
        break;
    }
  }
  if (roop_count == SPIDER_DBTON_SIZE)
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
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
#ifdef _MSC_VER
    spider_free(NULL, need_mons, MYF(MY_WME));
#endif
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
      my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
      my_thread_end();
#ifdef _MSC_VER
      spider_free(NULL, need_mons, MYF(MY_WME));
#endif
      DBUG_RETURN(NULL);
    }
    if (spider.search_link_idx == -1)
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
          pthread_mutex_lock(&spider_global_trx_mutex);
          spider_get_conn(share, spider.search_link_idx,
            share->conn_keys[spider.search_link_idx],
            spider_global_trx, &spider, FALSE, FALSE, SPIDER_CONN_KIND_MYSQL,
            &error_num);
          conns[spider.search_link_idx]->error_mode = 0;
          pthread_mutex_unlock(&spider_global_trx_mutex);
/*
          if (
            error_num &&
            share->monitoring_kind[spider.search_link_idx] &&
            need_mons[spider.search_link_idx]
          ) {
            lex_start(thd);
            error_num = spider_ping_table_mon_from_table(
                spider_global_trx,
                thd,
                share,
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
          DBUG_ASSERT(!conns[spider.search_link_idx]->thd);
#ifdef WITH_PARTITION_STORAGE_ENGINE
          if (spider_get_crd(share, spider.search_link_idx,
            share->bg_crd_try_time, &spider, &table,
            share->bg_crd_interval, share->bg_crd_mode,
            share->bg_crd_sync,
            2))
#else
          if (spider_get_crd(share, spider.search_link_idx,
            share->bg_crd_try_time, &spider, &table,
            share->bg_crd_interval, share->bg_crd_mode,
            2))
#endif
          {
/*
            if (
              share->monitoring_kind[spider.search_link_idx] &&
              need_mons[spider.search_link_idx]
            ) {
              lex_start(thd);
              error_num = spider_ping_table_mon_from_table(
                  spider_global_trx,
                  thd,
                  share,
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
#ifdef _MSC_VER
      spider_string conv_name_str(share->table_name_length +
        SPIDER_SQL_INT_LEN + 1);
      conv_name_str.set_charset(system_charset_info);
#else
      char buf[share->table_name_length + SPIDER_SQL_INT_LEN + 1];
      spider_string conv_name_str(buf, share->table_name_length +
        SPIDER_SQL_INT_LEN + 1, system_charset_info);
#endif
      conv_name_str.init_calc_mem(105);
      conv_name_str.length(0);
      conv_name_str.q_append(share->table_name, share->table_name_length);
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (share->monitoring_bg_kind[roop_count])
        {
          conv_name_str.length(share->table_name_length);
          link_idx_str_length = my_sprintf(link_idx_str, (link_idx_str,
            "%010d", roop_count));
          conv_name_str.q_append(link_idx_str, link_idx_str_length + 1);
          conv_name_str.length(conv_name_str.length() - 1);
          if (!(table_mon_list = spider_get_ping_table_mon_list(trx, trx->thd,
            &conv_name_str, share->table_name_length, roop_count,
            (uint32) share->monitoring_sid[roop_count], FALSE, &error_num)))
            goto error_get_ping_table_mon_list;
          spider_free_ping_table_mon_list(table_mon_list);
        }
      }
      if (!(share->bg_mon_thds = (THD **)
        spider_bulk_malloc(spider_current_trx, 23, MYF(MY_WME | MY_ZEROFILL),
          &share->bg_mon_thds, sizeof(THD *) * share->all_link_count,
          &share->bg_mon_threads, sizeof(pthread_t) * share->all_link_count,
          &share->bg_mon_mutexes, sizeof(pthread_mutex_t) *
            share->all_link_count,
          &share->bg_mon_conds, sizeof(pthread_cond_t) * share->all_link_count,
          &share->bg_mon_sleep_conds,
            sizeof(pthread_cond_t) * share->all_link_count,
          NullS))
      ) {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error_alloc_base;
      }
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (
          share->monitoring_bg_kind[roop_count] &&
#if MYSQL_VERSION_ID < 50500
          pthread_mutex_init(&share->bg_mon_mutexes[roop_count],
            MY_MUTEX_INIT_FAST)
#else
          mysql_mutex_init(spd_key_mutex_bg_mon,
            &share->bg_mon_mutexes[roop_count], MY_MUTEX_INIT_FAST)
#endif
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_mutex_init;
        }
      }
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (
          share->monitoring_bg_kind[roop_count] &&
#if MYSQL_VERSION_ID < 50500
          pthread_cond_init(&share->bg_mon_conds[roop_count], NULL)
#else
          mysql_cond_init(spd_key_cond_bg_mon,
            &share->bg_mon_conds[roop_count], NULL)
#endif
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_cond_init;
        }
      }
      for (roop_count = 0; roop_count < (int) share->all_link_count;
        roop_count++)
      {
        if (
          share->monitoring_bg_kind[roop_count] &&
#if MYSQL_VERSION_ID < 50500
          pthread_cond_init(&share->bg_mon_sleep_conds[roop_count], NULL)
#else
          mysql_cond_init(spd_key_cond_bg_mon_sleep,
            &share->bg_mon_sleep_conds[roop_count], NULL)
#endif
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
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
#if MYSQL_VERSION_ID < 50500
          if (pthread_create(&share->bg_mon_threads[roop_count],
            &spider_pt_attr, spider_bg_mon_action, (void *) &link_pack)
          )
#else
          if (mysql_thread_create(spd_key_thd_bg_mon,
            &share->bg_mon_threads[roop_count], &spider_pt_attr,
            spider_bg_mon_action, (void *) &link_pack)
          )
#endif
          {
            error_num = HA_ERR_OUT_OF_MEM;
            goto error_thread_create;
          }
          pthread_cond_wait(&share->bg_mon_conds[roop_count],
            &share->bg_mon_mutexes[roop_count]);
          pthread_mutex_unlock(&share->bg_mon_mutexes[roop_count]);
        }
      }
      share->bg_mon_init = TRUE;
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
  if (!(thd = new THD()))
  {
    share->bg_mon_kill = FALSE;
    share->bg_mon_init = FALSE;
    pthread_cond_signal(&share->bg_mon_conds[link_idx]);
    pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  pthread_mutex_lock(&LOCK_thread_count);
  thd->thread_id = (*spd_db_att_thread_id)++;
  pthread_mutex_unlock(&LOCK_thread_count);
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
    my_pthread_setspecific_ptr(THR_THD, NULL);
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
      my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
      my_thread_end();
      DBUG_RETURN(NULL);
    }
    if (share->monitoring_bg_kind[link_idx])
    {
      lex_start(thd);
      error_num = spider_ping_table_mon_from_table(
        spider_global_trx,
        thd,
        share,
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
#endif

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
#ifdef _MSC_VER
  int *link_idxs, link_idx;
  long *balances;
#else
  int link_idxs[link_count];
  long balances[link_count];
#endif
  DBUG_ENTER("spider_conn_first_link_idx");
#ifdef _MSC_VER
  if (!(link_idxs = (int *)
    spider_bulk_malloc(spider_current_trx, 24, MYF(MY_WME),
      &link_idxs, sizeof(int) * link_count,
      &balances, sizeof(long) * link_count,
      NullS))
  ) {
    DBUG_PRINT("info",("spider out of memory"));
    DBUG_RETURN(-1);
  }
#endif
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
#ifdef _MSC_VER
    spider_free(spider_current_trx, link_idxs, MYF(MY_WME));
#endif
    DBUG_RETURN(-1);
  }
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
  DBUG_PRINT("info",("spider server_id=%lu", thd->variables.server_id));
#else
  DBUG_PRINT("info",("spider server_id=%u", thd->server_id));
#endif
  DBUG_PRINT("info",("spider thread_id=%lu", thd_get_thread_id(thd)));
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
  rand_val = spider_rand(thd->variables.server_id + thd_get_thread_id(thd));
#else
  rand_val = spider_rand(thd->server_id + thd_get_thread_id(thd));
#endif
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
#ifdef _MSC_VER
  link_idx = link_idxs[roop_count];
  spider_free(spider_current_trx, link_idxs, MYF(MY_WME));
  DBUG_RETURN(link_idx);
#else
  DBUG_RETURN(link_idxs[roop_count]);
#endif
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

int spider_conn_lock_mode(
  ha_spider *spider
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_conn_lock_mode");
  if (result_list->lock_type == F_WRLCK || spider->lock_mode == 2)
    DBUG_RETURN(SPIDER_LOCK_MODE_EXCLUSIVE);
  else if (spider->lock_mode == 1)
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
  THD *thd = spider->trx->thd;
  int use_handler = spider_param_use_handler(thd,
    spider->share->use_handlers[link_idx]);
  DBUG_ENTER("spider_conn_use_handler");
  DBUG_PRINT("info",("spider use_handler=%d", use_handler));
  DBUG_PRINT("info",("spider spider->conn_kind[link_idx]=%u",
    spider->conn_kind[link_idx]));
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (spider->conn_kind[link_idx] != SPIDER_CONN_KIND_MYSQL)
  {
    DBUG_PRINT("info",("spider TRUE by HS"));
    spider->sql_kinds |= SPIDER_SQL_KIND_HS;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_HS;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    if (
      spider->do_direct_update &&
      spider_bit_is_set(spider->do_hs_direct_update, link_idx)
    ) {
      DBUG_PRINT("info",("spider using HS direct_update"));
      spider->direct_update_kinds |= SPIDER_SQL_KIND_HS;
    }
#endif
    DBUG_RETURN(TRUE);
  }
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  if (spider->do_direct_update)
  {
    spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    if (spider_bit_is_set(spider->do_hs_direct_update, link_idx))
    {
      spider->direct_update_kinds |= SPIDER_SQL_KIND_HS;
      DBUG_PRINT("info",("spider TRUE by using HS direct_update"));
      DBUG_RETURN(TRUE);
    } else {
#endif
      spider->direct_update_kinds |= SPIDER_SQL_KIND_SQL;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    }
    if (spider->conn_kind[link_idx] == SPIDER_CONN_KIND_MYSQL)
    {
#endif
      DBUG_PRINT("info",("spider FALSE by using direct_update"));
      DBUG_RETURN(FALSE);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    } else {
      DBUG_PRINT("info",("spider TRUE by using BOTH"));
      DBUG_RETURN(TRUE);
    }
#endif
  }
#endif
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
    spider->sql_command == SQLCOM_HA_READ &&
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
    spider->sql_command != SQLCOM_HA_READ &&
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  SPIDER_CONN *conn;
#endif
  DBUG_ENTER("spider_conn_need_open_handler");
  DBUG_PRINT("info",("spider spider=%p", spider));
  if (spider->handler_opened(link_idx, spider->conn_kind[link_idx]))
  {
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    if (
      spider->do_direct_update &&
      spider_bit_is_set(spider->do_hs_direct_update, link_idx)
    ) {
      conn = spider->hs_w_conns[link_idx];
      if (
        !conn->server_lost &&
        conn->hs_pre_age == spider->hs_w_conn_ages[link_idx]
      ) {
        DBUG_PRINT("info",("spider hs_write is already opened"));
        DBUG_RETURN(FALSE);
      }
    } else
#endif
    if (spider->conn_kind[link_idx] == SPIDER_CONN_KIND_MYSQL)
    {
#endif
      DBUG_PRINT("info",("spider HA already opened"));
      DBUG_RETURN(FALSE);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    } else if (spider->conn_kind[link_idx] == SPIDER_CONN_KIND_HS_READ)
    {
      DBUG_PRINT("info",("spider r_handler_index[%d]=%d",
        link_idx, spider->r_handler_index[link_idx]));
      DBUG_PRINT("info",("spider idx=%d", idx));
      DBUG_PRINT("info",("spider hs_pushed_ret_fields_num=%zu",
        spider->hs_pushed_ret_fields_num));
      DBUG_PRINT("info",("spider hs_r_ret_fields_num[%d]=%lu",
        link_idx, spider->hs_r_ret_fields_num[link_idx]));
      DBUG_PRINT("info",("spider hs_r_ret_fields[%d]=%p",
        link_idx, spider->hs_r_ret_fields[link_idx]));
#ifndef DBUG_OFF
      if (
        spider->hs_pushed_ret_fields_num < MAX_FIELDS &&
        spider->hs_r_ret_fields[link_idx] &&
        spider->hs_pushed_ret_fields_num ==
          spider->hs_r_ret_fields_num[link_idx]
      ) {
        int roop_count;
        for (roop_count = 0; roop_count < (int) spider->hs_pushed_ret_fields_num;
          ++roop_count)
        {
          DBUG_PRINT("info",("spider hs_pushed_ret_fields[%d]=%u",
            roop_count, spider->hs_pushed_ret_fields[roop_count]));
          DBUG_PRINT("info",("spider hs_r_ret_fields[%d][%d]=%u",
            link_idx, roop_count,
            spider->hs_r_ret_fields[link_idx][roop_count]));
        }
      }
#endif
      if (
        spider->r_handler_index[link_idx] == idx
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
        && (
          (
            spider->hs_pushed_ret_fields_num == MAX_FIELDS &&
            spider->hs_r_ret_fields_num[link_idx] == MAX_FIELDS
          ) ||
          (
            spider->hs_pushed_ret_fields_num < MAX_FIELDS &&
            spider->hs_r_ret_fields[link_idx] &&
            spider->hs_pushed_ret_fields_num ==
              spider->hs_r_ret_fields_num[link_idx] &&
            !memcmp(spider->hs_pushed_ret_fields,
              spider->hs_r_ret_fields[link_idx],
              sizeof(uint32) * spider->hs_pushed_ret_fields_num)
          )
        )
#endif
      ) {
        SPIDER_CONN *conn = spider->hs_r_conns[link_idx];
        DBUG_PRINT("info",("spider conn=%p", conn));
        DBUG_PRINT("info",("spider conn->conn_id=%llu", conn->conn_id));
        DBUG_PRINT("info",("spider conn->connection_id=%llu",
          conn->connection_id));
        DBUG_PRINT("info",("spider conn->server_lost=%s",
          conn->server_lost ? "TRUE" : "FALSE"));
        DBUG_PRINT("info",("spider conn->hs_pre_age=%llu", conn->hs_pre_age));
        DBUG_PRINT("info",("spider hs_w_conn_ages[%d]=%llu",
          link_idx, spider->hs_w_conn_ages[link_idx]));
        if (
          !conn->server_lost &&
          conn->hs_pre_age == spider->hs_r_conn_ages[link_idx]
        ) {
          DBUG_PRINT("info",("spider hs_r same idx"));
          DBUG_RETURN(FALSE);
        }
      }
    } else if (spider->conn_kind[link_idx] == SPIDER_CONN_KIND_HS_WRITE)
    {
      DBUG_PRINT("info",("spider w_handler_index[%d]=%d",
        link_idx, spider->w_handler_index[link_idx]));
      DBUG_PRINT("info",("spider idx=%d", idx));
      DBUG_PRINT("info",("spider hs_pushed_ret_fields_num=%zu",
        spider->hs_pushed_ret_fields_num));
      DBUG_PRINT("info",("spider hs_w_ret_fields_num[%d]=%lu",
        link_idx, spider->hs_w_ret_fields_num[link_idx]));
      DBUG_PRINT("info",("spider hs_w_ret_fields[%d]=%p",
        link_idx, spider->hs_w_ret_fields[link_idx]));
#ifndef DBUG_OFF
      if (
        spider->hs_pushed_ret_fields_num < MAX_FIELDS &&
        spider->hs_w_ret_fields[link_idx] &&
        spider->hs_pushed_ret_fields_num ==
          spider->hs_w_ret_fields_num[link_idx]
      ) {
        int roop_count;
        for (roop_count = 0; roop_count < (int) spider->hs_pushed_ret_fields_num;
          ++roop_count)
        {
          DBUG_PRINT("info",("spider hs_pushed_ret_fields[%d]=%u",
            roop_count, spider->hs_pushed_ret_fields[roop_count]));
          DBUG_PRINT("info",("spider hs_w_ret_fields[%d][%d]=%u",
            link_idx, roop_count,
            spider->hs_w_ret_fields[link_idx][roop_count]));
        }
      }
#endif
      if (
        spider->w_handler_index[link_idx] == idx
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
        && (
          (
            spider->hs_pushed_ret_fields_num == MAX_FIELDS &&
            spider->hs_w_ret_fields_num[link_idx] == MAX_FIELDS
          ) ||
          (
            spider->hs_pushed_ret_fields_num < MAX_FIELDS &&
            spider->hs_w_ret_fields[link_idx] &&
            spider->hs_pushed_ret_fields_num ==
              spider->hs_w_ret_fields_num[link_idx] &&
            !memcmp(spider->hs_pushed_ret_fields,
              spider->hs_w_ret_fields[link_idx],
              sizeof(uint32) * spider->hs_pushed_ret_fields_num)
          )
        )
#endif
      ) {
        SPIDER_CONN *conn = spider->hs_w_conns[link_idx];
        DBUG_PRINT("info",("spider conn=%p", conn));
        DBUG_PRINT("info",("spider conn->conn_id=%llu", conn->conn_id));
        DBUG_PRINT("info",("spider conn->connection_id=%llu",
          conn->connection_id));
        DBUG_PRINT("info",("spider conn->server_lost=%s",
          conn->server_lost ? "TRUE" : "FALSE"));
        DBUG_PRINT("info",("spider conn->hs_pre_age=%llu", conn->hs_pre_age));
        DBUG_PRINT("info",("spider hs_w_conn_ages[%d]=%llu",
          link_idx, spider->hs_w_conn_ages[link_idx]));
        if (
          !conn->server_lost &&
          conn->hs_pre_age == spider->hs_w_conn_ages[link_idx]
        ) {
          DBUG_PRINT("info",("spider hs_w same idx"));
          DBUG_RETURN(FALSE);
        }
      }
    }
#endif
  }
  DBUG_RETURN(TRUE);
}
