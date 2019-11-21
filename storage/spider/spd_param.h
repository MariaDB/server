/* Copyright (C) 2008-2015 Kentoku Shiba

  This program is free software); you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation); version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY); without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program); if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

my_bool spider_param_support_xa();
my_bool spider_param_connect_mutex();
uint spider_param_connect_error_interval();
uint spider_param_table_init_error_interval();
int spider_param_use_table_charset(
  int use_table_charset
);
uint spider_param_conn_recycle_mode(
  THD *thd
);
uint spider_param_conn_recycle_strict(
  THD *thd
);
bool spider_param_sync_trx_isolation(
  THD *thd
);
bool spider_param_use_consistent_snapshot(
  THD *thd
);
bool spider_param_internal_xa(
  THD *thd
);
uint spider_param_internal_xa_snapshot(
  THD *thd
);
uint spider_param_force_commit(
  THD *thd
);
longlong spider_param_internal_offset(
  THD *thd,
  longlong internal_offset
);
longlong spider_param_internal_limit(
  THD *thd,
  longlong internal_limit
);
longlong spider_param_split_read(
  THD *thd,
  longlong split_read
);
double spider_param_semi_split_read(
  THD *thd,
  double semi_split_read
);
longlong spider_param_semi_split_read_limit(
  THD *thd,
  longlong semi_split_read_limit
);
int spider_param_init_sql_alloc_size(
  THD *thd,
  int init_sql_alloc_size
);
int spider_param_reset_sql_alloc(
  THD *thd,
  int reset_sql_alloc
);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
longlong spider_param_hs_result_free_size(
  THD *thd,
  longlong hs_result_free_size
);
#endif
int spider_param_multi_split_read(
  THD *thd,
  int multi_split_read
);
int spider_param_max_order(
  THD *thd,
  int max_order
);
int spider_param_semi_trx_isolation(
  THD *thd
);
int spider_param_semi_table_lock(
  THD *thd,
  int semi_table_lock
);
int spider_param_semi_table_lock_connection(
  THD *thd,
  int semi_table_lock_connection
);
uint spider_param_block_size(
  THD *thd
);
int spider_param_selupd_lock_mode(
  THD *thd,
  int selupd_lock_mode
);
bool spider_param_sync_autocommit(
  THD *thd
);
bool spider_param_sync_time_zone(
  THD *thd
);
bool spider_param_use_default_database(
  THD *thd
);
int spider_param_internal_sql_log_off(
  THD *thd
);
int spider_param_bulk_size(
  THD *thd,
  int bulk_size
);
int spider_param_bulk_update_mode(
  THD *thd,
  int bulk_update_mode
);
int spider_param_bulk_update_size(
  THD *thd,
  int bulk_update_size
);
int spider_param_internal_optimize(
  THD *thd,
  int internal_optimize
);
int spider_param_internal_optimize_local(
  THD *thd,
  int internal_optimize_local
);
bool spider_param_use_flash_logs(
  THD *thd
);
int spider_param_use_snapshot_with_flush_tables(
  THD *thd
);
bool spider_param_use_all_conns_snapshot(
  THD *thd
);
bool spider_param_lock_exchange(
  THD *thd
);
bool spider_param_internal_unlock(
  THD *thd
);
bool spider_param_semi_trx(
  THD *thd
);
int spider_param_connect_timeout(
  THD *thd,
  int connect_timeout
);
int spider_param_net_read_timeout(
  THD *thd,
  int net_read_timeout
);
int spider_param_net_write_timeout(
  THD *thd,
  int net_write_timeout
);
int spider_param_quick_mode(
  THD *thd,
  int quick_mode
);
longlong spider_param_quick_page_size(
  THD *thd,
  longlong quick_page_size
);
int spider_param_low_mem_read(
  THD *thd,
  int low_mem_read
);
int spider_param_select_column_mode(
  THD *thd,
  int select_column_mode
);
#ifndef WITHOUT_SPIDER_BG_SEARCH
int spider_param_bgs_mode(
  THD *thd,
  int bgs_mode
);
longlong spider_param_bgs_first_read(
  THD *thd,
  longlong bgs_first_read
);
longlong spider_param_bgs_second_read(
  THD *thd,
  longlong bgs_second_read
);
#endif
longlong spider_param_first_read(
  THD *thd,
  longlong first_read
);
longlong spider_param_second_read(
  THD *thd,
  longlong second_read
);
double spider_param_crd_interval(
  THD *thd,
  double crd_interval
);
int spider_param_crd_mode(
  THD *thd,
  int crd_mode
);
#ifdef WITH_PARTITION_STORAGE_ENGINE
int spider_param_crd_sync(
  THD *thd,
  int crd_sync
);
#endif
int spider_param_crd_type(
  THD *thd,
  int crd_type
);
double spider_param_crd_weight(
  THD *thd,
  double crd_weight
);
#ifndef WITHOUT_SPIDER_BG_SEARCH
int spider_param_crd_bg_mode(
  THD *thd,
  int crd_bg_mode
);
#endif
double spider_param_sts_interval(
  THD *thd,
  double sts_interval
);
int spider_param_sts_mode(
  THD *thd,
  int sts_mode
);
#ifdef WITH_PARTITION_STORAGE_ENGINE
int spider_param_sts_sync(
  THD *thd,
  int sts_sync
);
#endif
#ifndef WITHOUT_SPIDER_BG_SEARCH
int spider_param_sts_bg_mode(
  THD *thd,
  int sts_bg_mode
);
#endif
double spider_param_ping_interval_at_trx_start(
  THD *thd
);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
double spider_param_hs_ping_interval(
  THD *thd
);
#endif
int spider_param_auto_increment_mode(
  THD *thd,
  int auto_increment_mode
);
bool spider_param_same_server_link(
  THD *thd
);
bool spider_param_local_lock_table(
  THD *thd
);
int spider_param_use_pushdown_udf(
  THD *thd,
  int use_pushdown_udf
);
int spider_param_direct_dup_insert(
  THD *thd,
  int direct_dup_insert
);
uint spider_param_udf_table_lock_mutex_count();
uint spider_param_udf_table_mon_mutex_count();
longlong spider_param_udf_ds_bulk_insert_rows(
  THD *thd,
  longlong udf_ds_bulk_insert_rows
);
int spider_param_udf_ds_table_loop_mode(
  THD *thd,
  int udf_ds_table_loop_mode
);
char *spider_param_remote_access_charset();
int spider_param_remote_autocommit();
char *spider_param_remote_time_zone();
int spider_param_remote_sql_log_off();
int spider_param_remote_trx_isolation();
char *spider_param_remote_default_database();
longlong spider_param_connect_retry_interval(
  THD *thd
);
int spider_param_connect_retry_count(
  THD *thd
);
char *spider_param_bka_engine(
  THD *thd,
  char *bka_engine
);
int spider_param_bka_mode(
  THD *thd,
  int bka_mode
);
int spider_param_udf_ct_bulk_insert_interval(
  int udf_ct_bulk_insert_interval
);
longlong spider_param_udf_ct_bulk_insert_rows(
  longlong udf_ct_bulk_insert_rows
);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
uint spider_param_hs_r_conn_recycle_mode(
  THD *thd
);
uint spider_param_hs_r_conn_recycle_strict(
  THD *thd
);
uint spider_param_hs_w_conn_recycle_mode(
  THD *thd
);
uint spider_param_hs_w_conn_recycle_strict(
  THD *thd
);
int spider_param_use_hs_read(
  THD *thd,
  int use_hs_read
);
int spider_param_use_hs_write(
  THD *thd,
  int use_hs_write
);
#endif
int spider_param_use_handler(
  THD *thd,
  int use_handler
);
int spider_param_error_read_mode(
  THD *thd,
  int error_read_mode
);
int spider_param_error_write_mode(
  THD *thd,
  int error_write_mode
);
int spider_param_skip_default_condition(
  THD *thd,
  int skip_default_condition
);
longlong spider_param_direct_order_limit(
  THD *thd,
  longlong direct_order_limit
);
int spider_param_read_only_mode(
  THD *thd,
  int read_only_mode
);
#ifdef HA_CAN_BULK_ACCESS
int spider_param_bulk_access_free(
  int bulk_access_free
);
#endif
#if MYSQL_VERSION_ID < 50500
#else
int spider_param_udf_ds_use_real_table(
  THD *thd,
  int udf_ds_use_real_table
);
#endif
my_bool spider_param_general_log();
uint spider_param_log_result_errors();
uint spider_param_log_result_error_with_sql();
uint spider_param_internal_xa_id_type(
  THD *thd
);
int spider_param_casual_read(
  THD *thd,
  int casual_read
);
my_bool spider_param_dry_access();
int spider_param_delete_all_rows_type(
  THD *thd,
  int delete_all_rows_type
);
int spider_param_bka_table_name_type(
  THD *thd,
  int bka_table_name_type
);
