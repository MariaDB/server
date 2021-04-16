/* Copyright (C) 2008-2020 Kentoku Shiba
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

#define SPIDER_LOCK_MODE_NO_LOCK             0
#define SPIDER_LOCK_MODE_SHARED              1
#define SPIDER_LOCK_MODE_EXCLUSIVE           2

#define SPIDER_SIMPLE_NO_ACTION           0
#define SPIDER_SIMPLE_CONNECT             1
#define SPIDER_SIMPLE_DISCONNECT          2
#define SPIDER_SIMPLE_RECORDS             3
#ifdef HA_HAS_CHECKSUM_EXTENDED
#define SPIDER_SIMPLE_CHECKSUM_TABLE      4
#endif

#define SPIDER_LOP_CHK_QUEUED             (1 << 0)
#define SPIDER_LOP_CHK_MERAGED            (1 << 1)
#define SPIDER_LOP_CHK_IGNORED            (1 << 2)

typedef struct st_spider_conn_loop_check
{
  uint               flag;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value_to;
  my_hash_value_type hash_value_full;
#endif
  LEX_CSTRING        from_name;
  LEX_CSTRING        cur_name;
  LEX_CSTRING        to_name;
  LEX_CSTRING        full_name;
  LEX_CSTRING        from_value;
  LEX_CSTRING        merged_value;
  st_spider_conn_loop_check *next;
} SPIDER_CONN_LOOP_CHECK;

uchar *spider_conn_get_key(
  SPIDER_CONN *conn,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

uchar *spider_ipport_conn_get_key(
  SPIDER_IP_PORT_CONN *ip_port,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

int spider_conn_init(
  SPIDER_CONN *conn
);

void spider_conn_done(
  SPIDER_CONN *conn
);

int spider_reset_conn_setted_parameter(
  SPIDER_CONN *conn,
  THD *thd
);

int spider_free_conn_alloc(
  SPIDER_CONN *conn
);

void spider_free_conn_from_trx(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn,
  bool another,
  bool trx_free,
  int *roop_count
);

SPIDER_CONN *spider_create_conn(
  SPIDER_SHARE *share,
  ha_spider *spider,
  int link_id,
  int base_link_id,
  uint conn_kind,
  int *error_num
);

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
);

int spider_free_conn(
  SPIDER_CONN *conn
);

int spider_check_and_get_casual_read_conn(
  THD *thd,
  ha_spider *spider,
  int link_idx
);

int spider_check_and_init_casual_read(
  THD *thd,
  ha_spider *spider,
  int link_idx
);

void spider_conn_queue_connect(
  SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int link_idx
);

void spider_conn_queue_connect_rewrite(
  SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int link_idx
);

void spider_conn_queue_ping(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
);

void spider_conn_queue_ping_rewrite(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
);

void spider_conn_queue_trx_isolation(
  SPIDER_CONN *conn,
  int trx_isolation
);

void spider_conn_queue_semi_trx_isolation(
  SPIDER_CONN *conn,
  int trx_isolation
);

void spider_conn_queue_autocommit(
  SPIDER_CONN *conn,
  bool autocommit
);

void spider_conn_queue_sql_log_off(
  SPIDER_CONN *conn,
  bool sql_log_off
);

void spider_conn_queue_wait_timeout(
  SPIDER_CONN *conn,
  int wait_timeout
);

void spider_conn_queue_sql_mode(
  SPIDER_CONN *conn,
  sql_mode_t sql_mode
);

void spider_conn_queue_time_zone(
  SPIDER_CONN *conn,
  Time_zone *time_zone
);

void spider_conn_queue_UTC_time_zone(
  SPIDER_CONN *conn
);

int spider_conn_queue_and_merge_loop_check(
  SPIDER_CONN *conn,
  SPIDER_CONN_LOOP_CHECK *lcptr
);

int spider_conn_reset_queue_loop_check(
  SPIDER_CONN *conn
);

int spider_conn_queue_loop_check(
  SPIDER_CONN *conn,
  ha_spider *spider,
  int link_idx
);

void spider_conn_queue_start_transaction(
  SPIDER_CONN *conn
);

void spider_conn_queue_xa_start(
  SPIDER_CONN *conn,
  XID *xid
);

void spider_conn_clear_queue(
  SPIDER_CONN *conn
);

void spider_conn_clear_queue_at_commit(
  SPIDER_CONN *conn
);

void spider_conn_set_timeout(
  SPIDER_CONN *conn,
  uint net_read_timeout,
  uint net_write_timeout
);

void spider_conn_set_timeout_from_share(
  SPIDER_CONN *conn,
  int link_idx,
  THD *thd,
  SPIDER_SHARE *share
);

void spider_conn_set_timeout_from_direct_sql(
  SPIDER_CONN *conn,
  THD *thd,
  SPIDER_DIRECT_SQL *direct_sql
);

void spider_tree_insert(
  SPIDER_CONN *top,
  SPIDER_CONN *conn
);

SPIDER_CONN *spider_tree_first(
  SPIDER_CONN *top
);

SPIDER_CONN *spider_tree_last(
  SPIDER_CONN *top
);

SPIDER_CONN *spider_tree_next(
  SPIDER_CONN *current
);

SPIDER_CONN *spider_tree_delete(
  SPIDER_CONN *conn,
  SPIDER_CONN *top
);

#ifndef WITHOUT_SPIDER_BG_SEARCH
int spider_set_conn_bg_param(
  ha_spider *spider
);

int spider_create_conn_thread(
  SPIDER_CONN *conn
);

void spider_free_conn_thread(
  SPIDER_CONN *conn
);

void spider_bg_conn_wait(
  SPIDER_CONN *conn
);

void spider_bg_all_conn_wait(
  ha_spider *spider
);

int spider_bg_all_conn_pre_next(
  ha_spider *spider,
  int link_idx
);

void spider_bg_conn_break(
  SPIDER_CONN *conn,
  ha_spider *spider
);

void spider_bg_all_conn_break(
  ha_spider *spider
);

bool spider_bg_conn_get_job(
  SPIDER_CONN *conn
);

int spider_bg_conn_search(
  ha_spider *spider,
  int link_idx,
  int first_link_idx,
  bool first,
  bool pre_next,
  bool discard_result
);

void spider_bg_conn_simple_action(
  SPIDER_CONN *conn,
  uint simple_action,
  bool caller_wait,
  void *target,
  uint link_idx,
  int *error_num
);

void *spider_bg_conn_action(
  void *arg
);

int spider_create_sts_thread(
  SPIDER_SHARE *share
);

void spider_free_sts_thread(
  SPIDER_SHARE *share
);

void *spider_bg_sts_action(
  void *arg
);

int spider_create_crd_thread(
  SPIDER_SHARE *share
);

void spider_free_crd_thread(
  SPIDER_SHARE *share
);

void *spider_bg_crd_action(
  void *arg
);

int spider_create_mon_threads(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share
);

void spider_free_mon_threads(
  SPIDER_SHARE *share
);

void *spider_bg_mon_action(
  void *arg
);
#endif

int spider_conn_first_link_idx(
  THD *thd,
  long *link_statuses,
  long *access_balances,
  uint *conn_link_idx,
  int link_count,
  int link_status
);

int spider_conn_next_link_idx(
  THD *thd,
  long *link_statuses,
  long *access_balances,
  uint *conn_link_idx,
  int link_idx,
  int link_count,
  int link_status
);

int spider_conn_link_idx_next(
  long *link_statuses,
  uint *conn_link_idx,
  int link_idx,
  int link_count,
  int link_status
);

int spider_conn_get_link_status(
  long *link_statuses,
  uint *conn_link_idx,
  int link_idx
);

int spider_conn_lock_mode(
  ha_spider *spider
);

bool spider_conn_check_recovery_link(
  SPIDER_SHARE *share
);

bool spider_conn_use_handler(
  ha_spider *spider,
  int lock_mode,
  int link_idx
);

bool spider_conn_need_open_handler(
  ha_spider *spider,
  uint idx,
  int link_idx
);

SPIDER_IP_PORT_CONN *spider_create_ipport_conn(SPIDER_CONN *conn);
SPIDER_CONN* spider_get_conn_from_idle_connection
(
 SPIDER_SHARE *share,
 int link_idx,
 char *conn_key,
 ha_spider *spider,
 uint conn_kind,
 int base_link_idx,
 int *error_num
 );
void spider_free_ipport_conn(void *info);
