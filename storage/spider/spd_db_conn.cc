/* Copyright (C) 2008-2019 Kentoku Shiba
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
#include "sql_analyse.h"
#include "sql_base.h"
#include "tztime.h"
#include "errmsg.h"
#include "sql_select.h"
#include "sql_common.h"
#include <errmsg.h>
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_sys_table.h"
#include "ha_spider.h"
#include "spd_db_conn.h"
#include "spd_table.h"
#include "spd_trx.h"
#include "spd_conn.h"
#include "spd_direct_sql.h"
#include "spd_ping_table.h"
#include "spd_copy_tables.h"
#include "spd_malloc.h"

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];

#define SPIDER_SQL_COALESCE_STR "coalesce("
#define SPIDER_SQL_COALESCE_LEN (sizeof(SPIDER_SQL_COALESCE_STR) - 1)
#define SPIDER_SQL_HEX_STR "0x"
#define SPIDER_SQL_HEX_LEN (sizeof(SPIDER_SQL_HEX_STR) - 1)
#define SPIDER_SQL_SQL_FORCE_IDX_STR " force index("
#define SPIDER_SQL_SQL_FORCE_IDX_LEN (sizeof(SPIDER_SQL_SQL_FORCE_IDX_STR) - 1)
#define SPIDER_SQL_SQL_USE_IDX_STR " use index("
#define SPIDER_SQL_SQL_USE_IDX_LEN (sizeof(SPIDER_SQL_SQL_USE_IDX_STR) - 1)
#define SPIDER_SQL_SQL_IGNORE_IDX_STR " ignore index("
#define SPIDER_SQL_SQL_IGNORE_IDX_LEN (sizeof(SPIDER_SQL_SQL_IGNORE_IDX_STR) - 1)

#define SPIDER_SQL_SET_NAMES_STR "set names "
#define SPIDER_SQL_SET_NAMES_LEN sizeof(SPIDER_SQL_SET_NAMES_STR) - 1

#define SPIDER_SQL_PING_TABLE_STR "spider_ping_table("
#define SPIDER_SQL_PING_TABLE_LEN (sizeof(SPIDER_SQL_PING_TABLE_STR) - 1)

extern HASH spider_open_connections;
pthread_mutex_t spider_open_conn_mutex;
const char spider_dig_upper[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

/* UTC time zone for timestamp columns */
Time_zone *UTC = 0;

int spider_db_connect(
  const SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num, connect_retry_count;
  THD* thd = current_thd;
  longlong connect_retry_interval;
  DBUG_ENTER("spider_db_connect");
  DBUG_ASSERT(conn->conn_kind != SPIDER_CONN_KIND_MYSQL || conn->need_mon);
  DBUG_PRINT("info",("spider link_idx=%d", link_idx));
  DBUG_PRINT("info",("spider conn=%p", conn));

  if (conn->connect_error)
  {
    time_t tmp_time = (time_t) time((time_t*) 0);
    DBUG_PRINT("info",("spider diff=%f",
      difftime(tmp_time, conn->connect_error_time)));
    if (
      (
        conn->thd &&
        conn->thd == conn->connect_error_thd &&
        conn->thd->query_id == conn->connect_error_query_id
      ) ||
      (
        difftime(tmp_time, conn->connect_error_time) <
          spider_param_connect_error_interval()
      )
    ) {
      DBUG_PRINT("info",("spider set same error"));
      if (conn->connect_error_with_message)
        my_message(conn->connect_error, conn->connect_error_msg, MYF(0));
      DBUG_RETURN(conn->connect_error);
    }
  }

  if (thd)
  {
    conn->connect_timeout = spider_param_connect_timeout(thd,
      share->connect_timeouts[link_idx]);
    conn->net_read_timeout = spider_param_net_read_timeout(thd,
      share->net_read_timeouts[link_idx]);
    conn->net_write_timeout = spider_param_net_write_timeout(thd,
      share->net_write_timeouts[link_idx]);
    connect_retry_interval = spider_param_connect_retry_interval(thd);
    if (conn->disable_connect_retry)
      connect_retry_count = 0;
    else
      connect_retry_count = spider_param_connect_retry_count(thd);
  } else {
    conn->connect_timeout = spider_param_connect_timeout(NULL,
      share->connect_timeouts[link_idx]);
    conn->net_read_timeout = spider_param_net_read_timeout(NULL,
      share->net_read_timeouts[link_idx]);
    conn->net_write_timeout = spider_param_net_write_timeout(NULL,
      share->net_write_timeouts[link_idx]);
    connect_retry_interval = spider_param_connect_retry_interval(NULL);
    connect_retry_count = spider_param_connect_retry_count(NULL);
  }
  DBUG_PRINT("info",("spider connect_timeout=%u", conn->connect_timeout));
  DBUG_PRINT("info",("spider net_read_timeout=%u", conn->net_read_timeout));
  DBUG_PRINT("info",("spider net_write_timeout=%u", conn->net_write_timeout));

    if ((error_num = spider_reset_conn_setted_parameter(conn, thd)))
      DBUG_RETURN(error_num);

  if (conn->dbton_id == SPIDER_DBTON_SIZE)
  {
      my_printf_error(
        ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM,
        ER_SPIDER_SQL_WRAPPER_IS_INVALID_STR,
        MYF(0), conn->tgt_wrapper);
      DBUG_RETURN(ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM);
  }

  if ((error_num = conn->db_conn->connect(
    share->tgt_hosts[link_idx],
    share->tgt_usernames[link_idx],
    share->tgt_passwords[link_idx],
    share->tgt_ports[link_idx],
    share->tgt_sockets[link_idx],
    share->server_names[link_idx],
    connect_retry_count, connect_retry_interval)))
  {
    if (conn->thd)
    {
      conn->connect_error_thd = conn->thd;
      conn->connect_error_query_id = conn->thd->query_id;
      conn->connect_error_time = (time_t) time((time_t*) 0);
      conn->connect_error = error_num;
      if ((conn->connect_error_with_message = thd->is_error()))
        strmov(conn->connect_error_msg, spider_stmt_da_message(thd));
    }
    DBUG_RETURN(error_num);
  }

  conn->connect_error = 0;
  conn->opened_handlers = 0;
  conn->db_conn->reset_opened_handler();
  ++conn->connection_id;

  /* Set the connection's time zone to UTC */
  spider_conn_queue_UTC_time_zone(conn);
  DBUG_RETURN(0);
}

int spider_db_ping_internal(
  SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int all_link_idx,
  int *need_mon
) {
  int error_num;
  DBUG_ENTER("spider_db_ping_internal");
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
  DBUG_ASSERT(conn->mta_conn_mutex_file_pos.file_name);
  if (conn->server_lost || conn->queued_connect)
  {
    if ((error_num = spider_db_connect(share, conn, all_link_idx)))
    {
      pthread_mutex_assert_owner(&conn->mta_conn_mutex);
      DBUG_RETURN(error_num);
    }
    conn->server_lost = FALSE;
    conn->queued_connect = FALSE;
  }
  if ((error_num = conn->db_conn->ping()))
  {
    spider_db_disconnect(conn);
    if ((error_num = spider_db_connect(share, conn, all_link_idx)))
    {
      DBUG_PRINT("info", ("spider conn=%p SERVER_LOST", conn));
      conn->server_lost = TRUE;
      pthread_mutex_assert_owner(&conn->mta_conn_mutex);
      DBUG_RETURN(error_num);
    }
    if((error_num = conn->db_conn->ping()))
    {
      spider_db_disconnect(conn);
      DBUG_PRINT("info", ("spider conn=%p SERVER_LOST", conn));
      conn->server_lost = TRUE;
      pthread_mutex_assert_owner(&conn->mta_conn_mutex);
      DBUG_RETURN(error_num);
    }
  }
  conn->ping_time = (time_t) time((time_t*) 0);
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_db_ping(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_db_ping");
#ifndef DBUG_OFF
  if (spider->wide_handler->trx->thd)
    DBUG_PRINT("info", ("spider thd->query_id is %lld",
      spider->wide_handler->trx->thd->query_id));
#endif
  DBUG_RETURN(spider_db_ping_internal(spider->share, conn,
    spider->conn_link_idx[link_idx], &spider->need_mons[link_idx]));
}

void spider_db_disconnect(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_db_disconnect");
  DBUG_PRINT("info",("spider conn=%p", conn));
  DBUG_PRINT("info",("spider conn->conn_kind=%u", conn->conn_kind));
  if (conn->db_conn->is_connected())
  {
    conn->db_conn->disconnect();
  }
  DBUG_VOID_RETURN;
}

int spider_db_conn_queue_action(
  SPIDER_CONN *conn
) {
  int error_num;
  char sql_buf[MAX_FIELD_WIDTH * 2];
  spider_string sql_str(sql_buf, sizeof(sql_buf), system_charset_info);
  DBUG_ENTER("spider_db_conn_queue_action");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  sql_str.init_calc_mem(106);
  sql_str.length(0);
  if (conn->queued_connect)
  {
    if ((error_num = spider_db_connect(conn->queued_connect_share, conn,
      conn->queued_connect_link_idx)))
    {
      DBUG_PRINT("info", ("spider conn=%p SERVER_LOST", conn));
      conn->server_lost = TRUE;
      DBUG_RETURN(error_num);
    }
    conn->server_lost = FALSE;
    conn->queued_connect = FALSE;
  }

  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
    if (conn->queued_ping)
    {
      if ((error_num = spider_db_ping(conn->queued_ping_spider, conn,
        conn->queued_ping_link_idx)))
        DBUG_RETURN(error_num);
      conn->queued_ping = FALSE;
    }

    if (conn->server_lost)
    {
      DBUG_PRINT("info", ("spider no reconnect queue"));
      DBUG_RETURN(CR_SERVER_GONE_ERROR);
    }

    if (conn->queued_net_timeout)
    {
      conn->db_conn->set_net_timeout();
      conn->queued_net_timeout = FALSE;
    }
    if (
      (
        conn->queued_trx_isolation &&
        !conn->queued_semi_trx_isolation &&
        conn->queued_trx_isolation_val != conn->trx_isolation &&
        conn->db_conn->set_trx_isolation_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_trx_isolation(&sql_str, conn->queued_trx_isolation_val))
      ) ||
      (
        conn->queued_semi_trx_isolation &&
        conn->queued_semi_trx_isolation_val != conn->trx_isolation &&
        conn->db_conn->set_trx_isolation_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_trx_isolation(&sql_str, conn->queued_semi_trx_isolation_val))
      ) ||
      (
        conn->queued_autocommit &&
        (
          (conn->queued_autocommit_val && conn->autocommit != 1) ||
          (!conn->queued_autocommit_val && conn->autocommit != 0)
        ) &&
        conn->db_conn->set_autocommit_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_autocommit(&sql_str, conn->queued_autocommit_val))
      ) ||
      (
        conn->queued_sql_log_off &&
        (
          (conn->queued_sql_log_off_val && conn->sql_log_off != 1) ||
          (!conn->queued_sql_log_off_val && conn->sql_log_off != 0)
        ) &&
        conn->db_conn->set_sql_log_off_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_sql_log_off(&sql_str, conn->queued_sql_log_off_val))
      ) ||
      (
        conn->queued_wait_timeout &&
        conn->queued_wait_timeout_val != conn->wait_timeout &&
        conn->db_conn->set_wait_timeout_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_wait_timeout(&sql_str, conn->queued_wait_timeout_val))
      ) ||
      (
        conn->queued_sql_mode &&
        conn->queued_sql_mode_val != conn->sql_mode &&
        conn->db_conn->set_sql_mode_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_sql_mode(&sql_str, conn->queued_sql_mode_val))
      ) ||
      (
        conn->queued_time_zone &&
        conn->queued_time_zone_val != conn->time_zone &&
        conn->db_conn->set_time_zone_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_time_zone(&sql_str, conn->queued_time_zone_val))
      ) ||
      (
        conn->loop_check_queue.records &&
        conn->db_conn->set_loop_check_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_loop_check(&sql_str, conn))
      ) ||
      (
        conn->queued_trx_start &&
        conn->db_conn->trx_start_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_start_transaction(&sql_str))
      ) ||
      (
        conn->queued_xa_start &&
        conn->db_conn->xa_start_in_bulk_sql() &&
        (error_num = spider_dbton[conn->dbton_id].db_util->
          append_xa_start(&sql_str, conn->queued_xa_start_xid))
      )
    )
      DBUG_RETURN(error_num);
    if (sql_str.length())
    {
      if ((error_num = conn->db_conn->exec_query(sql_str.ptr(),
        sql_str.length(), -1)))
        DBUG_RETURN(error_num);
      spider_db_result *result;
      do {
        st_spider_db_request_key request_key;
        request_key.spider_thread_id = 1;
        request_key.query_id = 1;
        request_key.handler = NULL;
        request_key.request_id = 1;
        request_key.next = NULL;
        if ((result = conn->db_conn->store_result(NULL, &request_key,
          &error_num)))
        {
          result->free_result();
          delete result;
        } else if ((error_num = conn->db_conn->get_errno()))
        {
          break;
        }
      } while (!(error_num = conn->db_conn->next_result()));
      if (error_num > 0)
        DBUG_RETURN(error_num);
    }

    if (
      conn->queued_autocommit &&
      (
        (conn->queued_autocommit_val && conn->autocommit != 1) ||
        (!conn->queued_autocommit_val && conn->autocommit != 0)
      ) &&
      !conn->db_conn->set_autocommit_in_bulk_sql() &&
      (error_num = conn->db_conn->
        set_autocommit(conn->queued_autocommit_val, (int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->queued_sql_log_off &&
      (
        (conn->queued_sql_log_off_val && conn->sql_log_off != 1) ||
        (!conn->queued_sql_log_off_val && conn->sql_log_off != 0)
      ) &&
      !conn->db_conn->set_sql_log_off_in_bulk_sql() &&
      (error_num = conn->db_conn->
        set_sql_log_off(conn->queued_sql_log_off_val, (int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->queued_wait_timeout &&
      conn->queued_wait_timeout_val != conn->wait_timeout &&
      !conn->db_conn->set_wait_timeout_in_bulk_sql() &&
      (error_num = conn->db_conn->
        set_wait_timeout(conn->queued_wait_timeout_val,
        (int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->queued_sql_mode &&
      conn->queued_sql_mode_val != conn->sql_mode &&
      !conn->db_conn->set_sql_mode_in_bulk_sql() &&
      (error_num = conn->db_conn->
        set_sql_mode(conn->queued_sql_mode_val,
        (int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->queued_time_zone &&
      conn->queued_time_zone_val != conn->time_zone &&
      !conn->db_conn->set_time_zone_in_bulk_sql() &&
      (error_num = conn->db_conn->
        set_time_zone(conn->queued_time_zone_val,
        (int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->loop_check_queue.records &&
      !conn->db_conn->set_loop_check_in_bulk_sql() &&
      (error_num = conn->db_conn->set_loop_check((int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->queued_trx_isolation &&
      !conn->queued_semi_trx_isolation &&
      conn->queued_trx_isolation_val != conn->trx_isolation &&
      !conn->db_conn->set_trx_isolation_in_bulk_sql() &&
      (error_num = conn->db_conn->set_trx_isolation(
        conn->queued_trx_isolation_val, (int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->queued_semi_trx_isolation &&
      conn->queued_semi_trx_isolation_val != conn->trx_isolation &&
      !conn->db_conn->set_trx_isolation_in_bulk_sql() &&
      (error_num = conn->db_conn->set_trx_isolation(
        conn->queued_semi_trx_isolation_val, (int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->queued_trx_start &&
      !conn->db_conn->trx_start_in_bulk_sql() &&
      (error_num = conn->db_conn->
        start_transaction((int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }
    if (
      conn->queued_xa_start &&
      !conn->db_conn->xa_start_in_bulk_sql() &&
      (error_num = conn->db_conn->
        xa_start(conn->queued_xa_start_xid, (int *) conn->need_mon))
    ) {
      DBUG_RETURN(error_num);
    }

    if (
      conn->queued_trx_isolation &&
      !conn->queued_semi_trx_isolation &&
      conn->queued_trx_isolation_val != conn->trx_isolation
    ) {
      conn->trx_isolation = conn->queued_trx_isolation_val;
      DBUG_PRINT("info", ("spider conn->trx_isolation=%d",
        conn->trx_isolation));
    }

    if (
      conn->queued_semi_trx_isolation &&
      conn->queued_semi_trx_isolation_val != conn->trx_isolation
    ) {
      conn->semi_trx_isolation = conn->queued_semi_trx_isolation_val;
      DBUG_PRINT("info", ("spider conn->semi_trx_isolation=%d",
        conn->semi_trx_isolation));
      conn->trx_isolation = thd_tx_isolation(conn->thd);
      DBUG_PRINT("info", ("spider conn->trx_isolation=%d",
        conn->trx_isolation));
    }

    if (
      conn->queued_wait_timeout &&
      conn->queued_wait_timeout_val != conn->wait_timeout
    ) {
      conn->wait_timeout = conn->queued_wait_timeout_val;
    }

    if (
      conn->queued_sql_mode &&
      conn->queued_sql_mode_val != conn->sql_mode
    ) {
      conn->sql_mode = conn->queued_sql_mode_val;
    }

    if (conn->queued_autocommit)
    {
      if (conn->queued_autocommit_val && conn->autocommit != 1)
      {
        conn->autocommit = 1;
      } else if (!conn->queued_autocommit_val && conn->autocommit != 0)
      {
        conn->autocommit = 0;
      }
      DBUG_PRINT("info", ("spider conn->autocommit=%d",
        conn->autocommit));
    }

    if (conn->queued_sql_log_off)
    {
      if (conn->queued_sql_log_off_val && conn->sql_log_off != 1)
      {
        conn->sql_log_off = 1;
      } else if (!conn->queued_sql_log_off_val && conn->sql_log_off != 0)
      {
        conn->sql_log_off = 0;
      }
      DBUG_PRINT("info", ("spider conn->sql_log_off=%d",
        conn->sql_log_off));
    }

    if (
      conn->queued_time_zone &&
      conn->queued_time_zone_val != conn->time_zone
    ) {
      conn->time_zone = conn->queued_time_zone_val;
      DBUG_PRINT("info", ("spider conn->time_zone=%p",
        conn->time_zone));
    }

    if (conn->loop_check_queue.records)
    {
      conn->db_conn->fin_loop_check();
    }
    spider_conn_clear_queue(conn);
  DBUG_RETURN(0);
}

int spider_db_before_query(
  SPIDER_CONN *conn,
  int *need_mon
) {
  int error_num;
  DBUG_ENTER("spider_db_before_query");
  DBUG_ASSERT(need_mon);
  if (conn->bg_search)
    spider_bg_conn_break(conn, NULL);
  conn->in_before_query = TRUE;
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
  DBUG_ASSERT(conn->mta_conn_mutex_file_pos.file_name);
  if ((error_num = spider_db_conn_queue_action(conn)))
  {
    conn->in_before_query = FALSE;
    pthread_mutex_assert_owner(&conn->mta_conn_mutex);
    DBUG_RETURN(error_num);
  }
  if (conn->server_lost)
  {
    conn->in_before_query = FALSE;
    pthread_mutex_assert_owner(&conn->mta_conn_mutex);
    DBUG_RETURN(CR_SERVER_GONE_ERROR);
  }
  DBUG_PRINT("info", ("spider conn[%p]->quick_target=%p",
    conn, conn->quick_target));
  if (conn->quick_target)
  {
    bool tmp_mta_conn_mutex_unlock_later;
    ha_spider *spider = (ha_spider*) conn->quick_target;
    SPIDER_RESULT_LIST *result_list = &spider->result_list;
    DBUG_PRINT("info", ("spider result_list->quick_mode=%d",
      result_list->quick_mode));
    if (result_list->quick_mode == 2)
    {
      result_list->quick_phase = 1;
      spider->connection_ids[conn->link_idx] = conn->connection_id;
      tmp_mta_conn_mutex_unlock_later = conn->mta_conn_mutex_unlock_later;
      conn->mta_conn_mutex_unlock_later = TRUE;
      while (conn->quick_target)
      {
        if (
          (error_num = spider_db_store_result(spider, conn->link_idx,
            result_list->table)) &&
          error_num != HA_ERR_END_OF_FILE
        ) {
          conn->mta_conn_mutex_unlock_later = tmp_mta_conn_mutex_unlock_later;
          conn->in_before_query = FALSE;
          pthread_mutex_assert_owner(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
      }
      conn->mta_conn_mutex_unlock_later = tmp_mta_conn_mutex_unlock_later;
      result_list->quick_phase = 2;
    } else {
      result_list->bgs_current->result->free_result();
      delete result_list->bgs_current->result;
      result_list->bgs_current->result = NULL;
      DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
      conn->quick_target = NULL;
      spider->quick_targets[conn->link_idx] = NULL;
    }
  }
  conn->in_before_query = FALSE;
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
  DBUG_RETURN(0);
}

int spider_db_query(
  SPIDER_CONN *conn,
  const char *query,
  uint length,
  int quick_mode,
  int *need_mon
) {
  int error_num;
  DBUG_ENTER("spider_db_query");
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
    DBUG_PRINT("info", ("spider conn->db_conn %p", conn->db_conn));
    if (
      !conn->in_before_query &&
      (error_num = spider_db_before_query(conn, need_mon))
    )
      DBUG_RETURN(error_num);
#ifndef DBUG_OFF
    spider_string tmp_query_str(sizeof(char) * (length + 1));
    tmp_query_str.init_calc_mem(107);
    char *tmp_query = (char *) tmp_query_str.c_ptr_safe();
    memcpy(tmp_query, query, length);
    tmp_query[length] = '\0';
    query = (const char *) tmp_query;
    DBUG_PRINT("info", ("spider query=%s", query));
    DBUG_PRINT("info", ("spider length=%u", length));
#endif
    if ((error_num = conn->db_conn->exec_query(query, length, quick_mode)))
      DBUG_RETURN(error_num);
    DBUG_RETURN(0);
}

int spider_db_errorno(
  SPIDER_CONN *conn
) {
  int error_num;
  DBUG_ENTER("spider_db_errorno");
  DBUG_ASSERT(conn->need_mon);
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
    if (conn->server_lost)
    {
      *conn->need_mon = ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM;
      if (!current_thd->is_error())
      {
        my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
          ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
      }
      if (!conn->mta_conn_mutex_unlock_later)
      {
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM);
    }
    if ((error_num = conn->db_conn->get_errno()))
    {
      DBUG_PRINT("info",("spider error_num = %d", error_num));
      if (conn->db_conn->is_server_gone_error(error_num))
      {
        spider_db_disconnect(conn);
        DBUG_PRINT("info", ("spider conn=%p SERVER_LOST", conn));
        conn->server_lost = TRUE;
        if (conn->disable_reconnect)
        {
          *conn->need_mon = ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM;
          my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
            ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
        }
        if (!conn->mta_conn_mutex_unlock_later)
        {
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM);
      } else if (
        conn->ignore_dup_key &&
        conn->db_conn->is_dup_entry_error(error_num)
      ) {
        conn->error_str = (char*) conn->db_conn->get_error();
        conn->error_length = strlen(conn->error_str);
        if (!conn->mta_conn_mutex_unlock_later)
        {
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
      } else if (
        conn->db_conn->is_xa_nota_error(error_num) &&
        current_thd &&
        spider_param_force_commit(current_thd) == 1
      ) {
        push_warning(current_thd, SPIDER_WARN_LEVEL_WARN,
          error_num, conn->db_conn->get_error());
        if (spider_param_log_result_errors() >= 3)
        {
          time_t cur_time = (time_t) time((time_t*) 0);
          struct tm lt;
          struct tm *l_time = localtime_r(&cur_time, &lt);
          fprintf(stderr, "%04d%02d%02d %02d:%02d:%02d [WARN SPIDER RESULT] "
            "to %lld: %d %s\n",
            l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
            l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
            (long long int) current_thd->thread_id, error_num,
            conn->db_conn->get_error());
        }
        if (!conn->mta_conn_mutex_unlock_later)
        {
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(error_num);
      }
      *conn->need_mon = error_num;
      my_message(error_num, conn->db_conn->get_error(), MYF(0));
      if (spider_param_log_result_errors() >= 1)
      {
        time_t cur_time = (time_t) time((time_t*) 0);
        struct tm lt;
        struct tm *l_time = localtime_r(&cur_time, &lt);
        fprintf(stderr, "%04d%02d%02d %02d:%02d:%02d [ERROR SPIDER RESULT] "
          "to %lld: %d %s\n",
          l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
          l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
          (long long int) current_thd->thread_id, error_num,
          conn->db_conn->get_error());
      }
      if (!conn->mta_conn_mutex_unlock_later)
      {
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(error_num);
    }
  if (!conn->mta_conn_mutex_unlock_later)
  {
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  }
  DBUG_RETURN(0);
}

int spider_db_set_trx_isolation(
  SPIDER_CONN *conn,
  int trx_isolation,
  int *need_mon
) {
  DBUG_ENTER("spider_db_set_trx_isolation");
  DBUG_RETURN(conn->db_conn->set_trx_isolation(trx_isolation, need_mon));
}

int spider_db_set_names_internal(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int all_link_idx,
  int *need_mon
) {
  DBUG_ENTER("spider_db_set_names_internal");
    pthread_mutex_assert_owner(&conn->mta_conn_mutex);
    DBUG_ASSERT(conn->mta_conn_mutex_file_pos.file_name);
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    if (
      !conn->access_charset ||
      share->access_charset->cset != conn->access_charset->cset
    ) {
      if (
        spider_db_before_query(conn, need_mon) ||
        conn->db_conn->set_character_set(share->access_charset->cs_name.str)
      ) {
        DBUG_RETURN(spider_db_errorno(conn));
      }
      conn->access_charset = share->access_charset;
    }
    if (
      spider_param_use_default_database(trx->thd) &&
      share->tgt_dbs[all_link_idx] &&
      (
        !conn->default_database.length() ||
        conn->default_database.length() !=
          share->tgt_dbs_lengths[all_link_idx] ||
        memcmp(share->tgt_dbs[all_link_idx], conn->default_database.ptr(),
          share->tgt_dbs_lengths[all_link_idx])
      )
    ) {
      DBUG_PRINT("info",("spider all_link_idx=%d db=%s", all_link_idx,
        share->tgt_dbs[all_link_idx]));
      if (
        spider_db_before_query(conn, need_mon) ||
        conn->db_conn->select_db(share->tgt_dbs[all_link_idx])
      ) {
        DBUG_RETURN(spider_db_errorno(conn));
      }
      conn->default_database.length(0);
      if (conn->default_database.reserve(
        share->tgt_dbs_lengths[all_link_idx] + 1))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      conn->default_database.q_append(share->tgt_dbs[all_link_idx],
        share->tgt_dbs_lengths[all_link_idx] + 1);
      conn->default_database.length(share->tgt_dbs_lengths[all_link_idx]);
    }
  DBUG_RETURN(0);
}

int spider_db_set_names(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_db_set_names");
  DBUG_RETURN(spider_db_set_names_internal(spider->wide_handler->trx,
    spider->share, conn,
    spider->conn_link_idx[link_idx], &spider->need_mons[link_idx]));
}

int spider_db_query_with_set_names(
  ulong sql_type,
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
  DBUG_ENTER("spider_db_query_with_set_names");

  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  if ((error_num = spider_db_set_names(spider, conn, link_idx)))
  {
    if (
      share->monitoring_kind[link_idx] &&
      spider->need_mons[link_idx]
    ) {
      error_num = spider_ping_table_mon_from_table(
          spider->wide_handler->trx,
          spider->wide_handler->trx->thd,
          share,
          link_idx,
          (uint32) share->monitoring_sid[link_idx],
          share->table_name,
          share->table_name_length,
          spider->conn_link_idx[link_idx],
          NULL,
          0,
          share->monitoring_kind[link_idx],
          share->monitoring_limit[link_idx],
          share->monitoring_flag[link_idx],
          TRUE
        );
    }
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx,
    spider->wide_handler->trx->thd,
    share);
  if (dbton_hdl->execute_sql(
    sql_type,
    conn,
    -1,
    &spider->need_mons[link_idx])
  ) {
    error_num = spider_db_errorno(conn);
    if (
      share->monitoring_kind[link_idx] &&
      spider->need_mons[link_idx]
    ) {
      error_num = spider_ping_table_mon_from_table(
          spider->wide_handler->trx,
          spider->wide_handler->trx->thd,
          share,
          link_idx,
          (uint32) share->monitoring_sid[link_idx],
          share->table_name,
          share->table_name_length,
          spider->conn_link_idx[link_idx],
          NULL,
          0,
          share->monitoring_kind[link_idx],
          share->monitoring_limit[link_idx],
          share->monitoring_flag[link_idx],
          TRUE
        );
    }
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_query_for_bulk_update(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx,
  ha_rows *dup_key_found
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_db_query_for_bulk_update");

  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
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
    if (
      share->monitoring_kind[link_idx] &&
      spider->need_mons[link_idx]
    ) {
      error_num = spider_ping_table_mon_from_table(
          spider->wide_handler->trx,
          spider->wide_handler->trx->thd,
          share,
          link_idx,
          (uint32) share->monitoring_sid[link_idx],
          share->table_name,
          share->table_name_length,
          spider->conn_link_idx[link_idx],
          NULL,
          0,
          share->monitoring_kind[link_idx],
          share->monitoring_limit[link_idx],
          share->monitoring_flag[link_idx],
          TRUE
        );
    }
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, link_idx,
    spider->wide_handler->trx->thd,
    share);
  spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
  if (dbton_hdl->execute_sql(
    SPIDER_SQL_TYPE_BULK_UPDATE_SQL,
    conn,
    -1,
    &spider->need_mons[link_idx])
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    error_num = spider_db_errorno(conn);
    if (
      error_num != ER_DUP_ENTRY &&
      error_num != ER_DUP_KEY &&
      error_num != HA_ERR_FOUND_DUPP_KEY &&
      share->monitoring_kind[link_idx] &&
      spider->need_mons[link_idx]
    ) {
      error_num = spider_ping_table_mon_from_table(
          spider->wide_handler->trx,
          spider->wide_handler->trx->thd,
          share,
          link_idx,
          (uint32) share->monitoring_sid[link_idx],
          share->table_name,
          share->table_name_length,
          spider->conn_link_idx[link_idx],
          NULL,
          0,
          share->monitoring_kind[link_idx],
          share->monitoring_limit[link_idx],
          share->monitoring_flag[link_idx],
          TRUE
        );
    }
    if (
      spider->wide_handler->ignore_dup_key &&
      (
        error_num == ER_DUP_ENTRY ||
        error_num == ER_DUP_KEY ||
        error_num == HA_ERR_FOUND_DUPP_KEY
      )
    ) {
      ++(*dup_key_found);
      spider->wide_handler->trx->thd->clear_error();
      DBUG_RETURN(0);
    }
    DBUG_RETURN(error_num);
  }
  while (!(error_num = conn->db_conn->next_result()))
  {
    ;
  }
  if (error_num > 0 && !conn->db_conn->is_dup_entry_error(error_num))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    if (
      share->monitoring_kind[link_idx] &&
      spider->need_mons[link_idx]
    ) {
      error_num = spider_ping_table_mon_from_table(
          spider->wide_handler->trx,
          spider->wide_handler->trx->thd,
          share,
          link_idx,
          (uint32) share->monitoring_sid[link_idx],
          share->table_name,
          share->table_name_length,
          spider->conn_link_idx[link_idx],
          NULL,
          0,
          share->monitoring_kind[link_idx],
          share->monitoring_limit[link_idx],
          share->monitoring_flag[link_idx],
          TRUE
        );
    }
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

size_t spider_db_real_escape_string(
  SPIDER_CONN *conn,
  char *to,
  const char *from,
  size_t from_length
) {
  DBUG_ENTER("spider_db_real_escape_string");
  DBUG_RETURN(conn->db_conn->escape_string(to, from, from_length));
}

int spider_db_consistent_snapshot(
  SPIDER_CONN *conn,
  int *need_mon
) {
  int error_num;
  DBUG_ENTER("spider_db_consistent_snapshot");
  if ((error_num = conn->db_conn->consistent_snapshot(need_mon)))
  {
    DBUG_RETURN(error_num);
  }
  conn->trx_start = TRUE;
  DBUG_RETURN(0);
}

int spider_db_start_transaction(
  SPIDER_CONN *conn,
  int *need_mon
) {
  int error_num;
  DBUG_ENTER("spider_db_start_transaction");
  if ((error_num = conn->db_conn->start_transaction(need_mon)))
  {
    DBUG_RETURN(error_num);
  }
  conn->trx_start = TRUE;
  DBUG_RETURN(0);
}

int spider_db_commit(
  SPIDER_CONN *conn
) {
  int need_mon = 0, error_num;
  DBUG_ENTER("spider_db_commit");
  if (!conn->queued_connect && !conn->queued_trx_start)
  {
    if (conn->use_for_active_standby && conn->server_lost)
    {
      my_message(ER_SPIDER_LINK_IS_FAILOVER_NUM,
        ER_SPIDER_LINK_IS_FAILOVER_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_LINK_IS_FAILOVER_NUM);
    }
    if ((error_num = conn->db_conn->commit(&need_mon)))
    {
      DBUG_RETURN(error_num);
    }
    conn->trx_start = FALSE;
  } else
    conn->trx_start = FALSE;
  DBUG_RETURN(0);
}

int spider_db_rollback(
  SPIDER_CONN *conn
) {
  int error_num, need_mon = 0;
  DBUG_ENTER("spider_db_rollback");
  if (!conn->queued_connect && !conn->queued_trx_start)
  {
    if ((error_num = conn->db_conn->rollback(&need_mon)))
    {
      DBUG_RETURN(error_num);
    }
    conn->trx_start = FALSE;
  } else
    conn->trx_start = FALSE;
  DBUG_RETURN(0);
}

int spider_db_append_hex_string(
  spider_string *str,
  uchar *hex_ptr,
  int hex_ptr_length
) {
  uchar *end_ptr;
  char *str_ptr;
  DBUG_ENTER("spider_db_append_hex_string");
  if (hex_ptr_length)
  {
    if (str->reserve(SPIDER_SQL_HEX_LEN + hex_ptr_length * 2))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_HEX_STR, SPIDER_SQL_HEX_LEN);
    str_ptr = (char *) str->ptr() + str->length();
    for (end_ptr = hex_ptr + hex_ptr_length; hex_ptr < end_ptr; hex_ptr++)
    {
      *str_ptr++ = spider_dig_upper[(*hex_ptr) >> 4];
      *str_ptr++ = spider_dig_upper[(*hex_ptr) & 0x0F];
    }
    str->length(str->length() + hex_ptr_length * 2);
  } else {
    if (str->reserve((SPIDER_SQL_VALUE_QUOTE_LEN) * 2))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  }
  DBUG_RETURN(0);
}

void spider_db_append_xid_str(
  spider_string *tmp_str,
  XID *xid
) {
  char format_id[sizeof(long) + 3];
  uint format_id_length;
  DBUG_ENTER("spider_db_append_xid_str");

  format_id_length =
    my_sprintf(format_id, (format_id, "%lu", xid->formatID));
  spider_db_append_hex_string(tmp_str, (uchar *) xid->data, xid->gtrid_length);
/*
  tmp_str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  tmp_str->q_append(xid->data, xid->gtrid_length);
  tmp_str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
*/
  tmp_str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  spider_db_append_hex_string(tmp_str,
    (uchar *) xid->data + xid->gtrid_length, xid->bqual_length);
/*
  tmp_str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  tmp_str->q_append(xid->data + xid->gtrid_length, xid->bqual_length);
  tmp_str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
*/
  tmp_str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  tmp_str->q_append(format_id, format_id_length);
#ifndef DBUG_OFF
  ((char *) tmp_str->ptr())[tmp_str->length()] = '\0';
#endif

  DBUG_VOID_RETURN;
}

int spider_db_xa_end(
  SPIDER_CONN *conn,
  XID *xid
) {
  int need_mon = 0;
  DBUG_ENTER("spider_db_xa_end");
  if (!conn->queued_connect && !conn->queued_xa_start)
  {
    DBUG_RETURN(conn->db_conn->xa_end(xid, &need_mon));
  }
  DBUG_RETURN(0);
}

int spider_db_xa_prepare(
  SPIDER_CONN *conn,
  XID *xid
) {
  int need_mon = 0;
  DBUG_ENTER("spider_db_xa_prepare");
  if (!conn->queued_connect && !conn->queued_xa_start)
  {
    if (conn->use_for_active_standby && conn->server_lost)
    {
      my_message(ER_SPIDER_LINK_IS_FAILOVER_NUM,
        ER_SPIDER_LINK_IS_FAILOVER_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_LINK_IS_FAILOVER_NUM);
    }
    DBUG_RETURN(conn->db_conn->xa_prepare(xid, &need_mon));
  }
  DBUG_RETURN(0);
}

int spider_db_xa_commit(
  SPIDER_CONN *conn,
  XID *xid
) {
  int need_mon = 0;
  DBUG_ENTER("spider_db_xa_commit");
  if (!conn->queued_connect && !conn->queued_xa_start)
  {
    DBUG_RETURN(conn->db_conn->xa_commit(xid, &need_mon));
  }
  DBUG_RETURN(0);
}

int spider_db_xa_rollback(
  SPIDER_CONN *conn,
  XID *xid
) {
  int need_mon = 0;
  DBUG_ENTER("spider_db_xa_rollback");
  if (!conn->queued_connect && !conn->queued_xa_start)
  {
    DBUG_RETURN(conn->db_conn->xa_rollback(xid, &need_mon));
  }
  DBUG_RETURN(0);
}

int spider_db_lock_tables(
  ha_spider *spider,
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  DBUG_ENTER("spider_db_lock_tables");
  error_num = spider->dbton_handler[conn->dbton_id]->lock_tables(link_idx);
  DBUG_RETURN(error_num);
}

int spider_db_unlock_tables(
  ha_spider *spider,
  int link_idx
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  DBUG_ENTER("spider_db_unlock_tables");
  error_num = spider->dbton_handler[conn->dbton_id]->unlock_tables(link_idx);
  DBUG_RETURN(error_num);
}

int spider_db_append_name_with_quote_str(
  spider_string *str,
  const char *name,
  uint dbton_id
) {
  DBUG_ENTER("spider_db_append_name_with_quote_str");
  DBUG_RETURN(spider_db_append_name_with_quote_str_internal(
    str, name, strlen(name), system_charset_info, dbton_id));
}

int spider_db_append_name_with_quote_str(
  spider_string *str,
  LEX_CSTRING &name,
  uint dbton_id
) {
  DBUG_ENTER("spider_db_append_name_with_quote_str");
  DBUG_RETURN(spider_db_append_name_with_quote_str_internal(
    str, name.str, name.length, system_charset_info, dbton_id));
}

int spider_db_append_name_with_quote_str_internal(
  spider_string *str,
  const char *name,
  int length,
  uint dbton_id
) {
  DBUG_ENTER("spider_db_append_name_with_quote_str_internal");
  DBUG_RETURN(spider_db_append_name_with_quote_str_internal(
    str, name, length, system_charset_info, dbton_id));
}

int spider_db_append_name_with_quote_str_internal(
  spider_string *str,
  const char *name,
  int length,
  CHARSET_INFO *cs,
  uint dbton_id
) {
  int error_num;
  const char *name_end;
  char head_code;
  DBUG_ENTER("spider_db_append_name_with_quote_str_internal");
  for (name_end = name + length; name < name_end; name += length)
  {
    head_code = *name;
#ifdef SPIDER_HAS_MY_CHARLEN
    if ((length = my_ci_charlen(cs, (const uchar *) name, (const uchar *) name_end)) < 1)
#else
    if (!(length = my_mbcharlen(cs, (uchar) head_code)))
#endif
    {
      my_message(ER_SPIDER_WRONG_CHARACTER_IN_NAME_NUM,
        ER_SPIDER_WRONG_CHARACTER_IN_NAME_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_WRONG_CHARACTER_IN_NAME_NUM);
    }
    if (
      length == 1 &&
      spider_dbton[dbton_id].db_util->is_name_quote(head_code)
    ) {
      if ((error_num = spider_dbton[dbton_id].db_util->
        append_escaped_name_quote(str)))
      {
        DBUG_RETURN(error_num);
      }
    } else {
      if (str->append(name, length, cs))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }
  DBUG_RETURN(0);
}

int spider_db_append_select(
  ha_spider *spider
) {
  int error_num;
  DBUG_ENTER("spider_db_append_select");

  if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
  {
    if ((error_num = spider->append_select_sql_part(
      SPIDER_SQL_TYPE_SELECT_SQL)))
      DBUG_RETURN(error_num);
  }
  if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
  {
    if ((error_num = spider->append_select_sql_part(
      SPIDER_SQL_TYPE_HANDLER)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_append_select_columns(
  ha_spider *spider
) {
  int error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_append_select_columns");
  if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
  {
    if (
      result_list->direct_aggregate &&
      (error_num = spider->append_sum_select_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL, NULL, 0))
    )
      DBUG_RETURN(error_num);
    if ((error_num = spider->append_match_select_sql_part(
      SPIDER_SQL_TYPE_SELECT_SQL, NULL, 0)))
      DBUG_RETURN(error_num);
    if (!spider->select_column_mode)
    {
      if (result_list->keyread)
      {
        if ((error_num = spider->append_key_select_sql_part(
          SPIDER_SQL_TYPE_SELECT_SQL, spider->active_index)))
          DBUG_RETURN(error_num);
      } else {
        if ((error_num = spider->append_table_select_sql_part(
          SPIDER_SQL_TYPE_SELECT_SQL)))
          DBUG_RETURN(error_num);
      }
    } else {
      if ((error_num = spider->append_minimum_select_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
  }
  if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
  {
    if ((error_num = spider->append_from_sql_part(SPIDER_SQL_TYPE_HANDLER)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_append_null_value(
  spider_string *str,
  KEY_PART_INFO *key_part,
  const uchar **ptr
) {
  DBUG_ENTER("spider_db_append_null_value");
  if (key_part->null_bit)
  {
    if (*(*ptr)++)
    {
      if (str->reserve(SPIDER_SQL_NULL_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
      DBUG_RETURN(-1);
    }
  }
  DBUG_RETURN(0);
}

int spider_db_append_key_columns(
  const key_range *start_key,
  ha_spider *spider,
  spider_string *str
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  KEY *key_info = result_list->key_info;
  uint key_name_length, key_count;
  key_part_map full_key_part_map =
    make_prev_keypart_map(spider_user_defined_key_parts(key_info));
  key_part_map start_key_part_map;
  char tmp_buf[MAX_FIELD_WIDTH];
  DBUG_ENTER("spider_db_append_key_columns");

  start_key_part_map = start_key->keypart_map & full_key_part_map;
  DBUG_PRINT("info", ("spider spider_user_defined_key_parts=%u",
    spider_user_defined_key_parts(key_info)));
  DBUG_PRINT("info", ("spider full_key_part_map=%lu", full_key_part_map));
  DBUG_PRINT("info", ("spider start_key_part_map=%lu", start_key_part_map));

  if (!start_key_part_map)
    DBUG_RETURN(0);

  for (
    key_count = 0;
    start_key_part_map;
    start_key_part_map >>= 1,
    key_count++
  ) {
    key_name_length = my_sprintf(tmp_buf, (tmp_buf, "c%u", key_count));
    if (str->reserve(key_name_length + SPIDER_SQL_COMMA_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(tmp_buf, key_name_length);
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  }
  str->length(str->length() - SPIDER_SQL_COMMA_LEN);

  DBUG_RETURN(0);
}

int spider_db_append_key_hint(
  spider_string *str,
  char *hint_str
) {
  int hint_str_len = strlen(hint_str);
  DBUG_ENTER("spider_db_append_key_hint");
  if (hint_str_len >= 2 &&
    (hint_str[0] == 'f' || hint_str[0] == 'F') && hint_str[1] == ' '
  ) {
    if (str->reserve(hint_str_len - 2 +
      SPIDER_SQL_SQL_FORCE_IDX_LEN + SPIDER_SQL_CLOSE_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    hint_str += 2;
    str->q_append(SPIDER_SQL_SQL_FORCE_IDX_STR, SPIDER_SQL_SQL_FORCE_IDX_LEN);
    str->q_append(hint_str, hint_str_len - 2);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  } else if (hint_str_len >= 2 &&
    (hint_str[0] == 'u' || hint_str[0] == 'U') && hint_str[1] == ' '
  ) {
    if (str->reserve(hint_str_len - 2 +
      SPIDER_SQL_SQL_USE_IDX_LEN + SPIDER_SQL_CLOSE_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    hint_str += 2;
    str->q_append(SPIDER_SQL_SQL_USE_IDX_STR, SPIDER_SQL_SQL_USE_IDX_LEN);
    str->q_append(hint_str, hint_str_len - 2);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  } else if (hint_str_len >= 3 &&
    (hint_str[0] == 'i' || hint_str[0] == 'I') &&
    (hint_str[1] == 'g' || hint_str[1] == 'G') && hint_str[2] == ' '
  ) {
    if (str->reserve(hint_str_len - 3 +
      SPIDER_SQL_SQL_IGNORE_IDX_LEN + SPIDER_SQL_CLOSE_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    hint_str += 3;
    str->q_append(
      SPIDER_SQL_SQL_IGNORE_IDX_STR, SPIDER_SQL_SQL_IGNORE_IDX_LEN);
    str->q_append(hint_str, hint_str_len - 3);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  } else if (str->reserve(hint_str_len + SPIDER_SQL_SPACE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  else
  {
    str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    str->q_append(hint_str, hint_str_len);
  }
  DBUG_RETURN(0);
}

int spider_db_append_hint_after_table(
  ha_spider *spider,
  spider_string *str,
  spider_string *hint
) {
  DBUG_ENTER("spider_db_append_hint_after_table");
  if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
  {
    if (str->append(*hint))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  DBUG_RETURN(0);
}

int spider_db_append_key_where_internal(
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  const key_range *start_key,
  const key_range *end_key,
  ha_spider *spider,
  bool set_order,
  ulong sql_type,
  uint dbton_id
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
#ifndef DBUG_OFF
  TABLE *table = spider->get_table();
#endif
  KEY *key_info = result_list->key_info;
  int error_num;
  uint key_name_length;
  key_part_map full_key_part_map;
  key_part_map start_key_part_map;
  key_part_map end_key_part_map;
  key_part_map tgt_key_part_map;
  int key_count;
  uint length;
  uint store_length;
  uint current_pos = str->length();
  const uchar *ptr, *another_ptr;
  const key_range *use_key, *another_key;
  KEY_PART_INFO *key_part;
  Field *field;
  bool use_both = TRUE, key_eq;
  uint sql_kind;
  spider_db_handler *dbton_hdl = spider->dbton_handler[dbton_id];
  spider_db_share *dbton_share = share->dbton_share[dbton_id];
  DBUG_ENTER("spider_db_append_key_where_internal");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_HANDLER:
      sql_kind = SPIDER_SQL_KIND_HANDLER;
      break;
    default:
      sql_kind = SPIDER_SQL_KIND_SQL;
      break;
  }

  if (key_info)
    full_key_part_map =
      make_prev_keypart_map(spider_user_defined_key_parts(key_info));
  else
    full_key_part_map = 0;

  if (start_key)
  {
    start_key_part_map = start_key->keypart_map & full_key_part_map;
  } else {
    start_key_part_map = 0;
    use_both = FALSE;
  }
  if (end_key) {
    end_key_part_map = end_key->keypart_map & full_key_part_map;
    result_list->end_key = end_key;
  } else {
    end_key_part_map = 0;
    use_both = FALSE;
  }
  DBUG_PRINT("info", ("spider spider_user_defined_key_parts=%u", key_info ?
    spider_user_defined_key_parts(key_info) : 0));
  DBUG_PRINT("info", ("spider full_key_part_map=%lu", full_key_part_map));
  DBUG_PRINT("info", ("spider start_key_part_map=%lu", start_key_part_map));
  DBUG_PRINT("info", ("spider end_key_part_map=%lu", end_key_part_map));

#ifndef DBUG_OFF
  MY_BITMAP *tmp_map = dbug_tmp_use_all_columns(table, &table->read_set);
#endif

  if (sql_kind == SPIDER_SQL_KIND_HANDLER)
  {
#ifdef SPIDER_use_LEX_CSTRING_for_KEY_Field_name
    const char *key_name = key_info->name.str;
    key_name_length = key_info->name.length;
#else
    const char *key_name = key_info->name;
    key_name_length = strlen(key_name);
#endif
    if (str->reserve(SPIDER_SQL_READ_LEN +
      /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + key_name_length))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_READ_STR, SPIDER_SQL_READ_LEN);
    if ((error_num = spider_dbton[dbton_id].db_util->
      append_name(str, key_name, key_name_length)))
    {
      DBUG_RETURN(error_num);
    }
    dbton_hdl->set_order_pos(SPIDER_SQL_TYPE_HANDLER);
    if (
      (start_key_part_map || end_key_part_map) &&
      !(use_both && (!start_key_part_map || !end_key_part_map))
    ) {
      if (str_part->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str_part->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      result_list->ha_read_kind = 0;
    } else if (!result_list->desc_flg)
    {
      if (str->reserve(SPIDER_SQL_FIRST_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_FIRST_STR, SPIDER_SQL_FIRST_LEN);
      result_list->ha_read_kind = 1;
    } else {
      if (str->reserve(SPIDER_SQL_LAST_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_LAST_STR, SPIDER_SQL_LAST_LEN);
      result_list->ha_read_kind = 2;
    }
  }
  if (!start_key_part_map && !end_key_part_map)
  {
    result_list->key_order = 0;
    goto end;
  } else if (use_both && (!start_key_part_map || !end_key_part_map))
  {
    result_list->key_order = 0;
    goto end;
  } else if (start_key_part_map >= end_key_part_map)
  {
    use_key = start_key;
    another_key = end_key;
    tgt_key_part_map = start_key_part_map;
  } else {
    use_key = end_key;
    another_key = start_key;
    tgt_key_part_map = end_key_part_map;
  }
  DBUG_PRINT("info", ("spider tgt_key_part_map=%lu", tgt_key_part_map));
  if (start_key_part_map == end_key_part_map)
    result_list->use_both_key = TRUE;

  if (sql_kind == SPIDER_SQL_KIND_SQL)
  {
    if (str->reserve(SPIDER_SQL_WHERE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
  } else if (sql_kind == SPIDER_SQL_KIND_HANDLER)
  {
    if (str_part2->reserve(SPIDER_SQL_WHERE_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str_part2->q_append(SPIDER_SQL_WHERE_STR, SPIDER_SQL_WHERE_LEN);
  }

  for (
    key_part = key_info->key_part,
    length = 0,
    key_count = 0;
    tgt_key_part_map;
    length += store_length,
    tgt_key_part_map >>= 1,
    start_key_part_map >>= 1,
    end_key_part_map >>= 1,
    key_part++,
    key_count++
  ) {
    DBUG_PRINT("info", ("spider tgt_key_part_map=%lu", tgt_key_part_map));
    bool rev = key_part->key_part_flag & HA_REVERSE_SORT;
    store_length = key_part->store_length;
    field = key_part->field;
    key_name_length = dbton_share->get_column_name_length(field->field_index);
    ptr = use_key->key + length;
    if (use_both)
    {
      another_ptr = another_key->key + length;
      if (
        start_key_part_map &&
        end_key_part_map &&
        !memcmp(ptr, another_ptr, store_length)
      )
        key_eq = TRUE;
      else {
        key_eq = FALSE;
#ifndef DBUG_OFF
        if (
          start_key_part_map &&
          end_key_part_map
        )
          DBUG_PRINT("info", ("spider memcmp=%d",
            memcmp(ptr, another_ptr, store_length)));
#endif
      }
    } else {
      if (tgt_key_part_map > 1)
        key_eq = TRUE;
      else
        key_eq = FALSE;
    }
    if (
      (key_eq && use_key == start_key) ||
      (!key_eq && start_key_part_map)
    ) {
      bool tgt_final = (use_key == start_key &&
        (tgt_key_part_map == 1 || !end_key_part_map));
      ptr = start_key->key + length;
      if (
        (error_num = dbton_hdl->append_is_null_part(sql_type, key_part,
          start_key, &ptr, key_eq, tgt_final))
      ) {
        if (error_num > 0)
          DBUG_RETURN(error_num);
        if (
          !set_order &&
          start_key->flag != HA_READ_KEY_EXACT &&
          sql_kind == SPIDER_SQL_KIND_SQL
        ) {
          result_list->key_order = key_count;
          set_order = TRUE;
        }
      } else if (key_eq)
      {
        DBUG_PRINT("info", ("spider key_eq"));
        if (sql_kind == SPIDER_SQL_KIND_SQL)
        {
          if (str->reserve(store_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
            SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_AND_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          dbton_share->append_column_name(str, field->field_index);
          str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
          if (spider_dbton[dbton_id].db_util->
            append_column_value(spider, str, field, ptr,
              share->access_charset))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        } else if (sql_kind == SPIDER_SQL_KIND_HANDLER)
        {
          if (str_part2->reserve(store_length + key_name_length +
            /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
            SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_AND_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          dbton_share->append_column_name(str_part2, field->field_index);
          str_part2->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
          if (spider_dbton[dbton_id].db_util->
            append_column_value(spider, str_part2, field, ptr,
              share->access_charset))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);

          if (use_key == start_key)
          {
            if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
            {
              if (str->reserve(SPIDER_SQL_EQUAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str_part, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            }
          }
        }
      } else {
        DBUG_PRINT("info", ("spider start_key->flag=%d", start_key->flag));
        switch (start_key->flag)
        {
          case HA_READ_PREFIX_LAST:
            result_list->desc_flg = TRUE;
            /* fall through */
          case HA_READ_KEY_EXACT:
            if (sql_kind == SPIDER_SQL_KIND_SQL)
            {
              if (str->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                SPIDER_SQL_EQUAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str, field->field_index);
              str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            } else if (sql_kind == SPIDER_SQL_KIND_HANDLER)
            {
              if (str_part2->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                SPIDER_SQL_EQUAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str_part2, field->field_index);
              str_part2->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str_part2, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);

              if (use_key == start_key)
              {
                if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
                {
                  if (str->reserve(SPIDER_SQL_EQUAL_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
                  if (spider_dbton[dbton_id].db_util->
                    append_column_value(spider, str_part, field, ptr,
                      share->access_charset))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                }
              }
            }
            break;
          case HA_READ_AFTER_KEY:
            if (sql_kind == SPIDER_SQL_KIND_SQL)
            {
              const char* op_str;
              uint32 op_len;
              if (start_key_part_map == 1) {
                op_str = rev ? SPIDER_SQL_LT_STR : SPIDER_SQL_GT_STR;
                op_len = rev ? SPIDER_SQL_LT_LEN : SPIDER_SQL_GT_LEN;
              } else {
                op_str = rev ? SPIDER_SQL_LTEQUAL_STR : SPIDER_SQL_GTEQUAL_STR;
                op_len = rev ? SPIDER_SQL_LTEQUAL_LEN : SPIDER_SQL_GTEQUAL_LEN;
              }
              if (str->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + op_len))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str, field->field_index);
              str->q_append(op_str, op_len);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              if (use_both)
                start_key_part_map = 0;
              if (!set_order)
              {
                result_list->key_order = key_count;
                set_order = TRUE;
              }
            } else if (sql_kind == SPIDER_SQL_KIND_HANDLER)
            {
              if (str_part2->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                SPIDER_SQL_GT_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str_part2, field->field_index);
              if (rev)
                str_part2->q_append(SPIDER_SQL_LT_STR, SPIDER_SQL_LT_LEN);
              else
                str_part2->q_append(SPIDER_SQL_GT_STR, SPIDER_SQL_GT_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str_part2, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);

              if (use_key == start_key)
              {
                if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
                {
                  if (str->reserve(SPIDER_SQL_GT_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_GT_STR, SPIDER_SQL_GT_LEN);
                  if (spider_dbton[dbton_id].db_util->
                    append_column_value(spider, str_part, field, ptr,
                      share->access_charset))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                }
              }
            }
            break;
          case HA_READ_BEFORE_KEY:
            result_list->desc_flg = TRUE;
            if (sql_kind == SPIDER_SQL_KIND_SQL)
            {
              const char* op_str;
              uint32 op_len;
              if (start_key_part_map == 1) {
                op_str = rev ? SPIDER_SQL_GT_STR : SPIDER_SQL_LT_STR;
                op_len = rev ? SPIDER_SQL_GT_LEN : SPIDER_SQL_LT_LEN;
              } else {
                op_str = rev ? SPIDER_SQL_GTEQUAL_STR : SPIDER_SQL_LTEQUAL_STR;
                op_len = rev ? SPIDER_SQL_GTEQUAL_LEN : SPIDER_SQL_LTEQUAL_LEN;
              }
              if (str->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + op_len))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str, field->field_index);
              str->q_append(op_str, op_len);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              if (use_both)
                start_key_part_map = 0;
              if (!set_order)
              {
                result_list->key_order = key_count;
                set_order = TRUE;
              }
            } else if (sql_kind == SPIDER_SQL_KIND_HANDLER)
            {
              if (str_part2->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                SPIDER_SQL_LT_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str_part2, field->field_index);
              if (rev)
                str_part2->q_append(SPIDER_SQL_GT_STR, SPIDER_SQL_GT_LEN);
              else
                str_part2->q_append(SPIDER_SQL_LT_STR, SPIDER_SQL_LT_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str_part2, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);

              if (use_key == start_key)
              {
                if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
                {
                  if (str->reserve(SPIDER_SQL_LT_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_LT_STR, SPIDER_SQL_LT_LEN);
                  if (spider_dbton[dbton_id].db_util->
                    append_column_value(spider, str_part, field, ptr,
                      share->access_charset))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                }
              }
            }
            break;
          case HA_READ_KEY_OR_PREV:
          case HA_READ_PREFIX_LAST_OR_PREV:
            result_list->desc_flg = TRUE;
            if (sql_kind == SPIDER_SQL_KIND_SQL)
            {
              if (str->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                SPIDER_SQL_LTEQUAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str, field->field_index);
              if (rev)
                str->q_append(SPIDER_SQL_GTEQUAL_STR, SPIDER_SQL_GTEQUAL_LEN);
              else
                str->q_append(SPIDER_SQL_LTEQUAL_STR, SPIDER_SQL_LTEQUAL_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              if (!set_order)
              {
                result_list->key_order = key_count;
                set_order = TRUE;
              }
            } else if (sql_kind == SPIDER_SQL_KIND_HANDLER)
            {
              if (str_part2->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                SPIDER_SQL_LTEQUAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str_part2, field->field_index);
              if (rev)
                str_part2->q_append(SPIDER_SQL_GTEQUAL_STR, SPIDER_SQL_GTEQUAL_LEN);
              else
                str_part2->q_append(SPIDER_SQL_LTEQUAL_STR, SPIDER_SQL_LTEQUAL_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str_part2, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);

              if (use_key == start_key)
              {
                if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
                {
                  if (str->reserve(SPIDER_SQL_LTEQUAL_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_LTEQUAL_STR, SPIDER_SQL_LTEQUAL_LEN);
                  if (spider_dbton[dbton_id].db_util->
                    append_column_value(spider, str_part, field, ptr,
                      share->access_charset))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                }
              }
            }
            break;
          case HA_READ_MBR_CONTAIN:
            if (str->reserve(SPIDER_SQL_MBR_CONTAIN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_MBR_CONTAIN_STR,
              SPIDER_SQL_MBR_CONTAIN_LEN);
            if (
              spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset) ||
              str->reserve(SPIDER_SQL_COMMA_LEN + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_CLOSE_PAREN_LEN)
            )
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
            dbton_share->append_column_name(str, field->field_index);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
            break;
          case HA_READ_MBR_INTERSECT:
            if (str->reserve(SPIDER_SQL_MBR_INTERSECT_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_MBR_INTERSECT_STR,
              SPIDER_SQL_MBR_INTERSECT_LEN);
            if (
              spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset) ||
              str->reserve(SPIDER_SQL_COMMA_LEN + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_CLOSE_PAREN_LEN)
            )
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
            dbton_share->append_column_name(str, field->field_index);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
            break;
          case HA_READ_MBR_WITHIN:
            if (str->reserve(SPIDER_SQL_MBR_WITHIN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_MBR_WITHIN_STR,
              SPIDER_SQL_MBR_WITHIN_LEN);
            if (
              spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset) ||
              str->reserve(SPIDER_SQL_COMMA_LEN + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_CLOSE_PAREN_LEN)
            )
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
            dbton_share->append_column_name(str, field->field_index);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
            break;
          case HA_READ_MBR_DISJOINT:
            if (str->reserve(SPIDER_SQL_MBR_DISJOINT_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_MBR_DISJOINT_STR,
              SPIDER_SQL_MBR_DISJOINT_LEN);
            if (
              spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset) ||
              str->reserve(SPIDER_SQL_COMMA_LEN + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_CLOSE_PAREN_LEN)
            )
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
            dbton_share->append_column_name(str, field->field_index);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
            break;
          case HA_READ_MBR_EQUAL:
            if (str->reserve(SPIDER_SQL_MBR_EQUAL_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_MBR_EQUAL_STR, SPIDER_SQL_MBR_EQUAL_LEN);
            if (
              spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset) ||
              str->reserve(SPIDER_SQL_COMMA_LEN + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + SPIDER_SQL_CLOSE_PAREN_LEN)
            )
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
            dbton_share->append_column_name(str, field->field_index);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
            break;
          default:
            if (sql_kind == SPIDER_SQL_KIND_SQL)
            {
              if (str->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                SPIDER_SQL_GTEQUAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str, field->field_index);
              if (rev)
                str->q_append(SPIDER_SQL_LTEQUAL_STR, SPIDER_SQL_LTEQUAL_LEN);
              else
                str->q_append(SPIDER_SQL_GTEQUAL_STR, SPIDER_SQL_GTEQUAL_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              if (!set_order)
              {
                result_list->key_order = key_count;
                set_order = TRUE;
              }
            } else if (sql_kind == SPIDER_SQL_KIND_HANDLER)
            {
              if (str_part2->reserve(store_length + key_name_length +
                /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                SPIDER_SQL_GTEQUAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              dbton_share->append_column_name(str_part2, field->field_index);
              if (rev)
                str_part2->q_append(SPIDER_SQL_LTEQUAL_STR, SPIDER_SQL_LTEQUAL_LEN);
              else
                str_part2->q_append(SPIDER_SQL_GTEQUAL_STR, SPIDER_SQL_GTEQUAL_LEN);
              if (spider_dbton[dbton_id].db_util->
                append_column_value(spider, str_part2, field, ptr,
                  share->access_charset))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);

              if (use_key == start_key)
              {
                if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
                {
                  if (str->reserve(SPIDER_SQL_GTEQUAL_LEN))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  str->q_append(SPIDER_SQL_GTEQUAL_STR, SPIDER_SQL_GTEQUAL_LEN);
                  if (spider_dbton[dbton_id].db_util->
                    append_column_value(spider, str_part, field, ptr,
                      share->access_charset))
                    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                }
              }
            }
            break;
        }
      }
      if (sql_kind == SPIDER_SQL_KIND_SQL)
      {
        if (str->reserve(SPIDER_SQL_AND_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_AND_STR,
          SPIDER_SQL_AND_LEN);
      } else if (sql_kind == SPIDER_SQL_KIND_HANDLER)
      {
        if (str_part2->reserve(SPIDER_SQL_AND_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str_part2->q_append(SPIDER_SQL_AND_STR,
          SPIDER_SQL_AND_LEN);

      }
    }

      if (
        (key_eq && use_key == end_key) ||
        (!key_eq && end_key_part_map)
      ) {
        bool tgt_final = (use_key == end_key && tgt_key_part_map == 1);
        ptr = end_key->key + length;
        if ((error_num = dbton_hdl->append_is_null_part(sql_type, key_part,
          end_key, &ptr, key_eq, tgt_final)))
        {
          if (error_num > 0)
            DBUG_RETURN(error_num);
          if (
            !set_order &&
            end_key->flag != HA_READ_KEY_EXACT &&
            sql_kind == SPIDER_SQL_KIND_SQL
          ) {
            result_list->key_order = key_count;
            set_order = TRUE;
          }
        } else if (key_eq)
        {
          DBUG_PRINT("info", ("spider key_eq"));
          if (sql_kind == SPIDER_SQL_KIND_SQL)
          {
            if (str->reserve(store_length + key_name_length +
              /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
              SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_AND_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            dbton_share->append_column_name(str, field->field_index);
            str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
            if (spider_dbton[dbton_id].db_util->
              append_column_value(spider, str, field, ptr,
                share->access_charset))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          } else {
            if (str_part2->reserve(store_length + key_name_length +
              /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
              SPIDER_SQL_EQUAL_LEN + SPIDER_SQL_AND_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            dbton_share->append_column_name(str_part2, field->field_index);
            str_part2->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
            if (spider_dbton[dbton_id].db_util->
              append_column_value(spider, str_part2, field, ptr,
                share->access_charset))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);

            if (use_key == end_key)
            {
              if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
              {
                if (str->reserve(SPIDER_SQL_EQUAL_LEN))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
                if (spider_dbton[dbton_id].db_util->
                  append_column_value(spider, str_part, field, ptr,
                    share->access_charset))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              }
            }
          }
        } else {
          DBUG_PRINT("info", ("spider end_key->flag=%d", end_key->flag));
          switch (end_key->flag)
          {
            case HA_READ_BEFORE_KEY:
              if (sql_kind == SPIDER_SQL_KIND_SQL)
              {
                const char* op_str;
                uint32 op_len;
                if (end_key_part_map == 1) {
                  op_str = rev ? SPIDER_SQL_GT_STR : SPIDER_SQL_LT_STR;
                  op_len = rev ? SPIDER_SQL_GT_LEN : SPIDER_SQL_LT_LEN;
                } else {
                  op_str = rev ? SPIDER_SQL_GTEQUAL_STR : SPIDER_SQL_LTEQUAL_STR;
                  op_len = rev ? SPIDER_SQL_GTEQUAL_LEN : SPIDER_SQL_LTEQUAL_LEN;
                }
                if (str->reserve(store_length + key_name_length +
                  /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 + op_len))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                dbton_share->append_column_name(str, field->field_index);
                str->q_append(op_str, op_len);
                if (spider_dbton[dbton_id].db_util->
                  append_column_value(spider, str, field, ptr,
                    share->access_charset))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                if (use_both)
                  end_key_part_map = 0;
                if (!set_order)
                {
                  result_list->key_order = key_count;
                  set_order = TRUE;
                }
              } else {
                if (str_part2->reserve(store_length + key_name_length +
                  /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                  SPIDER_SQL_LT_LEN))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                dbton_share->append_column_name(str_part2, field->field_index);
                if (rev)
                  str_part2->q_append(SPIDER_SQL_GT_STR, SPIDER_SQL_GT_LEN);
                else
                  str_part2->q_append(SPIDER_SQL_LT_STR, SPIDER_SQL_LT_LEN);
                if (spider_dbton[dbton_id].db_util->
                  append_column_value(spider, str_part2, field, ptr,
                    share->access_charset))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);

                if (use_key == end_key)
                {
                  if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
                  {
                    if (str->reserve(SPIDER_SQL_LT_LEN))
                      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                    str->q_append(SPIDER_SQL_LT_STR, SPIDER_SQL_LT_LEN);
                    if (spider_dbton[dbton_id].db_util->
                      append_column_value(spider, str_part, field, ptr,
                        share->access_charset))
                      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  }
                }
              }
              break;
            default:
              if (sql_kind == SPIDER_SQL_KIND_SQL)
              {
                if (str->reserve(store_length + key_name_length +
                  /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                  SPIDER_SQL_LTEQUAL_LEN))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                dbton_share->append_column_name(str, field->field_index);
                if (rev)
                  str->q_append(SPIDER_SQL_GTEQUAL_STR, SPIDER_SQL_GTEQUAL_LEN);
                else
                  str->q_append(SPIDER_SQL_LTEQUAL_STR, SPIDER_SQL_LTEQUAL_LEN);
                if (spider_dbton[dbton_id].db_util->
                  append_column_value(spider, str, field, ptr,
                    share->access_charset))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                if (!set_order)
                {
                  result_list->key_order = key_count;
                  set_order = TRUE;
                }
              } else {
                if (str_part2->reserve(store_length + key_name_length +
                  /* SPIDER_SQL_NAME_QUOTE_LEN */ 2 +
                  SPIDER_SQL_LTEQUAL_LEN))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                dbton_share->append_column_name(str_part2, field->field_index);
                if (rev)
                  str_part2->q_append(SPIDER_SQL_GTEQUAL_STR, SPIDER_SQL_GTEQUAL_LEN);
                else
                  str_part2->q_append(SPIDER_SQL_LTEQUAL_STR, SPIDER_SQL_LTEQUAL_LEN);
                if (spider_dbton[dbton_id].db_util->
                  append_column_value(spider, str_part2, field, ptr,
                    share->access_charset))
                  DBUG_RETURN(HA_ERR_OUT_OF_MEM);

                if (use_key == end_key)
                {
                  if (str_part->length() == SPIDER_SQL_OPEN_PAREN_LEN)
                  {
                    if (str->reserve(SPIDER_SQL_LTEQUAL_LEN))
                      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                    str->q_append(SPIDER_SQL_LTEQUAL_STR, SPIDER_SQL_LTEQUAL_LEN);
                    if (spider_dbton[dbton_id].db_util->
                      append_column_value(spider, str_part, field, ptr,
                        share->access_charset))
                      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
                  }
                }
              }
              break;
          }
        }
        if (sql_kind == SPIDER_SQL_KIND_SQL)
        {
          if (str->reserve(SPIDER_SQL_AND_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_AND_STR,
            SPIDER_SQL_AND_LEN);
        } else {
          if (str_part2->reserve(SPIDER_SQL_AND_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str_part2->q_append(SPIDER_SQL_AND_STR,
            SPIDER_SQL_AND_LEN);

        }
      }
      if (use_both && (!start_key_part_map || !end_key_part_map))
        break;
  }
  if ((error_num = dbton_hdl->append_where_terminator_part(sql_type,
    set_order, key_count)))
    DBUG_RETURN(error_num);

end:
  if (spider->multi_range_num && current_pos == str->length())
  {
    DBUG_PRINT("info", ("spider no key where condition"));
    dbton_hdl->no_where_cond = TRUE;
  }
  /* use condition */
  if (dbton_hdl->append_condition_part(NULL, 0, sql_type, FALSE))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (sql_kind == SPIDER_SQL_KIND_SQL)
    dbton_hdl->set_order_pos(sql_type);
#ifndef DBUG_OFF
  dbug_tmp_restore_column_map(&table->read_set, tmp_map);
#endif
  DBUG_RETURN(0);
}

int spider_db_append_key_where(
  const key_range *start_key,
  const key_range *end_key,
  ha_spider *spider
) {
  int error_num;
  DBUG_ENTER("spider_db_append_key_where");
  if ((spider->sql_kinds & SPIDER_SQL_KIND_SQL))
  {
    DBUG_PRINT("info",("spider call internal by SPIDER_SQL_KIND_SQL"));
    if ((error_num = spider->append_key_where_sql_part(start_key, end_key,
      SPIDER_SQL_TYPE_SELECT_SQL)))
      DBUG_RETURN(error_num);
  }
  if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
  {
    DBUG_PRINT("info",("spider call internal by SPIDER_SQL_KIND_HANDLER"));
    if ((error_num = spider->append_key_where_sql_part(start_key, end_key,
      SPIDER_SQL_TYPE_HANDLER)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_append_charset_name_before_string(
  spider_string *str,
  CHARSET_INFO *cs
) {
  const char *csname = cs->cs_name.str;
  uint csname_length = cs->cs_name.length;
  DBUG_ENTER("spider_db_append_charset_name_before_string");
  if (str->reserve(SPIDER_SQL_UNDERSCORE_LEN + csname_length))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_UNDERSCORE_STR, SPIDER_SQL_UNDERSCORE_LEN);
  str->q_append(csname, csname_length);
  DBUG_RETURN(0);
}

int spider_db_refetch_for_item_sum_funcs(
  ha_spider *spider
) {
  int error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_refetch_for_item_sum_funcs");
  if (result_list->snap_direct_aggregate)
  {
    SPIDER_DB_ROW *row = result_list->snap_row;
    row->first();
    if (result_list->snap_mrr_with_cnt)
    {
      row->next();
    }
    if ((error_num = spider_db_fetch_for_item_sum_funcs(row, spider)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_fetch_for_item_sum_funcs(
  SPIDER_DB_ROW *row,
  ha_spider *spider
) {
  int error_num;
  st_select_lex *select_lex;
  DBUG_ENTER("spider_db_fetch_for_item_sum_funcs");
  select_lex = spider_get_select_lex(spider);
  JOIN *join = select_lex->join;
  Item_sum **item_sum_ptr;
  spider->direct_aggregate_item_current = NULL;
  for (item_sum_ptr = join->sum_funcs; *item_sum_ptr; ++item_sum_ptr)
  {
    if ((error_num = spider_db_fetch_for_item_sum_func(row, *item_sum_ptr,
      spider)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_fetch_for_item_sum_func(
  SPIDER_DB_ROW *row,
  Item_sum *item_sum,
  ha_spider *spider
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  THD *thd = spider->wide_handler->trx->thd;
  DBUG_ENTER("spider_db_fetch_for_item_sum_func");
  DBUG_PRINT("info",("spider Sumfunctype = %d", item_sum->sum_func()));
  switch (item_sum->sum_func())
  {
    case Item_sum::COUNT_FUNC:
      {
        Item_sum_count *item_sum_count = (Item_sum_count *) item_sum;
        if (!row->is_null())
          item_sum_count->direct_add(row->val_int());
        else
          DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
        row->next();
      }
      break;
    case Item_sum::SUM_FUNC:
      {
        Item_sum_sum *item_sum_sum = (Item_sum_sum *) item_sum;
        if (item_sum_sum->result_type() == DECIMAL_RESULT)
        {
          my_decimal decimal_value;
          item_sum_sum->direct_add(row->val_decimal(&decimal_value,
            share->access_charset));
        } else {
          item_sum_sum->direct_add(row->val_real(), row->is_null());
        }
        row->next();
      }
      break;
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
      {
        if (!spider->direct_aggregate_item_current)
        {
          if (!spider->direct_aggregate_item_first)
          {
            if (!spider_bulk_malloc(spider_current_trx, 240, MYF(MY_WME),
              &spider->direct_aggregate_item_first,
              (uint) (sizeof(SPIDER_ITEM_HLD)),
              NullS)
            ) {
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            }
            spider->direct_aggregate_item_first->next = NULL;
            spider->direct_aggregate_item_first->item = NULL;
            spider->direct_aggregate_item_first->tgt_num = 0;
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY_AND_THDPTR
            spider->direct_aggregate_item_first->init_mem_root = FALSE;
#endif
          }
          spider->direct_aggregate_item_current =
            spider->direct_aggregate_item_first;
        } else {
          if (!spider->direct_aggregate_item_current->next)
          {
            if (!spider_bulk_malloc(spider_current_trx, 241, MYF(MY_WME),
              &spider->direct_aggregate_item_current->next,
              (uint) (sizeof(SPIDER_ITEM_HLD)), NullS)
            ) {
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            }
            spider->direct_aggregate_item_current->next->next = NULL;
            spider->direct_aggregate_item_current->next->item = NULL;
            spider->direct_aggregate_item_current->next->tgt_num =
              spider->direct_aggregate_item_current->tgt_num + 1;
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY_AND_THDPTR
            spider->direct_aggregate_item_current->next->init_mem_root = FALSE;
#endif
          }
          spider->direct_aggregate_item_current =
            spider->direct_aggregate_item_current->next;
        }
        if (!spider->direct_aggregate_item_current->item)
        {
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY_AND_THDPTR
          if (!spider->direct_aggregate_item_current->init_mem_root)
          {
            SPD_INIT_ALLOC_ROOT(
              &spider->direct_aggregate_item_current->mem_root,
              4096, 0, MYF(MY_WME));
            spider->direct_aggregate_item_current->init_mem_root = TRUE;
          }
#endif
          Item *free_list = thd->free_list;
          spider->direct_aggregate_item_current->item =
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY_AND_THDPTR
            new (&spider->direct_aggregate_item_current->mem_root)
              Item_string(thd, "", 0, share->access_charset);
#else
            new Item_string("", 0, share->access_charset);
#endif
#else
            new Item_string(share->access_charset);
#endif
          if (!spider->direct_aggregate_item_current->item)
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          thd->free_list = free_list;
        }

        Item_sum_min_max *item_sum_min_max = (Item_sum_min_max *) item_sum;
        Item_string *item =
          (Item_string *) spider->direct_aggregate_item_current->item;
        if (row->is_null())
        {
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY
          item->val_str(NULL)->length(0);
          item->append(NULL, 0);
#else
          item->set_str_with_copy(NULL, 0);
#endif
          item->null_value = TRUE;
        } else {
          char buf[MAX_FIELD_WIDTH];
          spider_string tmp_str(buf, MAX_FIELD_WIDTH, share->access_charset);
          tmp_str.init_calc_mem(242);
          tmp_str.length(0);
          if ((error_num = row->append_to_str(&tmp_str)))
            DBUG_RETURN(error_num);
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY
          item->val_str(NULL)->length(0);
          item->append((char *) tmp_str.ptr(), tmp_str.length());
#else
          item->set_str_with_copy(tmp_str.ptr(), tmp_str.length());
#endif
          item->null_value = FALSE;
        }
        item_sum_min_max->direct_add(item);
        row->next();
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

int spider_db_append_match_fetch(
  ha_spider *spider,
  st_spider_ft_info *ft_first,
  st_spider_ft_info *ft_current,
  SPIDER_DB_ROW *row
) {
  DBUG_ENTER("spider_db_append_match_fetch");
  if (ft_current)
  {
    st_spider_ft_info *ft_info = ft_first;
    while (TRUE)
    {
      DBUG_PRINT("info",("spider ft_info=%p", ft_info));
      if (!row->is_null())
        ft_info->score = (float) row->val_real();
      else
        DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
      row->next();
      if (ft_info == ft_current)
        break;
      ft_info = ft_info->next;
    }
  }
  DBUG_RETURN(0);
}

int spider_db_append_match_where(
  ha_spider *spider
) {
  int error_num;
  DBUG_ENTER("spider_db_append_match_where");
  if ((error_num = spider->append_match_where_sql_part(
    SPIDER_SQL_TYPE_SELECT_SQL)))
    DBUG_RETURN(error_num);

  /* use condition */
  if ((error_num = spider->append_condition_sql_part(
    NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL, FALSE)))
    DBUG_RETURN(error_num);

  spider->set_order_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
  DBUG_RETURN(0);
}

void spider_db_append_handler_next(
  ha_spider *spider
) {
  const char *alias;
  uint alias_length;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_append_handler_next");
  if (result_list->sorted && result_list->desc_flg)
  {
    alias = SPIDER_SQL_PREV_STR;
    alias_length = SPIDER_SQL_PREV_LEN;
  } else {
    alias = SPIDER_SQL_NEXT_STR;
    alias_length = SPIDER_SQL_NEXT_LEN;
  }
  spider->set_order_to_pos_sql(SPIDER_SQL_TYPE_HANDLER);
  spider->append_key_order_with_alias_sql_part(alias, alias_length,
    SPIDER_SQL_TYPE_HANDLER);
  DBUG_VOID_RETURN;
}

void spider_db_get_row_from_tmp_tbl_rec(
  SPIDER_RESULT *current,
  SPIDER_DB_ROW **row
) {
  DBUG_ENTER("spider_db_get_row_from_tmp_tbl_rec");
  *row = current->result->fetch_row_from_tmp_table(current->result_tmp_tbl);
  DBUG_VOID_RETURN;
}

int spider_db_get_row_from_tmp_tbl(
  SPIDER_RESULT *current,
  SPIDER_DB_ROW **row
) {
  int error_num;
  DBUG_ENTER("spider_db_get_row_from_tmp_tbl");
  if (current->result_tmp_tbl_inited == 2)
  {
    current->result_tmp_tbl->file->ha_rnd_end();
    current->result_tmp_tbl_inited = 0;
  }
  if (current->result_tmp_tbl_inited == 0)
  {
    current->result_tmp_tbl->file->extra(HA_EXTRA_CACHE);
    if ((error_num = current->result_tmp_tbl->file->ha_rnd_init(TRUE)))
      DBUG_RETURN(error_num);
    current->result_tmp_tbl_inited = 1;
  }
  if (
    (error_num = current->result_tmp_tbl->file->ha_rnd_next(
      current->result_tmp_tbl->record[0]))
  ) {
    DBUG_RETURN(error_num);
  }
  spider_db_get_row_from_tmp_tbl_rec(current, row);
  DBUG_RETURN(0);
}

int spider_db_get_row_from_tmp_tbl_pos(
  SPIDER_POSITION *pos,
  SPIDER_DB_ROW **row
) {
  int error_num;
  SPIDER_RESULT *result = pos->result;
  TABLE *tmp_tbl = result->result_tmp_tbl;
  DBUG_ENTER("spider_db_get_row_from_tmp_tbl_pos");
  if (result->result_tmp_tbl_inited == 1)
  {
    tmp_tbl->file->ha_rnd_end();
    result->result_tmp_tbl_inited = 0;
  }
  if (result->result_tmp_tbl_inited == 0)
  {
    if ((error_num = tmp_tbl->file->ha_rnd_init(FALSE)))
      DBUG_RETURN(error_num);
    result->result_tmp_tbl_inited = 2;
  }
  if (
    (error_num = tmp_tbl->file->ha_rnd_pos(tmp_tbl->record[0],
      (uchar *) &pos->tmp_tbl_pos))
  ) {
    DBUG_RETURN(error_num);
  }
  spider_db_get_row_from_tmp_tbl_rec(result, row);
  DBUG_RETURN(0);
}

int spider_db_fetch_row(
  SPIDER_SHARE *share,
  Field *field,
  SPIDER_DB_ROW *row,
  my_ptrdiff_t ptr_diff
) {
  int error_num;
  THD *thd = field->table->in_use;
  Time_zone *saved_time_zone = thd->variables.time_zone;
  DBUG_ENTER("spider_db_fetch_row");
  DBUG_PRINT("info", ("spider field_name %s", SPIDER_field_name_str(field)));
  DBUG_PRINT("info", ("spider fieldcharset %s", field->charset()->cs_name.str));

  thd->variables.time_zone = UTC;

  field->move_field_offset(ptr_diff);
  error_num = row->store_to_field(field, share->access_charset);
  field->move_field_offset(-ptr_diff);

  thd->variables.time_zone = saved_time_zone;

  DBUG_RETURN(error_num);
}

int spider_db_fetch_table(
  ha_spider *spider,
  uchar *buf,
  TABLE *table,
  SPIDER_RESULT_LIST *result_list
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  SPIDER_RESULT *current = (SPIDER_RESULT*) result_list->current;
  SPIDER_DB_ROW *row;
  Field **field;
  DBUG_ENTER("spider_db_fetch_table");
    if (result_list->quick_mode == 0)
    {
      SPIDER_DB_RESULT *result = current->result;
      if (!(row = result->fetch_row()))
      {
        table->status = STATUS_NOT_FOUND;
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      }
    } else {
      if (result_list->current_row_num < result_list->quick_page_size)
      {
        if (!current->first_position)
        {
          table->status = STATUS_NOT_FOUND;
          DBUG_RETURN(HA_ERR_END_OF_FILE);
        }
        row = current->first_position[result_list->current_row_num].row;
      } else {
        if ((error_num = spider_db_get_row_from_tmp_tbl(
          current, &row)))
        {
          if (error_num == HA_ERR_END_OF_FILE)
            table->status = STATUS_NOT_FOUND;
          DBUG_RETURN(error_num);
        }
      }
    }

    DBUG_PRINT("info", ("spider row=%p", row));
    DBUG_PRINT("info", ("spider direct_aggregate=%s",
      result_list->direct_aggregate ? "TRUE" : "FALSE"));
    result_list->snap_mrr_with_cnt = spider->mrr_with_cnt;
    result_list->snap_direct_aggregate = result_list->direct_aggregate;
    result_list->snap_row = row;

    /* for mrr */
    if (spider->mrr_with_cnt)
    {
      DBUG_PRINT("info", ("spider mrr_with_cnt"));
      if (spider->sql_kind[spider->result_link_idx] == SPIDER_SQL_KIND_SQL)
      {
        if (!row->is_null())
          spider->multi_range_hit_point = row->val_int();
        else if (result_list->direct_aggregate)
        {
          table->status = STATUS_NOT_FOUND;
          DBUG_RETURN(HA_ERR_END_OF_FILE);
        }
        else
          DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
        row->next();
      } else {
        spider->multi_range_hit_point = 0;
        result_list->snap_mrr_with_cnt = FALSE;
      }
    }

    /* for direct_aggregate */
    if (result_list->direct_aggregate)
    {
      if ((error_num = spider_db_fetch_for_item_sum_funcs(row, spider)))
        DBUG_RETURN(error_num);
    }

    if (!spider->use_fields)
    {
      if ((error_num = spider_db_append_match_fetch(spider,
        spider->ft_first, spider->ft_current, row)))
        DBUG_RETURN(error_num);
    }

    for (
      field = table->field;
      *field;
      field++
    ) {
      if ((
        bitmap_is_set(table->read_set, (*field)->field_index) |
        bitmap_is_set(table->write_set, (*field)->field_index)
      )) {
#ifndef DBUG_OFF
        MY_BITMAP *tmp_map =
          dbug_tmp_use_all_columns(table, &table->write_set);
#endif
        DBUG_PRINT("info", ("spider bitmap is set %s",
          SPIDER_field_name_str(*field)));
        if ((error_num =
          spider_db_fetch_row(share, *field, row, ptr_diff)))
          DBUG_RETURN(error_num);
#ifndef DBUG_OFF
        dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
      } else {
        DBUG_PRINT("info", ("spider bitmap is not set %s",
          SPIDER_field_name_str(*field)));
      }
      row->next();
    }
  table->status = 0;
  DBUG_RETURN(0);
}

int spider_db_fetch_key(
  ha_spider *spider,
  uchar *buf,
  TABLE *table,
  const KEY *key_info,
  SPIDER_RESULT_LIST *result_list
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  SPIDER_RESULT *current = (SPIDER_RESULT*) result_list->current;
  KEY_PART_INFO *key_part;
  uint part_num;
  SPIDER_DB_ROW *row;
  Field *field;
  DBUG_ENTER("spider_db_fetch_key");
  if (result_list->quick_mode == 0)
  {
    SPIDER_DB_RESULT *result = current->result;
    if (!(row = result->fetch_row()))
    {
      table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
  } else {
    if (result_list->current_row_num < result_list->quick_page_size)
    {
      if (!current->first_position)
      {
        table->status = STATUS_NOT_FOUND;
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      }
      row = current->first_position[result_list->current_row_num].row;
    } else {
      if ((error_num = spider_db_get_row_from_tmp_tbl(
        current, &row)))
      {
        if (error_num == HA_ERR_END_OF_FILE)
          table->status = STATUS_NOT_FOUND;
        DBUG_RETURN(error_num);
      }
    }
  }

  DBUG_PRINT("info", ("spider row=%p", row));
  DBUG_PRINT("info", ("spider direct_aggregate=%s",
    result_list->direct_aggregate ? "TRUE" : "FALSE"));
  result_list->snap_mrr_with_cnt = spider->mrr_with_cnt;
  result_list->snap_direct_aggregate = result_list->direct_aggregate;
  result_list->snap_row = row;

  /* for mrr */
  if (spider->mrr_with_cnt)
  {
    DBUG_PRINT("info", ("spider mrr_with_cnt"));
    if (!row->is_null())
      spider->multi_range_hit_point = row->val_int();
    else if (result_list->direct_aggregate)
    {
      table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    else
      DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
    row->next();
  }

  /* for direct_aggregate */
  if (result_list->direct_aggregate)
  {
    if ((error_num = spider_db_fetch_for_item_sum_funcs(row, spider)))
      DBUG_RETURN(error_num);
  }

  if ((error_num = spider_db_append_match_fetch(spider,
    spider->ft_first, spider->ft_current, row)))
    DBUG_RETURN(error_num);

  for (
    key_part = key_info->key_part,
    part_num = 0;
    part_num < spider_user_defined_key_parts(key_info);
    key_part++,
    part_num++
  ) {
    field = key_part->field;
    if ((
      bitmap_is_set(table->read_set, field->field_index) |
      bitmap_is_set(table->write_set, field->field_index)
    )) {
#ifndef DBUG_OFF
      MY_BITMAP *tmp_map =
        dbug_tmp_use_all_columns(table, &table->write_set);
#endif
      DBUG_PRINT("info", ("spider bitmap is set %s",
        SPIDER_field_name_str(field)));
      if ((error_num =
        spider_db_fetch_row(share, field, row, ptr_diff)))
        DBUG_RETURN(error_num);
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
    }
    row->next();
  }
  table->status = 0;
  DBUG_RETURN(0);
}

int spider_db_fetch_minimum_columns(
  ha_spider *spider,
  uchar *buf,
  TABLE *table,
  SPIDER_RESULT_LIST *result_list
) {
  int error_num;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT *current = (SPIDER_RESULT*) result_list->current;
  SPIDER_DB_ROW *row;
  Field **field;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_fetch_minimum_columns");
  if (result_list->quick_mode == 0)
  {
    SPIDER_DB_RESULT *result = current->result;
    if (!(row = result->fetch_row()))
    {
      table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
  } else {
    if (result_list->current_row_num < result_list->quick_page_size)
    {
      DBUG_PRINT("info", ("spider current=%p", current));
      DBUG_PRINT("info", ("spider first_position=%p", current->first_position));
      if (!current->first_position)
      {
        table->status = STATUS_NOT_FOUND;
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      }
      DBUG_PRINT("info", ("spider current_row_num=%lld", result_list->current_row_num));
      DBUG_PRINT("info", ("spider first_position[]=%p", &current->first_position[result_list->current_row_num]));
      row = current->first_position[result_list->current_row_num].row;
    } else {
      if ((error_num = spider_db_get_row_from_tmp_tbl(
        current, &row)))
      {
        if (error_num == HA_ERR_END_OF_FILE)
          table->status = STATUS_NOT_FOUND;
        DBUG_RETURN(error_num);
      }
    }
  }

  DBUG_PRINT("info", ("spider row=%p", row));
  DBUG_PRINT("info", ("spider direct_aggregate=%s",
    result_list->direct_aggregate ? "TRUE" : "FALSE"));
  result_list->snap_mrr_with_cnt = spider->mrr_with_cnt;
  result_list->snap_direct_aggregate = result_list->direct_aggregate;
  result_list->snap_row = row;

  /* for mrr */
  if (spider->mrr_with_cnt)
  {
    DBUG_PRINT("info", ("spider mrr_with_cnt"));
    if (!row->is_null())
      spider->multi_range_hit_point = row->val_int();
    else if (result_list->direct_aggregate)
    {
      table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    else
      DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
    row->next();
  }

  /* for direct_aggregate */
  if (result_list->direct_aggregate)
  {
    if ((error_num = spider_db_fetch_for_item_sum_funcs(row, spider)))
      DBUG_RETURN(error_num);
  }

  if ((error_num = spider_db_append_match_fetch(spider,
    spider->ft_first, spider->ft_current, row)))
    DBUG_RETURN(error_num);

  dbton_hdl = spider->dbton_handler[row->dbton_id];
  for (
    field = table->field;
    *field;
    field++
  ) {
    DBUG_PRINT("info", ("spider field_index %u", (*field)->field_index));
    DBUG_PRINT("info", ("spider searched_bitmap %u",
      spider_bit_is_set(spider->wide_handler->searched_bitmap,
      (*field)->field_index)));
    DBUG_PRINT("info", ("spider read_set %u",
      bitmap_is_set(table->read_set, (*field)->field_index)));
    DBUG_PRINT("info", ("spider write_set %u",
      bitmap_is_set(table->write_set, (*field)->field_index)));
    if (dbton_hdl->minimum_select_bit_is_set((*field)->field_index))
    {
      if ((
        bitmap_is_set(table->read_set, (*field)->field_index) |
        bitmap_is_set(table->write_set, (*field)->field_index)
      )) {
#ifndef DBUG_OFF
        MY_BITMAP *tmp_map =
          dbug_tmp_use_all_columns(table, &table->write_set);
#endif
        DBUG_PRINT("info", ("spider bitmap is set %s",
          SPIDER_field_name_str(*field)));
        if ((error_num = spider_db_fetch_row(share, *field, row, ptr_diff)))
          DBUG_RETURN(error_num);
#ifndef DBUG_OFF
        dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
      }
      row->next();
    }
  }
  table->status = 0;
  DBUG_RETURN(0);
}

void spider_db_free_one_result_for_start_next(
  ha_spider *spider
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_RESULT *result = (SPIDER_RESULT *) result_list->current;
  DBUG_ENTER("spider_db_free_one_result_for_start_next");
  spider_bg_all_conn_break(spider);

    if (result_list->low_mem_read)
    {
      if (result)
      {
        do {
          spider_db_free_one_result(result_list, result);
          DBUG_PRINT("info",("spider result=%p", result));
          DBUG_PRINT("info",("spider result->finish_flg = FALSE"));
          result->finish_flg = FALSE;
          result = (SPIDER_RESULT *) result->next;
        } while (result && (result->result || result->first_position));
        result = (SPIDER_RESULT *) result_list->current;
        if (
          !result->result &&
          !result->first_position &&
          !result->tmp_tbl_use_position
        )
          result_list->current = result->prev;
      }
    } else {
      while (
        result && result->next &&
        (result->next->result || result->next->first_position)
      ) {
        result_list->current = result->next;
        result = (SPIDER_RESULT *) result->next;
      }
    }
  DBUG_VOID_RETURN;
}

void spider_db_free_one_result(
  SPIDER_RESULT_LIST *result_list,
  SPIDER_RESULT *result
) {
  DBUG_ENTER("spider_db_free_one_result");
  if (result_list->quick_mode == 0)
  {
    if (
      !result->use_position &&
      result->result
    ) {
      result->result->free_result();
      delete result->result;
      result->result = NULL;
    }
  } else {
    int roop_count;
    SPIDER_POSITION *position = result->first_position;
    if (position)
    {
      for (roop_count = 0; roop_count < result->pos_page_size; roop_count++)
      {
        if (
          position[roop_count].row &&
          !position[roop_count].use_position
        ) {
          delete position[roop_count].row;
          position[roop_count].row = NULL;
        }
      }
      if (result_list->quick_mode == 3)
      {
        if (!result->first_pos_use_position)
        {
          spider_free(spider_current_trx, position, MYF(0));
          result->first_position = NULL;
        }
        if (result->result)
        {
          result->result->free_result();
          if (!result->tmp_tbl_use_position)
          {
            delete result->result;
            result->result = NULL;
          }
        }
        if (!result->tmp_tbl_use_position)
        {
          if (result->result_tmp_tbl)
          {
            if (result->result_tmp_tbl_inited)
            {
              result->result_tmp_tbl->file->ha_rnd_end();
              result->result_tmp_tbl_inited = 0;
            }
            spider_rm_sys_tmp_table_for_result(result->result_tmp_tbl_thd,
              result->result_tmp_tbl, &result->result_tmp_tbl_prm);
            result->result_tmp_tbl = NULL;
            result->result_tmp_tbl_thd = NULL;
          }
        }
      }
    }
  }
  DBUG_VOID_RETURN;
}

void spider_db_free_one_quick_result(
  SPIDER_RESULT *result
) {
  DBUG_ENTER("spider_db_free_one_quick_result");
  if (result && result->result)
  {
    result->result->free_result();
    if (!result->result_tmp_tbl)
    {
      delete result->result;
      result->result = NULL;
    }
  }
  DBUG_VOID_RETURN;
}

int spider_db_free_result(
  ha_spider *spider,
  bool final
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_RESULT *result;
  SPIDER_RESULT *prev;
  SPIDER_SHARE *share = spider->share;
  SPIDER_TRX *trx = spider->wide_handler->trx;
  SPIDER_POSITION *position;
  int roop_count, error_num;
  DBUG_ENTER("spider_db_free_result");
  spider_bg_all_conn_break(spider);
  result = (SPIDER_RESULT*) result_list->first;

  while (result_list->tmp_pos_row_first)
  {
    SPIDER_DB_ROW *tmp_pos_row = result_list->tmp_pos_row_first;
    result_list->tmp_pos_row_first = tmp_pos_row->next_pos;
    delete tmp_pos_row;
  }

  if (
    final ||
    spider_param_reset_sql_alloc(trx->thd, share->reset_sql_alloc) == 1
  ) {
    int alloc_size = final ? 0 :
      (spider_param_init_sql_alloc_size(trx->thd, share->init_sql_alloc_size));
    while (result)
    {
      position = result->first_position;
      if (position)
      {
        for (roop_count = 0; roop_count < result->pos_page_size; roop_count++)
        {
          if (position[roop_count].row)
          {
            delete position[roop_count].row;
          }
        }
        spider_free(spider_current_trx, position, MYF(0));
      }
      if (result->result)
      {
        result->result->free_result();
        delete result->result;
        result->result = NULL;
      }
      if (result->result_tmp_tbl)
      {
        if (result->result_tmp_tbl_inited)
        {
          result->result_tmp_tbl->file->ha_rnd_end();
          result->result_tmp_tbl_inited = 0;
        }
        spider_rm_sys_tmp_table_for_result(result->result_tmp_tbl_thd,
          result->result_tmp_tbl, &result->result_tmp_tbl_prm);
        result->result_tmp_tbl = NULL;
        result->result_tmp_tbl_thd = NULL;
      }
      prev = result;
      result = (SPIDER_RESULT*) result->next;
      spider_free(spider_current_trx, prev, MYF(0));
    }
    result_list->first = NULL;
    result_list->last = NULL;
    if (!final)
    {
      ulong realloced = 0;
      int init_sql_alloc_size =
        spider_param_init_sql_alloc_size(trx->thd, share->init_sql_alloc_size);
      for (roop_count = 0; roop_count < (int) share->use_dbton_count;
        roop_count++)
      {
        uint dbton_id = share->use_dbton_ids[roop_count];
        if ((error_num = spider->dbton_handler[dbton_id]->
          realloc_sql(&realloced)))
        {
          DBUG_RETURN(error_num);
        }
      }
      if (realloced & (SPIDER_SQL_TYPE_SELECT_SQL | SPIDER_SQL_TYPE_HANDLER))
      {
        for (roop_count = 0; roop_count < (int) share->link_count;
          roop_count++)
        {
          if ((int) result_list->sqls[roop_count].alloced_length() >
            alloc_size * 2)
          {
            result_list->sqls[roop_count].free();
            if (result_list->sqls[roop_count].real_alloc(
              init_sql_alloc_size))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
        }
      }
      if (realloced & SPIDER_SQL_TYPE_INSERT_SQL)
      {
        for (roop_count = 0; roop_count < (int) share->link_count;
          roop_count++)
        {
          if ((int) result_list->insert_sqls[roop_count].alloced_length() >
            alloc_size * 2)
          {
            result_list->insert_sqls[roop_count].free();
            if (result_list->insert_sqls[roop_count].real_alloc(
              init_sql_alloc_size))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
        }
      }
      if (realloced & SPIDER_SQL_TYPE_UPDATE_SQL)
      {
        for (roop_count = 0; roop_count < (int) share->link_count;
          roop_count++)
        {
          if ((int) result_list->update_sqls[roop_count].alloced_length() >
            alloc_size * 2)
          {
            result_list->update_sqls[roop_count].free();
            if (result_list->update_sqls[roop_count].real_alloc(
              init_sql_alloc_size))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
        }
      }
      if ((error_num = spider->reset_sql_sql(SPIDER_SQL_TYPE_BULK_UPDATE_SQL)))
        DBUG_RETURN(error_num);

      if (realloced & SPIDER_SQL_TYPE_TMP_SQL)
      {
        for (roop_count = 0; roop_count < (int) share->link_count;
          roop_count++)
        {
          if ((int) result_list->tmp_sqls[roop_count].alloced_length() >
            alloc_size * 2)
          {
            result_list->tmp_sqls[roop_count].free();
            if (result_list->tmp_sqls[roop_count].real_alloc(
              init_sql_alloc_size))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
        }
      }
    }
  } else {
    while (result)
    {
      position = result->first_position;
      if (position)
      {
        for (roop_count = 0; roop_count < result->pos_page_size; roop_count++)
        {
          if (position[roop_count].row)
          {
            delete position[roop_count].row;
          }
        }
        spider_free(spider_current_trx, position, MYF(0));
      }
      result->first_position = NULL;
      if (result->result)
      {
        result->result->free_result();
        delete result->result;
        result->result = NULL;
      }
      if (result->result_tmp_tbl)
      {
        if (result->result_tmp_tbl_inited)
        {
          result->result_tmp_tbl->file->ha_rnd_end();
          result->result_tmp_tbl_inited = 0;
        }
        spider_rm_sys_tmp_table_for_result(result->result_tmp_tbl_thd,
          result->result_tmp_tbl, &result->result_tmp_tbl_prm);
        result->result_tmp_tbl = NULL;
        result->result_tmp_tbl_thd = NULL;
      }
      result->record_num = 0;
      DBUG_PRINT("info",("spider result->finish_flg = FALSE"));
      result->finish_flg = FALSE;
      result->first_pos_use_position = FALSE;
      result->tmp_tbl_use_position = FALSE;
      result->use_position = FALSE;
      result = (SPIDER_RESULT*) result->next;
    }
  }
  result_list->current = NULL;
  result_list->record_num = 0;
  DBUG_PRINT("info",("spider result_list->finish_flg = FALSE"));
  result_list->finish_flg = FALSE;
  result_list->quick_phase = 0;
  result_list->bgs_phase = 0;
  DBUG_RETURN(0);
}

int spider_db_store_result(
  ha_spider *spider,
  int link_idx,
  TABLE *table
) {
  int error_num;
  SPIDER_CONN *conn;
  SPIDER_DB_CONN *db_conn;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_RESULT *current;
  DBUG_ENTER("spider_db_store_result");
    conn = spider->conns[link_idx];
    DBUG_PRINT("info",("spider conn->connection_id=%llu",
      conn->connection_id));
    DBUG_PRINT("info",("spider spider->connection_ids[%d]=%llu",
      link_idx, spider->connection_ids[link_idx]));
    if (conn->connection_id != spider->connection_ids[link_idx])
    {
      my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
        ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
      if (!conn->mta_conn_mutex_unlock_later)
      {
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM);
    }
    db_conn = conn->db_conn;
    if (!result_list->current)
    {
      if (!result_list->first)
      {
        if (!(result_list->first = (SPIDER_RESULT *)
          spider_malloc(spider_current_trx, 4, sizeof(*result_list->first),
            MYF(MY_WME | MY_ZEROFILL)))
        ) {
          if (!conn->mta_conn_mutex_unlock_later)
          {
            DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        TMP_TABLE_PARAM *tmp_tbl_prm = (TMP_TABLE_PARAM *)
          &result_list->first->result_tmp_tbl_prm;
        tmp_tbl_prm->init();
        tmp_tbl_prm->field_count = 3;
        result_list->last = result_list->first;
        result_list->current = result_list->first;
      } else {
        result_list->current = result_list->first;
      }
      result_list->bgs_current = result_list->current;
      current = (SPIDER_RESULT*) result_list->current;
    } else {
      if (
        result_list->bgs_phase > 0 ||
        result_list->quick_phase > 0
      ) {
        if (result_list->bgs_current == result_list->last)
        {
          if (!(result_list->last = (SPIDER_RESULT *)
            spider_malloc(spider_current_trx, 5, sizeof(*result_list->last),
               MYF(MY_WME | MY_ZEROFILL)))
          ) {
            if (!conn->mta_conn_mutex_unlock_later)
            {
              DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
          TMP_TABLE_PARAM *tmp_tbl_prm = (TMP_TABLE_PARAM *)
            &result_list->last->result_tmp_tbl_prm;
          tmp_tbl_prm->init();
          tmp_tbl_prm->field_count = 3;
          result_list->bgs_current->next = result_list->last;
          result_list->last->prev = result_list->bgs_current;
          result_list->bgs_current = result_list->last;
        } else {
          result_list->bgs_current = result_list->bgs_current->next;
        }
        if (
          result_list->bgs_phase == 1 ||
          result_list->quick_phase == 2
        ) {
          if (result_list->low_mem_read &&
            result_list->current->result->limit_mode() == 0)
          {
            do {
              spider_db_free_one_result(result_list,
                (SPIDER_RESULT*) result_list->current);
              result_list->current = result_list->current->next;
            } while (result_list->current != result_list->bgs_current);
          } else {
            result_list->current = result_list->bgs_current;
          }
          result_list->quick_phase = 0;
        }
        current = (SPIDER_RESULT*) result_list->bgs_current;
      } else {
        if (result_list->current == result_list->last)
        {
          if (!(result_list->last = (SPIDER_RESULT *)
            spider_malloc(spider_current_trx, 6, sizeof(*result_list->last),
              MYF(MY_WME | MY_ZEROFILL)))
          ) {
            if (!conn->mta_conn_mutex_unlock_later)
            {
              DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
          TMP_TABLE_PARAM *tmp_tbl_prm = (TMP_TABLE_PARAM *)
            &result_list->last->result_tmp_tbl_prm;
          tmp_tbl_prm->init();
          tmp_tbl_prm->field_count = 3;
          result_list->current->next = result_list->last;
          result_list->last->prev = result_list->current;
          result_list->current = result_list->last;
        } else {
          result_list->current = result_list->current->next;
        }
        result_list->bgs_current = result_list->current;
        current = (SPIDER_RESULT*) result_list->current;
      }
    }

    if (result_list->quick_mode == 0)
    {
      if (spider_bit_is_set(spider->db_request_phase, link_idx))
      {
        spider_clear_bit(spider->db_request_phase, link_idx);
      }
      st_spider_db_request_key request_key;
      request_key.spider_thread_id =
        spider->wide_handler->trx->spider_thread_id;
      request_key.query_id = spider->wide_handler->trx->thd->query_id;
      request_key.handler = spider;
      request_key.request_id = spider->db_request_id[link_idx];
      request_key.next = NULL;
      if (!(current->result = db_conn->store_result(NULL, &request_key,
        &error_num)))
      {
        if (error_num && error_num != HA_ERR_END_OF_FILE)
        {
          if (!conn->mta_conn_mutex_unlock_later)
          {
            DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
          DBUG_RETURN(error_num);
        }
        bool call_db_errorno = FALSE;
        if (error_num != HA_ERR_END_OF_FILE)
        {
          call_db_errorno = TRUE;
          if ((error_num = spider_db_errorno(conn)))
            DBUG_RETURN(error_num);
        }
        DBUG_PRINT("info",("spider set finish_flg point 1"));
        DBUG_PRINT("info",("spider current->finish_flg = TRUE"));
        DBUG_PRINT("info",("spider result_list->finish_flg = TRUE"));
        current->finish_flg = TRUE;
        result_list->finish_flg = TRUE;
        if (result_list->bgs_phase <= 1)
        {
          result_list->current_row_num = 0;
          table->status = STATUS_NOT_FOUND;
        }
        if (!conn->mta_conn_mutex_unlock_later && !call_db_errorno)
        {
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      } else {
        if (!conn->mta_conn_mutex_unlock_later)
        {
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        current->record_num = current->result->num_rows();
        current->dbton_id = current->result->dbton_id;
        result_list->record_num += current->record_num;
        DBUG_PRINT("info",("spider current->record_num=%lld",
          current->record_num));
        DBUG_PRINT("info",("spider result_list->record_num=%lld",
          result_list->record_num));
        DBUG_PRINT("info",("spider result_list->internal_limit=%lld",
          result_list->internal_limit));
        DBUG_PRINT("info",("spider result_list->split_read=%lld",
          result_list->split_read));
        if (
          result_list->internal_limit <= result_list->record_num ||
          result_list->split_read > current->record_num
        ) {
          DBUG_PRINT("info",("spider set finish_flg point 2"));
          DBUG_PRINT("info",("spider current->finish_flg = TRUE"));
          DBUG_PRINT("info",("spider result_list->finish_flg = TRUE"));
          current->finish_flg = TRUE;
          result_list->finish_flg = TRUE;
        }
        if (result_list->bgs_phase <= 1)
        {
          result_list->current_row_num = 0;
        }
      }
    } else {
      /* has_result() for case of result with result_tmp_tbl */
      if (current->prev && current->prev->result &&
        current->prev->result->has_result())
      {
        current->result = current->prev->result;
        current->prev->result = NULL;
        result_list->limit_num -= current->prev->record_num;
        if (!conn->mta_conn_mutex_unlock_later)
        {
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
      } else {
        if (spider_bit_is_set(spider->db_request_phase, link_idx))
        {
          spider_clear_bit(spider->db_request_phase, link_idx);
        }
        st_spider_db_request_key request_key;
        request_key.spider_thread_id =
          spider->wide_handler->trx->spider_thread_id;
        request_key.query_id = spider->wide_handler->trx->thd->query_id;
        request_key.handler = spider;
        request_key.request_id = spider->db_request_id[link_idx];
        request_key.next = NULL;
        if (!(current->result = conn->db_conn->use_result(spider, &request_key,
          &error_num)))
        {
          if (!error_num)
          {
            error_num = spider_db_errorno(conn);
          } else {
            if (!conn->mta_conn_mutex_unlock_later)
            {
              DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
          }
          DBUG_RETURN(error_num);
        }
        DBUG_PRINT("info", ("spider conn[%p]->quick_target=%p", conn, spider));
        conn->quick_target = spider;
        spider->quick_targets[link_idx] = spider;
        if (!conn->mta_conn_mutex_unlock_later)
        {
          DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
      }
      current->dbton_id = current->result->dbton_id;
      SPIDER_DB_ROW *row;
      if (!(row = current->result->fetch_row()))
      {
        error_num = current->result->get_errno();
        DBUG_PRINT("info",("spider set finish_flg point 3"));
        DBUG_PRINT("info",("spider current->finish_flg = TRUE"));
        DBUG_PRINT("info",("spider result_list->finish_flg = TRUE"));
        current->finish_flg = TRUE;
        result_list->finish_flg = TRUE;
        current->result->free_result();
        delete current->result;
        current->result = NULL;
        DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
        conn->quick_target = NULL;
        spider->quick_targets[link_idx] = NULL;
        if (
          result_list->bgs_phase <= 1 &&
          result_list->quick_phase == 0
        ) {
          result_list->current_row_num = 0;
          table->status = STATUS_NOT_FOUND;
        }
        if (error_num)
          DBUG_RETURN(error_num);
        else if (result_list->quick_phase > 0)
          DBUG_RETURN(0);
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      }
      SPIDER_DB_ROW *tmp_row;
      uint field_count = current->result->num_fields();
      SPIDER_POSITION *position;
      longlong page_size;
      int roop_count = 0;
      if (!result_list->quick_page_size)
      {
        if (result_list->quick_mode == 3)
        {
          page_size = 0;
        } else {
          result_list->quick_page_size = result_list->limit_num;
          page_size = result_list->limit_num;
        }
      } else {
        page_size =
          result_list->limit_num < result_list->quick_page_size ?
          result_list->limit_num : result_list->quick_page_size;
      }
      current->field_count = field_count;
      if (!(position = (SPIDER_POSITION *)
        spider_bulk_malloc(spider_current_trx, 7, MYF(MY_WME | MY_ZEROFILL),
          &position, (uint) (sizeof(SPIDER_POSITION) * page_size),
          &tmp_row, (uint) (sizeof(SPIDER_DB_ROW) * field_count),
          NullS))
      )
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      current->pos_page_size = (int) page_size;
      current->first_position = position;
      current->tmp_tbl_row = tmp_row;
      if (result_list->quick_mode == 3)
      {
        while (page_size > roop_count && row)
        {
          if (result_list->quick_page_byte < row->get_byte_size())
          {
            current->pos_page_size = roop_count;
            page_size = roop_count;
            result_list->quick_page_size = roop_count;
            result_list->quick_page_byte = 0;
            break;
          } else {
            result_list->quick_page_byte -= row->get_byte_size();
          }
          if (!(position->row = row->clone()))
          {
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
          position++;
          roop_count++;
          row = current->result->fetch_row();
        }
      } else {
        do {
          if (!(position->row = row->clone()))
          {
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
          position++;
          roop_count++;
          if (result_list->quick_page_byte < row->get_byte_size())
          {
            current->pos_page_size = roop_count;
            page_size = roop_count;
            result_list->quick_page_size = roop_count;
            result_list->quick_page_byte = 0;
            break;
          } else {
            result_list->quick_page_byte -= row->get_byte_size();
          }
        } while (
          page_size > roop_count &&
          (row = current->result->fetch_row())
        );
      }
      if (
        result_list->quick_mode == 3 &&
        page_size == roop_count &&
        result_list->limit_num > roop_count &&
        row
      ) {
        THD *thd = current_thd;
        char buf[MAX_FIELD_WIDTH];
        spider_string tmp_str(buf, MAX_FIELD_WIDTH, &my_charset_bin);
        tmp_str.init_calc_mem(120);

        DBUG_PRINT("info",("spider store result to temporary table"));
        DBUG_ASSERT(!current->result_tmp_tbl);
#ifdef SPIDER_use_LEX_CSTRING_for_Field_blob_constructor
        LEX_CSTRING field_name1 = {STRING_WITH_LEN("a")};
        LEX_CSTRING field_name2 = {STRING_WITH_LEN("b")};
        LEX_CSTRING field_name3 = {STRING_WITH_LEN("c")};
        if (!(current->result_tmp_tbl = spider_mk_sys_tmp_table_for_result(
          thd, table, &current->result_tmp_tbl_prm, &field_name1, &field_name2,
          &field_name3, &my_charset_bin)))
#else
        if (!(current->result_tmp_tbl = spider_mk_sys_tmp_table_for_result(
          thd, table, &current->result_tmp_tbl_prm, "a", "b", "c",
          &my_charset_bin)))
#endif
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        current->result_tmp_tbl_thd = thd;
        TABLE *tmp_tbl = current->result_tmp_tbl;
        tmp_tbl->file->extra(HA_EXTRA_WRITE_CACHE);
        tmp_tbl->file->ha_start_bulk_insert((ha_rows) 0);
        do {
          if ((error_num = row->store_to_tmp_table(tmp_tbl, &tmp_str)))
          {
            tmp_tbl->file->ha_end_bulk_insert();
            DBUG_RETURN(error_num);
          }
          roop_count++;
        } while (
          result_list->limit_num > roop_count &&
          (row = current->result->fetch_row())
        );
        tmp_tbl->file->ha_end_bulk_insert();
        page_size = result_list->limit_num;
      }
      current->record_num = roop_count;
      result_list->record_num += roop_count;
      if (
        result_list->internal_limit <= result_list->record_num ||
        page_size > roop_count ||
        (
          result_list->quick_mode == 3 &&
          result_list->limit_num > roop_count
        )
      ) {
        DBUG_PRINT("info",("spider set finish_flg point 4"));
        DBUG_PRINT("info",("spider current->finish_flg = TRUE"));
        DBUG_PRINT("info",("spider result_list->finish_flg = TRUE"));
        current->finish_flg = TRUE;
        result_list->finish_flg = TRUE;
        current->result->free_result();
        if (!current->result_tmp_tbl)
        {
          delete current->result;
          current->result = NULL;
        }
        DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
        conn->quick_target = NULL;
        spider->quick_targets[link_idx] = NULL;
      } else if (
        result_list->quick_mode == 3 ||
        result_list->limit_num == roop_count
      ) {
        if (
          result_list->limit_num != roop_count ||
          conn->db_conn->limit_mode() != 1
        ) {
          current->result->free_result();
          if (!current->result_tmp_tbl)
          {
            delete current->result;
            current->result = NULL;
          }
          DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
          conn->quick_target = NULL;
          spider->quick_targets[link_idx] = NULL;
        }
      }
      DBUG_PRINT("info", ("spider bgs_phase=%d", result_list->bgs_phase));
      DBUG_PRINT("info", ("spider quick_phase=%d", result_list->quick_phase));
      if (
        result_list->bgs_phase <= 1 &&
        result_list->quick_phase == 0
      ) {
        result_list->current_row_num = 0;
      }
      DBUG_PRINT("info", ("spider result_list->current=%p", result_list->current));
      DBUG_PRINT("info", ("spider current=%p", current));
      DBUG_PRINT("info", ("spider first_position=%p", current->first_position));
      DBUG_PRINT("info", ("spider current_row_num=%lld", result_list->current_row_num));
      DBUG_PRINT("info", ("spider first_position[]=%p", &current->first_position[result_list->current_row_num]));
      DBUG_PRINT("info", ("spider row=%p", current->first_position[result_list->current_row_num].row));
    }
  DBUG_RETURN(0);
}

int spider_db_store_result_for_reuse_cursor(
  ha_spider *spider,
  int link_idx,
  TABLE *table
) {
  int error_num;
  SPIDER_CONN *conn;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_RESULT *current;
  DBUG_ENTER("spider_db_store_result_for_reuse_cursor");
  conn = spider->conns[link_idx];
  DBUG_PRINT("info",("spider conn->connection_id=%llu",
    conn->connection_id));
  DBUG_PRINT("info",("spider spider->connection_ids[%d]=%llu",
    link_idx, spider->connection_ids[link_idx]));
  if (conn->connection_id != spider->connection_ids[link_idx])
  {
    my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
      ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM);
  }
  if (!result_list->current)
  {
    if (!result_list->first)
    {
      if (!(result_list->first = (SPIDER_RESULT *)
        spider_malloc(spider_current_trx, 4, sizeof(*result_list->first),
          MYF(MY_WME | MY_ZEROFILL)))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      TMP_TABLE_PARAM *tmp_tbl_prm = (TMP_TABLE_PARAM *)
        &result_list->first->result_tmp_tbl_prm;
      tmp_tbl_prm->init();
      tmp_tbl_prm->field_count = 3;
      result_list->last = result_list->first;
      result_list->current = result_list->first;
    } else {
      result_list->current = result_list->first;
    }
    result_list->bgs_current = result_list->current;
    current = (SPIDER_RESULT*) result_list->current;
  } else {
    if (
      result_list->bgs_phase > 0 ||
      result_list->quick_phase > 0
    ) {
      if (result_list->bgs_current == result_list->last)
      {
        if (!(result_list->last = (SPIDER_RESULT *)
          spider_malloc(spider_current_trx, 5, sizeof(*result_list->last),
             MYF(MY_WME | MY_ZEROFILL)))
        ) {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        TMP_TABLE_PARAM *tmp_tbl_prm = (TMP_TABLE_PARAM *)
          &result_list->last->result_tmp_tbl_prm;
        tmp_tbl_prm->init();
        tmp_tbl_prm->field_count = 3;
        result_list->bgs_current->next = result_list->last;
        result_list->last->prev = result_list->bgs_current;
        result_list->bgs_current = result_list->last;
      } else {
        result_list->bgs_current = result_list->bgs_current->next;
      }
      if (
        result_list->bgs_phase == 1 ||
        result_list->quick_phase == 2
      ) {
        result_list->current = result_list->bgs_current;
        result_list->quick_phase = 0;
      }
      current = (SPIDER_RESULT*) result_list->bgs_current;
    } else {
      if (result_list->current == result_list->last)
      {
        if (!(result_list->last = (SPIDER_RESULT *)
          spider_malloc(spider_current_trx, 6, sizeof(*result_list->last),
            MYF(MY_WME | MY_ZEROFILL)))
        ) {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        TMP_TABLE_PARAM *tmp_tbl_prm = (TMP_TABLE_PARAM *)
          &result_list->last->result_tmp_tbl_prm;
        tmp_tbl_prm->init();
        tmp_tbl_prm->field_count = 3;
        result_list->current->next = result_list->last;
        result_list->last->prev = result_list->current;
        result_list->current = result_list->last;
      } else {
        result_list->current = result_list->current->next;
      }
      result_list->bgs_current = result_list->current;
      current = (SPIDER_RESULT*) result_list->current;
    }
  }

  if (result_list->quick_mode == 0)
  {
    if (spider_bit_is_set(spider->db_request_phase, link_idx))
    {
      spider_clear_bit(spider->db_request_phase, link_idx);
    }
    current->result = current->prev->result;
    current->result->set_limit(result_list->limit_num);
    current->record_num = current->result->num_rows();
    current->dbton_id = current->result->dbton_id;
    result_list->record_num += current->record_num;
    DBUG_PRINT("info",("spider current->record_num=%lld",
      current->record_num));
    DBUG_PRINT("info",("spider result_list->record_num=%lld",
      result_list->record_num));
    DBUG_PRINT("info",("spider result_list->internal_limit=%lld",
      result_list->internal_limit));
    DBUG_PRINT("info",("spider result_list->split_read=%lld",
      result_list->split_read));
    if (
      result_list->internal_limit <= result_list->record_num ||
      result_list->split_read > current->record_num
    ) {
      DBUG_PRINT("info",("spider set finish_flg point 2"));
      DBUG_PRINT("info",("spider current->finish_flg = TRUE"));
      DBUG_PRINT("info",("spider result_list->finish_flg = TRUE"));
      current->finish_flg = TRUE;
      result_list->finish_flg = TRUE;
    }
    if (result_list->bgs_phase <= 1)
    {
      result_list->current_row_num = 0;
    }
  } else {
    DBUG_ASSERT(current->prev);
    DBUG_ASSERT(current->prev->result);
    /* has_result() for case of result with result_tmp_tbl */
    if (current->prev->result->has_result())
    {
      current->result = current->prev->result;
      current->result->set_limit(result_list->limit_num);
      current->prev->result = NULL;
      result_list->limit_num -= current->prev->record_num;
    } else {
      if (spider_bit_is_set(spider->db_request_phase, link_idx))
      {
        spider_clear_bit(spider->db_request_phase, link_idx);
      }
      current->result = current->prev->result;
      current->result->set_limit(result_list->limit_num);
      DBUG_PRINT("info", ("spider conn[%p]->quick_target=%p", conn, spider));
      conn->quick_target = spider;
      spider->quick_targets[link_idx] = spider;
    }
    current->dbton_id = current->result->dbton_id;
    SPIDER_DB_ROW *row;
    if (!(row = current->result->fetch_row()))
    {
      error_num = current->result->get_errno();
      DBUG_PRINT("info",("spider set finish_flg point 3"));
      DBUG_PRINT("info",("spider current->finish_flg = TRUE"));
      DBUG_PRINT("info",("spider result_list->finish_flg = TRUE"));
      current->finish_flg = TRUE;
      result_list->finish_flg = TRUE;
      current->result->free_result();
      delete current->result;
      current->result = NULL;
      DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
      conn->quick_target = NULL;
      spider->quick_targets[link_idx] = NULL;
      if (
        result_list->bgs_phase <= 1 &&
        result_list->quick_phase == 0
      ) {
        result_list->current_row_num = 0;
        table->status = STATUS_NOT_FOUND;
      }
      if (error_num && error_num != HA_ERR_END_OF_FILE)
        DBUG_RETURN(error_num);
      /* This shouldn't return HA_ERR_END_OF_FILE */
      DBUG_RETURN(0);
    }
    SPIDER_DB_ROW *tmp_row;
    uint field_count = current->result->num_fields();
    SPIDER_POSITION *position;
    longlong page_size;
    int roop_count = 0;
    if (!result_list->quick_page_size)
    {
      if (result_list->quick_mode == 3)
      {
        page_size = 0;
      } else {
        result_list->quick_page_size = result_list->limit_num;
        page_size = result_list->limit_num;
      }
    } else {
      page_size =
        result_list->limit_num < result_list->quick_page_size ?
        result_list->limit_num : result_list->quick_page_size;
    }
    current->field_count = field_count;
    if (!(position = (SPIDER_POSITION *)
      spider_bulk_malloc(spider_current_trx, 7, MYF(MY_WME | MY_ZEROFILL),
        &position, (uint) (sizeof(SPIDER_POSITION) * page_size),
        &tmp_row, (uint) (sizeof(SPIDER_DB_ROW) * field_count),
        NullS))
    )
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    current->pos_page_size = (int) page_size;
    current->first_position = position;
    current->tmp_tbl_row = tmp_row;
    if (result_list->quick_mode == 3)
    {
      while (page_size > roop_count && row)
      {
        if (result_list->quick_page_byte < row->get_byte_size())
        {
          current->pos_page_size = roop_count;
          page_size = roop_count;
          result_list->quick_page_size = roop_count;
          result_list->quick_page_byte = 0;
          break;
        } else {
          result_list->quick_page_byte -= row->get_byte_size();
        }
        if (!(position->row = row->clone()))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        position++;
        roop_count++;
        row = current->result->fetch_row();
      }
    } else {
      do {
        if (!(position->row = row->clone()))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        position++;
        roop_count++;
        if (result_list->quick_page_byte < row->get_byte_size())
        {
          current->pos_page_size = roop_count;
          page_size = roop_count;
          result_list->quick_page_size = roop_count;
          result_list->quick_page_byte = 0;
          break;
        } else {
          result_list->quick_page_byte -= row->get_byte_size();
        }
      } while (
        page_size > roop_count &&
        (row = current->result->fetch_row())
      );
    }
    if (
      result_list->quick_mode == 3 &&
      page_size == roop_count &&
      result_list->limit_num > roop_count &&
      row
    ) {
      THD *thd = current_thd;
      char buf[MAX_FIELD_WIDTH];
      spider_string tmp_str(buf, MAX_FIELD_WIDTH, &my_charset_bin);
      tmp_str.init_calc_mem(120);

      DBUG_PRINT("info",("spider store result to temporary table"));
      DBUG_ASSERT(!current->result_tmp_tbl);
#ifdef SPIDER_use_LEX_CSTRING_for_Field_blob_constructor
      LEX_CSTRING field_name1 = {STRING_WITH_LEN("a")};
      LEX_CSTRING field_name2 = {STRING_WITH_LEN("b")};
      LEX_CSTRING field_name3 = {STRING_WITH_LEN("c")};
      if (!(current->result_tmp_tbl = spider_mk_sys_tmp_table_for_result(
        thd, table, &current->result_tmp_tbl_prm, &field_name1, &field_name2,
        &field_name3, &my_charset_bin)))
#else
      if (!(current->result_tmp_tbl = spider_mk_sys_tmp_table_for_result(
        thd, table, &current->result_tmp_tbl_prm, "a", "b", "c",
        &my_charset_bin)))
#endif
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      current->result_tmp_tbl_thd = thd;
      TABLE *tmp_tbl = current->result_tmp_tbl;
      tmp_tbl->file->extra(HA_EXTRA_WRITE_CACHE);
      tmp_tbl->file->ha_start_bulk_insert((ha_rows) 0);
      do {
        if ((error_num = row->store_to_tmp_table(tmp_tbl, &tmp_str)))
        {
          tmp_tbl->file->ha_end_bulk_insert();
          DBUG_RETURN(error_num);
        }
        roop_count++;
      } while (
        result_list->limit_num > roop_count &&
        (row = current->result->fetch_row())
      );
      tmp_tbl->file->ha_end_bulk_insert();
      page_size = result_list->limit_num;
    }
    current->record_num = roop_count;
    result_list->record_num += roop_count;
    if (
      result_list->internal_limit <= result_list->record_num ||
      page_size > roop_count ||
      (
        result_list->quick_mode == 3 &&
        result_list->limit_num > roop_count
      )
    ) {
      DBUG_PRINT("info",("spider set finish_flg point 4"));
      DBUG_PRINT("info",("spider current->finish_flg = TRUE"));
      DBUG_PRINT("info",("spider result_list->finish_flg = TRUE"));
      current->finish_flg = TRUE;
      result_list->finish_flg = TRUE;
      current->result->free_result();
      if (!current->result_tmp_tbl)
      {
        delete current->result;
        current->result = NULL;
      }
      DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
      conn->quick_target = NULL;
      spider->quick_targets[link_idx] = NULL;
    } else if (
      result_list->quick_mode == 3 ||
      result_list->limit_num == roop_count
    ) {
      if (result_list->limit_num != roop_count)
      {
        current->result->free_result();
        if (!current->result_tmp_tbl)
        {
          delete current->result;
          current->result = NULL;
        }
        DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
        conn->quick_target = NULL;
        spider->quick_targets[link_idx] = NULL;
      }
    }
    DBUG_PRINT("info", ("spider bgs_phase=%d", result_list->bgs_phase));
    DBUG_PRINT("info", ("spider quick_phase=%d", result_list->quick_phase));
    if (
      result_list->bgs_phase <= 1 &&
      result_list->quick_phase == 0
    ) {
      result_list->current_row_num = 0;
    }
    DBUG_PRINT("info", ("spider result_list->current=%p", result_list->current));
    DBUG_PRINT("info", ("spider current=%p", current));
    DBUG_PRINT("info", ("spider first_position=%p", current->first_position));
    DBUG_PRINT("info", ("spider current_row_num=%lld", result_list->current_row_num));
    DBUG_PRINT("info", ("spider first_position[]=%p", &current->first_position[result_list->current_row_num]));
    DBUG_PRINT("info", ("spider row=%p", current->first_position[result_list->current_row_num].row));
  }
  DBUG_RETURN(0);
}

void spider_db_discard_result(
  ha_spider *spider,
  int link_idx,
  SPIDER_CONN *conn
) {
  int error_num;
  SPIDER_DB_RESULT *result;
  DBUG_ENTER("spider_db_discard_result");
  if (spider_bit_is_set(spider->db_request_phase, link_idx))
  {
    spider_clear_bit(spider->db_request_phase, link_idx);
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = spider->wide_handler->trx->spider_thread_id;
  request_key.query_id = spider->wide_handler->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = spider->db_request_id[link_idx];
  request_key.next = NULL;
  if ((result = conn->db_conn->use_result(spider, &request_key, &error_num)))
  {
    result->free_result();
    delete result;
  }
  DBUG_VOID_RETURN;
}

void spider_db_discard_multiple_result(
  ha_spider *spider,
  int link_idx,
  SPIDER_CONN *conn
) {
  int error_num;
  SPIDER_DB_RESULT *result;
  st_spider_db_request_key request_key;
  DBUG_ENTER("spider_db_discard_multiple_result");
  if (spider_bit_is_set(spider->db_request_phase, link_idx))
  {
    spider_clear_bit(spider->db_request_phase, link_idx);
  }
  request_key.spider_thread_id = spider->wide_handler->trx->spider_thread_id;
  request_key.query_id = spider->wide_handler->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = spider->db_request_id[link_idx];
  request_key.next = NULL;
  do
  {
    if (!conn->db_conn->cmp_request_key_to_snd(&request_key))
      break;
    if ((result = conn->db_conn->use_result(spider, &request_key, &error_num)))
    {
      result->free_result();
      delete result;
    }
  } while (!conn->db_conn->next_result());
  DBUG_VOID_RETURN;
}


int spider_db_fetch(
  uchar *buf,
  ha_spider *spider,
  TABLE *table
) {
  int error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_fetch");
  if (spider->sql_kind[spider->result_link_idx] == SPIDER_SQL_KIND_SQL)
  {
    if (!spider->select_column_mode) {
      if (result_list->keyread)
        error_num = spider_db_fetch_key(spider, buf, table,
          result_list->key_info, result_list);
      else
        error_num = spider_db_fetch_table(spider, buf, table,
          result_list);
    } else
      error_num = spider_db_fetch_minimum_columns(spider, buf, table,
        result_list);
  } else {
    error_num = spider_db_fetch_table(spider, buf, table,
      result_list);
  }
  result_list->current_row_num++;
  DBUG_PRINT("info",("spider error_num=%d", error_num));
  spider->pushed_pos = NULL;
  DBUG_RETURN(error_num);
}

int spider_db_seek_prev(
  uchar *buf,
  ha_spider *spider,
  TABLE *table
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_seek_prev");
  if (result_list->current_row_num <= 1)
  {
    if (result_list->current == result_list->first)
    {
      table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    if (result_list->low_mem_read == 1)
    {
      my_message(ER_SPIDER_LOW_MEM_READ_PREV_NUM,
        ER_SPIDER_LOW_MEM_READ_PREV_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_LOW_MEM_READ_PREV_NUM);
    }
    result_list->current = result_list->current->prev;
    result_list->current_row_num = result_list->current->record_num - 1;
  } else {
    result_list->current_row_num -= 2;
  }
  if (result_list->quick_mode == 0)
    result_list->current->result->move_to_pos(result_list->current_row_num);
  DBUG_RETURN(spider_db_fetch(buf, spider, table));
}

int spider_db_seek_next(
  uchar *buf,
  ha_spider *spider,
  int link_idx,
  TABLE *table
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn = spider->conns[link_idx];
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_seek_next");
  if (
    result_list->current_row_num >= result_list->current->record_num
  ) {
    DBUG_PRINT("info",("spider result_list->current_row_num=%lld",
      result_list->current_row_num));
    DBUG_PRINT("info",("spider result_list->current->record_num=%lld",
      result_list->current->record_num));
    if (result_list->low_mem_read)
      spider_db_free_one_result(result_list,
        (SPIDER_RESULT*) result_list->current);

    int roop_start = 0, roop_end = 1, roop_count, lock_mode, link_ok = 0;
    if (!spider->use_fields)
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
    }

    if (result_list->bgs_phase > 0)
    {
      if (spider->use_fields)
      {
        SPIDER_LINK_IDX_CHAIN *link_idx_chain;
        SPIDER_LINK_IDX_HOLDER *link_idx_holder;
        spider_fields *fields = spider->fields;
        fields->set_pos_to_first_link_idx_chain();
        while ((link_idx_chain = fields->get_next_link_idx_chain()))
        {
          conn = link_idx_chain->conn;
          link_idx_holder = link_idx_chain->link_idx_holder;
          spider_db_handler *dbton_hdl =
            spider->dbton_handler[conn->dbton_id];
          spider->link_idx_chain = link_idx_chain;
          if ((error_num = spider_bg_conn_search(spider,
            link_idx_holder->link_idx, dbton_hdl->first_link_idx,
            FALSE, FALSE,
            !fields->is_first_link_ok_chain(link_idx_chain))))
          {
            DBUG_PRINT("info",("spider error_num 1=%d", error_num));
            DBUG_RETURN(error_num);
          }
        }
      } else {
        for (roop_count = roop_start; roop_count < roop_end;
          roop_count = spider_conn_link_idx_next(share->link_statuses,
            spider->conn_link_idx, roop_count, share->link_count,
            SPIDER_LINK_STATUS_RECOVERY)
        ) {
          if ((error_num = spider_bg_conn_search(spider, roop_count, roop_start,
            FALSE, FALSE, (roop_count != link_ok))))
          {
            DBUG_PRINT("info",("spider error_num 1=%d", error_num));
            DBUG_RETURN(error_num);
          }
        }
      }
    } else {
      if (result_list->current == result_list->bgs_current)
      {
        if (result_list->finish_flg)
        {
          table->status = STATUS_NOT_FOUND;
          DBUG_PRINT("info",("spider error_num 2=%d", HA_ERR_END_OF_FILE));
          DBUG_RETURN(HA_ERR_END_OF_FILE);
        }
        spider_next_split_read_param(spider);
        if (
          result_list->quick_mode == 0 ||
          result_list->quick_mode == 3 ||
          !result_list->current->result
        ) {
          result_list->limit_num =
            result_list->internal_limit - result_list->record_num >=
            result_list->split_read ?
            result_list->split_read :
            result_list->internal_limit - result_list->record_num;
          if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
          {
            if ((error_num = spider->reappend_limit_sql_part(
              result_list->record_num, result_list->limit_num,
              SPIDER_SQL_TYPE_SELECT_SQL)))
            {
              DBUG_PRINT("info",("spider error_num 3=%d", error_num));
              DBUG_RETURN(error_num);
            }
            if (
              !result_list->use_union &&
              (error_num = spider->append_select_lock_sql_part(
                SPIDER_SQL_TYPE_SELECT_SQL))
            ) {
              DBUG_PRINT("info",("spider error_num 4=%d", error_num));
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
              DBUG_PRINT("info",("spider error_num 5=%d", error_num));
              DBUG_RETURN(error_num);
            }
          }

          if (spider->use_fields)
          {
            SPIDER_LINK_IDX_CHAIN *link_idx_chain;
            SPIDER_LINK_IDX_HOLDER *link_idx_holder;
            spider_fields *fields = spider->fields;
            fields->set_pos_to_first_link_idx_chain();
            while ((link_idx_chain = fields->get_next_link_idx_chain()))
            {
              ulong sql_type;
              conn = link_idx_chain->conn;
              sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
              link_idx_holder = link_idx_chain->link_idx_holder;
              link_idx = link_idx_holder->link_idx;
              spider_db_handler *dbton_handler =
                spider->dbton_handler[conn->dbton_id];
              pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
              if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
              {
                pthread_mutex_lock(&conn->mta_conn_mutex);
                SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
              }
              if ((error_num = dbton_handler->set_sql_for_exec(sql_type,
                link_idx)))
              {
                if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
                {
                  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                  pthread_mutex_unlock(&conn->mta_conn_mutex);
                }
                DBUG_PRINT("info",("spider error_num 6=%d", error_num));
                DBUG_RETURN(error_num);
              }
              if (!dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
              {
                pthread_mutex_lock(&conn->mta_conn_mutex);
                SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
              }
              if (conn->db_conn->limit_mode() == 1)
              {
                conn->db_conn->set_limit(result_list->limit_num);
                if (fields->is_first_link_ok_chain(link_idx_chain))
                {
                  if ((error_num = spider_db_store_result_for_reuse_cursor(
                    spider, link_idx, table)))
                  {
                    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                    pthread_mutex_unlock(&conn->bg_conn_mutex);
                    DBUG_RETURN(error_num);
                  }
                }
                SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                pthread_mutex_unlock(&conn->bg_conn_mutex);
              } else {
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
                  if (
                    spider->need_mons[link_idx]
                  ) {
                    error_num = fields->ping_table_mon_from_table(link_idx_chain);
                  }
                  DBUG_PRINT("info",("spider error_num 7a=%d", error_num));
                  DBUG_RETURN(error_num);
                }
                spider_conn_set_timeout_from_share(conn, link_idx,
                  spider->wide_handler->trx->thd, share);
                if (dbton_handler->execute_sql(
                  sql_type,
                  conn,
                  result_list->quick_mode,
                  &spider->need_mons[link_idx])
                ) {
                  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                  conn->mta_conn_mutex_lock_already = FALSE;
                  conn->mta_conn_mutex_unlock_later = FALSE;
                  error_num = spider_db_errorno(conn);
                  if (
                    spider->need_mons[link_idx]
                  ) {
                    error_num = fields->ping_table_mon_from_table(link_idx_chain);
                  }
                  DBUG_PRINT("info",("spider error_num 8a=%d", error_num));
                  DBUG_RETURN(error_num);
                }
                spider->connection_ids[link_idx] = conn->connection_id;
                DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                conn->mta_conn_mutex_lock_already = FALSE;
                conn->mta_conn_mutex_unlock_later = FALSE;
                if (fields->is_first_link_ok_chain(link_idx_chain))
                {
                  if ((error_num = spider_db_store_result(spider, link_idx,
                    table)))
                  {
                    if (
                      error_num != HA_ERR_END_OF_FILE &&
                      spider->need_mons[link_idx]
                    ) {
                      error_num =
                        fields->ping_table_mon_from_table(link_idx_chain);
                    }
                    DBUG_PRINT("info",("spider error_num 9a=%d", error_num));
                    DBUG_RETURN(error_num);
                  }
                  spider->result_link_idx = link_ok;
                } else {
                  spider_db_discard_result(spider, link_idx, conn);
                  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                  pthread_mutex_unlock(&conn->mta_conn_mutex);
                }
              }
            }
          } else {
            for (roop_count = roop_start; roop_count < roop_end;
              roop_count = spider_conn_link_idx_next(share->link_statuses,
                spider->conn_link_idx, roop_count, share->link_count,
                SPIDER_LINK_STATUS_RECOVERY)
            ) {
              ulong sql_type;
              conn = spider->conns[roop_count];
              if (spider->sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
              {
                sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
              } else {
                sql_type = SPIDER_SQL_TYPE_HANDLER;
              }
              spider_db_handler *dbton_handler =
                spider->dbton_handler[conn->dbton_id];
              pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
              if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
              {
                pthread_mutex_lock(&conn->mta_conn_mutex);
                SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
              }
              if ((error_num = dbton_handler->set_sql_for_exec(sql_type,
                roop_count)))
              {
                if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
                {
                  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                  pthread_mutex_unlock(&conn->mta_conn_mutex);
                }
                DBUG_PRINT("info",("spider error_num 6=%d", error_num));
                DBUG_RETURN(error_num);
              }
              if (!dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
              {
                pthread_mutex_lock(&conn->mta_conn_mutex);
                SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
              }
              if (conn->db_conn->limit_mode() == 1)
              {
                conn->db_conn->set_limit(result_list->limit_num);
                if (roop_count == link_ok)
                {
                  if ((error_num = spider_db_store_result_for_reuse_cursor(
                    spider, link_idx, table)))
                  {
                    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                    pthread_mutex_unlock(&conn->bg_conn_mutex);
                    DBUG_RETURN(error_num);
                  }
                }
                SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                pthread_mutex_unlock(&conn->bg_conn_mutex);
              } else {
                conn->need_mon = &spider->need_mons[roop_count];
                DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
                DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
                conn->mta_conn_mutex_lock_already = TRUE;
                conn->mta_conn_mutex_unlock_later = TRUE;
                if ((error_num = spider_db_set_names(spider, conn, roop_count)))
                {
                  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                  conn->mta_conn_mutex_lock_already = FALSE;
                  conn->mta_conn_mutex_unlock_later = FALSE;
                  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                  pthread_mutex_unlock(&conn->mta_conn_mutex);
                  if (
                    share->monitoring_kind[roop_count] &&
                    spider->need_mons[roop_count]
                  ) {
                    error_num = spider_ping_table_mon_from_table(
                        spider->wide_handler->trx,
                        spider->wide_handler->trx->thd,
                        share,
                        roop_count,
                        (uint32) share->monitoring_sid[roop_count],
                        share->table_name,
                        share->table_name_length,
                        spider->conn_link_idx[roop_count],
                        NULL,
                        0,
                        share->monitoring_kind[roop_count],
                        share->monitoring_limit[roop_count],
                        share->monitoring_flag[roop_count],
                        TRUE
                      );
                  }
                  DBUG_PRINT("info",("spider error_num 7=%d", error_num));
                  DBUG_RETURN(error_num);
                }
                spider_conn_set_timeout_from_share(conn, roop_count,
                  spider->wide_handler->trx->thd, share);
                if (dbton_handler->execute_sql(
                  sql_type,
                  conn,
                  result_list->quick_mode,
                  &spider->need_mons[roop_count])
                ) {
                  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                  conn->mta_conn_mutex_lock_already = FALSE;
                  conn->mta_conn_mutex_unlock_later = FALSE;
                  error_num = spider_db_errorno(conn);
                  if (
                    share->monitoring_kind[roop_count] &&
                    spider->need_mons[roop_count]
                  ) {
                    error_num = spider_ping_table_mon_from_table(
                        spider->wide_handler->trx,
                        spider->wide_handler->trx->thd,
                        share,
                        roop_count,
                        (uint32) share->monitoring_sid[roop_count],
                        share->table_name,
                        share->table_name_length,
                        spider->conn_link_idx[roop_count],
                        NULL,
                        0,
                        share->monitoring_kind[roop_count],
                        share->monitoring_limit[roop_count],
                        share->monitoring_flag[roop_count],
                        TRUE
                      );
                  }
                  DBUG_PRINT("info",("spider error_num 8=%d", error_num));
                  DBUG_RETURN(error_num);
                }
                spider->connection_ids[roop_count] = conn->connection_id;
                DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
                DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
                conn->mta_conn_mutex_lock_already = FALSE;
                conn->mta_conn_mutex_unlock_later = FALSE;
                if (roop_count == link_ok)
                {
                  if ((error_num = spider_db_store_result(spider, roop_count,
                    table)))
                  {
                    if (
                      error_num != HA_ERR_END_OF_FILE &&
                      share->monitoring_kind[roop_count] &&
                      spider->need_mons[roop_count]
                    ) {
                      error_num = spider_ping_table_mon_from_table(
                          spider->wide_handler->trx,
                          spider->wide_handler->trx->thd,
                          share,
                          roop_count,
                          (uint32) share->monitoring_sid[roop_count],
                          share->table_name,
                          share->table_name_length,
                          spider->conn_link_idx[roop_count],
                          NULL,
                          0,
                          share->monitoring_kind[roop_count],
                          share->monitoring_limit[roop_count],
                          share->monitoring_flag[roop_count],
                          TRUE
                        );
                    }
                    DBUG_PRINT("info",("spider error_num 9=%d", error_num));
                    DBUG_RETURN(error_num);
                  }
                  spider->result_link_idx = link_ok;
                } else {
                  spider_db_discard_result(spider, roop_count, conn);
                  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
                  pthread_mutex_unlock(&conn->mta_conn_mutex);
                }
              }
            }
          }
        } else {
          spider->connection_ids[link_idx] = conn->connection_id;
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
          DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_unlock_later = TRUE;
          if ((error_num = spider_db_store_result(spider, link_idx, table)))
          {
            conn->mta_conn_mutex_unlock_later = FALSE;
            DBUG_PRINT("info",("spider error_num 10=%d", error_num));
            DBUG_RETURN(error_num);
          }
          conn->mta_conn_mutex_unlock_later = FALSE;
        }
      } else {
        result_list->current = result_list->current->next;
        result_list->current_row_num = 0;
        if (
          result_list->current == result_list->bgs_current &&
          result_list->finish_flg
        ) {
          table->status = STATUS_NOT_FOUND;
          DBUG_PRINT("info",("spider error_num 11=%d", HA_ERR_END_OF_FILE));
          DBUG_RETURN(HA_ERR_END_OF_FILE);
        }
      }
    }
    DBUG_RETURN(spider_db_fetch(buf, spider, table));
  } else
    DBUG_RETURN(spider_db_fetch(buf, spider, table));
}

int spider_db_seek_last(
  uchar *buf,
  ha_spider *spider,
  int link_idx,
  TABLE *table
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_seek_last");
  if (result_list->finish_flg)
  {
    if (result_list->low_mem_read == 1)
    {
      my_message(ER_SPIDER_LOW_MEM_READ_PREV_NUM,
        ER_SPIDER_LOW_MEM_READ_PREV_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_LOW_MEM_READ_PREV_NUM);
    }
    result_list->current = result_list->last;
    result_list->current_row_num = result_list->current->record_num - 1;
    if (result_list->quick_mode == 0)
      result_list->current->result->move_to_pos(result_list->current_row_num);
    DBUG_RETURN(spider_db_fetch(buf, spider, table));
  } else if (!result_list->sorted ||
    result_list->internal_limit <= result_list->record_num * 2)
  {
    if (result_list->low_mem_read == 1)
    {
      my_message(ER_SPIDER_LOW_MEM_READ_PREV_NUM,
        ER_SPIDER_LOW_MEM_READ_PREV_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_LOW_MEM_READ_PREV_NUM);
    }
    spider_next_split_read_param(spider);
    result_list->limit_num =
      result_list->internal_limit - result_list->record_num;
    if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
    {
      if ((error_num = spider->reappend_limit_sql_part(
        result_list->internal_offset + result_list->record_num,
        result_list->limit_num,
        SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
      if (
        !result_list->use_union &&
        (error_num = spider->append_select_lock_sql_part(
        SPIDER_SQL_TYPE_SELECT_SQL))
      )
        DBUG_RETURN(error_num);
    }
    if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      spider_db_append_handler_next(spider);
      if ((error_num = spider->reappend_limit_sql_part(
        result_list->internal_offset + result_list->record_num,
        result_list->limit_num,
        SPIDER_SQL_TYPE_HANDLER)))
        DBUG_RETURN(error_num);
      if (
        !result_list->use_union &&
        (error_num = spider->append_select_lock_sql_part(
        SPIDER_SQL_TYPE_HANDLER))
      )
        DBUG_RETURN(error_num);
    }

    int roop_start, roop_end, roop_count, lock_mode, link_ok;
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
      ulong sql_type;
      if (spider->sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
      {
        sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
      } else {
        sql_type = SPIDER_SQL_TYPE_HANDLER;
      }
      conn = spider->conns[roop_count];
      spider_db_handler *dbton_handler = spider->dbton_handler[conn->dbton_id];
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      if ((error_num = dbton_handler->set_sql_for_exec(sql_type, roop_count)))
      {
        if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
        {
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(error_num);
      }
      if (!dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
      if (conn->db_conn->limit_mode() == 1)
      {
        conn->db_conn->set_limit(result_list->limit_num);
        if (roop_count == link_ok)
        {
          if ((error_num = spider_db_store_result_for_reuse_cursor(
            spider, roop_count, table)))
          {
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->bg_conn_mutex);
            DBUG_RETURN(error_num);
          }
        }
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->bg_conn_mutex);
      } else {
        conn->need_mon = &spider->need_mons[roop_count];
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        if ((error_num = spider_db_set_names(spider, conn, roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            spider->need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
        spider_conn_set_timeout_from_share(conn, roop_count,
          spider->wide_handler->trx->thd,
          share);
        if (dbton_handler->execute_sql(
          sql_type,
          conn,
          result_list->quick_mode,
          &spider->need_mons[roop_count])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          error_num = spider_db_errorno(conn);
          if (
            share->monitoring_kind[roop_count] &&
            spider->need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
        spider->connection_ids[roop_count] = conn->connection_id;
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        if (roop_count == link_ok)
        {
          if ((error_num = spider_db_store_result(spider, roop_count, table)))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              share->monitoring_kind[roop_count] &&
              spider->need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                  spider->wide_handler->trx,
                  spider->wide_handler->trx->thd,
                  share,
                  roop_count,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  spider->conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
            }
            DBUG_RETURN(error_num);
          }
          spider->result_link_idx = link_ok;
        } else {
          spider_db_discard_result(spider, roop_count, conn);
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
      }
    }
    result_list->current_row_num = result_list->current->record_num - 1;
    if (result_list->quick_mode == 0)
      result_list->current->result->move_to_pos(result_list->current_row_num);
    DBUG_RETURN(spider_db_fetch(buf, spider, table));
  }
  if ((error_num = spider_db_free_result(spider, FALSE)))
    DBUG_RETURN(error_num);
  spider_first_split_read_param(spider);
  result_list->desc_flg = !(result_list->desc_flg);
  result_list->limit_num =
    result_list->internal_limit >= result_list->split_read ?
    result_list->split_read : result_list->internal_limit;
  if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
  {
    spider->set_order_to_pos_sql(SPIDER_SQL_TYPE_SELECT_SQL);
    if (
      (error_num = spider->append_key_order_with_alias_sql_part(
        NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL)) ||
      (error_num = spider->append_limit_sql_part(
        result_list->internal_offset,
        result_list->limit_num, SPIDER_SQL_TYPE_SELECT_SQL)) ||
      (
        !result_list->use_union &&
        (spider->sql_kinds & SPIDER_SQL_KIND_SQL) &&
        (error_num = spider->append_select_lock_sql_part(
          SPIDER_SQL_TYPE_SELECT_SQL))
      )
    )
      DBUG_RETURN(error_num);
  }
  if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
  {
    const char *alias;
    uint alias_length;
    if (result_list->sorted && result_list->desc_flg)
    {
      alias = SPIDER_SQL_LAST_STR;
      alias_length = SPIDER_SQL_LAST_LEN;
    } else {
      alias = SPIDER_SQL_FIRST_STR;
      alias_length = SPIDER_SQL_FIRST_LEN;
    }
    spider->set_order_to_pos_sql(SPIDER_SQL_TYPE_HANDLER);
    if (
      (error_num = spider->append_key_order_with_alias_sql_part(
        alias, alias_length, SPIDER_SQL_TYPE_HANDLER)) ||
      (error_num = spider->reappend_limit_sql_part(
        result_list->internal_offset,
        result_list->limit_num, SPIDER_SQL_TYPE_HANDLER))
    )
      DBUG_RETURN(error_num);
  }

  int roop_start, roop_end, roop_count, lock_mode, link_ok;
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
    ulong sql_type;
    if (spider->sql_kind[roop_count] == SPIDER_SQL_KIND_SQL)
    {
      sql_type = SPIDER_SQL_TYPE_SELECT_SQL;
    } else {
      sql_type = SPIDER_SQL_TYPE_HANDLER;
    }
    conn = spider->conns[roop_count];
    spider_db_handler *dbton_handler = spider->dbton_handler[conn->dbton_id];
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    if ((error_num = dbton_handler->set_sql_for_exec(sql_type, roop_count)))
    {
      if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
      {
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(error_num);
    }
    if (!dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    DBUG_PRINT("info",("spider sql_type=%lu", sql_type));
    conn->need_mon = &spider->need_mons[roop_count];
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if ((error_num = spider_db_set_names(spider, conn, roop_count)))
    {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      if (
        share->monitoring_kind[roop_count] &&
        spider->need_mons[roop_count]
      ) {
        error_num = spider_ping_table_mon_from_table(
            spider->wide_handler->trx,
            spider->wide_handler->trx->thd,
            share,
            roop_count,
            (uint32) share->monitoring_sid[roop_count],
            share->table_name,
            share->table_name_length,
            spider->conn_link_idx[roop_count],
            NULL,
            0,
            share->monitoring_kind[roop_count],
            share->monitoring_limit[roop_count],
            share->monitoring_flag[roop_count],
            TRUE
          );
      }
      DBUG_RETURN(error_num);
    }
    spider_conn_set_timeout_from_share(conn, roop_count,
      spider->wide_handler->trx->thd,
      share);
    if (dbton_handler->execute_sql(
      sql_type,
      conn,
      result_list->quick_mode,
      &spider->need_mons[roop_count])
    ) {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      error_num = spider_db_errorno(conn);
      if (
        share->monitoring_kind[roop_count] &&
        spider->need_mons[roop_count]
      ) {
        error_num = spider_ping_table_mon_from_table(
            spider->wide_handler->trx,
            spider->wide_handler->trx->thd,
            share,
            roop_count,
            (uint32) share->monitoring_sid[roop_count],
            share->table_name,
            share->table_name_length,
            spider->conn_link_idx[roop_count],
            NULL,
            0,
            share->monitoring_kind[roop_count],
            share->monitoring_limit[roop_count],
            share->monitoring_flag[roop_count],
            TRUE
          );
      }
      DBUG_RETURN(error_num);
    }
    spider->connection_ids[roop_count] = conn->connection_id;
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    if (roop_count == link_ok)
    {
      if ((error_num = spider_db_store_result(spider, roop_count, table)))
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
      spider->result_link_idx = link_ok;
    } else {
      spider_db_discard_result(spider, roop_count, conn);
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
    }
  }
  DBUG_RETURN(spider_db_fetch(buf, spider, table));
}

int spider_db_seek_first(
  uchar *buf,
  ha_spider *spider,
  TABLE *table
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_seek_first");
  if (
    result_list->current != result_list->first &&
    result_list->low_mem_read == 1
  ) {
    my_message(ER_SPIDER_LOW_MEM_READ_PREV_NUM, ER_SPIDER_LOW_MEM_READ_PREV_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_LOW_MEM_READ_PREV_NUM);
  }
  result_list->current = result_list->first;
  spider_db_set_pos_to_first_row(result_list);
  DBUG_RETURN(spider_db_fetch(buf, spider, table));
}

void spider_db_set_pos_to_first_row(
  SPIDER_RESULT_LIST *result_list
) {
  DBUG_ENTER("spider_db_set_pos_to_first_row");
  result_list->current_row_num = 0;
  if (result_list->quick_mode == 0)
    result_list->current->result->move_to_pos(0);
  DBUG_VOID_RETURN;
}

void spider_db_create_position(
  ha_spider *spider,
  SPIDER_POSITION *pos
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_RESULT *current = (SPIDER_RESULT*) result_list->current;
  DBUG_ENTER("spider_db_create_position");
  if (result_list->quick_mode == 0)
  {
    SPIDER_DB_RESULT *result = current->result;
    pos->row = result->current_row();
    pos->pos_mode = 2;
    pos->row->next_pos = result_list->tmp_pos_row_first;
    result_list->tmp_pos_row_first = pos->row;
  } else {
    if (result_list->current_row_num <= result_list->quick_page_size)
    {
      SPIDER_POSITION *tmp_pos =
        &current->first_position[result_list->current_row_num - 1];
      memcpy(pos, tmp_pos, sizeof(SPIDER_POSITION));
      tmp_pos->use_position = TRUE;
      tmp_pos->pos_mode = 0;
      pos->pos_mode = 0;
      current->first_pos_use_position = TRUE;
    } else {
      TABLE *tmp_tbl = current->result_tmp_tbl;
      pos->row = NULL;
      pos->pos_mode = 1;
      DBUG_PRINT("info",("spider tmp_tbl=%p", tmp_tbl));
      DBUG_PRINT("info",("spider tmp_tbl->file=%p", tmp_tbl->file));
      DBUG_PRINT("info",("spider tmp_tbl->file->ref=%p", tmp_tbl->file->ref));
      tmp_tbl->file->ref = (uchar *) &pos->tmp_tbl_pos;
      tmp_tbl->file->position(tmp_tbl->record[0]);
      current->tmp_tbl_use_position = TRUE;
    }
  }
  current->use_position = TRUE;
  pos->use_position = TRUE;
  pos->mrr_with_cnt = spider->mrr_with_cnt;
  pos->direct_aggregate = result_list->direct_aggregate;
  pos->sql_kind = spider->sql_kind[spider->result_link_idx];
  pos->position_bitmap = spider->wide_handler->position_bitmap;
  pos->ft_first = spider->ft_first;
  pos->ft_current = spider->ft_current;
  pos->result = current;
  DBUG_VOID_RETURN;
}

int spider_db_seek_tmp(
  uchar *buf,
  SPIDER_POSITION *pos,
  ha_spider *spider,
  TABLE *table
) {
  int error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_db_seek_tmp");
  if (pos->pos_mode != 1)
  {
    if (!pos->row)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    pos->row->first();
  }
  if (pos->sql_kind == SPIDER_SQL_KIND_SQL)
  {
    if (!spider->select_column_mode)
    {
      if (result_list->keyread)
        error_num = spider_db_seek_tmp_key(buf, pos, spider, table,
          result_list->key_info);
      else
        error_num = spider_db_seek_tmp_table(buf, pos, spider, table);
    } else
      error_num = spider_db_seek_tmp_minimum_columns(buf, pos, spider, table);
  } else
    error_num = spider_db_seek_tmp_table(buf, pos, spider, table);

  DBUG_PRINT("info",("spider error_num=%d", error_num));
  DBUG_RETURN(error_num);
}

int spider_db_seek_tmp_table(
  uchar *buf,
  SPIDER_POSITION *pos,
  ha_spider *spider,
  TABLE *table
) {
  int error_num;
  Field **field;
  SPIDER_DB_ROW *row = pos->row;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("spider_db_seek_tmp_table");
  if (pos->pos_mode == 1)
  {
    if ((error_num = spider_db_get_row_from_tmp_tbl_pos(pos, &row)))
      DBUG_RETURN(error_num);
  } else if (pos->pos_mode == 2)
  {
/*
    SPIDER_DB_RESULT *result = pos->result->result;
    result->current_row = row;
*/
  }

  DBUG_PRINT("info", ("spider row=%p", row));
  if (!spider->result_list.in_cmp_ref)
  {
    DBUG_PRINT("info", ("spider direct_aggregate=%s",
      pos->direct_aggregate ? "TRUE" : "FALSE"));
    spider->result_list.snap_mrr_with_cnt = pos->mrr_with_cnt;
    spider->result_list.snap_direct_aggregate = pos->direct_aggregate;
    spider->result_list.snap_row = row;
  }

  /* for mrr */
  if (pos->mrr_with_cnt)
  {
    DBUG_PRINT("info", ("spider mrr_with_cnt"));
    if (pos->sql_kind == SPIDER_SQL_KIND_SQL)
    {
      row->next();
    } else {
      spider->result_list.snap_mrr_with_cnt = FALSE;
    }
  }

  /* for direct_aggregate */
  if (pos->direct_aggregate)
  {
    if ((error_num = spider_db_fetch_for_item_sum_funcs(row, spider)))
      DBUG_RETURN(error_num);
  }

  if ((error_num = spider_db_append_match_fetch(spider,
    pos->ft_first, pos->ft_current, row)))
    DBUG_RETURN(error_num);

  for (
    field = table->field;
    *field;
    field++
  ) {
    if ((
      bitmap_is_set(table->read_set, (*field)->field_index) |
      bitmap_is_set(table->write_set, (*field)->field_index)
    )) {
#ifndef DBUG_OFF
      MY_BITMAP *tmp_map =
        dbug_tmp_use_all_columns(table, &table->write_set);
#endif
      DBUG_PRINT("info", ("spider bitmap is set %s",
        SPIDER_field_name_str(*field)));
      if ((error_num =
        spider_db_fetch_row(spider->share, *field, row, ptr_diff)))
        DBUG_RETURN(error_num);
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
    }
    row->next();
  }
  DBUG_RETURN(0);
}

int spider_db_seek_tmp_key(
  uchar *buf,
  SPIDER_POSITION *pos,
  ha_spider *spider,
  TABLE *table,
  const KEY *key_info
) {
  int error_num;
  KEY_PART_INFO *key_part;
  uint part_num;
  SPIDER_DB_ROW *row = pos->row;
  Field *field;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("spider_db_seek_tmp_key");
  if (pos->pos_mode == 1)
  {
    if ((error_num = spider_db_get_row_from_tmp_tbl_pos(pos, &row)))
      DBUG_RETURN(error_num);
  } else if (pos->pos_mode == 2)
  {
/*
    SPIDER_DB_RESULT *result = pos->result->result;
    result->current_row = row;
*/
  }

  DBUG_PRINT("info", ("spider row=%p", row));
  if (!spider->result_list.in_cmp_ref)
  {
    DBUG_PRINT("info", ("spider direct_aggregate=%s",
      pos->direct_aggregate ? "TRUE" : "FALSE"));
    spider->result_list.snap_mrr_with_cnt = pos->mrr_with_cnt;
    spider->result_list.snap_direct_aggregate = pos->direct_aggregate;
    spider->result_list.snap_row = row;
  }

  /* for mrr */
  if (pos->mrr_with_cnt)
  {
    DBUG_PRINT("info", ("spider mrr_with_cnt"));
    row->next();
  }

  /* for direct_aggregate */
  if (pos->direct_aggregate)
  {
    if ((error_num = spider_db_fetch_for_item_sum_funcs(row, spider)))
      DBUG_RETURN(error_num);
  }

  if ((error_num = spider_db_append_match_fetch(spider,
    pos->ft_first, pos->ft_current, row)))
    DBUG_RETURN(error_num);

  for (
    key_part = key_info->key_part,
    part_num = 0;
    part_num < spider_user_defined_key_parts(key_info);
    key_part++,
    part_num++
  ) {
    field = key_part->field;
    if ((
      bitmap_is_set(table->read_set, field->field_index) |
      bitmap_is_set(table->write_set, field->field_index)
    )) {
#ifndef DBUG_OFF
      MY_BITMAP *tmp_map =
        dbug_tmp_use_all_columns(table, &table->write_set);
#endif
      DBUG_PRINT("info", ("spider bitmap is set %s",
        SPIDER_field_name_str(field)));
      if ((error_num =
        spider_db_fetch_row(spider->share, field, row, ptr_diff)))
        DBUG_RETURN(error_num);
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
    }
    row->next();
  }
  DBUG_RETURN(0);
}

int spider_db_seek_tmp_minimum_columns(
  uchar *buf,
  SPIDER_POSITION *pos,
  ha_spider *spider,
  TABLE *table
) {
  int error_num;
  Field **field;
  SPIDER_DB_ROW *row = pos->row;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("spider_db_seek_tmp_minimum_columns");
  if (pos->pos_mode == 1)
  {
    if ((error_num = spider_db_get_row_from_tmp_tbl_pos(pos, &row)))
      DBUG_RETURN(error_num);
  } else if (pos->pos_mode == 2)
  {
/*
    SPIDER_DB_RESULT *result = pos->result->result;
    result->current_row = row;
*/
  }

  DBUG_PRINT("info", ("spider row=%p", row));
  if (!spider->result_list.in_cmp_ref)
  {
    DBUG_PRINT("info", ("spider direct_aggregate=%s",
      pos->direct_aggregate ? "TRUE" : "FALSE"));
    spider->result_list.snap_mrr_with_cnt = pos->mrr_with_cnt;
    spider->result_list.snap_direct_aggregate = pos->direct_aggregate;
    spider->result_list.snap_row = row;
  }

  /* for mrr */
  if (pos->mrr_with_cnt)
  {
    DBUG_PRINT("info", ("spider mrr_with_cnt"));
    row->next();
  }

  /* for direct_aggregate */
  if (pos->direct_aggregate)
  {
    if ((error_num = spider_db_fetch_for_item_sum_funcs(row, spider)))
      DBUG_RETURN(error_num);
  }

  if ((error_num = spider_db_append_match_fetch(spider,
    pos->ft_first, pos->ft_current, row)))
    DBUG_RETURN(error_num);

  for (
    field = table->field;
    *field;
    field++
  ) {
    DBUG_PRINT("info", ("spider field_index %u", (*field)->field_index));
    if (spider_bit_is_set(pos->position_bitmap, (*field)->field_index))
    {
/*
    if ((
      bitmap_is_set(table->read_set, (*field)->field_index) |
      bitmap_is_set(table->write_set, (*field)->field_index)
    )) {
      DBUG_PRINT("info", ("spider read_set %u",
        bitmap_is_set(table->read_set, (*field)->field_index)));
      DBUG_PRINT("info", ("spider write_set %u",
        bitmap_is_set(table->write_set, (*field)->field_index)));
*/
#ifndef DBUG_OFF
      MY_BITMAP *tmp_map =
        dbug_tmp_use_all_columns(table, &table->write_set);
#endif
      DBUG_PRINT("info", ("spider bitmap is set %s",
        SPIDER_field_name_str(*field)));
      if ((error_num =
        spider_db_fetch_row(spider->share, *field, row, ptr_diff)))
        DBUG_RETURN(error_num);
      row->next();
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
    }
    else if (bitmap_is_set(table->read_set, (*field)->field_index))
    {
      DBUG_PRINT("info", ("spider bitmap is cleared %s",
        SPIDER_field_name_str(*field)));
      bitmap_clear_bit(table->read_set, (*field)->field_index);
    }
  }
  DBUG_RETURN(0);
}

int spider_db_show_table_status(
  ha_spider *spider,
  int link_idx,
  int sts_mode,
  uint flag
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
  DBUG_ENTER("spider_db_show_table_status");
  DBUG_PRINT("info",("spider sts_mode=%d", sts_mode));
  sts_mode = dbton_hdl->sts_mode_exchange(sts_mode);
  error_num = dbton_hdl->show_table_status(
    link_idx,
    sts_mode,
    flag
  );
  DBUG_RETURN(error_num);
}

int spider_db_simple_action(
  uint simple_action,
  spider_db_handler *db_handler,
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_db_simple_action");
  switch (simple_action)
  {
    case SPIDER_SIMPLE_RECORDS:
      DBUG_PRINT("info",("spider simple records"));
      error_num = db_handler->show_records(
        link_idx
      );
      break;
    case SPIDER_SIMPLE_CHECKSUM_TABLE:
      DBUG_PRINT("info",("spider simple checksum_table"));
      error_num = db_handler->checksum_table(
        link_idx
      );
      break;
    default:
      DBUG_ASSERT(0);
      error_num = HA_ERR_CRASHED;
      break;
  }
  DBUG_RETURN(error_num);
}

int spider_db_simple_action(
  uint simple_action,
  ha_spider *spider,
  int link_idx,
  bool pre_call
) {
  int error_num;
  THD *thd = spider->wide_handler->trx->thd;
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_db_simple_action");
  if (pre_call)
  {
    if (spider_param_bgs_mode(thd, spider->share->bgs_mode))
    {
      if ((error_num = spider_check_and_get_casual_read_conn(thd, spider,
        link_idx)))
      {
        DBUG_RETURN(error_num);
      }
      conn = spider->conns[link_idx];
      if (!(error_num = spider_create_conn_thread(conn)))
      {
        spider_bg_conn_simple_action(conn, simple_action, FALSE,
          spider, link_idx, (int *) &spider->result_list.bgs_error);
      }
    } else {
      conn = spider->conns[link_idx];
      error_num = spider_db_simple_action(
        simple_action,
        spider->dbton_handler[conn->dbton_id],
        link_idx
      );
    }
  } else {
    conn = spider->conns[link_idx];
    if (spider->use_pre_action)
    {
      if (spider_param_bgs_mode(thd, spider->share->bgs_mode))
      {
        spider_bg_conn_wait(conn);
        error_num = spider->result_list.bgs_error;
        if (conn->casual_read_base_conn)
        {
          spider->conns[link_idx] = conn->casual_read_base_conn;
        }
      } else {
        error_num = 0;
      }
    } else {
      error_num = spider_db_simple_action(
        simple_action,
        spider->dbton_handler[conn->dbton_id],
        link_idx
      );
    }
  }
  DBUG_RETURN(error_num);
}

void spider_db_set_cardinarity(
  ha_spider *spider,
  TABLE *table
) {
  int roop_count, roop_count2;
  SPIDER_SHARE *share = spider->share;
  KEY *key_info;
  KEY_PART_INFO *key_part;
  Field *field;
  ha_rows rec_per_key;
  DBUG_ENTER("spider_db_set_cardinarity");
  for (roop_count = 0; roop_count < (int) table->s->keys; roop_count++)
  {
    key_info = &table->key_info[roop_count];
    for (roop_count2 = 0;
      roop_count2 < (int) spider_user_defined_key_parts(key_info);
      roop_count2++)
    {
      key_part = &key_info->key_part[roop_count2];
      field = key_part->field;
      if (share->cardinality[field->field_index])
      {
        rec_per_key = (ha_rows) share->stat.records /
          share->cardinality[field->field_index];
        if (rec_per_key > ~(ulong) 0)
          key_info->rec_per_key[roop_count2] = ~(ulong) 0;
        else if (rec_per_key == 0)
          key_info->rec_per_key[roop_count2] = 1;
        else
          key_info->rec_per_key[roop_count2] = (ulong) rec_per_key;
      } else {
        key_info->rec_per_key[roop_count2] = 1;
      }
      DBUG_PRINT("info",
        ("spider column id=%d", field->field_index));
      DBUG_PRINT("info",
        ("spider cardinality=%lld",
        share->cardinality[field->field_index]));
      DBUG_PRINT("info",
        ("spider rec_per_key=%lu",
        key_info->rec_per_key[roop_count2]));
    }
  }
  DBUG_VOID_RETURN;
}

int spider_db_show_index(
  ha_spider *spider,
  int link_idx,
  TABLE *table,
  int crd_mode
) {
  int error_num;
  SPIDER_CONN *conn = spider->conns[link_idx];
  spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
  DBUG_ENTER("spider_db_show_index");
  crd_mode = dbton_hdl->crd_mode_exchange(crd_mode);
  error_num = spider->dbton_handler[conn->dbton_id]->show_index(
    link_idx,
    crd_mode
  );
  DBUG_RETURN(error_num);
}

ha_rows spider_db_explain_select(
  const key_range *start_key,
  const key_range *end_key,
  ha_spider *spider,
  int link_idx
) {
  SPIDER_CONN *conn = spider->conns[link_idx];
  ha_rows rows;
  DBUG_ENTER("spider_db_explain_select");
  rows = spider->dbton_handler[conn->dbton_id]->explain_select(
    start_key,
    end_key,
    link_idx
  );
  DBUG_RETURN(rows);
}

int spider_db_bulk_insert_init(
  ha_spider *spider,
  const TABLE *table
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_db_bulk_insert_init");
  spider->sql_kinds = 0;
  spider->reset_sql_sql(SPIDER_SQL_TYPE_INSERT_SQL);
  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    if (spider->conns[roop_count])
      spider->conns[roop_count]->ignore_dup_key =
        spider->wide_handler->ignore_dup_key;
    spider_conn_use_handler(spider, spider->wide_handler->lock_mode,
      roop_count);
  }
    if (
      (error_num = spider->append_insert_sql_part()) ||
      (error_num = spider->append_into_sql_part(
        SPIDER_SQL_TYPE_INSERT_SQL))
    )
      DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}

int spider_db_bulk_insert(
  ha_spider *spider,
  TABLE *table,
  ha_copy_info *copy_info,
  bool bulk_end
) {
  int error_num, first_insert_link_idx = -1;
  SPIDER_SHARE *share = spider->share;
  THD *thd = spider->wide_handler->trx->thd;
  DBUG_ENTER("spider_db_bulk_insert");

  if (!bulk_end)
  {
      if ((error_num = spider->append_insert_values_sql_part(
        SPIDER_SQL_TYPE_INSERT_SQL)))
      {
        if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
          spider->set_insert_to_pos_sql(SPIDER_SQL_TYPE_INSERT_SQL);
        DBUG_RETURN(error_num);
      }
  }

  if (spider->is_bulk_insert_exec_period(bulk_end))
  {
    int roop_count2;
    SPIDER_CONN *conn, *first_insert_conn = NULL;
    if ((error_num = spider->append_insert_terminator_sql_part(
      SPIDER_SQL_TYPE_INSERT_SQL)))
    {
      if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
        spider->set_insert_to_pos_sql(SPIDER_SQL_TYPE_INSERT_SQL);
      DBUG_RETURN(error_num);
    }
      bool insert_info = FALSE;
      for (
        roop_count2 = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_count2 < (int) share->link_count;
        roop_count2 = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, roop_count2, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        ulong sql_type;
        spider_db_handler *dbton_handler;
          sql_type = SPIDER_SQL_TYPE_INSERT_SQL;
          conn = spider->conns[roop_count2];
          dbton_handler = spider->dbton_handler[conn->dbton_id];
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
          if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          if ((error_num = dbton_handler->set_sql_for_exec(sql_type,
            roop_count2)))
          {
            if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
              spider->set_insert_to_pos_sql(SPIDER_SQL_TYPE_INSERT_SQL);
            if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
            {
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
            DBUG_RETURN(error_num);
          }
          if (!dbton_handler->need_lock_before_set_sql_for_exec(sql_type))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
        conn->need_mon = &spider->need_mons[roop_count2];
        DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        if ((error_num = spider_db_set_names(spider, conn, roop_count2)))
        {
          if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
            spider->set_insert_to_pos_sql(SPIDER_SQL_TYPE_INSERT_SQL);
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count2] &&
            spider->need_mons[roop_count2]
          ) {
            error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count2,
                (uint32) share->monitoring_sid[roop_count2],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count2],
                NULL,
                0,
                share->monitoring_kind[roop_count2],
                share->monitoring_limit[roop_count2],
                share->monitoring_flag[roop_count2],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
        spider_conn_set_timeout_from_share(conn, roop_count2,
          spider->wide_handler->trx->thd,
          share);
        if (dbton_handler->execute_sql(
          sql_type,
          conn,
          -1,
          &spider->need_mons[roop_count2])
        ) {
          if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
            spider->set_insert_to_pos_sql(SPIDER_SQL_TYPE_INSERT_SQL);
          error_num = spider_db_errorno(conn);
          if (error_num == HA_ERR_FOUND_DUPP_KEY)
          {
            conn->db_conn->set_dup_key_idx(spider, roop_count2);
          }
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            error_num != ER_DUP_ENTRY &&
            error_num != ER_DUP_KEY &&
            error_num != HA_ERR_FOUND_DUPP_KEY &&
            share->monitoring_kind[roop_count2] &&
            spider->need_mons[roop_count2]
          ) {
            error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count2,
                (uint32) share->monitoring_sid[roop_count2],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count2],
                NULL,
                0,
                share->monitoring_kind[roop_count2],
                share->monitoring_limit[roop_count2],
                share->monitoring_flag[roop_count2],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        if (!insert_info && copy_info)
        {
          insert_info =
            conn->db_conn->inserted_info(dbton_handler, copy_info);
        }
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        if (first_insert_link_idx == -1)
        {
          first_insert_link_idx = roop_count2;
          first_insert_conn = conn;
        }
      }

      conn = first_insert_conn;
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      conn->need_mon = &spider->need_mons[first_insert_link_idx];
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
        spider->set_insert_to_pos_sql(SPIDER_SQL_TYPE_INSERT_SQL);
      if (table->next_number_field &&
        (
          !table->auto_increment_field_not_null ||
          (
            !table->next_number_field->val_int() &&
            !(thd->variables.sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO)
          )
        )
      ) {
        ulonglong last_insert_id;
        spider_db_handler *dbton_handler =
          spider->dbton_handler[conn->dbton_id];
        if (spider->store_last_insert_id)
          last_insert_id = spider->store_last_insert_id;
        else if ((error_num = dbton_handler->
          show_last_insert_id(first_insert_link_idx, last_insert_id)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          DBUG_RETURN(error_num);
        }
        table->next_number_field->set_notnull();
        if (
          (error_num = spider_db_update_auto_increment(spider,
            first_insert_link_idx)) ||
          (error_num = table->next_number_field->store(
            last_insert_id, TRUE))
        ) {
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
      spider->store_last_insert_id = 0;
  }
  if (
    (bulk_end || !spider->bulk_insert) &&
    (error_num = spider_trx_check_link_idx_failed(spider))
  )
    DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}


int spider_db_update_auto_increment(
  ha_spider *spider,
  int link_idx
) {
  int roop_count;
  THD *thd = spider->wide_handler->trx->thd;
  ulonglong last_insert_id, affected_rows;
  SPIDER_SHARE *share = spider->share;
  TABLE *table = spider->get_table();
  int auto_increment_mode = spider_param_auto_increment_mode(thd,
    share->auto_increment_mode);
  DBUG_ENTER("spider_db_update_auto_increment");
  if (
    auto_increment_mode == 2 ||
    (auto_increment_mode == 3 && !table->auto_increment_field_not_null)
  ) {
    last_insert_id = spider->conns[link_idx]->db_conn->last_insert_id();
      affected_rows = spider->conns[link_idx]->db_conn->affected_rows();
    DBUG_PRINT("info",("spider last_insert_id=%llu", last_insert_id));
    share->lgtm_tblhnd_share->auto_increment_value =
      last_insert_id + affected_rows;
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      share->lgtm_tblhnd_share->auto_increment_value));
/*
    thd->record_first_successful_insert_id_in_cur_stmt(last_insert_id);
*/
    if (
      thd->first_successful_insert_id_in_cur_stmt == 0 ||
      thd->first_successful_insert_id_in_cur_stmt > last_insert_id
    ) {
      bool first_set = (thd->first_successful_insert_id_in_cur_stmt == 0);
      thd->first_successful_insert_id_in_cur_stmt = last_insert_id;
      if (
        table->s->next_number_keypart == 0 &&
        mysql_bin_log.is_open() &&
        !thd->is_current_stmt_binlog_format_row()
      ) {
        if (
          spider->check_partitioned() &&
          thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0
        ) {
          DBUG_PRINT("info",("spider table partitioning"));
          Discrete_interval *current =
            thd->auto_inc_intervals_in_cur_stmt_for_binlog.get_current();
          current->replace(last_insert_id, affected_rows, 1);
        } else {
          DBUG_PRINT("info",("spider table"));
          thd->auto_inc_intervals_in_cur_stmt_for_binlog.append(
            last_insert_id, affected_rows, 1);
        }
        if (affected_rows > 1 || !first_set)
        {
          for (roop_count = first_set ? 1 : 0;
            roop_count < (int) affected_rows;
            roop_count++)
            push_warning_printf(thd, SPIDER_WARN_LEVEL_NOTE,
              ER_SPIDER_AUTOINC_VAL_IS_DIFFERENT_NUM,
              ER_SPIDER_AUTOINC_VAL_IS_DIFFERENT_STR);
        }
      }
    } else {
      if (
        table->s->next_number_keypart == 0 &&
        mysql_bin_log.is_open() &&
        !thd->is_current_stmt_binlog_format_row()
      ) {
        for (roop_count = 0; roop_count < (int) affected_rows; roop_count++)
          push_warning_printf(thd, SPIDER_WARN_LEVEL_NOTE,
            ER_SPIDER_AUTOINC_VAL_IS_DIFFERENT_NUM,
            ER_SPIDER_AUTOINC_VAL_IS_DIFFERENT_STR);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_bulk_update_size_limit(
  ha_spider *spider,
  TABLE *table
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_CONN *conn;
  ha_rows dup_key_found = 0;
  DBUG_ENTER("spider_db_bulk_update_size_limit");

  if (result_list->bulk_update_mode == 1)
  {
    /* execute bulk updating */
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
      spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      if (dbton_hdl->need_lock_before_set_sql_for_exec(
        SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      if ((error_num = dbton_hdl->set_sql_for_exec(
        SPIDER_SQL_TYPE_BULK_UPDATE_SQL, roop_count)))
      {
        if (dbton_hdl->need_lock_before_set_sql_for_exec(
          SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
        {
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(error_num);
      }
      if (!dbton_hdl->need_lock_before_set_sql_for_exec(
        SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      if ((error_num = spider_db_query_for_bulk_update(
        spider, conn, roop_count, &dup_key_found)))
      {
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    }
    spider->reset_sql_sql(SPIDER_SQL_TYPE_BULK_UPDATE_SQL);
  } else {
    /* store query to temporary tables */
    if ((error_num = spider->mk_bulk_tmp_table_and_bulk_start()))
    {
      goto error_mk_table;
    }
    if ((error_num = spider->bulk_tmp_table_insert()))
    {
      goto error_write_row;
    }
    spider->reset_sql_sql(SPIDER_SQL_TYPE_BULK_UPDATE_SQL);
  }
  DBUG_RETURN(0);

error_write_row:
  spider->bulk_tmp_table_end_bulk_insert();
  spider->rm_bulk_tmp_table();
  spider->reset_sql_sql(SPIDER_SQL_TYPE_BULK_UPDATE_SQL);
error_mk_table:
  DBUG_RETURN(error_num);
}

int spider_db_bulk_update_end(
  ha_spider *spider,
  ha_rows *dup_key_found
) {
  int error_num = 0, error_num2, roop_count;
  THD *thd = spider->wide_handler->trx->thd;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  bool is_error = thd->is_error();
  DBUG_ENTER("spider_db_bulk_update_end");

  if (spider->bulk_tmp_table_created())
  {
    if ((error_num2 = spider->bulk_tmp_table_end_bulk_insert()))
    {
      error_num = error_num2;
    }

    if (!is_error)
    {
      if (error_num)
        goto error_last_query;

      if ((error_num = spider->bulk_tmp_table_rnd_init()))
      {
        goto error_rnd_init;
      }

      while (!(error_num = spider->bulk_tmp_table_rnd_next()))
      {
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
          spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
          if (dbton_hdl->need_lock_before_set_sql_for_exec(
            SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          if ((error_num = dbton_hdl->set_sql_for_exec(
            SPIDER_SQL_TYPE_BULK_UPDATE_SQL, roop_count)))
          {
            if (dbton_hdl->need_lock_before_set_sql_for_exec(
              SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
            {
              SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&conn->mta_conn_mutex);
            }
            if (error_num == ER_SPIDER_COND_SKIP_NUM)
            {
              continue;
            }
            DBUG_RETURN(error_num);
          }
          if (!dbton_hdl->need_lock_before_set_sql_for_exec(
            SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
          {
            pthread_mutex_lock(&conn->mta_conn_mutex);
            SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
          }
          if ((error_num = spider_db_query_for_bulk_update(
            spider, conn, roop_count, dup_key_found)))
          {
            pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
            goto error_query;
          }
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        }
      }
      if (error_num != HA_ERR_END_OF_FILE)
        goto error_rnd_next;

      spider->bulk_tmp_table_rnd_end();
    }
  }

  if (!is_error)
  {
    if (!spider->sql_is_empty(SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
    {
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
        spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
        if (dbton_hdl->need_lock_before_set_sql_for_exec(
          SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if ((error_num = dbton_hdl->set_sql_for_exec(
          SPIDER_SQL_TYPE_BULK_UPDATE_SQL, roop_count)))
        {
          if (dbton_hdl->need_lock_before_set_sql_for_exec(
            SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
          {
            SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&conn->mta_conn_mutex);
          }
          DBUG_RETURN(error_num);
        }
        if (!dbton_hdl->need_lock_before_set_sql_for_exec(
          SPIDER_SQL_TYPE_BULK_UPDATE_SQL))
        {
          pthread_mutex_lock(&conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        }
        if ((error_num = spider_db_query_for_bulk_update(
          spider, conn, roop_count, dup_key_found)))
        {
          pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
          goto error_last_query;
        }
        pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      }
    }
  }
  spider->rm_bulk_tmp_table();
  spider->reset_sql_sql(SPIDER_SQL_TYPE_BULK_UPDATE_SQL);
  DBUG_RETURN(0);

error_query:
error_rnd_next:
  spider->bulk_tmp_table_rnd_end();
error_rnd_init:
error_last_query:
  spider->rm_bulk_tmp_table();
  spider->reset_sql_sql(SPIDER_SQL_TYPE_BULK_UPDATE_SQL);
  DBUG_RETURN(error_num);
}

int spider_db_bulk_update(
  ha_spider *spider,
  TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  int error_num;
  DBUG_ENTER("spider_db_bulk_update");

  if ((error_num = spider->append_update_sql(table, ptr_diff, TRUE)))
    DBUG_RETURN(error_num);

  if (
    spider->sql_is_filled_up(SPIDER_SQL_TYPE_BULK_UPDATE_SQL) &&
    (error_num = spider_db_bulk_update_size_limit(spider, table))
  )
    DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}

int spider_db_update(
  ha_spider *spider,
  TABLE *table,
  const uchar *old_data
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(old_data, table->record[0]);
  DBUG_ENTER("spider_db_update");
  if (result_list->bulk_update_mode)
    DBUG_RETURN(spider_db_bulk_update(spider, table, ptr_diff));

  if ((error_num = spider->append_update_sql(table, ptr_diff, FALSE)))
    DBUG_RETURN(error_num);

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
    spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
    conn->ignore_dup_key = spider->wide_handler->ignore_dup_key;
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    if (dbton_hdl->need_lock_before_set_sql_for_exec(
      SPIDER_SQL_TYPE_UPDATE_SQL))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    if ((error_num = dbton_hdl->set_sql_for_exec(
      SPIDER_SQL_TYPE_UPDATE_SQL, roop_count)))
    {
      if (dbton_hdl->need_lock_before_set_sql_for_exec(
        SPIDER_SQL_TYPE_UPDATE_SQL))
      {
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(error_num);
    }
    if (!dbton_hdl->need_lock_before_set_sql_for_exec(
      SPIDER_SQL_TYPE_UPDATE_SQL))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    conn->need_mon = &spider->need_mons[roop_count];
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if ((error_num = spider_db_set_names(spider, conn, roop_count)))
    {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      if (
        share->monitoring_kind[roop_count] &&
        spider->need_mons[roop_count]
      ) {
        error_num = spider_ping_table_mon_from_table(
            spider->wide_handler->trx,
            spider->wide_handler->trx->thd,
            share,
            roop_count,
            (uint32) share->monitoring_sid[roop_count],
            share->table_name,
            share->table_name_length,
            spider->conn_link_idx[roop_count],
            NULL,
            0,
            share->monitoring_kind[roop_count],
            share->monitoring_limit[roop_count],
            share->monitoring_flag[roop_count],
            TRUE
          );
      }
      DBUG_RETURN(error_num);
    }
    spider_conn_set_timeout_from_share(conn, roop_count,
      spider->wide_handler->trx->thd,
      share);
    if (dbton_hdl->execute_sql(
      SPIDER_SQL_TYPE_UPDATE_SQL,
      conn,
      -1,
      &spider->need_mons[roop_count])
    ) {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      error_num = spider_db_errorno(conn);
      if (
        error_num != ER_DUP_ENTRY &&
        error_num != ER_DUP_KEY &&
        error_num != HA_ERR_FOUND_DUPP_KEY &&
        share->monitoring_kind[roop_count] &&
        spider->need_mons[roop_count]
      ) {
        error_num = spider_ping_table_mon_from_table(
            spider->wide_handler->trx,
            spider->wide_handler->trx->thd,
            share,
            roop_count,
            (uint32) share->monitoring_sid[roop_count],
            share->table_name,
            share->table_name_length,
            spider->conn_link_idx[roop_count],
            NULL,
            0,
            share->monitoring_kind[roop_count],
            share->monitoring_limit[roop_count],
            share->monitoring_flag[roop_count],
            TRUE
          );
      }
      DBUG_RETURN(error_num);
    }

    if (
      !conn->db_conn->affected_rows() &&
      share->link_statuses[roop_count] == SPIDER_LINK_STATUS_RECOVERY &&
      spider->pk_update
    ) {
      /* insert */
      if ((error_num = dbton_hdl->append_insert_for_recovery(
        SPIDER_SQL_TYPE_INSERT_SQL, roop_count)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }
      spider_conn_set_timeout_from_share(conn, roop_count,
        spider->wide_handler->trx->thd,
        share);
      if (dbton_hdl->execute_sql(
        SPIDER_SQL_TYPE_INSERT_SQL,
        conn,
        -1,
        &spider->need_mons[roop_count])
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        error_num = spider_db_errorno(conn);
        if (
          error_num != ER_DUP_ENTRY &&
          error_num != ER_DUP_KEY &&
          error_num != HA_ERR_FOUND_DUPP_KEY &&
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    result_list->update_sqls[roop_count].length(0);
  }
  spider->reset_sql_sql(SPIDER_SQL_TYPE_UPDATE_SQL);
  DBUG_RETURN(0);
}

int spider_db_direct_update(
  ha_spider *spider,
  TABLE *table,
  ha_rows *update_rows,
  ha_rows *found_rows
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  bool counted = FALSE;
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  DBUG_ENTER("spider_db_direct_update");

  spider_set_result_list_param(spider);
  result_list->finish_flg = FALSE;
  DBUG_PRINT("info", ("spider do_direct_update=%s",
    spider->do_direct_update ? "TRUE" : "FALSE"));
  DBUG_PRINT("info", ("spider direct_update_kinds=%u",
    spider->direct_update_kinds));
  if ((error_num = spider->append_update_sql_part()))
    DBUG_RETURN(error_num);

/*
  SQL access -> SQL remote access
    !spider->do_direct_update &&
    (spider->sql_kinds & SPIDER_SQL_KIND_SQL)

  SQL access -> SQL remote access with dirct_update
    spider->do_direct_update &&
    spider->direct_update_kinds == SPIDER_SQL_KIND_SQL &&
    spider->wide_handler->direct_update_fields
*/

  if (!spider->do_direct_update)
  {
    if (
      (spider->sql_kinds & SPIDER_SQL_KIND_SQL) &&
      (error_num = spider->append_update_set_sql_part())
    ) {
      DBUG_RETURN(error_num);
    }
  } else {
    if (
      (spider->direct_update_kinds & SPIDER_SQL_KIND_SQL) &&
      (error_num = spider->append_direct_update_set_sql_part())
    ) {
      DBUG_RETURN(error_num);
    }
  }

  result_list->desc_flg = FALSE;
  result_list->sorted = TRUE;
  if (spider->active_index == MAX_KEY)
    result_list->key_info = NULL;
  else
    result_list->key_info = &table->key_info[spider->active_index];
  spider_get_select_limit(spider, &select_lex, &select_limit, &offset_limit);
  result_list->limit_num =
    result_list->internal_limit >= select_limit ?
    select_limit : result_list->internal_limit;
  result_list->internal_offset += offset_limit;
  if (spider->direct_update_kinds & SPIDER_SQL_KIND_SQL)
  {
    if (
      (error_num = spider->append_key_where_sql_part(
        NULL,
        NULL,
        SPIDER_SQL_TYPE_UPDATE_SQL)) ||
      (error_num = spider->
        append_key_order_for_direct_order_limit_with_alias_sql_part(
        NULL, 0, SPIDER_SQL_TYPE_UPDATE_SQL)) ||
      (error_num = spider->append_limit_sql_part(
        result_list->internal_offset, result_list->limit_num,
        SPIDER_SQL_TYPE_UPDATE_SQL))
    ) {
      DBUG_RETURN(error_num);
    }
  }

  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    ulong sql_type;
    DBUG_PRINT("info", ("spider exec sql"));
    conn = spider->conns[roop_count];
    sql_type = SPIDER_SQL_TYPE_UPDATE_SQL;
    spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    if ((error_num = dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
    {
      if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
      {
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(error_num);
    }
    if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
      conn->need_mon = &spider->need_mons[roop_count];
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if ((error_num = spider_db_set_names(spider, conn, roop_count)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
      spider_conn_set_timeout_from_share(conn, roop_count,
        spider->wide_handler->trx->thd,
        share);
      if (
        (error_num = dbton_hdl->execute_sql(
          sql_type,
          conn,
          -1,
          &spider->need_mons[roop_count])
        ) &&
        (error_num != HA_ERR_FOUND_DUPP_KEY ||
          !spider->wide_handler->ignore_dup_key)
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        error_num = spider_db_errorno(conn);
        if (
          error_num != ER_DUP_ENTRY &&
          error_num != ER_DUP_KEY &&
          error_num != HA_ERR_FOUND_DUPP_KEY &&
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
      if (!counted)
      {
        *update_rows = spider->conns[roop_count]->db_conn->affected_rows();
        DBUG_PRINT("info", ("spider update_rows = %llu", *update_rows));
        *found_rows = spider->conns[roop_count]->db_conn->matched_rows();
        DBUG_PRINT("info", ("spider found_rows = %llu", *found_rows));
        counted = TRUE;
      }
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  }
  spider->reset_sql_sql(SPIDER_SQL_TYPE_UPDATE_SQL);
  DBUG_RETURN(0);
}


int spider_db_bulk_delete(
  ha_spider *spider,
  TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  int error_num;
  DBUG_ENTER("spider_db_bulk_delete");

  if ((error_num = spider->append_delete_sql(table, ptr_diff, TRUE)))
    DBUG_RETURN(error_num);

  if (
    spider->sql_is_filled_up(SPIDER_SQL_TYPE_BULK_UPDATE_SQL) &&
    (error_num = spider_db_bulk_update_size_limit(spider, table))
  )
    DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}

int spider_db_delete(
  ha_spider *spider,
  TABLE *table,
  const uchar *buf
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  my_ptrdiff_t ptr_diff = PTR_BYTE_DIFF(buf, table->record[0]);
  DBUG_ENTER("spider_db_delete");
  if (result_list->bulk_update_mode)
    DBUG_RETURN(spider_db_bulk_delete(spider, table, ptr_diff));

  if ((error_num = spider->append_delete_sql(table, ptr_diff, FALSE)))
    DBUG_RETURN(error_num);

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
    spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    if (dbton_hdl->need_lock_before_set_sql_for_exec(
      SPIDER_SQL_TYPE_DELETE_SQL))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    if ((error_num = dbton_hdl->set_sql_for_exec(
      SPIDER_SQL_TYPE_DELETE_SQL, roop_count)))
    {
      if (dbton_hdl->need_lock_before_set_sql_for_exec(
        SPIDER_SQL_TYPE_DELETE_SQL))
      {
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(error_num);
    }
    if (!dbton_hdl->need_lock_before_set_sql_for_exec(
      SPIDER_SQL_TYPE_DELETE_SQL))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if ((error_num = spider_db_query_with_set_names(
      SPIDER_SQL_TYPE_DELETE_SQL, spider, conn, roop_count)))
    {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      DBUG_RETURN(error_num);
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    result_list->update_sqls[roop_count].length(0);
  }
  if ((error_num = spider->reset_sql_sql(SPIDER_SQL_TYPE_DELETE_SQL)))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_direct_delete(
  ha_spider *spider,
  TABLE *table,
  ha_rows *delete_rows
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  bool counted = FALSE;
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  DBUG_ENTER("spider_db_direct_delete");

  spider_set_result_list_param(spider);
  result_list->finish_flg = FALSE;
  result_list->desc_flg = FALSE;
  result_list->sorted = TRUE;
  if (spider->active_index == MAX_KEY)
    result_list->key_info = NULL;
  else
    result_list->key_info = &table->key_info[spider->active_index];
  spider_get_select_limit(spider, &select_lex, &select_limit, &offset_limit);
  result_list->limit_num =
    result_list->internal_limit >= select_limit ?
    select_limit : result_list->internal_limit;
  result_list->internal_offset += offset_limit;
  if (spider->direct_update_kinds & SPIDER_SQL_KIND_SQL)
  {
    if (
      (error_num = spider->append_delete_sql_part()) ||
      (error_num = spider->append_from_sql_part(SPIDER_SQL_TYPE_DELETE_SQL))
    ) {
      DBUG_RETURN(error_num);
    }
    spider->set_where_pos_sql(SPIDER_SQL_TYPE_DELETE_SQL);
    if (
      (error_num = spider->append_key_where_sql_part(
        NULL,
        NULL,
        SPIDER_SQL_TYPE_DELETE_SQL)) ||
      (error_num = spider->
        append_key_order_for_direct_order_limit_with_alias_sql_part(
        NULL, 0, SPIDER_SQL_TYPE_DELETE_SQL)) ||
      (error_num = spider->append_limit_sql_part(
        result_list->internal_offset, result_list->limit_num,
        SPIDER_SQL_TYPE_DELETE_SQL))
    ) {
      DBUG_RETURN(error_num);
    }
  }

  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    ulong sql_type;
    DBUG_PRINT("info", ("spider exec sql"));
    conn = spider->conns[roop_count];
    sql_type = SPIDER_SQL_TYPE_DELETE_SQL;
    spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    if ((error_num = dbton_hdl->set_sql_for_exec(sql_type, roop_count)))
    {
      if (dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
      {
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(error_num);
    }
    if (!dbton_hdl->need_lock_before_set_sql_for_exec(sql_type))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
      conn->need_mon = &spider->need_mons[roop_count];
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if ((error_num = spider_db_set_names(spider, conn, roop_count)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
      spider_conn_set_timeout_from_share(conn, roop_count,
        spider->wide_handler->trx->thd,
        share);
      if (dbton_hdl->execute_sql(
        sql_type,
        conn,
        -1,
        &spider->need_mons[roop_count])
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        error_num = spider_db_errorno(conn);
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      if (!counted)
      {
        *delete_rows = spider->conns[roop_count]->db_conn->affected_rows();
        DBUG_PRINT("info", ("spider delete_rows = %llu", *delete_rows));
        counted = TRUE;
      }
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  }
  int error_num2 = 0;
  if (spider->direct_update_kinds & SPIDER_SQL_KIND_SQL)
  {
    if ((error_num = spider->reset_sql_sql(SPIDER_SQL_TYPE_DELETE_SQL)))
      error_num2 = error_num;
  }
  DBUG_RETURN(error_num2);
}

int spider_db_delete_all_rows(
  ha_spider *spider
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_db_delete_all_rows");
  if ((error_num = spider->append_delete_all_rows_sql_part(
    SPIDER_SQL_TYPE_DELETE_SQL)))
    DBUG_RETURN(error_num);

  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    uint dbton_id = share->use_sql_dbton_ids[roop_count];
    spider_db_handler *dbton_hdl = spider->dbton_handler[dbton_id];
    conn = spider->conns[roop_count];
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    if (dbton_hdl->need_lock_before_set_sql_for_exec(
      SPIDER_SQL_TYPE_DELETE_SQL))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    if ((error_num = dbton_hdl->set_sql_for_exec(
      SPIDER_SQL_TYPE_DELETE_SQL, roop_count)))
    {
      if (dbton_hdl->need_lock_before_set_sql_for_exec(
        SPIDER_SQL_TYPE_DELETE_SQL))
      {
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
      DBUG_RETURN(error_num);
    }
    if (!dbton_hdl->need_lock_before_set_sql_for_exec(
      SPIDER_SQL_TYPE_DELETE_SQL))
    {
      pthread_mutex_lock(&conn->mta_conn_mutex);
      SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    }
    conn->need_mon = &spider->need_mons[roop_count];
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    spider_conn_set_timeout_from_share(conn, roop_count,
      spider->wide_handler->trx->thd,
      share);
    if (
      (error_num = spider_db_set_names(spider, conn, roop_count)) ||
      (
        dbton_hdl->execute_sql(
          SPIDER_SQL_TYPE_DELETE_SQL,
          conn,
          -1,
          &spider->need_mons[roop_count]) &&
        (error_num = spider_db_errorno(conn))
      )
    ) {
      if (
        error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
        !conn->disable_reconnect
      ) {
        /* retry */
        if ((error_num = spider_db_ping(spider, conn, roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            spider->need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
        if ((error_num = spider_db_set_names(spider, conn, roop_count)))
        {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
          if (
            share->monitoring_kind[roop_count] &&
            spider->need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
        spider_conn_set_timeout_from_share(conn, roop_count,
          spider->wide_handler->trx->thd,
          share);
        if (dbton_hdl->execute_sql(
          SPIDER_SQL_TYPE_DELETE_SQL,
          conn,
          -1,
          &spider->need_mons[roop_count])
        ) {
          DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          error_num = spider_db_errorno(conn);
          if (
            share->monitoring_kind[roop_count] &&
            spider->need_mons[roop_count]
          ) {
            error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
          }
          DBUG_RETURN(error_num);
        }
      } else {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
  }
  if ((error_num = spider->reset_sql_sql(SPIDER_SQL_TYPE_DELETE_SQL)))
    DBUG_RETURN(error_num);
  DBUG_RETURN(0);
}

int spider_db_disable_keys(
  ha_spider *spider
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_disable_keys");
  if (
    spider_param_internal_optimize(spider->wide_handler->trx->thd,
      share->internal_optimize) == 1
  ) {
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
      dbton_hdl = spider->dbton_handler[conn->dbton_id];
      if ((error_num = dbton_hdl->disable_keys(conn, roop_count)))
      {
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_enable_keys(
  ha_spider *spider
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_enable_keys");
  if (
    spider_param_internal_optimize(spider->wide_handler->trx->thd,
      share->internal_optimize) == 1
  ) {
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
      dbton_hdl = spider->dbton_handler[conn->dbton_id];
      if ((error_num = dbton_hdl->enable_keys(conn, roop_count)))
      {
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_check_table(
  ha_spider *spider,
  HA_CHECK_OPT* check_opt
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_check_table");
  if (
    spider_param_internal_optimize(spider->wide_handler->trx->thd,
      share->internal_optimize) == 1
  ) {
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
      dbton_hdl = spider->dbton_handler[conn->dbton_id];
      if ((error_num = dbton_hdl->check_table(conn, roop_count, check_opt)))
      {
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_repair_table(
  ha_spider *spider,
  HA_CHECK_OPT* check_opt
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_repair_table");
  if (
    spider_param_internal_optimize(spider->wide_handler->trx->thd,
      share->internal_optimize) == 1
  ) {
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
      dbton_hdl = spider->dbton_handler[conn->dbton_id];
      if ((error_num = dbton_hdl->repair_table(conn, roop_count, check_opt)))
      {
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_analyze_table(
  ha_spider *spider
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_analyze_table");
  if (
    spider_param_internal_optimize(spider->wide_handler->trx->thd,
      share->internal_optimize) == 1
  ) {
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
      dbton_hdl = spider->dbton_handler[conn->dbton_id];
      if ((error_num = dbton_hdl->analyze_table(conn, roop_count)))
      {
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_optimize_table(
  ha_spider *spider
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_optimize_table");
  if (
    spider_param_internal_optimize(spider->wide_handler->trx->thd,
      share->internal_optimize) == 1
  ) {
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
      dbton_hdl = spider->dbton_handler[conn->dbton_id];
      if ((error_num = dbton_hdl->optimize_table(conn, roop_count)))
      {
        if (
          share->monitoring_kind[roop_count] &&
          spider->need_mons[roop_count]
        ) {
          error_num = spider_ping_table_mon_from_table(
              spider->wide_handler->trx,
              spider->wide_handler->trx->thd,
              share,
              roop_count,
              (uint32) share->monitoring_sid[roop_count],
              share->table_name,
              share->table_name_length,
              spider->conn_link_idx[roop_count],
              NULL,
              0,
              share->monitoring_kind[roop_count],
              share->monitoring_limit[roop_count],
              share->monitoring_flag[roop_count],
              TRUE
            );
        }
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_flush_tables(
  ha_spider *spider,
  bool lock
) {
  int error_num, roop_count;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_flush_tables");
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
    dbton_hdl = spider->dbton_handler[conn->dbton_id];
    if ((error_num = dbton_hdl->flush_tables(conn, roop_count, lock)))
    {
      if (
        share->monitoring_kind[roop_count] &&
        spider->need_mons[roop_count]
      ) {
        error_num = spider_ping_table_mon_from_table(
            spider->wide_handler->trx,
            spider->wide_handler->trx->thd,
            share,
            roop_count,
            (uint32) share->monitoring_sid[roop_count],
            share->table_name,
            share->table_name_length,
            spider->conn_link_idx[roop_count],
            NULL,
            0,
            share->monitoring_kind[roop_count],
            share->monitoring_limit[roop_count],
            share->monitoring_flag[roop_count],
            TRUE
          );
      }
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_db_flush_logs(
  ha_spider *spider
) {
  int roop_count, error_num;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_db_flush_logs");
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
    dbton_hdl = spider->dbton_handler[conn->dbton_id];
    if ((error_num = dbton_hdl->flush_logs(conn, roop_count)))
    {
      if (
        share->monitoring_kind[roop_count] &&
        spider->need_mons[roop_count]
      ) {
        error_num = spider_ping_table_mon_from_table(
            spider->wide_handler->trx,
            spider->wide_handler->trx->thd,
            share,
            roop_count,
            (uint32) share->monitoring_sid[roop_count],
            share->table_name,
            share->table_name_length,
            spider->conn_link_idx[roop_count],
            NULL,
            0,
            share->monitoring_kind[roop_count],
            share->monitoring_limit[roop_count],
            share->monitoring_flag[roop_count],
            TRUE
          );
      }
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

/**
  Find the field among the items in an expression tree.

  @param  item_list         List of items of the expression.
  @param  item_count        Number of items in the item list.
  @param  start_item        Index of the first item to consider.
  @param  str               String into which the expression is to be printed.
  @param  func_name         Function or operator name.
  @param  func_name_length  Length of function or operator name.

  @return                   Pointer to the field in the item list if the list
                            contains only one field; NULL otherwise.
*/

Field *spider_db_find_field_in_item_list(
  Item **item_list,
  uint item_count,
  uint start_item,
  spider_string *str,
  const char *func_name,
  int func_name_length
) {
  uint item_num;
  Item *item;
  Field *field = NULL;
  DBUG_ENTER("spider_db_find_field_in_item_list");

  if (str && func_name_length)
  {
    if (strncasecmp(func_name, ",", 1))
    {
      /* A known function or operator */
      for (item_num = start_item; item_num < item_count; item_num++)
      {
        item = item_list[item_num];

        if (item->type() == Item::FIELD_ITEM)
        {
          if (field)
          {
            /* Field is not relevant if there are multiple fields */
            DBUG_RETURN(NULL);
          }

          field = ((Item_field *) item)->field;
        }
      }
    }
  }

  DBUG_RETURN(field);
}

/**
  Print an operand value within a statement generated for an expression.

  @param  item              Operand value to print.
  @param  field             Field related to the operand value.
  @param  spider            Spider.
  @param  str               String into which the value is to be printed.
  @param  alias             Name related to the operand.
  @param  alias_length      Length of the name.
  @param  dbton_id          Spider Db/Table id.
  @param  use_fields        Use fields or exchange fields.
  @param  fields            Array of fields in the expression.

  @return                   Error code.
*/

int spider_db_print_item_type(
  Item *item,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  DBUG_ENTER("spider_db_print_item_type");
  DBUG_PRINT("info",("spider COND type=%d", item->type()));

  switch (item->type())
  {
    case Item::FUNC_ITEM:
      DBUG_RETURN(spider_db_open_item_func((Item_func *) item, spider, str,
        alias, alias_length, dbton_id, use_fields, fields));
    case Item::SUM_FUNC_ITEM:
      DBUG_RETURN(spider_db_open_item_sum_func((Item_sum *)item, spider, str,
        alias, alias_length, dbton_id, use_fields, fields));
    case Item::COND_ITEM:
      DBUG_RETURN(spider_db_open_item_cond((Item_cond *) item, spider, str,
        alias, alias_length, dbton_id, use_fields, fields));
    case Item::FIELD_ITEM:
      DBUG_RETURN(spider_db_open_item_field((Item_field *) item, spider, str,
        alias, alias_length, dbton_id, use_fields, fields));
    case Item::REF_ITEM:
      DBUG_RETURN(spider_db_open_item_ref((Item_ref *) item, spider, str,
        alias, alias_length, dbton_id, use_fields, fields));
    case Item::ROW_ITEM:
      DBUG_RETURN(spider_db_open_item_row((Item_row *) item, spider, str,
        alias, alias_length, dbton_id, use_fields, fields));
    case Item::CONST_ITEM:
    {
      switch (item->cmp_type()) {
        case TIME_RESULT:
        case STRING_RESULT:
          DBUG_RETURN(spider_db_open_item_string(item, field, spider, str,
            alias, alias_length, dbton_id, use_fields, fields));
        case INT_RESULT:
        case REAL_RESULT:
        case DECIMAL_RESULT:
          DBUG_RETURN(spider_db_open_item_int(item, field, spider, str,
            alias, alias_length, dbton_id, use_fields, fields));
        default:
          DBUG_ASSERT(FALSE);
          DBUG_RETURN(spider_db_print_item_type_default(item, spider, str));
      }
    }
    case Item::CACHE_ITEM:
      DBUG_RETURN(spider_db_open_item_cache((Item_cache *) item, field, spider,
        str, alias, alias_length, dbton_id, use_fields, fields));
    case Item::INSERT_VALUE_ITEM:
      DBUG_RETURN(spider_db_open_item_insert_value((Item_insert_value *) item,
        field, spider, str, alias, alias_length, dbton_id, use_fields, fields));
    case Item::SUBSELECT_ITEM:
    case Item::TRIGGER_FIELD_ITEM:
#ifdef SPIDER_HAS_EXPR_CACHE_ITEM
    case Item::EXPR_CACHE_ITEM:
#endif
      DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
    default:
      DBUG_RETURN(spider_db_print_item_type_default(item, spider, str));
  }

  DBUG_RETURN(0);
}

int spider_db_print_item_type_default(
  Item *item,
  ha_spider *spider,
  spider_string *str
) {
  DBUG_ENTER("spider_db_print_item_type_default");
  THD *thd = spider->wide_handler->trx->thd;
  SPIDER_SHARE *share = spider->share;
  if (spider_param_skip_default_condition(thd,
    share->skip_default_condition))
    DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
  if (str)
  {
    if (spider->share->access_charset->cset == system_charset_info->cset)
    {
      item->print(str->get_str(), QT_TO_SYSTEM_CHARSET);
    } else {
      item->print(str->get_str(), QT_ORDINARY);
    }
    str->mem_calc();
  }
  DBUG_RETURN(0);
}

int spider_db_open_item_cond(
  Item_cond *item_cond,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num = 0;
  List_iterator_fast<Item> lif(*(item_cond->argument_list()));
  Item *item;
  LEX_CSTRING func_name= {0,0};
  int restart_pos = 0;
  DBUG_ENTER("spider_db_open_item_cond");
  if (str)
  {
    if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  }

restart_first:
  if ((item = lif++))
  {
    if (str)
      restart_pos = str->length();
    if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
      alias, alias_length, dbton_id, use_fields, fields)))
    {
      if (
        str &&
        error_num == ER_SPIDER_COND_SKIP_NUM &&
        item_cond->functype() == Item_func::COND_AND_FUNC
      ) {
        DBUG_PRINT("info",("spider COND skip"));
        str->length(restart_pos);
        goto restart_first;
      }
      DBUG_RETURN(error_num);
    }
  }
  if (error_num)
    DBUG_RETURN(error_num);
  while ((item = lif++))
  {
    if (str)
    {
      restart_pos = str->length();
      if (!func_name.str)
        func_name= item_cond->func_name_cstring();

      if (str->reserve(func_name.length + SPIDER_SQL_SPACE_LEN * 2))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      str->q_append(func_name.str, func_name.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    }

    if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
      alias, alias_length, dbton_id, use_fields, fields)))
    {
      if (
        str &&
        error_num == ER_SPIDER_COND_SKIP_NUM &&
        item_cond->functype() == Item_func::COND_AND_FUNC
      ) {
        DBUG_PRINT("info",("spider COND skip"));
        str->length(restart_pos);
      } else
        DBUG_RETURN(error_num);
    }
  }
  if (str)
  {
    if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  }
  DBUG_RETURN(0);
}

int spider_db_open_item_func(
  Item_func *item_func,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  DBUG_ENTER("spider_db_open_item_func");
  DBUG_RETURN(spider_dbton[dbton_id].db_util->open_item_func(
    item_func, spider, str, alias, alias_length, use_fields, fields));
}

int spider_db_open_item_sum_func(
  Item_sum *item_sum,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  DBUG_ENTER("spider_db_open_item_func");
  DBUG_RETURN(spider_dbton[dbton_id].db_util->open_item_sum_func(
    item_sum, spider, str, alias, alias_length, use_fields, fields));
}

int spider_db_open_item_ident(
  Item_ident *item_ident,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num, field_name_length;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_db_open_item_ident");
  if (
    item_ident->cached_field_index != NO_CACHED_FIELD_INDEX &&
    item_ident->cached_table
  ) {
    Field *field = item_ident->cached_table->table->field[
      item_ident->cached_field_index];
    DBUG_PRINT("info",("spider use cached_field_index"));
    DBUG_PRINT("info",("spider const_table=%s",
      field->table->const_table ? "TRUE" : "FALSE"));
    if (field->table->const_table)
    {
      if (str)
      {
        String str_value;
        String *tmp_str;
        tmp_str = field->val_str(&str_value);
        if (!tmp_str)
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN * 2 +
          tmp_str->length() * 2))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
        str->append_escape_string(tmp_str->ptr(), tmp_str->length());
        str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      }
    } else {
      if (!use_fields)
      {
        if (!(field = spider->field_exchange(field)))
          DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
        if (str)
        {
          if ((error_num = share->dbton_share[dbton_id]->
            append_column_name_with_alias(str, field->field_index,
            alias, alias_length)))
            DBUG_RETURN(error_num);
        }
      } else {
        if (str)
        {
          SPIDER_FIELD_CHAIN *field_chain = fields->get_next_field_chain();
          SPIDER_FIELD_HOLDER *field_holder = field_chain->field_holder;
          spider = field_holder->spider;
          share = spider->share;
          field = spider->field_exchange(field);
          DBUG_ASSERT(field);
          if ((error_num = share->dbton_share[dbton_id]->
            append_column_name_with_alias(str, field->field_index,
            field_holder->alias->ptr(), field_holder->alias->length())))
            DBUG_RETURN(error_num);
        } else {
          if ((error_num = fields->add_field(field)))
          {
            DBUG_RETURN(error_num);
          }
        }
      }
    }
    DBUG_RETURN(0);
  }
  if (str)
  {
#ifdef SPIDER_use_LEX_CSTRING_for_KEY_Field_name
    if (item_ident->field_name.str)
      field_name_length = item_ident->field_name.length;
#else
    if (item_ident->field_name)
      field_name_length = strlen(item_ident->field_name);
#endif
    else
      field_name_length = 0;
    if (share->access_charset->cset == system_charset_info->cset)
    {
      if (str->reserve(alias_length +
        field_name_length + /* SPIDER_SQL_NAME_QUOTE_LEN */ 2))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(alias, alias_length);
#ifdef SPIDER_use_LEX_CSTRING_for_KEY_Field_name
      if ((error_num = spider_dbton[dbton_id].db_util->
        append_escaped_name(str, item_ident->field_name.str,
          field_name_length)))
#else
      if ((error_num = spider_dbton[dbton_id].db_util->
        append_escaped_name(str, item_ident->field_name, field_name_length)))
#endif
      {
        DBUG_RETURN(error_num);
      }
    } else {
      if (str->reserve(alias_length))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(alias, alias_length);
#ifdef SPIDER_use_LEX_CSTRING_for_KEY_Field_name
      if ((error_num = spider_dbton[dbton_id].db_util->
        append_escaped_name_with_charset(str, item_ident->field_name.str,
          field_name_length, system_charset_info)))
#else
      if ((error_num = spider_dbton[dbton_id].db_util->
        append_escaped_name_with_charset(str, item_ident->field_name,
          field_name_length, system_charset_info)))
#endif
      {
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_db_open_item_field(
  Item_field *item_field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num;
  Field *field = item_field->field;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_db_open_item_field");
  if (field)
  {
    DBUG_PRINT("info",("spider field=%p", field));
    DBUG_PRINT("info",("spider db=%s", field->table->s->db.str));
    DBUG_PRINT("info",("spider table_name=%s",
      field->table->s->table_name.str));
    DBUG_PRINT("info",("spider const_table=%s",
      field->table->const_table ? "TRUE" : "FALSE"));
    if (field->table->const_table)
    {
      if (str)
      {
        String str_value;
        String *tmp_str;
        tmp_str = field->val_str(&str_value);
        if (!tmp_str)
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN * 2 +
          tmp_str->length() * 2))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
        str->append_escape_string(tmp_str->ptr(), tmp_str->length());
        str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      }
      DBUG_RETURN(0);
    } else {
      DBUG_PRINT("info",("spider tmp_table=%u", field->table->s->tmp_table));
      if (field->table->s->tmp_table != INTERNAL_TMP_TABLE)
      {
        if (!use_fields)
        {
          if (!(field = spider->field_exchange(field)))
            DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
          if (str)
          {
            if ((error_num = share->dbton_share[dbton_id]->
              append_column_name_with_alias(str, field->field_index,
              alias, alias_length)))
              DBUG_RETURN(error_num);
          }
          DBUG_RETURN(0);
        } else {
          if (str)
          {
            SPIDER_FIELD_CHAIN *field_chain = fields->get_next_field_chain();
            SPIDER_FIELD_HOLDER *field_holder = field_chain->field_holder;
            spider = field_holder->spider;
            share = spider->share;
            field = spider->field_exchange(field);
            DBUG_ASSERT(field);
            if ((error_num = share->dbton_share[dbton_id]->
              append_column_name_with_alias(str, field->field_index,
              field_holder->alias->ptr(), field_holder->alias->length())))
              DBUG_RETURN(error_num);
          } else {
            if ((error_num = fields->add_field(field)))
            {
              DBUG_RETURN(error_num);
            }
          }
          DBUG_RETURN(0);
        }
      }
    }
  }
  DBUG_RETURN(spider_db_open_item_ident(
    (Item_ident *) item_field, spider, str, alias, alias_length, dbton_id,
    use_fields, fields));
}

int spider_db_open_item_ref(
  Item_ref *item_ref,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num;
  DBUG_ENTER("spider_db_open_item_ref");
  if (item_ref->ref)
  {
#ifdef SPIDER_use_LEX_CSTRING_for_KEY_Field_name
    if (
      (*(item_ref->ref))->type() != Item::CACHE_ITEM &&
      item_ref->ref_type() != Item_ref::VIEW_REF &&
      !item_ref->table_name.str &&
      item_ref->name.str &&
      item_ref->alias_name_used
    )
#else
    if (
      (*(item_ref->ref))->type() != Item::CACHE_ITEM &&
      item_ref->ref_type() != Item_ref::VIEW_REF &&
      !item_ref->table_name &&
      item_ref->name &&
      item_ref->alias_name_used
    )
#endif
    {
      if (str)
      {
#ifdef SPIDER_use_LEX_CSTRING_for_KEY_Field_name
        uint length = item_ref->name.length;
#else
        uint length = strlen(item_ref->name);
#endif
        if (str->reserve(length + /* SPIDER_SQL_NAME_QUOTE_LEN */ 2))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
#ifdef SPIDER_use_LEX_CSTRING_for_KEY_Field_name
        if ((error_num = spider_dbton[dbton_id].db_util->
          append_name(str, item_ref->name.str, length)))
#else
        if ((error_num = spider_dbton[dbton_id].db_util->
          append_name(str, item_ref->name, length)))
#endif
        {
          DBUG_RETURN(error_num);
        }
      }
      DBUG_RETURN(0);
    }
    DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM); // MDEV-25116
  }
  DBUG_RETURN(spider_db_open_item_ident((Item_ident *) item_ref, spider, str,
    alias, alias_length, dbton_id, use_fields, fields));
}

int spider_db_open_item_row(
  Item_row *item_row,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num;
  uint roop_count, cols = item_row->cols() - 1;
  Item *item;
  DBUG_ENTER("spider_db_open_item_row");
  if (str)
  {
    if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  }
  for (roop_count = 0; roop_count < cols; roop_count++)
  {
    item = item_row->element_index(roop_count);
    if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
      alias, alias_length, dbton_id, use_fields, fields)))
      DBUG_RETURN(error_num);
    if (str)
    {
      if (str->reserve(SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
  }
  item = item_row->element_index(roop_count);
  if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
    alias, alias_length, dbton_id, use_fields, fields)))
    DBUG_RETURN(error_num);
  if (str)
  {
    if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
      SPIDER_SQL_CLOSE_PAREN_LEN);
  }
  DBUG_RETURN(0);
}

/**
  Print a string value within a generated statement.

  @param  item              String value to print.
  @param  field             Field related to the string value.
  @param  spider            Spider.
  @param  str               String into which the value is to be printed.
  @param  alias             Name related to the string value.
  @param  alias_length      Length of the name.
  @param  dbton_id          Spider Db/Table id.
  @param  use_fields        Use fields or exchange fields.
  @param  fields            Array of fields in an expression containing
                            the string value.

  @return                   Error code.
*/

int spider_db_open_item_string(
  Item *item,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num = 0;
  DBUG_ENTER("spider_db_open_item_string");

  if (str)
  {
    THD *thd = NULL;
    TABLE *UNINIT_VAR(table);
    MY_BITMAP *saved_map = NULL;
    Time_zone *UNINIT_VAR(saved_time_zone);
    String str_value;
    char tmp_buf[MAX_FIELD_WIDTH];
    spider_string tmp_str(tmp_buf, MAX_FIELD_WIDTH, str->charset());
    String *tmp_str2;
    tmp_str.init_calc_mem(126);

    if (!(tmp_str2 = item->val_str(tmp_str.get_str())))
    {
      if (str->reserve(SPIDER_SQL_NULL_LEN))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto end;
      }
      str->q_append(SPIDER_SQL_NULL_STR, SPIDER_SQL_NULL_LEN);
    } else {
      if (
        field &&
        field->type() == FIELD_TYPE_TIMESTAMP &&
        field->table->in_use->variables.time_zone != UTC
      ) {
        /*
          Store the string value in the field. This is necessary
          when the statement contains more than one value for the
          same field.
        */
        table = field->table;
        thd = table->in_use;
        saved_map = dbug_tmp_use_all_columns(table, &table->write_set);
        item->save_in_field(field, FALSE);
        saved_time_zone = thd->variables.time_zone;
        thd->variables.time_zone = UTC;

        /* Retrieve the stored value converted to UTC */
        tmp_str2 = field->val_str(&str_value);

        if (!tmp_str2)
        {
          error_num = HA_ERR_OUT_OF_MEM;
          goto end;
        }
      }
      DBUG_PRINT("info",("spider dbton_id=%u", dbton_id));
      if (str->charset() != tmp_str2->charset() &&
        spider_dbton[dbton_id].db_util->append_charset_name_before_string())
      {
        if ((error_num = spider_db_append_charset_name_before_string(str,
          tmp_str2->charset())))
        {
          goto end;
        }
      }
      if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN * 2 +
        tmp_str2->length() * 2))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto end;
      }
      if (!thd)
        tmp_str.mem_calc();
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      str->append_escape_string(tmp_str2->ptr(), tmp_str2->length(),
        tmp_str2->charset());
      if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto end;
      }
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    }

end:
    if (thd)
    {
      thd->variables.time_zone = saved_time_zone;
      dbug_tmp_restore_column_map(&table->write_set, saved_map);
    }
  }

  DBUG_RETURN(error_num);
}

/**
  Print an integer value within a generated statement.

  @param  item              Integer value to print.
  @param  field             Field related to the integer value.
  @param  spider            Spider.
  @param  str               String into which the value is to be printed.
  @param  alias             Name related to the integer value.
  @param  alias_length      Length of the name.
  @param  dbton_id          Spider Db/Table id.
  @param  use_fields        Use fields or exchange fields.
  @param  fields            Array of fields in an expression containing
                            the integer value.

  @return                   Error code.
*/

int spider_db_open_item_int(
  Item *item,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num = 0;
  DBUG_ENTER("spider_db_open_item_int");

  if (str)
  {
    THD *thd = NULL;
    TABLE *table;
    MY_BITMAP *saved_map;
    Time_zone *saved_time_zone;
    String str_value;
    bool print_quoted_string;
    char tmp_buf[MAX_FIELD_WIDTH];
    spider_string tmp_str(tmp_buf, MAX_FIELD_WIDTH, str->charset());
    String *tmp_str2;
    tmp_str.init_calc_mem(127);

    if (!(tmp_str2 = item->val_str(tmp_str.get_str())))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto end;
    }
    tmp_str.mem_calc();

    if (
      field &&
      field->type() == FIELD_TYPE_TIMESTAMP &&
      field->table->in_use->variables.time_zone != UTC
    ) {
      /*
        Store the int value in the field.  This is necessary
        when the statement contains more than one value for the
        same field.
      */
      table = field->table;
      thd = table->in_use;
      saved_map = dbug_tmp_use_all_columns(table, &table->write_set);
      item->save_in_field(field, FALSE);
      saved_time_zone = thd->variables.time_zone;
      thd->variables.time_zone = UTC;
      print_quoted_string = TRUE;
    } else {
#ifdef SPIDER_ITEM_HAS_CMP_TYPE
      DBUG_PRINT("info",("spider cmp_type=%u", item->cmp_type()));
      if (item->cmp_type() == TIME_RESULT)
        print_quoted_string = TRUE;
      else
#endif
        print_quoted_string = FALSE;
    }

    if (print_quoted_string)
    {
      if (thd)
      {
        /* Retrieve the stored value converted to UTC */
        tmp_str2 = field->val_str(&str_value);

        if (!tmp_str2)
        {
          error_num = HA_ERR_OUT_OF_MEM;
          goto end;
        }
      }

      if (str->reserve(SPIDER_SQL_VALUE_QUOTE_LEN * 2 + tmp_str2->length()))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto end;
      }
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      str->append(*tmp_str2);
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    } else {
      if (str->append(*tmp_str2))
        error_num = HA_ERR_OUT_OF_MEM;
    }

end:
    if (thd)
    {
      thd->variables.time_zone = saved_time_zone;
      dbug_tmp_restore_column_map(&table->write_set, saved_map);
    }
  }

  DBUG_RETURN(error_num);
}

/**
  Print a cached value within a generated statement.

  @param  item              Cached value to print.
  @param  field             Field related to the cached value.
  @param  spider            Spider.
  @param  str               String into which the value is to be printed.
  @param  alias             Name related to the cached value.
  @param  alias_length      Length of the name.
  @param  dbton_id          Spider Db/Table id.
  @param  use_fields        Use fields or exchange fields.
  @param  fields            Array of fields in the expression containing
                            the cached value.

  @return                   Error code.
*/

int spider_db_open_item_cache(
  Item_cache *item_cache,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  DBUG_ENTER("spider_db_open_item_cache");
  if (!item_cache->const_item())
    DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
  DBUG_PRINT("info",("spider result_type=%u", item_cache->result_type()));

  switch (item_cache->result_type())
  {
    case STRING_RESULT:
      DBUG_RETURN(spider_db_open_item_string(item_cache, field, spider, str,
        alias, alias_length, dbton_id, use_fields, fields));
    case ROW_RESULT:
      {
        int error_num;
        Item_cache_row *item_cache_row = (Item_cache_row *) item_cache;
        uint item_count = item_cache_row->cols() - 1, roop_count;
        if (str)
        {
          if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
        }
        for (roop_count = 0; roop_count < item_count; ++roop_count)
        {
          if ((error_num = spider_db_open_item_cache(
            (Item_cache *) item_cache_row->element_index(roop_count), NULL,
            spider, str, alias, alias_length, dbton_id, use_fields, fields
          ))) {
            DBUG_RETURN(error_num);
          }
          if (str)
          {
            if (str->reserve(SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          }
        }
        if ((error_num = spider_db_open_item_cache(
          (Item_cache *) item_cache_row->element_index(roop_count), NULL,
          spider, str, alias, alias_length, dbton_id, use_fields, fields
        ))) {
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
      DBUG_RETURN(0);
    case REAL_RESULT:
    case INT_RESULT:
    case DECIMAL_RESULT:
    default:
      break;
  }
  DBUG_RETURN(spider_db_open_item_int(item_cache, field, spider, str,
    alias, alias_length, dbton_id, use_fields, fields));
}

/**
  Print an INSERT value within a generated INSERT statement.

  @param  item              INSERT value to print.
  @param  field             Field related to the INSERT value.
  @param  spider            Spider.
  @param  str               String into which the value is to be printed.
  @param  alias             Name related to the INSERT value.
  @param  alias_length      Length of the name.
  @param  dbton_id          Spider Db/Table id.
  @param  use_fields        Use fields or exchange fields.
  @param  fields            Array of fields in the expression.

  @return                   Error code.
*/

int spider_db_open_item_insert_value(
  Item_insert_value *item_insert_value,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num;
  DBUG_ENTER("spider_db_open_item_insert_value");

  if (item_insert_value->arg)
  {
    if (str)
    {
      if (str->reserve(SPIDER_SQL_VALUES_LEN + SPIDER_SQL_OPEN_PAREN_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_VALUES_STR, SPIDER_SQL_VALUES_LEN);
      str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
    }
    if ((error_num = spider_db_print_item_type(item_insert_value->arg, field,
      spider, str, alias, alias_length, dbton_id, use_fields, fields)))
      DBUG_RETURN(error_num);
    if (str)
    {
      if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
    }
  }

  DBUG_RETURN(0);
}

int spider_db_append_condition(
  ha_spider *spider,
  const char *alias,
  uint alias_length,
  bool test_flg
) {
  int error_num;
  DBUG_ENTER("spider_db_append_condition");
  if (!test_flg)
  {
    if (spider->sql_kinds & SPIDER_SQL_KIND_SQL)
    {
      if ((error_num = spider->append_condition_sql_part(
        alias, alias_length, SPIDER_SQL_TYPE_SELECT_SQL, FALSE)))
        DBUG_RETURN(error_num);
    }
    if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      if ((error_num = spider->append_condition_sql_part(
        alias, alias_length, SPIDER_SQL_TYPE_HANDLER, FALSE)))
        DBUG_RETURN(error_num);
    }
  } else {
    if (spider->wide_handler->cond_check)
      DBUG_RETURN(spider->wide_handler->cond_check_error);
    spider->wide_handler->cond_check = TRUE;
    if ((spider->wide_handler->cond_check_error =
      spider->append_condition_sql_part(
        NULL, 0, SPIDER_SQL_TYPE_SELECT_SQL, TRUE)))
      DBUG_RETURN(spider->wide_handler->cond_check_error);
  }
  DBUG_RETURN(0);
}

int spider_db_append_update_columns(
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
) {
  int error_num;
  bool add_comma = FALSE;
  List_iterator_fast<Item> fi(*spider->wide_handler->direct_update_fields),
    vi(*spider->wide_handler->direct_update_values);
  Item *field, *value;
  DBUG_ENTER("spider_db_append_update_columns");
  while ((field = fi++))
  {
    value = vi++;
    if ((error_num = spider_db_print_item_type(
      (Item *) field, NULL, spider, str, alias, alias_length, dbton_id,
      use_fields, fields)))
    {
      if (
        error_num == ER_SPIDER_COND_SKIP_NUM &&
        field->type() == Item::FIELD_ITEM &&
        ((Item_field *) field)->field
      ) {
        DBUG_PRINT("info",("spider no match field(ex. vp child table)"));
        continue;
      }
      DBUG_RETURN(error_num);
    }
    if (str)
    {
      if (str->reserve(SPIDER_SQL_EQUAL_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_EQUAL_STR, SPIDER_SQL_EQUAL_LEN);
    }
    if ((error_num = spider_db_print_item_type(
      (Item *) value, ((Item_field *) field)->field, spider, str,
      alias, alias_length, dbton_id, use_fields, fields)))
      DBUG_RETURN(error_num);
    if (str)
    {
      if (str->reserve(SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      add_comma = TRUE;
    }
  }
  if (str && add_comma)
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

bool spider_db_check_select_colum_in_group(
  st_select_lex *select_lex,
  Field *field
) {
  ORDER *group;
  DBUG_ENTER("spider_db_check_select_colum_in_group");
  for (group = (ORDER *) select_lex->group_list.first; group;
    group = group->next)
  {
    Item *item = *group->item;
    if (item->type() == Item::FIELD_ITEM)
    {
      Item_field *item_field = (Item_field *) item;
      if (item_field->field == field)
      {
        /* This field can be used directly */
        DBUG_RETURN(TRUE);
      }
    }
  }
  DBUG_RETURN(FALSE);
}

uint spider_db_check_ft_idx(
  Item_func *item_func,
  ha_spider *spider
) {
  uint roop_count, roop_count2, part_num;
  uint item_count = item_func->argument_count();
  Item **item_list = item_func->arguments();
  Item_field *item_field;
  Field *field;
  TABLE *table = spider->get_table();
  TABLE_SHARE *table_share = table->s;
  KEY *key_info;
  KEY_PART_INFO *key_part;
  bool match1, match2;
  DBUG_ENTER("spider_db_check_ft_idx");

  for (roop_count = 0; roop_count < table_share->keys; roop_count++)
  {
    key_info = &table->key_info[roop_count];
    if (
      key_info->algorithm == HA_KEY_ALG_FULLTEXT &&
      item_count - 1 == spider_user_defined_key_parts(key_info)
    ) {
      match1 = TRUE;
      for (roop_count2 = 1; roop_count2 < item_count; roop_count2++)
      {
        item_field = (Item_field *) item_list[roop_count2];
        field = item_field->field;
        if (!(field = spider->field_exchange(field)))
          DBUG_RETURN(MAX_KEY);
        match2 = FALSE;
        for (key_part = key_info->key_part, part_num = 0;
          part_num < spider_user_defined_key_parts(key_info);
          key_part++, part_num++)
        {
          if (key_part->field == field)
          {
            match2 = TRUE;
            break;
          }
        }
        if (!match2)
        {
          match1 = FALSE;
          break;
        }
      }
      if (match1)
        DBUG_RETURN(roop_count);
    }
  }
  DBUG_RETURN(MAX_KEY);
}

int spider_db_udf_fetch_row(
  SPIDER_TRX *trx,
  Field *field,
  SPIDER_DB_ROW *row
) {
  DBUG_ENTER("spider_db_udf_fetch_row");
  DBUG_RETURN(row->store_to_field(field, trx->udf_access_charset));
  DBUG_RETURN(0);
}

int spider_db_udf_fetch_table(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn,
  TABLE *table,
  SPIDER_DB_RESULT *result,
  uint set_on,
  uint set_off
) {
  int error_num;
  SPIDER_DB_ROW *row = NULL;
  Field **field;
  uint roop_count;
  DBUG_ENTER("spider_db_udf_fetch_table");
  if (!(row = result->fetch_row()))
    DBUG_RETURN(HA_ERR_END_OF_FILE);

#ifndef DBUG_OFF
  MY_BITMAP *tmp_map =
    dbug_tmp_use_all_columns(table, &table->write_set);
#endif
  for (
    roop_count = 0,
    field = table->field;
    roop_count < set_on;
    roop_count++,
    field++
  ) {
    if ((error_num =
      spider_db_udf_fetch_row(trx, *field, row)))
    {
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
      DBUG_RETURN(error_num);
    }
    row->next();
  }

  for (; roop_count < set_off; roop_count++, field++)
    (*field)->set_default();
#ifndef DBUG_OFF
  dbug_tmp_restore_column_map(&table->write_set, tmp_map);
#endif
  table->status = 0;
  DBUG_RETURN(0);
}

int spider_db_udf_direct_sql_connect(
  const SPIDER_DIRECT_SQL *direct_sql,
  SPIDER_CONN *conn
) {
  int error_num, connect_retry_count;
  THD* thd = current_thd;
  longlong connect_retry_interval;
  DBUG_ENTER("spider_db_udf_direct_sql_connect");

  if (thd)
  {
    conn->connect_timeout = spider_param_connect_timeout(thd,
      direct_sql->connect_timeout);
    conn->net_read_timeout = spider_param_net_read_timeout(thd,
      direct_sql->net_read_timeout);
    conn->net_write_timeout = spider_param_net_write_timeout(thd,
      direct_sql->net_write_timeout);
    connect_retry_interval = spider_param_connect_retry_interval(thd);
    connect_retry_count = spider_param_connect_retry_count(thd);
  } else {
    conn->connect_timeout = spider_param_connect_timeout(NULL,
      direct_sql->connect_timeout);
    conn->net_read_timeout = spider_param_net_read_timeout(NULL,
      direct_sql->net_read_timeout);
    conn->net_write_timeout = spider_param_net_write_timeout(NULL,
      direct_sql->net_write_timeout);
    connect_retry_interval = spider_param_connect_retry_interval(NULL);
    connect_retry_count = spider_param_connect_retry_count(NULL);
  }
  DBUG_PRINT("info",("spider connect_timeout=%u", conn->connect_timeout));
  DBUG_PRINT("info",("spider net_read_timeout=%u", conn->net_read_timeout));
  DBUG_PRINT("info",("spider net_write_timeout=%u", conn->net_write_timeout));

    if ((error_num = spider_reset_conn_setted_parameter(conn, thd)))
      DBUG_RETURN(error_num);

  if (conn->dbton_id == SPIDER_DBTON_SIZE)
  {
      my_printf_error(
        ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM,
        ER_SPIDER_SQL_WRAPPER_IS_INVALID_STR,
        MYF(0), conn->tgt_wrapper);
      DBUG_RETURN(ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM);
  }

/*
  if (!(conn->db_conn = spider_dbton[conn->dbton_id].create_db_conn(conn)))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
*/

  if ((error_num = conn->db_conn->connect(
    direct_sql->tgt_host,
    direct_sql->tgt_username,
    direct_sql->tgt_password,
    direct_sql->tgt_port,
    direct_sql->tgt_socket,
    direct_sql->server_name,
    connect_retry_count, connect_retry_interval)))
  {
    DBUG_RETURN(error_num);
  }
  ++conn->connection_id;
  DBUG_RETURN(0);
}

int spider_db_udf_direct_sql_ping(
  SPIDER_DIRECT_SQL *direct_sql
) {
  int error_num;
  SPIDER_CONN *conn = direct_sql->conn;
  DBUG_ENTER("spider_db_udf_direct_sql_ping");
  if (conn->server_lost)
  {
    if ((error_num = spider_db_udf_direct_sql_connect(direct_sql, conn)))
      DBUG_RETURN(error_num);
    conn->server_lost = FALSE;
  }
    if ((error_num = conn->db_conn->ping()))
    {
      spider_db_disconnect(conn);
      if ((error_num = spider_db_udf_direct_sql_connect(direct_sql, conn)))
      {
        DBUG_PRINT("info", ("spider conn=%p SERVER_LOST", conn));
        conn->server_lost = TRUE;
        DBUG_RETURN(error_num);
      }
      if((error_num = conn->db_conn->ping()))
      {
        spider_db_disconnect(conn);
        DBUG_PRINT("info", ("spider conn=%p SERVER_LOST", conn));
        conn->server_lost = TRUE;
        DBUG_RETURN(error_num);
      }
    }
  conn->ping_time = (time_t) time((time_t*) 0);
  DBUG_RETURN(0);
}

int spider_db_udf_direct_sql(
  SPIDER_DIRECT_SQL *direct_sql
) {
  int error_num = 0, status = 0, roop_count = 0, need_mon = 0;
  uint udf_table_mutex_index, field_num, set_on, set_off;
  long long roop_count2;
  bool end_of_file;
  SPIDER_TRX *trx = direct_sql->trx;
  THD *thd = trx->thd, *c_thd = current_thd;
  SPIDER_CONN *conn = direct_sql->conn;
  SPIDER_DB_RESULT *result = NULL;
  TABLE *table;
  int bulk_insert_rows = (int) spider_param_udf_ds_bulk_insert_rows(thd,
    direct_sql->bulk_insert_rows);
  int table_loop_mode = spider_param_udf_ds_table_loop_mode(thd,
    direct_sql->table_loop_mode);
  double ping_interval_at_trx_start =
    spider_param_ping_interval_at_trx_start(thd);
  time_t tmp_time = (time_t) time((time_t*) 0);
  bool need_trx_end, need_all_commit, insert_start = FALSE;
  enum_sql_command sql_command_backup;
  DBUG_ENTER("spider_db_udf_direct_sql");
  if (direct_sql->real_table_used)
  {
    if (spider_sys_open_and_lock_tables(c_thd, &direct_sql->table_list_first,
                               &direct_sql->open_tables_backup))
    {
      direct_sql->real_table_used = FALSE;
      DBUG_RETURN(my_errno);
    }
    for (roop_count = 0; roop_count < direct_sql->table_count; roop_count++)
    {
      if (!spider_bit_is_set(direct_sql->real_table_bitmap, roop_count))
        continue;
      direct_sql->tables[roop_count] =
        direct_sql->table_list[roop_count].table;
    }
    direct_sql->open_tables_thd = c_thd;
    roop_count = 0;
  }

  if (c_thd != thd)
  {
    need_all_commit = TRUE;
    need_trx_end = TRUE;
  } else {
    need_all_commit = FALSE;
    if (direct_sql->real_table_used)
    {
      need_trx_end = TRUE;
    } else {
      if (c_thd->transaction->stmt.ha_list)
        need_trx_end = FALSE;
      else
        need_trx_end = TRUE;
    }
  }

  if (!conn->disable_reconnect)
  {
    if (
      (
        conn->server_lost ||
        difftime(tmp_time, conn->ping_time) >= ping_interval_at_trx_start
      ) &&
      (error_num = spider_db_udf_direct_sql_ping(direct_sql))
    )
      DBUG_RETURN(error_num);
  } else if (conn->server_lost)
  {
    my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
      ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM);
  }

  sql_command_backup = c_thd->lex->sql_command;
  c_thd->lex->sql_command = SQLCOM_INSERT;

  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &need_mon;
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (
    !(error_num = spider_db_udf_direct_sql_set_names(direct_sql, trx, conn)) &&
    !(error_num = spider_db_udf_direct_sql_select_db(direct_sql, conn))
  ) {
    spider_conn_set_timeout_from_direct_sql(conn, thd, direct_sql);
    if (spider_db_query(
      conn,
      direct_sql->sql,
      direct_sql->sql_length,
      -1,
      &need_mon)
    ) {
      error_num = spider_db_errorno(conn);
      if (error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM)
        my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
          ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
    } else {
      DBUG_PRINT("info",("spider conn=%p", conn));
      if (!direct_sql->table_count)
        roop_count = -1;
      do {
        if (roop_count == direct_sql->table_count)
        {
          if (table_loop_mode == 1)
            roop_count--;
          else if (table_loop_mode == 2)
            roop_count = 0;
          else
            roop_count = -1;
        }
        st_spider_db_request_key request_key;
        request_key.spider_thread_id = direct_sql->trx->spider_thread_id;
        request_key.query_id = direct_sql->trx->thd->query_id;
        request_key.handler = direct_sql;
        request_key.request_id = 1;
        request_key.next = NULL;
        if ((result = conn->db_conn->use_result(NULL, &request_key, &error_num)))
        {
          end_of_file = FALSE;
          if (roop_count >= 0)
          {
            while (!error_num && !end_of_file)
            {
              udf_table_mutex_index = spider_udf_calc_hash(
                direct_sql->db_names[roop_count],
                spider_param_udf_table_lock_mutex_count());
              udf_table_mutex_index += spider_udf_calc_hash(
                direct_sql->table_names[roop_count],
                spider_param_udf_table_lock_mutex_count());
              udf_table_mutex_index %=
                spider_param_udf_table_lock_mutex_count();
              pthread_mutex_lock(
                &trx->udf_table_mutexes[udf_table_mutex_index]);
              table = direct_sql->tables[roop_count];
              table->in_use = c_thd;
              memset((uchar *) table->null_flags, ~(uchar) 0,
                sizeof(uchar) * table->s->null_bytes);
              insert_start = TRUE;

              field_num = result->num_fields();
              if (field_num > table->s->fields)
              {
                set_on = table->s->fields;
                set_off = table->s->fields;
              } else {
                set_on = field_num;
                set_off = table->s->fields;
              }
              for (roop_count2 = 0; roop_count2 < set_on; roop_count2++)
                bitmap_set_bit(table->write_set, (uint) roop_count2);
              for (; roop_count2 < set_off; roop_count2++)
                bitmap_clear_bit(table->write_set, (uint) roop_count2);

              {
                THR_LOCK_DATA *to[2];
                table->file->store_lock(table->in_use, to,
                  TL_WRITE_CONCURRENT_INSERT);
                if ((error_num = table->file->ha_external_lock(table->in_use,
                  F_WRLCK)))
                {
                  table->file->print_error(error_num, MYF(0));
                  break;
                }
                if (
                  table->s->tmp_table == NO_TMP_TABLE &&
                  table->pos_in_table_list
                ) {
                  TABLE_LIST *next_tables =
                    table->pos_in_table_list->next_global;
                  while (next_tables && next_tables->parent_l)
                  {
                    DBUG_PRINT("info",("spider call child lock"));
                    TABLE *child_table = next_tables->table;
                    child_table->file->store_lock(child_table->in_use, to,
                      TL_WRITE_CONCURRENT_INSERT);
                    if ((error_num = child_table->file->ha_external_lock(
                      child_table->in_use, F_WRLCK)))
                    {
                      table->file->print_error(error_num, MYF(0));
                      break;
                    }
                    next_tables = next_tables->next_global;
                  }
                }
              }

              if (direct_sql->iop)
              {
                if (direct_sql->iop[roop_count] == 1)
                  table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
                else if (direct_sql->iop[roop_count] == 2)
                  table->file->extra(HA_EXTRA_WRITE_CAN_REPLACE);
              }
              table->file->ha_start_bulk_insert(
                (ha_rows) bulk_insert_rows);

              for (roop_count2 = 0;
                roop_count2 < bulk_insert_rows;
                roop_count2++)
              {
                if ((error_num = spider_db_udf_fetch_table(
                  trx, conn, table, result, set_on, set_off)))
                {
                  if (error_num == HA_ERR_END_OF_FILE)
                  {
                    end_of_file = TRUE;
                    error_num = 0;
                  }
                  break;
                }
                if (direct_sql->iop && direct_sql->iop[roop_count] == 2)
                {
                  if ((error_num = spider_sys_replace(table,
                    &direct_sql->modified_non_trans_table)))
                  {
                    table->file->print_error(error_num, MYF(0));
                    break;
                  }
                } else if ((error_num =
                  table->file->ha_write_row(table->record[0])))
                {
                  /* insert */
                  if (
                    !direct_sql->iop || direct_sql->iop[roop_count] != 1 ||
                    table->file->is_fatal_error(error_num, HA_CHECK_DUP)
                  ) {
                    DBUG_PRINT("info",("spider error_num=%d", error_num));
                    table->file->print_error(error_num, MYF(0));
                    break;
                  } else
                    error_num = 0;
                }
              }

              if (error_num)
                table->file->ha_end_bulk_insert();
              else
                error_num = table->file->ha_end_bulk_insert();
              if (direct_sql->iop)
              {
                if (direct_sql->iop[roop_count] == 1)
                  table->file->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);
                else if (direct_sql->iop[roop_count] == 2)
                  table->file->extra(HA_EXTRA_WRITE_CANNOT_REPLACE);
              }
              {
                table->file->ha_external_unlock(table->in_use);
                if (
                  table->s->tmp_table == NO_TMP_TABLE &&
                  table->pos_in_table_list
                ) {
                  TABLE_LIST *next_tables =
                    table->pos_in_table_list->next_global;
                  while (next_tables && next_tables->parent_l)
                  {
                    DBUG_PRINT("info",("spider call child lock"));
                    TABLE *child_table = next_tables->table;
                    child_table->file->ha_external_lock(child_table->in_use,
                      F_UNLCK);
                    next_tables = next_tables->next_global;
                  }
                }
              }
              table->file->ha_reset();
              table->in_use = thd;
              pthread_mutex_unlock(
                &trx->udf_table_mutexes[udf_table_mutex_index]);
            }
            if (error_num)
              roop_count = -1;
          }
          result->free_result();
          delete result;
        } else {
          if (!error_num)
          {
            error_num = spider_db_errorno(conn);
          }
          if (error_num)
          {
            if (error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM)
              my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
                ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
            else if (error_num == HA_ERR_FOUND_DUPP_KEY)
            {
              my_printf_error(ER_SPIDER_HS_NUM, ER_SPIDER_HS_STR, MYF(0),
                conn->db_conn->get_errno(), conn->db_conn->get_error());
            }
            break;
          }
        }
        if ((status = conn->db_conn->next_result()) > 0)
        {
          error_num = status;
          break;
        }
        if (roop_count >= 0)
          roop_count++;
      } while (status == 0);
    }
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  if (need_trx_end && insert_start)
  {
    if (error_num)
    {
      (void) ha_rollback_trans(c_thd, FALSE);
      if (need_all_commit)
        (void) ha_rollback_trans(c_thd, TRUE);
    } else {
      if ((error_num = ha_commit_trans(c_thd, FALSE)))
        my_error(error_num, MYF(0));
      if (need_all_commit)
      {
        if ((error_num = ha_commit_trans(c_thd, TRUE)))
          my_error(error_num, MYF(0));
      }
    }
  }
  c_thd->lex->sql_command = sql_command_backup;
  DBUG_RETURN(error_num);
}

int spider_db_udf_direct_sql_select_db(
  SPIDER_DIRECT_SQL *direct_sql,
  SPIDER_CONN *conn
) {
  int error_num, need_mon = 0;
  SPIDER_DB_CONN *db_conn = conn->db_conn;
  DBUG_ENTER("spider_db_udf_direct_sql_select_db");
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
  if (
    spider_dbton[conn->dbton_id].db_util->database_has_default_value()
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_file_pos.file_name);
    if (
      !conn->default_database.length() ||
      conn->default_database.length() !=
        direct_sql->tgt_default_db_name_length ||
      memcmp(direct_sql->tgt_default_db_name, conn->default_database.ptr(),
        direct_sql->tgt_default_db_name_length)
    ) {
      if (
        (
          spider_db_before_query(conn, &need_mon) ||
          db_conn->select_db(direct_sql->tgt_default_db_name)
        ) &&
        (error_num = spider_db_errorno(conn))
      ) {
        if (
          error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
          !conn->disable_reconnect
        )
          my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
            ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
        DBUG_RETURN(error_num);
      }
      conn->default_database.length(0);
      if (conn->default_database.reserve(
        direct_sql->tgt_default_db_name_length + 1))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      conn->default_database.q_append(direct_sql->tgt_default_db_name,
        direct_sql->tgt_default_db_name_length + 1);
      conn->default_database.length(direct_sql->tgt_default_db_name_length);
    }
  }
  DBUG_RETURN(0);
}

int spider_db_udf_direct_sql_set_names(
  SPIDER_DIRECT_SQL *direct_sql,
  SPIDER_TRX *trx,
  SPIDER_CONN *conn
) {
  int error_num, need_mon = 0;
  DBUG_ENTER("spider_db_udf_direct_sql_set_names");
  pthread_mutex_assert_owner(&conn->mta_conn_mutex);
    DBUG_ASSERT(conn->mta_conn_mutex_file_pos.file_name);
    if (
      !conn->access_charset ||
      trx->udf_access_charset->cset != conn->access_charset->cset
    ) {
      if (
        (
          spider_db_before_query(conn, &need_mon) ||
          conn->db_conn->set_character_set(trx->udf_access_charset->cs_name.str)
        ) &&
        (error_num = spider_db_errorno(conn))
      ) {
        if (
          error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM &&
          !conn->disable_reconnect
        ) {
          my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
            ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
        }
        DBUG_RETURN(error_num);
      }
      conn->access_charset = trx->udf_access_charset;
    }
  DBUG_RETURN(0);
}

int spider_db_udf_check_and_set_set_names(
  SPIDER_TRX *trx
) {
  int error_num;
  DBUG_ENTER("spider_db_udf_check_and_set_set_names");
  if (
    !trx->udf_access_charset ||
    trx->udf_access_charset->cset !=
      trx->thd->variables.character_set_client->cset)
  {
    trx->udf_access_charset = trx->thd->variables.character_set_client;
    if ((error_num = spider_db_udf_append_set_names(trx)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_db_udf_append_set_names(
  SPIDER_TRX *trx
) {
  DBUG_ENTER("spider_db_udf_append_set_names");
  DBUG_RETURN(0);
}

void spider_db_udf_free_set_names(
  SPIDER_TRX *trx
) {
  DBUG_ENTER("spider_db_udf_free_set_names");
  DBUG_VOID_RETURN;
}

int spider_db_udf_ping_table(
  SPIDER_TABLE_MON_LIST *table_mon_list,
  SPIDER_SHARE *share,
  SPIDER_TRX *trx,
  SPIDER_CONN *conn,
  char *where_clause,
  uint where_clause_length,
  bool ping_only,
  bool use_where,
  longlong limit
) {
  int error_num;
  DBUG_ENTER("spider_db_udf_ping_table");
  if (!pthread_mutex_trylock(&table_mon_list->monitor_mutex))
  {
    int need_mon = 0;
    uint tmp_conn_link_idx = 0;
    ha_spider spider;
    SPIDER_WIDE_HANDLER wide_handler;
    uchar db_request_phase = 0;
    ulonglong db_request_id = 0;
    spider.share = share;
    spider.wide_handler = &wide_handler;
    wide_handler.trx = trx;
    spider.need_mons = &need_mon;
    spider.conn_link_idx = &tmp_conn_link_idx;
    spider.db_request_phase = &db_request_phase;
    spider.db_request_id = &db_request_id;
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = &need_mon;
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if ((error_num = spider_db_ping(&spider, conn, 0)))
    {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      table_mon_list->last_mon_result = error_num;
      pthread_mutex_unlock(&table_mon_list->monitor_mutex);
      if (error_num == ER_CON_COUNT_ERROR)
      {
        my_error(ER_CON_COUNT_ERROR, MYF(0));
        DBUG_RETURN(ER_CON_COUNT_ERROR);
      }
      my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
        share->server_names[0]);
      DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    if (!ping_only)
    {
      int init_sql_alloc_size =
        spider_param_init_sql_alloc_size(trx->thd, share->init_sql_alloc_size);
      char *sql_buf = (char *) my_alloca(init_sql_alloc_size * 2);
      if (!sql_buf)
      {
        table_mon_list->last_mon_result = HA_ERR_OUT_OF_MEM;
        pthread_mutex_unlock(&table_mon_list->monitor_mutex);
        my_error(HA_ERR_OUT_OF_MEM, MYF(0));
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      char *where_buf = sql_buf + init_sql_alloc_size;
      spider_string sql_str(sql_buf, sizeof(sql_buf),
        system_charset_info);
      spider_string where_str(where_buf, sizeof(where_buf),
        system_charset_info);
      sql_str.init_calc_mem(128);
      where_str.init_calc_mem(129);
      sql_str.length(0);
      where_str.length(0);
      if (
        use_where &&
        where_str.append(where_clause, where_clause_length,
          trx->thd->variables.character_set_client)
      ) {
        table_mon_list->last_mon_result = HA_ERR_OUT_OF_MEM;
        pthread_mutex_unlock(&table_mon_list->monitor_mutex);
        my_error(HA_ERR_OUT_OF_MEM, MYF(0));
        my_afree(sql_buf);
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      share->access_charset = system_charset_info;
      if ((error_num = spider_db_udf_ping_table_append_select(&sql_str,
        share, trx, &where_str, use_where, limit, conn->dbton_id)))
      {
        table_mon_list->last_mon_result = error_num;
        pthread_mutex_unlock(&table_mon_list->monitor_mutex);
        my_error(error_num, MYF(0));
        my_afree(sql_buf);
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
      if ((error_num = spider_db_set_names(&spider, conn, 0)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        table_mon_list->last_mon_result = error_num;
        pthread_mutex_unlock(&table_mon_list->monitor_mutex);
        DBUG_PRINT("info",("spider error_num=%d", error_num));
        my_afree(sql_buf);
        DBUG_RETURN(error_num);
      }
      spider_conn_set_timeout_from_share(conn, 0, trx->thd, share);
      if (spider_db_query(
        conn,
        sql_str.ptr(),
        sql_str.length(),
        -1,
        &need_mon)
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        error_num = spider_db_errorno(conn);
        table_mon_list->last_mon_result = error_num;
        pthread_mutex_unlock(&table_mon_list->monitor_mutex);
        DBUG_PRINT("info",("spider error_num=%d", error_num));
        my_afree(sql_buf);
        DBUG_RETURN(error_num);
      }
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      spider_db_discard_result(&spider, 0, conn);
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      my_afree(sql_buf);
    }
    table_mon_list->last_mon_result = 0;
    pthread_mutex_unlock(&table_mon_list->monitor_mutex);
  } else {
    pthread_mutex_lock(&table_mon_list->monitor_mutex);
    error_num = table_mon_list->last_mon_result;
    pthread_mutex_unlock(&table_mon_list->monitor_mutex);
    DBUG_RETURN(error_num);
  }

  DBUG_RETURN(0);
}

int spider_db_udf_ping_table_append_mon_next(
  spider_string *str,
  char *child_table_name,
  uint child_table_name_length,
  int link_id,
  char *static_link_id,
  uint static_link_id_length,
  char *where_clause,
  uint where_clause_length,
  longlong first_sid,
  int full_mon_count,
  int current_mon_count,
  int success_count,
  int fault_count,
  int flags,
  longlong limit
) {
  char limit_str[SPIDER_SQL_INT_LEN], sid_str[SPIDER_SQL_INT_LEN];
  int limit_str_length, sid_str_length;
  spider_string child_table_name_str(child_table_name,
    child_table_name_length + 1, str->charset());
  spider_string where_clause_str(where_clause ? where_clause : "",
    where_clause_length + 1, str->charset());
  DBUG_ENTER("spider_db_udf_ping_table_append_mon_next");
  child_table_name_str.init_calc_mem(130);
  where_clause_str.init_calc_mem(131);
  child_table_name_str.length(child_table_name_length);
  where_clause_str.length(where_clause_length);
  limit_str_length = my_sprintf(limit_str, (limit_str, "%lld", limit));
  sid_str_length = my_sprintf(sid_str, (sid_str, "%lld", first_sid));
  if (str->reserve(
    SPIDER_SQL_SELECT_LEN +
    SPIDER_SQL_PING_TABLE_LEN +
    (child_table_name_length * 2) +
    (
      static_link_id ?
      (SPIDER_SQL_INT_LEN * 5) +
      (SPIDER_SQL_VALUE_QUOTE_LEN * 2) +
      (static_link_id_length * 2) :
      (SPIDER_SQL_INT_LEN * 6)
    ) +
    sid_str_length +
    limit_str_length +
    (where_clause_length * 2) +
    (SPIDER_SQL_VALUE_QUOTE_LEN * 4) +
    (SPIDER_SQL_COMMA_LEN * 9) +
    SPIDER_SQL_CLOSE_PAREN_LEN
  ))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SELECT_STR, SPIDER_SQL_SELECT_LEN);
  str->q_append(SPIDER_SQL_PING_TABLE_STR, SPIDER_SQL_PING_TABLE_LEN);
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str->append_escape_string(child_table_name_str.ptr(), child_table_name_str.length());
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  if (static_link_id)
  {
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->append_for_single_quote(static_link_id, static_link_id_length);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  } else {
    str->qs_append(link_id);
  }
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->qs_append(flags);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->q_append(limit_str, limit_str_length);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str->append_escape_string(where_clause_str.ptr(), where_clause_str.length());
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->q_append(sid_str, sid_str_length);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->qs_append(full_mon_count);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->qs_append(current_mon_count);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->qs_append(success_count);
  str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
  str->qs_append(fault_count);
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_db_udf_ping_table_append_select(
  spider_string *str,
  SPIDER_SHARE *share,
  SPIDER_TRX *trx,
  spider_string *where_str,
  bool use_where,
  longlong limit,
  uint dbton_id
) {
  int error_num;
  char limit_str[SPIDER_SQL_INT_LEN];
  int limit_str_length;
  DBUG_ENTER("spider_db_udf_ping_table_append_select");
  if (str->reserve(SPIDER_SQL_SELECT_LEN + SPIDER_SQL_ONE_LEN +
    SPIDER_SQL_FROM_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_SELECT_STR, SPIDER_SQL_SELECT_LEN);
  str->q_append(SPIDER_SQL_ONE_STR, SPIDER_SQL_ONE_LEN);
  str->q_append(SPIDER_SQL_FROM_STR, SPIDER_SQL_FROM_LEN);
  if (share->tgt_dbs[0])
  {
    if ((error_num = spider_db_append_name_with_quote_str(str,
      share->tgt_dbs[0], dbton_id)))
      DBUG_RETURN(error_num);
    if (str->reserve(SPIDER_SQL_DOT_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  }
  if ((error_num = spider_db_append_name_with_quote_str(str,
    share->tgt_table_names[0], share->sql_dbton_ids[0])))
    DBUG_RETURN(error_num);

  if (spider_dbton[dbton_id].db_util->limit_mode() == 1)
  {
    if (use_where)
    {
      if (str->reserve(where_str->length() * 2))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->append_escape_string(where_str->ptr(), where_str->length());
    }
  } else {
    limit_str_length = my_sprintf(limit_str, (limit_str, "%lld", limit));
    if (str->reserve(
      (use_where ? (where_str->length() * 2) : 0) +
      SPIDER_SQL_LIMIT_LEN + limit_str_length
    ))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    if (use_where)
    {
      str->append_escape_string(where_str->ptr(), where_str->length());
    }
    str->q_append(SPIDER_SQL_LIMIT_STR, SPIDER_SQL_LIMIT_LEN);
    str->q_append(limit_str, limit_str_length);
  }
  DBUG_RETURN(0);
}

int spider_db_udf_ping_table_mon_next(
  THD *thd,
  SPIDER_TABLE_MON *table_mon,
  SPIDER_CONN *conn,
  SPIDER_MON_TABLE_RESULT *mon_table_result,
  char *child_table_name,
  uint child_table_name_length,
  int link_id,
  char *where_clause,
  uint where_clause_length,
  longlong first_sid,
  int full_mon_count,
  int current_mon_count,
  int success_count,
  int fault_count,
  int flags,
  longlong limit
) {
  int error_num, need_mon = 0;
  uint tmp_conn_link_idx = 0;
  SPIDER_DB_RESULT *res;
  SPIDER_SHARE *share = table_mon->share;
  int init_sql_alloc_size =
    spider_param_init_sql_alloc_size(thd, share->init_sql_alloc_size);
  ha_spider spider;
  SPIDER_WIDE_HANDLER wide_handler;
  SPIDER_TRX trx;
  DBUG_ENTER("spider_db_udf_ping_table_mon_next");
  char *sql_buf = (char *) my_alloca(init_sql_alloc_size);
  if (!sql_buf)
  {
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  spider_string sql_str(sql_buf, sizeof(sql_buf),
    thd->variables.character_set_client);
  sql_str.init_calc_mem(132);
  sql_str.length(0);
  trx.thd = thd;
  spider.share = share;
  spider.wide_handler = &wide_handler;
  wide_handler.trx = &trx;
  spider.need_mons = &need_mon;
  spider.conn_link_idx = &tmp_conn_link_idx;

  share->access_charset = thd->variables.character_set_client;
  if ((error_num = spider_db_udf_ping_table_append_mon_next(&sql_str,
    child_table_name, child_table_name_length, link_id,
    table_mon->parent->share->static_link_ids[0],
    table_mon->parent->share->static_link_ids_lengths[0],
    where_clause,
    where_clause_length, first_sid, full_mon_count, current_mon_count,
    success_count, fault_count, flags, limit)))
  {
    my_error(error_num, MYF(0));
    my_afree(sql_buf);
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
  if ((error_num = spider_db_ping(&spider, conn, 0)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
      share->server_names[0]);
    my_afree(sql_buf);
    DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);
  }
  if ((error_num = spider_db_set_names(&spider, conn, 0)))
  {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);
    my_afree(sql_buf);
    DBUG_RETURN(error_num);
  }
  spider_conn_set_timeout_from_share(conn, 0, thd, share);
  if (spider_db_query(
    conn,
    sql_str.ptr(),
    sql_str.length(),
    -1,
    &need_mon)
  ) {
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    my_afree(sql_buf);
    DBUG_RETURN(spider_db_errorno(conn));
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = trx.spider_thread_id;
  request_key.query_id = trx.thd->query_id;
  request_key.handler = table_mon;
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
      my_afree(sql_buf);
      DBUG_RETURN(error_num);
    }
    else if ((error_num = spider_db_errorno(conn)))
    {
      my_afree(sql_buf);
      DBUG_RETURN(error_num);
    }
    my_error(HA_ERR_OUT_OF_MEM, MYF(0));
    my_afree(sql_buf);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  my_afree(sql_buf);
  error_num = res->fetch_table_mon_status(mon_table_result->result_status);
  res->free_result();
  delete res;
  DBUG_RETURN(error_num);
}

int spider_db_udf_copy_key_row(
  spider_string *str,
  spider_string *source_str,
  Field *field,
  ulong *row_pos,
  ulong *length,
  const char *joint_str,
  const int joint_length,
  uint dbton_id
) {
  int error_num;
  DBUG_ENTER("spider_db_udf_copy_key_row");
#ifdef SPIDER_use_LEX_CSTRING_for_KEY_Field_name
  if ((error_num = spider_db_append_name_with_quote_str(str,
    (char *) field->field_name.str, dbton_id)))
#else
  if ((error_num = spider_db_append_name_with_quote_str(str,
    (char *) field->field_name, dbton_id)))
#endif
    DBUG_RETURN(error_num);
  if (str->reserve(joint_length + *length + SPIDER_SQL_AND_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(joint_str, joint_length);
  str->q_append(source_str->ptr() + *row_pos, *length);
  str->q_append(SPIDER_SQL_AND_STR, SPIDER_SQL_AND_LEN);
  DBUG_RETURN(0);
}

int spider_db_udf_copy_tables(
  SPIDER_COPY_TABLES *copy_tables,
  ha_spider *spider,
  TABLE *table,
  longlong bulk_insert_rows
) {
  int error_num = 0, roop_count;
  bool end_of_file = FALSE;
  ulong *last_lengths, *last_row_pos = NULL;
  ha_spider *tmp_spider;
  SPIDER_CONN *tmp_conn;
  int all_link_cnt =
    copy_tables->link_idx_count[0] + copy_tables->link_idx_count[1];
  SPIDER_COPY_TABLE_CONN *src_tbl_conn = copy_tables->table_conn[0];
  SPIDER_COPY_TABLE_CONN *dst_tbl_conn;
  spider_db_copy_table *select_ct = src_tbl_conn->copy_table;
  spider_db_copy_table *insert_ct = NULL;
  KEY *key_info = &table->key_info[table->s->primary_key];
  int bulk_insert_interval;
  DBUG_ENTER("spider_db_udf_copy_tables");
  if (!(last_row_pos = (ulong *)
    spider_bulk_malloc(spider_current_trx, 30, MYF(MY_WME),
      &last_row_pos, (uint) (sizeof(ulong) * table->s->fields),
      &last_lengths, (uint) (sizeof(ulong) * table->s->fields),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  while (!end_of_file)
  {
    if (copy_tables->trx->thd->killed)
    {
      my_error(ER_QUERY_INTERRUPTED, MYF(0));
      error_num = ER_QUERY_INTERRUPTED;
      goto error_killed;
    }
    if (copy_tables->use_transaction)
    {
      for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
      {
        tmp_spider = &spider[roop_count];
        tmp_conn = tmp_spider->conns[0];
        /* disable transaction */
        spider_conn_clear_queue_at_commit(tmp_conn);
        if (!tmp_conn->trx_start)
        {
          pthread_mutex_assert_not_owner(&tmp_conn->mta_conn_mutex);
          pthread_mutex_lock(&tmp_conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
          tmp_conn->need_mon = &tmp_spider->need_mons[0];
          DBUG_ASSERT(!tmp_conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(!tmp_conn->mta_conn_mutex_unlock_later);
          tmp_conn->mta_conn_mutex_lock_already = TRUE;
          tmp_conn->mta_conn_mutex_unlock_later = TRUE;
          if (spider_db_ping(tmp_spider, tmp_conn, 0))
          {
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
            tmp_conn->mta_conn_mutex_lock_already = FALSE;
            tmp_conn->mta_conn_mutex_unlock_later = FALSE;
            SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
            my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
              tmp_spider->share->server_names[0]);
            error_num = ER_CONNECT_TO_FOREIGN_DATA_SOURCE;
            goto error_db_ping;
          }
          if (
            (error_num = spider_db_set_names(tmp_spider, tmp_conn, 0)) ||
            (error_num = spider_db_start_transaction(tmp_conn,
              tmp_spider->need_mons))
          ) {
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
            tmp_conn->mta_conn_mutex_lock_already = FALSE;
            tmp_conn->mta_conn_mutex_unlock_later = FALSE;
            SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
            goto error_start_transaction;
          }
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
          tmp_conn->mta_conn_mutex_lock_already = FALSE;
          tmp_conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
        }
      }
    } else {
      for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
      {
        tmp_spider = &spider[roop_count];
        tmp_conn = tmp_spider->conns[0];
        /* disable transaction */
        spider_conn_clear_queue_at_commit(tmp_conn);
        spider_db_handler *tmp_dbton_hdl =
          tmp_spider->dbton_handler[tmp_conn->dbton_id];
        if ((error_num = tmp_dbton_hdl->insert_lock_tables_list(tmp_conn, 0)))
          goto error_lock_table_hash;
        tmp_conn->table_lock = 2;
      }
      for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
      {
        tmp_spider = &spider[roop_count];
        tmp_conn = tmp_spider->conns[0];
        pthread_mutex_assert_not_owner(&tmp_conn->mta_conn_mutex);
        pthread_mutex_lock(&tmp_conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
        tmp_conn->need_mon = &tmp_spider->need_mons[0];
        DBUG_ASSERT(!tmp_conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(!tmp_conn->mta_conn_mutex_unlock_later);
        tmp_conn->mta_conn_mutex_lock_already = TRUE;
        tmp_conn->mta_conn_mutex_unlock_later = TRUE;
        if (spider_db_ping(tmp_spider, tmp_conn, 0))
        {
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
          tmp_conn->mta_conn_mutex_lock_already = FALSE;
          tmp_conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
          my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
            tmp_spider->share->server_names[0]);
          error_num = ER_CONNECT_TO_FOREIGN_DATA_SOURCE;
          goto error_db_ping;
        }
        if (
          tmp_conn->db_conn->have_lock_table_list() &&
          (
            (error_num = spider_db_set_names(tmp_spider, tmp_conn, 0)) ||
            (error_num = spider_db_lock_tables(tmp_spider, 0))
          )
        ) {
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
          tmp_conn->mta_conn_mutex_lock_already = FALSE;
          tmp_conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
          tmp_conn->table_lock = 0;
          if (error_num == HA_ERR_OUT_OF_MEM)
            my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
          goto error_lock_tables;
        }
        DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
        tmp_conn->mta_conn_mutex_lock_already = FALSE;
        tmp_conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
        tmp_conn->table_lock = 1;
      }
    }

    tmp_conn = src_tbl_conn->conn;
    spider_conn_set_timeout_from_share(tmp_conn, 0,
      copy_tables->trx->thd, src_tbl_conn->share);
    pthread_mutex_assert_not_owner(&tmp_conn->mta_conn_mutex);
    pthread_mutex_lock(&tmp_conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
    tmp_conn->need_mon = &src_tbl_conn->need_mon;
    DBUG_ASSERT(!tmp_conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!tmp_conn->mta_conn_mutex_unlock_later);
    tmp_conn->mta_conn_mutex_lock_already = TRUE;
    tmp_conn->mta_conn_mutex_unlock_later = TRUE;
    if (select_ct->exec_query(
      tmp_conn,
      -1,
      &src_tbl_conn->need_mon)
    ) {
      DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
      tmp_conn->mta_conn_mutex_lock_already = FALSE;
      tmp_conn->mta_conn_mutex_unlock_later = FALSE;
      error_num = spider_db_errorno(tmp_conn);
      if (error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM)
        my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
          ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
      goto error_db_query;
    } else {
      SPIDER_DB_RESULT *result;
      st_spider_db_request_key request_key;
      request_key.spider_thread_id = copy_tables->trx->spider_thread_id;
      request_key.query_id = copy_tables->trx->thd->query_id;
      request_key.handler = copy_tables;
      request_key.request_id = 1;
      request_key.next = NULL;
      if ((result = tmp_conn->db_conn->use_result(NULL, &request_key, &error_num)))
      {
        SPIDER_DB_ROW *row;
        roop_count = 0;
        while ((row = result->fetch_row()))
        {
          dst_tbl_conn = copy_tables->table_conn[1];
          insert_ct = dst_tbl_conn->copy_table;
          if ((error_num = insert_ct->copy_rows(table, row,
            &last_row_pos, &last_lengths)))
          {
            if (error_num == HA_ERR_OUT_OF_MEM)
              my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
            result->free_result();
            delete result;
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
            tmp_conn->mta_conn_mutex_lock_already = FALSE;
            tmp_conn->mta_conn_mutex_unlock_later = FALSE;
            SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
            goto error_db_query;
          }
          for (dst_tbl_conn = dst_tbl_conn->next; dst_tbl_conn;
            dst_tbl_conn = dst_tbl_conn->next)
          {
            row->first();
            insert_ct = dst_tbl_conn->copy_table;
            if ((error_num = insert_ct->copy_rows(table, row)))
            {
              if (error_num == HA_ERR_OUT_OF_MEM)
                my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
              result->free_result();
              delete result;
              DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
              tmp_conn->mta_conn_mutex_lock_already = FALSE;
              tmp_conn->mta_conn_mutex_unlock_later = FALSE;
              SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
              goto error_db_query;
            }
          }
          ++roop_count;
        }
        error_num = result->get_errno();
        if (error_num == HA_ERR_END_OF_FILE)
        {
          if (roop_count < copy_tables->bulk_insert_rows)
          {
            end_of_file = TRUE;
            if (roop_count)
              error_num = 0;
          } else {
            /* add next where clause */
            select_ct->set_sql_to_pos();
            error_num = select_ct->append_copy_where(insert_ct, key_info,
              last_row_pos, last_lengths);
            if (error_num)
            {
              if (error_num == HA_ERR_OUT_OF_MEM)
                my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
              result->free_result();
              delete result;
              DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
              tmp_conn->mta_conn_mutex_lock_already = FALSE;
              tmp_conn->mta_conn_mutex_unlock_later = FALSE;
              SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
              goto error_db_query;
            }
            bulk_insert_rows = spider_param_udf_ct_bulk_insert_rows(
              copy_tables->bulk_insert_rows);
            if (
              select_ct->append_key_order_str(key_info, 0, FALSE) ||
              select_ct->append_limit(0, bulk_insert_rows) ||
              (
                copy_tables->use_transaction &&
                select_ct->append_select_lock_str(SPIDER_LOCK_MODE_SHARED)
              )
            ) {
              my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
              result->free_result();
              delete result;
              DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
              DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
              tmp_conn->mta_conn_mutex_lock_already = FALSE;
              tmp_conn->mta_conn_mutex_unlock_later = FALSE;
              SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
              pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
              error_num = ER_OUT_OF_RESOURCES;
              goto error_db_query;
            }
            error_num = 0;
          }
        } else {
          if (error_num == HA_ERR_OUT_OF_MEM)
            my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
          result->free_result();
          delete result;
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
          tmp_conn->mta_conn_mutex_lock_already = FALSE;
          tmp_conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
          goto error_db_query;
        }
        result->free_result();
        delete result;
        DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
        tmp_conn->mta_conn_mutex_lock_already = FALSE;
        tmp_conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
        for (dst_tbl_conn = copy_tables->table_conn[1]; dst_tbl_conn;
          dst_tbl_conn = dst_tbl_conn->next)
        {
          insert_ct = dst_tbl_conn->copy_table;
          if ((error_num = insert_ct->append_insert_terminator()))
          {
            if (error_num == HA_ERR_OUT_OF_MEM)
              my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
            goto error_db_query;
          }
        }
      } else {
        if (!error_num)
        {
          error_num = spider_db_errorno(tmp_conn);
        }
        if (error_num)
        {
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
          tmp_conn->mta_conn_mutex_lock_already = FALSE;
          tmp_conn->mta_conn_mutex_unlock_later = FALSE;
          SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
          if (error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM)
            my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
              ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
          goto error_db_query;
        }
        error_num = HA_ERR_END_OF_FILE;
        end_of_file = TRUE;
        DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
        tmp_conn->mta_conn_mutex_lock_already = FALSE;
        tmp_conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
      }
    }

    if (!error_num && roop_count)
    {
/*
      dst_tbl_conn = copy_tables->table_conn[1];
      spider_db_copy_table *source_ct = dst_tbl_conn->copy_table;
      for (dst_tbl_conn = dst_tbl_conn->next; dst_tbl_conn;
        dst_tbl_conn = dst_tbl_conn->next)
      {
        insert_ct = dst_tbl_conn->copy_table;
        if (insert_ct->copy_insert_values(source_ct))
        {
          my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
          error_num = ER_OUT_OF_RESOURCES;
          goto error_db_query;
        }
      }
*/
      if (copy_tables->bg_mode)
      {
        for (dst_tbl_conn = copy_tables->table_conn[1]; dst_tbl_conn;
          dst_tbl_conn = dst_tbl_conn->next)
        {
          if (spider_udf_bg_copy_exec_sql(dst_tbl_conn))
          {
            my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
            error_num = ER_OUT_OF_RESOURCES;
            goto error_db_query;
          }
        }
      } else {
        for (dst_tbl_conn = copy_tables->table_conn[1]; dst_tbl_conn;
          dst_tbl_conn = dst_tbl_conn->next)
        {
          tmp_conn = dst_tbl_conn->conn;
          insert_ct = dst_tbl_conn->copy_table;
          pthread_mutex_assert_not_owner(&tmp_conn->mta_conn_mutex);
          pthread_mutex_lock(&tmp_conn->mta_conn_mutex);
          SPIDER_SET_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
          tmp_conn->need_mon = &dst_tbl_conn->need_mon;
          DBUG_ASSERT(!tmp_conn->mta_conn_mutex_lock_already);
          DBUG_ASSERT(!tmp_conn->mta_conn_mutex_unlock_later);
          tmp_conn->mta_conn_mutex_lock_already = TRUE;
          tmp_conn->mta_conn_mutex_unlock_later = TRUE;
          spider_conn_set_timeout_from_share(tmp_conn, 0,
            copy_tables->trx->thd, dst_tbl_conn->share);
          if (insert_ct->exec_query(
            tmp_conn,
            -1,
            &dst_tbl_conn->need_mon)
          ) {
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
            tmp_conn->mta_conn_mutex_lock_already = FALSE;
            tmp_conn->mta_conn_mutex_unlock_later = FALSE;
            error_num = spider_db_errorno(tmp_conn);
            if (error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM)
              my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
                ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
            goto error_db_query;
          } else {
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_lock_already);
            DBUG_ASSERT(tmp_conn->mta_conn_mutex_unlock_later);
            tmp_conn->mta_conn_mutex_lock_already = FALSE;
            tmp_conn->mta_conn_mutex_unlock_later = FALSE;
            SPIDER_CLEAR_FILE_POS(&tmp_conn->mta_conn_mutex_file_pos);
            pthread_mutex_unlock(&tmp_conn->mta_conn_mutex);
          }
        }
      }

      if (copy_tables->bg_mode)
      {
        for (dst_tbl_conn = copy_tables->table_conn[1]; dst_tbl_conn;
          dst_tbl_conn = dst_tbl_conn->next)
        {
          tmp_conn = dst_tbl_conn->conn;
          if (tmp_conn->bg_exec_sql)
          {
            /* wait */
            pthread_mutex_lock(&tmp_conn->bg_conn_mutex);
            pthread_mutex_unlock(&tmp_conn->bg_conn_mutex);
          }

          if (dst_tbl_conn->bg_error_num)
          {
            if (error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM)
              my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
                ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
            goto error_db_query;
          }
        }
      }
    }

    if (copy_tables->use_transaction)
    {
      for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
      {
        tmp_spider = &spider[roop_count];
        tmp_conn = tmp_spider->conns[0];
        if (tmp_conn->trx_start)
        {
          if ((error_num = spider_db_commit(tmp_conn)))
            goto error_commit;
        }
      }
    } else {
      for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
      {
        tmp_spider = &spider[roop_count];
        tmp_conn = tmp_spider->conns[0];
        if (tmp_conn->table_lock == 1)
        {
          tmp_conn->table_lock = 0;
          if ((error_num = spider_db_unlock_tables(tmp_spider, 0)))
            goto error_unlock_tables;
        }
      }
    }
    if (!end_of_file)
    {
      for (dst_tbl_conn = copy_tables->table_conn[1]; dst_tbl_conn;
        dst_tbl_conn = dst_tbl_conn->next)
      {
        insert_ct = dst_tbl_conn->copy_table;
        insert_ct->set_sql_to_pos();
      }
      DBUG_PRINT("info",("spider sleep"));
      bulk_insert_interval = spider_param_udf_ct_bulk_insert_interval(
        copy_tables->bulk_insert_interval);
      my_sleep(bulk_insert_interval);
    }
  }
  spider_free(spider_current_trx, last_row_pos, MYF(0));
  DBUG_RETURN(0);

error_db_query:
  if (copy_tables->bg_mode)
  {
    for (dst_tbl_conn = copy_tables->table_conn[1]; dst_tbl_conn;
      dst_tbl_conn = dst_tbl_conn->next)
    {
      tmp_conn = dst_tbl_conn->conn;
      if (tmp_conn->bg_exec_sql)
      {
        /* wait */
        pthread_mutex_lock(&tmp_conn->bg_conn_mutex);
        pthread_mutex_unlock(&tmp_conn->bg_conn_mutex);
      }
    }
  }
error_unlock_tables:
error_commit:
error_lock_tables:
error_lock_table_hash:
error_start_transaction:
error_db_ping:
error_killed:
  if (copy_tables->use_transaction)
  {
    for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
    {
      tmp_spider = &spider[roop_count];
      tmp_conn = tmp_spider->conns[0];
      if (tmp_conn->trx_start)
        spider_db_rollback(tmp_conn);
    }
  } else {
    if (copy_tables->trx->locked_connections)
    {
      for (roop_count = 0; roop_count < all_link_cnt; roop_count++)
      {
        tmp_spider = &spider[roop_count];
        tmp_conn = tmp_spider->conns[0];
        if (tmp_conn->table_lock == 1)
        {
          tmp_conn->table_lock = 0;
          spider_db_unlock_tables(tmp_spider, 0);
        }
      }
    }
  }
error:
  if (last_row_pos)
  {
    spider_free(spider_current_trx, last_row_pos, MYF(0));
  }
  DBUG_RETURN(error_num);
}

int spider_db_open_handler(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_SHARE *share = spider->share;
  uint *handler_id_ptr =
      &spider->m_handler_id[link_idx]
    ;
  spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
  DBUG_ENTER("spider_db_open_handler");
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(conn->mta_conn_mutex_file_pos.file_name);
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (!spider->handler_opened(link_idx, conn->conn_kind))
    *handler_id_ptr = conn->opened_handlers;

  if (!spider->handler_opened(link_idx, conn->conn_kind))
    my_sprintf(spider->m_handler_cid[link_idx],
      (spider->m_handler_cid[link_idx], SPIDER_SQL_HANDLER_CID_FORMAT,
      *handler_id_ptr));

  if ((error_num = dbton_hdl->append_open_handler_part(
    SPIDER_SQL_TYPE_HANDLER, *handler_id_ptr, conn, link_idx)))
  {
    goto error;
  }

  spider_conn_set_timeout_from_share(conn, link_idx,
    spider->wide_handler->trx->thd,
    share);
  if (dbton_hdl->execute_sql(
    SPIDER_SQL_TYPE_HANDLER,
    conn,
    -1,
    &spider->need_mons[link_idx])
  ) {
    error_num = spider_db_errorno(conn);
    goto error;
  }
  dbton_hdl->reset_sql(SPIDER_SQL_TYPE_HANDLER);

  if (!spider->handler_opened(link_idx, conn->conn_kind))
  {
    if ((error_num = dbton_hdl->insert_opened_handler(conn, link_idx)))
      goto error;
    conn->opened_handlers++;
  }
  DBUG_PRINT("info",("spider conn=%p", conn));
  DBUG_PRINT("info",("spider opened_handlers=%u", conn->opened_handlers));
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);

error:
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(error_num);
}


int spider_db_close_handler(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx,
  uint tgt_conn_kind
) {
  int error_num;
  spider_db_handler *dbton_hdl = spider->dbton_handler[conn->dbton_id];
  DBUG_ENTER("spider_db_close_handler");
  DBUG_PRINT("info",("spider conn=%p", conn));
  pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
  pthread_mutex_lock(&conn->mta_conn_mutex);
  SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
  conn->need_mon = &spider->need_mons[link_idx];
  DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = TRUE;
  conn->mta_conn_mutex_unlock_later = TRUE;
  if (spider->handler_opened(link_idx, tgt_conn_kind))
  {
      dbton_hdl->reset_sql(SPIDER_SQL_TYPE_HANDLER);
      if ((error_num = dbton_hdl->append_close_handler_part(
        SPIDER_SQL_TYPE_HANDLER, link_idx)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        DBUG_RETURN(error_num);
      }

      spider_conn_set_timeout_from_share(conn, link_idx,
        spider->wide_handler->trx->thd,
        spider->share);
      if (dbton_hdl->execute_sql(
        SPIDER_SQL_TYPE_HANDLER,
        conn,
        -1,
        &spider->need_mons[link_idx])
      ) {
        error_num = spider_db_errorno(conn);
        goto error;
      }
      dbton_hdl->reset_sql(SPIDER_SQL_TYPE_HANDLER);
    if ((error_num = dbton_hdl->delete_opened_handler(conn, link_idx)))
      goto error;
    conn->opened_handlers--;
    DBUG_PRINT("info",("spider opened_handlers=%u", conn->opened_handlers));
  }
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(0);

error:
  DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
  DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
  conn->mta_conn_mutex_lock_already = FALSE;
  conn->mta_conn_mutex_unlock_later = FALSE;
  SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
  pthread_mutex_unlock(&conn->mta_conn_mutex);
  DBUG_RETURN(error_num);
}

bool spider_db_conn_is_network_error(
  int error_num
) {
  DBUG_ENTER("spider_db_conn_is_network_error");
  if (
    error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM ||
    error_num == ER_CONNECT_TO_FOREIGN_DATA_SOURCE ||
    (
      error_num >= CR_MIN_ERROR &&
      error_num <= CR_MAX_ERROR
    )
  ) {
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}
