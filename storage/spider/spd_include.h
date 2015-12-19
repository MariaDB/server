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

#define SPIDER_DETAIL_VERSION "3.2.37"
#define SPIDER_HEX_VERSION 0x0302

#if MYSQL_VERSION_ID < 50500
#else
#define my_free(A,B) my_free(A)
#ifdef pthread_mutex_t
#undef pthread_mutex_t
#endif
#define pthread_mutex_t mysql_mutex_t
#ifdef pthread_mutex_lock
#undef pthread_mutex_lock
#endif
#define pthread_mutex_lock mysql_mutex_lock
#ifdef pthread_mutex_trylock
#undef pthread_mutex_trylock
#endif
#define pthread_mutex_trylock mysql_mutex_trylock
#ifdef pthread_mutex_unlock
#undef pthread_mutex_unlock
#endif
#define pthread_mutex_unlock mysql_mutex_unlock
#ifdef pthread_mutex_destroy
#undef pthread_mutex_destroy
#endif
#define pthread_mutex_destroy mysql_mutex_destroy
#ifdef pthread_cond_t
#undef pthread_cond_t
#endif
#define pthread_cond_t mysql_cond_t
#ifdef pthread_cond_wait
#undef pthread_cond_wait
#endif
#define pthread_cond_wait mysql_cond_wait
#ifdef pthread_cond_timedwait
#undef pthread_cond_timedwait
#endif
#define pthread_cond_timedwait mysql_cond_timedwait
#ifdef pthread_cond_signal
#undef pthread_cond_signal
#endif
#define pthread_cond_signal mysql_cond_signal
#ifdef pthread_cond_broadcast
#undef pthread_cond_broadcast
#endif
#define pthread_cond_broadcast mysql_cond_broadcast
#ifdef pthread_cond_destroy
#undef pthread_cond_destroy
#endif
#define pthread_cond_destroy mysql_cond_destroy
#define my_sprintf(A,B) sprintf B
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100004
#define spider_stmt_da_message(A) thd_get_error_message(A)
#define spider_stmt_da_sql_errno(A) thd_get_error_number(A)
#define spider_user_defined_key_parts(A) (A)->user_defined_key_parts
#define spider_join_table_count(A) (A)->table_count
#define SPIDER_CAN_BG_UPDATE (1LL << 39)
#define SPIDER_ALTER_ADD_PARTITION        Alter_info::ALTER_ADD_PARTITION
#define SPIDER_ALTER_DROP_PARTITION       Alter_info::ALTER_DROP_PARTITION
#define SPIDER_ALTER_COALESCE_PARTITION   Alter_info::ALTER_COALESCE_PARTITION
#define SPIDER_ALTER_REORGANIZE_PARTITION Alter_info::ALTER_REORGANIZE_PARTITION
#define SPIDER_ALTER_TABLE_REORG          Alter_info::ALTER_TABLE_REORG
#define SPIDER_ALTER_REBUILD_PARTITION    Alter_info::ALTER_REBUILD_PARTITION
#define SPIDER_WARN_LEVEL_WARN            Sql_condition::WARN_LEVEL_WARN
#define SPIDER_WARN_LEVEL_NOTE            Sql_condition::WARN_LEVEL_NOTE
#define SPIDER_THD_KILL_CONNECTION        KILL_CONNECTION
#else
#if MYSQL_VERSION_ID < 50500
#define spider_stmt_da_message(A) (A)->main_da.message()
#define spider_stmt_da_sql_errno(A) (A)->main_da.sql_errno()
#else
#if MYSQL_VERSION_ID < 50600
#define spider_stmt_da_message(A) (A)->stmt_da->message()
#define spider_stmt_da_sql_errno(A) (A)->stmt_da->sql_errno()
#else
#define spider_stmt_da_message(A) (A)->get_stmt_da()->message()
#define spider_stmt_da_sql_errno(A) (A)->get_stmt_da()->sql_errno()
#endif
#endif
#define spider_user_defined_key_parts(A) (A)->key_parts
#define spider_join_table_count(A) (A)->tables
#define SPIDER_ALTER_ADD_PARTITION        ALTER_ADD_PARTITION
#define SPIDER_ALTER_DROP_PARTITION       ALTER_DROP_PARTITION
#define SPIDER_ALTER_COALESCE_PARTITION   ALTER_COALESCE_PARTITION
#define SPIDER_ALTER_REORGANIZE_PARTITION ALTER_REORGANIZE_PARTITION
#define SPIDER_ALTER_TABLE_REORG          ALTER_TABLE_REORG
#define SPIDER_ALTER_REBUILD_PARTITION    ALTER_REBUILD_PARTITION
#define SPIDER_WARN_LEVEL_WARN            MYSQL_ERROR::WARN_LEVEL_WARN
#define SPIDER_WARN_LEVEL_NOTE            MYSQL_ERROR::WARN_LEVEL_NOTE
#define SPIDER_THD_KILL_CONNECTION        THD::KILL_CONNECTION
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100005
#define SPIDER_HAS_EXPLAIN_QUERY
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100009
#define SPIDER_TEST(A) MY_TEST(A)
#else
#define SPIDER_TEST(A) test(A)
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100100
#define SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
#define SPIDER_ENGINE_CONDITION_PUSHDOWN_IS_ALWAYS_ON
#define SPIDER_XID_USES_xid_cache_iterate
#else
#define SPIDER_XID_STATE_HAS_in_thd
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100108
#define SPIDER_Item_args_arg_count_IS_PROTECTED
#endif

#if MYSQL_VERSION_ID >= 50500
#define SPIDER_HAS_HASH_VALUE_TYPE
#endif

#define spider_bitmap_size(A) ((A + 7) / 8)
#define spider_set_bit(BITMAP, BIT) \
  ((BITMAP)[(BIT) / 8] |= (1 << ((BIT) & 7)))
#define spider_clear_bit(BITMAP, BIT) \
  ((BITMAP)[(BIT) / 8] &= ~(1 << ((BIT) & 7)))
#define spider_bit_is_set(BITMAP, BIT) \
  (uint) ((BITMAP)[(BIT) / 8] & (1 << ((BIT) & 7)))

#define SPIDER_LINK_STATUS_NO_CHANGE         0
#define SPIDER_LINK_STATUS_OK                1
#define SPIDER_LINK_STATUS_RECOVERY          2
#define SPIDER_LINK_STATUS_NG                3

#define SPIDER_LINK_MON_OK                   0
#define SPIDER_LINK_MON_NG                  -1
#define SPIDER_LINK_MON_DRAW_FEW_MON         1
#define SPIDER_LINK_MON_DRAW                 2

#define SPIDER_TMP_SHARE_CHAR_PTR_COUNT     19
#define SPIDER_TMP_SHARE_UINT_COUNT         17
#define SPIDER_TMP_SHARE_LONG_COUNT         18
#define SPIDER_TMP_SHARE_LONGLONG_COUNT      3

#define SPIDER_MEM_CALC_LIST_NUM           247

#define SPIDER_BACKUP_DASTATUS \
  bool da_status; if (thd) da_status = thd->is_error(); else da_status = FALSE;
#define SPIDER_RESTORE_DASTATUS \
  if (!da_status && thd->is_error()) thd->clear_error();
#define SPIDER_CONN_RESTORE_DASTATUS \
  if (thd && conn->error_mode) {SPIDER_RESTORE_DASTATUS;}
#define SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM \
  if (thd && conn->error_mode) {SPIDER_RESTORE_DASTATUS; error_num = 0;}
#define SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM \
  if (thd && conn->error_mode) {SPIDER_RESTORE_DASTATUS; tmp_error_num = 0;}

#define SPIDER_SET_FILE_POS(A) \
  {(A)->thd = current_thd; (A)->func_name = __func__; (A)->file_name = __FILE__; (A)->line_no = __LINE__;}
#define SPIDER_CLEAR_FILE_POS(A) \
  {DBUG_PRINT("info", ("spider thd=%p func_name=%s file_name=%s line_no=%lu", (A)->thd, (A)->func_name ? (A)->func_name : "NULL", (A)->file_name ? (A)->file_name : "NULL", (A)->line_no)); (A)->thd = NULL; (A)->func_name = NULL; (A)->file_name = NULL; (A)->line_no = 0;}

class ha_spider;
typedef struct st_spider_share SPIDER_SHARE;

typedef struct st_spider_file_pos
{
  THD                *thd;
  const char         *func_name;
  const char         *file_name;
  ulong              line_no;
} SPIDER_FILE_POS;

typedef struct st_spider_link_for_hash
{
  ha_spider          *spider;
  int                link_idx;
  spider_string      *db_table_str;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type db_table_str_hash_value;
#endif
} SPIDER_LINK_FOR_HASH;

/* alter table */
typedef struct st_spider_alter_table
{
  bool               now_create;
  char               *table_name;
  uint               table_name_length;
  char               *tmp_char;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type table_name_hash_value;
#endif
  longlong           tmp_priority;
  uint               link_count;
  uint               all_link_count;

  char               **tmp_server_names;
  char               **tmp_tgt_table_names;
  char               **tmp_tgt_dbs;
  char               **tmp_tgt_hosts;
  char               **tmp_tgt_usernames;
  char               **tmp_tgt_passwords;
  char               **tmp_tgt_sockets;
  char               **tmp_tgt_wrappers;
  char               **tmp_tgt_ssl_cas;
  char               **tmp_tgt_ssl_capaths;
  char               **tmp_tgt_ssl_certs;
  char               **tmp_tgt_ssl_ciphers;
  char               **tmp_tgt_ssl_keys;
  char               **tmp_tgt_default_files;
  char               **tmp_tgt_default_groups;
  long               *tmp_tgt_ports;
  long               *tmp_tgt_ssl_vscs;
  long               *tmp_link_statuses;

  uint               *tmp_server_names_lengths;
  uint               *tmp_tgt_table_names_lengths;
  uint               *tmp_tgt_dbs_lengths;
  uint               *tmp_tgt_hosts_lengths;
  uint               *tmp_tgt_usernames_lengths;
  uint               *tmp_tgt_passwords_lengths;
  uint               *tmp_tgt_sockets_lengths;
  uint               *tmp_tgt_wrappers_lengths;
  uint               *tmp_tgt_ssl_cas_lengths;
  uint               *tmp_tgt_ssl_capaths_lengths;
  uint               *tmp_tgt_ssl_certs_lengths;
  uint               *tmp_tgt_ssl_ciphers_lengths;
  uint               *tmp_tgt_ssl_keys_lengths;
  uint               *tmp_tgt_default_files_lengths;
  uint               *tmp_tgt_default_groups_lengths;

  uint               tmp_server_names_charlen;
  uint               tmp_tgt_table_names_charlen;
  uint               tmp_tgt_dbs_charlen;
  uint               tmp_tgt_hosts_charlen;
  uint               tmp_tgt_usernames_charlen;
  uint               tmp_tgt_passwords_charlen;
  uint               tmp_tgt_sockets_charlen;
  uint               tmp_tgt_wrappers_charlen;
  uint               tmp_tgt_ssl_cas_charlen;
  uint               tmp_tgt_ssl_capaths_charlen;
  uint               tmp_tgt_ssl_certs_charlen;
  uint               tmp_tgt_ssl_ciphers_charlen;
  uint               tmp_tgt_ssl_keys_charlen;
  uint               tmp_tgt_default_files_charlen;
  uint               tmp_tgt_default_groups_charlen;

  uint               tmp_server_names_length;
  uint               tmp_tgt_table_names_length;
  uint               tmp_tgt_dbs_length;
  uint               tmp_tgt_hosts_length;
  uint               tmp_tgt_usernames_length;
  uint               tmp_tgt_passwords_length;
  uint               tmp_tgt_sockets_length;
  uint               tmp_tgt_wrappers_length;
  uint               tmp_tgt_ssl_cas_length;
  uint               tmp_tgt_ssl_capaths_length;
  uint               tmp_tgt_ssl_certs_length;
  uint               tmp_tgt_ssl_ciphers_length;
  uint               tmp_tgt_ssl_keys_length;
  uint               tmp_tgt_default_files_length;
  uint               tmp_tgt_default_groups_length;
  uint               tmp_tgt_ports_length;
  uint               tmp_tgt_ssl_vscs_length;
  uint               tmp_link_statuses_length;
} SPIDER_ALTER_TABLE;

/* database connection */
typedef struct st_spider_conn
{
  uint               conn_kind;
  char               *conn_key;
  uint               conn_key_length;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type conn_key_hash_value;
#endif
  int                link_idx;
  spider_db_conn     *db_conn;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  query_id_t         hsc_query_id;
  ulonglong          hs_pre_age;
  ulonglong          hs_age;
#endif
  uint               opened_handlers;
  ulonglong          conn_id;
  ulonglong          connection_id;
  query_id_t         casual_read_query_id;
  uint               casual_read_current_id;
  st_spider_conn     *casual_read_base_conn;
  pthread_mutex_t    mta_conn_mutex;
  volatile bool      mta_conn_mutex_lock_already;
  volatile bool      mta_conn_mutex_unlock_later;
  SPIDER_FILE_POS    mta_conn_mutex_file_pos;
  uint               join_trx;
  int                trx_isolation;
  bool               semi_trx_isolation_chk;
  int                semi_trx_isolation;
  bool               semi_trx_chk;
  bool               semi_trx;
  bool               trx_start;
  bool               table_locked;
  int                table_lock;
  bool               disable_xa;
  bool               disable_reconnect;
  int                autocommit;
  int                sql_log_off;
  THD                *thd;
  void               *another_ha_first;
  void               *another_ha_last;
  st_spider_conn     *p_small;
  st_spider_conn     *p_big;
  st_spider_conn     *c_small;
  st_spider_conn     *c_big;
  longlong           priority;
  bool               server_lost;
  bool               ignore_dup_key;
  char               *error_str;
  int                error_length;
  time_t             ping_time;
  CHARSET_INFO       *access_charset;
  Time_zone          *time_zone;
  uint               connect_timeout;
  uint               net_read_timeout;
  uint               net_write_timeout;
  int                error_mode;
  spider_string      default_database;

  char               *tgt_host;
  char               *tgt_username;
  char               *tgt_password;
  char               *tgt_socket;
  char               *tgt_wrapper;
  char               *tgt_ssl_ca;
  char               *tgt_ssl_capath;
  char               *tgt_ssl_cert;
  char               *tgt_ssl_cipher;
  char               *tgt_ssl_key;
  char               *tgt_default_file;
  char               *tgt_default_group;
  long               tgt_port;
  long               tgt_ssl_vsc;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char               *hs_sock;
  long               hs_port;
#endif

  uint               tgt_host_length;
  uint               tgt_username_length;
  uint               tgt_password_length;
  uint               tgt_socket_length;
  uint               tgt_wrapper_length;
  uint               tgt_ssl_ca_length;
  uint               tgt_ssl_capath_length;
  uint               tgt_ssl_cert_length;
  uint               tgt_ssl_cipher_length;
  uint               tgt_ssl_key_length;
  uint               tgt_default_file_length;
  uint               tgt_default_group_length;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint               hs_sock_length;
#endif
  uint               dbton_id;

#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile
#endif
    void             *quick_target;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile bool      bg_init;
  volatile bool      bg_break;
  volatile bool      bg_kill;
  volatile bool      bg_caller_wait;
  volatile bool      bg_caller_sync_wait;
  volatile bool      bg_search;
  volatile bool      bg_discard_result;
  volatile bool      bg_direct_sql;
  volatile bool      bg_exec_sql;
  volatile bool      bg_get_job_stack;
  volatile bool      bg_get_job_stack_off;
  volatile uint      bg_simple_action;
  THD                *bg_thd;
  pthread_t          bg_thread;
  pthread_cond_t     bg_conn_cond;
  pthread_mutex_t    bg_conn_mutex;
  pthread_cond_t     bg_conn_sync_cond;
  pthread_mutex_t    bg_conn_sync_mutex;
  pthread_mutex_t    bg_conn_chain_mutex;
  pthread_mutex_t    *bg_conn_chain_mutex_ptr;
  volatile void      *bg_target;
  volatile int       *bg_error_num;
  volatile ulong     bg_sql_type;
  pthread_mutex_t    bg_job_stack_mutex;
  DYNAMIC_ARRAY      bg_job_stack;
  uint               bg_job_stack_id;
  const char         *bg_job_stack_func_name;
  const char         *bg_job_stack_file_name;
  ulong              bg_job_stack_line_no;
  uint               bg_job_stack_cur_pos;
#endif
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile
#endif
    int              *need_mon;
  int                *conn_need_mon;

  bool               use_for_active_standby;
  bool               in_before_query;

  bool               queued_connect;
  bool               queued_ping;
  bool               queued_trx_isolation;
  bool               queued_semi_trx_isolation;
  bool               queued_autocommit;
  bool               queued_sql_log_off;
  bool               queued_time_zone;
  bool               queued_trx_start;
  bool               queued_xa_start;
  bool               queued_net_timeout;
  SPIDER_SHARE       *queued_connect_share;
  int                queued_connect_link_idx;
  ha_spider          *queued_ping_spider;
  int                queued_ping_link_idx;
  int                queued_trx_isolation_val;
  int                queued_semi_trx_isolation_val;
  bool               queued_autocommit_val;
  bool               queued_sql_log_off_val;
  Time_zone          *queued_time_zone_val;
  XID                *queued_xa_start_xid;

#ifdef HA_CAN_BULK_ACCESS
  uint               bulk_access_requests;
  uint               bulk_access_sended;
  int                bulk_access_error_num;
  st_spider_conn     *bulk_access_next;
#endif

  bool               connect_error_with_message;
  char               connect_error_msg[MYSQL_ERRMSG_SIZE];
  int                connect_error;
  THD                *connect_error_thd;
  query_id_t         connect_error_query_id;
  time_t             connect_error_time;
} SPIDER_CONN;

typedef struct st_spider_lgtm_tblhnd_share
{
  char               *table_name;
  uint               table_name_length;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type table_path_hash_value;
#endif
  pthread_mutex_t    auto_increment_mutex;
  volatile bool      auto_increment_init;
  volatile ulonglong auto_increment_lclval;
  ulonglong          auto_increment_value;
} SPIDER_LGTM_TBLHND_SHARE;

#ifdef WITH_PARTITION_STORAGE_ENGINE
typedef struct st_spider_patition_handler_share
{
  uint               use_count;
  TABLE              *table;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type table_hash_value;
#endif
  void               *creator;
  void               **handlers;
  uchar              *searched_bitmap;
  uchar              *ft_discard_bitmap;
  uchar              *idx_read_bitmap;
  uchar              *idx_write_bitmap;
  uchar              *rnd_read_bitmap;
  uchar              *rnd_write_bitmap;
  bool               between_flg;
  bool               idx_bitmap_is_set;
  bool               rnd_bitmap_is_set;
} SPIDER_PARTITION_HANDLER_SHARE;

typedef struct st_spider_patition_share
{
  char               *table_name;
  uint               table_name_length;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type table_path_hash_value;
#endif
  uint               use_count;
  pthread_mutex_t    sts_mutex;
  pthread_mutex_t    crd_mutex;
  pthread_mutex_t    pt_handler_mutex;
  HASH               pt_handler_hash;
  uint               pt_handler_hash_id;
  const char         *pt_handler_hash_func_name;
  const char         *pt_handler_hash_file_name;
  ulong              pt_handler_hash_line_no;

  volatile bool      sts_init;
  volatile bool      crd_init;
  volatile time_t    sts_get_time;
  volatile time_t    crd_get_time;
  ulonglong          data_file_length;
  ulonglong          max_data_file_length;
  ulonglong          index_file_length;
  ulonglong          auto_increment_value;
  ha_rows            records;
  ulong              mean_rec_length;
  time_t             check_time;
  time_t             create_time;
  time_t             update_time;

  longlong           *cardinality;
/*
  volatile SPIDER_PARTITION_HANDLER_SHARE *partition_handler_share;
*/
} SPIDER_PARTITION_SHARE;
#endif

typedef struct st_spider_transaction
{
  bool               trx_start;
  bool               trx_xa;
  bool               trx_consistent_snapshot;
  bool               trx_xa_prepared;

  bool               use_consistent_snapshot;
  bool               internal_xa;
  uint               internal_xa_snapshot;

  query_id_t         query_id;
  bool               tmp_flg;
  bool               registed_allocated_thds;

  THD                *thd;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type thd_hash_value;
#endif
  XID                xid;
  HASH               trx_conn_hash;
  uint               trx_conn_hash_id;
  const char         *trx_conn_hash_func_name;
  const char         *trx_conn_hash_file_name;
  ulong              trx_conn_hash_line_no;
  HASH               trx_another_conn_hash;
  uint               trx_another_conn_hash_id;
  const char         *trx_another_conn_hash_func_name;
  const char         *trx_another_conn_hash_file_name;
  ulong              trx_another_conn_hash_line_no;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  HASH               trx_hs_r_conn_hash;
  uint               trx_hs_r_conn_hash_id;
  const char         *trx_hs_r_conn_hash_func_name;
  const char         *trx_hs_r_conn_hash_file_name;
  ulong              trx_hs_r_conn_hash_line_no;
  HASH               trx_hs_w_conn_hash;
  uint               trx_hs_w_conn_hash_id;
  const char         *trx_hs_w_conn_hash_func_name;
  const char         *trx_hs_w_conn_hash_file_name;
  ulong              trx_hs_w_conn_hash_line_no;
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  HASH               trx_direct_hs_r_conn_hash;
  uint               trx_direct_hs_r_conn_hash_id;
  const char         *trx_direct_hs_r_conn_hash_func_name;
  const char         *trx_direct_hs_r_conn_hash_file_name;
  ulong              trx_direct_hs_r_conn_hash_line_no;
  HASH               trx_direct_hs_w_conn_hash;
  uint               trx_direct_hs_w_conn_hash_id;
  const char         *trx_direct_hs_w_conn_hash_func_name;
  const char         *trx_direct_hs_w_conn_hash_file_name;
  ulong              trx_direct_hs_w_conn_hash_line_no;
#endif
  HASH               trx_alter_table_hash;
  uint               trx_alter_table_hash_id;
  const char         *trx_alter_table_hash_func_name;
  const char         *trx_alter_table_hash_file_name;
  ulong              trx_alter_table_hash_line_no;
  HASH               trx_ha_hash;
  uint               trx_ha_hash_id;
  const char         *trx_ha_hash_func_name;
  const char         *trx_ha_hash_file_name;
  ulong              trx_ha_hash_line_no;
  uint               trx_ha_reuse_count;
  XID_STATE          internal_xid_state;
  SPIDER_CONN        *join_trx_top;
  ulonglong          spider_thread_id;
  ulonglong          trx_conn_adjustment;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  ulonglong          trx_hs_r_conn_adjustment;
  ulonglong          trx_hs_w_conn_adjustment;
#endif
  uint               locked_connections;

  ulonglong          direct_update_count;
  ulonglong          direct_delete_count;
  ulonglong          direct_order_limit_count;
  ulonglong          direct_aggregate_count;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  ulonglong          hs_result_free_count;
#endif

#ifdef HA_CAN_BULK_ACCESS
  SPIDER_CONN        *bulk_access_conn_first;
  SPIDER_CONN        *bulk_access_conn_last;
#endif

  pthread_mutex_t    *udf_table_mutexes;
  CHARSET_INFO       *udf_access_charset;
  spider_string      *udf_set_names;

  time_t             mem_calc_merge_time;
  const char         *alloc_func_name[SPIDER_MEM_CALC_LIST_NUM];
  const char         *alloc_file_name[SPIDER_MEM_CALC_LIST_NUM];
  ulong              alloc_line_no[SPIDER_MEM_CALC_LIST_NUM];
  ulonglong          total_alloc_mem[SPIDER_MEM_CALC_LIST_NUM];
  longlong           current_alloc_mem[SPIDER_MEM_CALC_LIST_NUM];
  ulonglong          alloc_mem_count[SPIDER_MEM_CALC_LIST_NUM];
  ulonglong          free_mem_count[SPIDER_MEM_CALC_LIST_NUM];
  ulonglong          total_alloc_mem_buffer[SPIDER_MEM_CALC_LIST_NUM];
  longlong           current_alloc_mem_buffer[SPIDER_MEM_CALC_LIST_NUM];
  ulonglong          alloc_mem_count_buffer[SPIDER_MEM_CALC_LIST_NUM];
  ulonglong          free_mem_count_buffer[SPIDER_MEM_CALC_LIST_NUM];

  MEM_ROOT           mem_root;

  /* for transaction level query */
  SPIDER_SHARE       *tmp_share;
  char               *tmp_connect_info[SPIDER_TMP_SHARE_CHAR_PTR_COUNT];
  uint               tmp_connect_info_length[SPIDER_TMP_SHARE_UINT_COUNT];
  long               tmp_long[SPIDER_TMP_SHARE_LONG_COUNT];
  longlong           tmp_longlong[SPIDER_TMP_SHARE_LONGLONG_COUNT];
  ha_spider          *tmp_spider;
  int                tmp_need_mon;
  spider_db_handler  *tmp_dbton_handler[SPIDER_DBTON_SIZE];
} SPIDER_TRX;

typedef struct st_spider_share
{
  char               *table_name;
  uint               table_name_length;
  uint               use_count;
  uint               link_count;
  uint               all_link_count;
  uint               link_bitmap_size;
  pthread_mutex_t    mutex;
  pthread_mutex_t    sts_mutex;
  pthread_mutex_t    crd_mutex;
/*
  pthread_mutex_t    auto_increment_mutex;
*/
  THR_LOCK           lock;
  TABLE_SHARE        *table_share;
  SPIDER_LGTM_TBLHND_SHARE *lgtm_tblhnd_share;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type table_name_hash_value;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  my_hash_value_type table_path_hash_value;
#endif
#endif

  volatile bool      init;
  volatile bool      init_error;
  volatile time_t    init_error_time;
  volatile bool      link_status_init;
  uchar              *table_mon_mutex_bitmap;
  volatile bool      sts_init;
  volatile time_t    sts_get_time;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile time_t    bg_sts_try_time;
  volatile double    bg_sts_interval;
  volatile int       bg_sts_mode;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  volatile int       bg_sts_sync;
#endif
  volatile bool      bg_sts_init;
  volatile bool      bg_sts_kill;
  volatile bool      bg_sts_thd_wait;
  THD                *bg_sts_thd;
  pthread_t          bg_sts_thread;
  pthread_cond_t     bg_sts_cond;
  pthread_cond_t     bg_sts_sync_cond;
  volatile bool      crd_init;
#endif
  volatile time_t    crd_get_time;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile time_t    bg_crd_try_time;
  volatile double    bg_crd_interval;
  volatile int       bg_crd_mode;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  volatile int       bg_crd_sync;
#endif
  volatile bool      bg_crd_init;
  volatile bool      bg_crd_kill;
  volatile bool      bg_crd_thd_wait;
  THD                *bg_crd_thd;
  pthread_t          bg_crd_thread;
  pthread_cond_t     bg_crd_cond;
  pthread_cond_t     bg_crd_sync_cond;
#endif
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile bool      bg_mon_init;
  volatile bool      bg_mon_kill;
  THD                **bg_mon_thds;
  pthread_t          *bg_mon_threads;
  pthread_mutex_t    *bg_mon_mutexes;
  pthread_cond_t     *bg_mon_conds;
  pthread_cond_t     *bg_mon_sleep_conds;
#endif
/*
  volatile bool      auto_increment_init;
  volatile ulonglong auto_increment_lclval;
*/
  ulonglong          data_file_length;
  ulonglong          max_data_file_length;
  ulonglong          index_file_length;
/*
  ulonglong          auto_increment_value;
*/
  ha_rows            records;
  ulong              mean_rec_length;
  time_t             check_time;
  time_t             create_time;
  time_t             update_time;

  longlong           static_records_for_status;
  longlong           static_mean_rec_length;

  int                bitmap_size;
  spider_string      *key_hint;
  CHARSET_INFO       *access_charset;
  longlong           *static_key_cardinality;
  longlong           *cardinality;
  uchar              *cardinality_upd;
  longlong           additional_table_flags;
  bool               have_recovery_link;

#ifndef WITHOUT_SPIDER_BG_SEARCH
  int                sts_bg_mode;
#endif
  double             sts_interval;
  int                sts_mode;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int                sts_sync;
#endif
#ifndef WITHOUT_SPIDER_BG_SEARCH
  int                crd_bg_mode;
#endif
  double             crd_interval;
  int                crd_mode;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int                crd_sync;
#endif
  int                crd_type;
  double             crd_weight;
  longlong           internal_offset;
  longlong           internal_limit;
  longlong           split_read;
  double             semi_split_read;
  longlong           semi_split_read_limit;
  int                init_sql_alloc_size;
  int                reset_sql_alloc;
  int                multi_split_read;
  int                max_order;
  int                semi_table_lock;
  int                semi_table_lock_conn;
  int                selupd_lock_mode;
  int                query_cache;
  int                query_cache_sync;
  int                internal_delayed;
  int                bulk_size;
  int                bulk_update_mode;
  int                bulk_update_size;
  int                internal_optimize;
  int                internal_optimize_local;
  double             scan_rate;
  double             read_rate;
  longlong           priority;
  int                quick_mode;
  longlong           quick_page_size;
  int                low_mem_read;
  int                table_count_mode;
  int                select_column_mode;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  int                bgs_mode;
  longlong           bgs_first_read;
  longlong           bgs_second_read;
#endif
  longlong           first_read;
  longlong           second_read;
  int                auto_increment_mode;
  int                use_table_charset;
  int                use_pushdown_udf;
  int                skip_default_condition;
  int                direct_dup_insert;
  longlong           direct_order_limit;
  int                read_only_mode;
  int                error_read_mode;
  int                error_write_mode;
  int                active_link_count;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  longlong           hs_result_free_size;
#endif
#ifdef HA_CAN_BULK_ACCESS
  int                bulk_access_free;
#endif
#ifdef HA_CAN_FORCE_BULK_UPDATE
  int                force_bulk_update;
#endif
#ifdef HA_CAN_FORCE_BULK_DELETE
  int                force_bulk_delete;
#endif
  int                casual_read;
  int                delete_all_rows_type;

  int                bka_mode;
  char               *bka_engine;
  int                bka_engine_length;

#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type *conn_keys_hash_value;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  my_hash_value_type *hs_read_conn_keys_hash_value;
  my_hash_value_type *hs_write_conn_keys_hash_value;
#endif
#endif
  char               **server_names;
  char               **tgt_table_names;
  char               **tgt_dbs;
  char               **tgt_hosts;
  char               **tgt_usernames;
  char               **tgt_passwords;
  char               **tgt_sockets;
  char               **tgt_wrappers;
  char               **tgt_ssl_cas;
  char               **tgt_ssl_capaths;
  char               **tgt_ssl_certs;
  char               **tgt_ssl_ciphers;
  char               **tgt_ssl_keys;
  char               **tgt_default_files;
  char               **tgt_default_groups;
  char               **tgt_pk_names;
  char               **tgt_sequence_names;
  char               **conn_keys;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char               **hs_read_socks;
  char               **hs_write_socks;
  char               **hs_read_conn_keys;
  char               **hs_write_conn_keys;
#endif
  long               *tgt_ports;
  long               *tgt_ssl_vscs;
  long               *link_statuses;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  long               *monitoring_bg_flag;
  long               *monitoring_bg_kind;
#endif
  long               *monitoring_flag;
  long               *monitoring_kind;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  longlong           *monitoring_bg_interval;
#endif
  longlong           *monitoring_limit;
  longlong           *monitoring_sid;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  long               *use_hs_reads;
  long               *use_hs_writes;
  long               *hs_read_ports;
  long               *hs_write_ports;
  long               *hs_write_to_reads;
#endif
  long               *use_handlers;
  long               *connect_timeouts;
  long               *net_read_timeouts;
  long               *net_write_timeouts;
  long               *access_balances;
  long               *bka_table_name_types;

  uint               *server_names_lengths;
  uint               *tgt_table_names_lengths;
  uint               *tgt_dbs_lengths;
  uint               *tgt_hosts_lengths;
  uint               *tgt_usernames_lengths;
  uint               *tgt_passwords_lengths;
  uint               *tgt_sockets_lengths;
  uint               *tgt_wrappers_lengths;
  uint               *tgt_ssl_cas_lengths;
  uint               *tgt_ssl_capaths_lengths;
  uint               *tgt_ssl_certs_lengths;
  uint               *tgt_ssl_ciphers_lengths;
  uint               *tgt_ssl_keys_lengths;
  uint               *tgt_default_files_lengths;
  uint               *tgt_default_groups_lengths;
  uint               *tgt_pk_names_lengths;
  uint               *tgt_sequence_names_lengths;
  uint               *conn_keys_lengths;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint               *hs_read_socks_lengths;
  uint               *hs_write_socks_lengths;
  uint               *hs_read_conn_keys_lengths;
  uint               *hs_write_conn_keys_lengths;
#endif
  uint               *sql_dbton_ids;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint               *hs_dbton_ids;
#endif

  uint               server_names_charlen;
  uint               tgt_table_names_charlen;
  uint               tgt_dbs_charlen;
  uint               tgt_hosts_charlen;
  uint               tgt_usernames_charlen;
  uint               tgt_passwords_charlen;
  uint               tgt_sockets_charlen;
  uint               tgt_wrappers_charlen;
  uint               tgt_ssl_cas_charlen;
  uint               tgt_ssl_capaths_charlen;
  uint               tgt_ssl_certs_charlen;
  uint               tgt_ssl_ciphers_charlen;
  uint               tgt_ssl_keys_charlen;
  uint               tgt_default_files_charlen;
  uint               tgt_default_groups_charlen;
  uint               tgt_pk_names_charlen;
  uint               tgt_sequence_names_charlen;
  uint               conn_keys_charlen;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint               hs_read_socks_charlen;
  uint               hs_write_socks_charlen;
  uint               hs_read_conn_keys_charlen;
  uint               hs_write_conn_keys_charlen;
#endif

  uint               server_names_length;
  uint               tgt_table_names_length;
  uint               tgt_dbs_length;
  uint               tgt_hosts_length;
  uint               tgt_usernames_length;
  uint               tgt_passwords_length;
  uint               tgt_sockets_length;
  uint               tgt_wrappers_length;
  uint               tgt_ssl_cas_length;
  uint               tgt_ssl_capaths_length;
  uint               tgt_ssl_certs_length;
  uint               tgt_ssl_ciphers_length;
  uint               tgt_ssl_keys_length;
  uint               tgt_default_files_length;
  uint               tgt_default_groups_length;
  uint               tgt_pk_names_length;
  uint               tgt_sequence_names_length;
  uint               conn_keys_length;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint               hs_read_socks_length;
  uint               hs_write_socks_length;
  uint               hs_read_conn_keys_length;
  uint               hs_write_conn_keys_length;
#endif
  uint               tgt_ports_length;
  uint               tgt_ssl_vscs_length;
  uint               link_statuses_length;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  uint               monitoring_bg_flag_length;
  uint               monitoring_bg_kind_length;
#endif
  uint               monitoring_flag_length;
  uint               monitoring_kind_length;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  uint               monitoring_bg_interval_length;
#endif
  uint               monitoring_limit_length;
  uint               monitoring_sid_length;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint               use_hs_reads_length;
  uint               use_hs_writes_length;
  uint               hs_read_ports_length;
  uint               hs_write_ports_length;
  uint               hs_write_to_reads_length;
#endif
  uint               use_handlers_length;
  uint               connect_timeouts_length;
  uint               net_read_timeouts_length;
  uint               net_write_timeouts_length;
  uint               access_balances_length;
  uint               bka_table_name_types_length;

  /* for dbton */
  uchar              dbton_bitmap[spider_bitmap_size(SPIDER_DBTON_SIZE)];
  spider_db_share    *dbton_share[SPIDER_DBTON_SIZE];
  uint               use_dbton_count;
  uint               use_dbton_ids[SPIDER_DBTON_SIZE];
  uint               dbton_id_to_seq[SPIDER_DBTON_SIZE];
  uint               use_sql_dbton_count;
  uint               use_sql_dbton_ids[SPIDER_DBTON_SIZE];
  uint               sql_dbton_id_to_seq[SPIDER_DBTON_SIZE];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint               use_hs_dbton_count;
  uint               use_hs_dbton_ids[SPIDER_DBTON_SIZE];
  uint               hs_dbton_id_to_seq[SPIDER_DBTON_SIZE];
#endif

  SPIDER_ALTER_TABLE alter_table;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  SPIDER_PARTITION_SHARE *partition_share;
#endif
} SPIDER_SHARE;

typedef struct st_spider_link_pack
{
  SPIDER_SHARE               *share;
  int                        link_idx;
} SPIDER_LINK_PACK;

typedef struct st_spider_init_error_table
{
  char               *table_name;
  uint               table_name_length;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type table_name_hash_value;
#endif
  bool               init_error_with_message;
  char               init_error_msg[MYSQL_ERRMSG_SIZE];
  volatile int       init_error;
  volatile time_t    init_error_time;
} SPIDER_INIT_ERROR_TABLE;

typedef struct st_spider_direct_sql
{
  int                  table_count;
  char                 **db_names;
  char                 **table_names;
  TABLE                **tables;
  int                  *iop;

#if MYSQL_VERSION_ID < 50500
#else
  /* for using real table */
  bool                 real_table_used;
  TABLE_LIST           *table_list_first;
  TABLE_LIST           *table_list;
  uchar                *real_table_bitmap;
  Open_tables_backup   open_tables_backup;
  THD                  *open_tables_thd;
#endif

  char                 *sql;
  ulong                sql_length;

  SPIDER_TRX           *trx;
  SPIDER_CONN          *conn;

  bool                 modified_non_trans_table;

  int                  table_loop_mode;
  longlong             priority;
  int                  connect_timeout;
  int                  net_read_timeout;
  int                  net_write_timeout;
  longlong             bulk_insert_rows;
  int                  connection_channel;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  int                  access_mode;
#endif
#if MYSQL_VERSION_ID < 50500
#else
  int                  use_real_table;
#endif
  int                  error_rw_mode;

  char                 *server_name;
  char                 *tgt_default_db_name;
  char                 *tgt_host;
  char                 *tgt_username;
  char                 *tgt_password;
  char                 *tgt_socket;
  char                 *tgt_wrapper;
  char                 *tgt_ssl_ca;
  char                 *tgt_ssl_capath;
  char                 *tgt_ssl_cert;
  char                 *tgt_ssl_cipher;
  char                 *tgt_ssl_key;
  char                 *tgt_default_file;
  char                 *tgt_default_group;
  char                 *conn_key;
  long                 tgt_port;
  long                 tgt_ssl_vsc;

  uint                 server_name_length;
  uint                 tgt_default_db_name_length;
  uint                 tgt_host_length;
  uint                 tgt_username_length;
  uint                 tgt_password_length;
  uint                 tgt_socket_length;
  uint                 tgt_wrapper_length;
  uint                 tgt_ssl_ca_length;
  uint                 tgt_ssl_capath_length;
  uint                 tgt_ssl_cert_length;
  uint                 tgt_ssl_cipher_length;
  uint                 tgt_ssl_key_length;
  uint                 tgt_default_file_length;
  uint                 tgt_default_group_length;
  uint                 conn_key_length;
  uint                 dbton_id;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type   conn_key_hash_value;
#endif

  pthread_mutex_t               *bg_mutex;
  pthread_cond_t                *bg_cond;
  volatile st_spider_direct_sql *prev;
  volatile st_spider_direct_sql *next;
  void                          *parent;
} SPIDER_DIRECT_SQL;

typedef struct st_spider_bg_direct_sql
{
  longlong                   called_cnt;
  char                       bg_error_msg[MYSQL_ERRMSG_SIZE];
  volatile int               bg_error;
  volatile bool              modified_non_trans_table;
  pthread_mutex_t            bg_mutex;
  pthread_cond_t             bg_cond;
  volatile SPIDER_DIRECT_SQL *direct_sql;
} SPIDER_BG_DIRECT_SQL;

typedef struct st_spider_mon_table_result
{
  int                        result_status;
  SPIDER_TRX                 *trx;
} SPIDER_MON_TABLE_RESULT;

typedef struct st_spider_table_mon
{
  SPIDER_SHARE               *share;
  uint32                     server_id;
  st_spider_table_mon        *next;
} SPIDER_TABLE_MON;

typedef struct st_spider_table_mon_list
{
  char                       *key;
  uint                       key_length;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type         key_hash_value;
#endif

  uint                       use_count;
  uint                       mutex_hash;
  ulonglong                  mon_table_cache_version;

  char                       *table_name;
  int                        link_id;
  uint                       table_name_length;

  int                        list_size;
  SPIDER_TABLE_MON           *first;
  SPIDER_TABLE_MON           *current;
  volatile int               mon_status;

  SPIDER_SHARE               *share;

  pthread_mutex_t            caller_mutex;
  pthread_mutex_t            receptor_mutex;
  pthread_mutex_t            monitor_mutex;
  pthread_mutex_t            update_status_mutex;
  volatile int               last_caller_result;
  volatile int               last_receptor_result;
  volatile int               last_mon_result;
} SPIDER_TABLE_MON_LIST;

typedef struct st_spider_copy_table_conn
{
  SPIDER_SHARE               *share;
  int                        link_idx;
  SPIDER_CONN                *conn;
  spider_db_copy_table       *copy_table;
  ha_spider                  *spider;
  int                        need_mon;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  int                        bg_error_num;
#endif
  st_spider_copy_table_conn  *next;
} SPIDER_COPY_TABLE_CONN;

typedef struct st_spider_copy_tables
{
  SPIDER_TRX                 *trx;
  char                       *spider_db_name;
  int                        spider_db_name_length;
  char                       *spider_table_name;
  int                        spider_table_name_length;
  char                       *spider_real_table_name;
  int                        spider_real_table_name_length;
  TABLE_LIST                 spider_table_list;
  CHARSET_INFO               *access_charset;

  SPIDER_COPY_TABLE_CONN     *table_conn[2];
  bool                       use_auto_mode[2];
  int                        link_idx_count[2];
  int                        *link_idxs[2];

  int                        bulk_insert_interval;
  longlong                   bulk_insert_rows;
  int                        use_table_charset;
  int                        use_transaction;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  int                        bg_mode;
#endif

  char                       *database;

  int                        database_length;
} SPIDER_COPY_TABLES;

class SPIDER_SORT
{
public:
  ulong sort;
};

typedef struct st_spider_trx_ha
{
  char                       *table_name;
  uint                       table_name_length;
  SPIDER_TRX                 *trx;
  SPIDER_SHARE               *share;
  uint                       link_count;
  uint                       link_bitmap_size;
  uint                       *conn_link_idx;
  uchar                      *conn_can_fo;
  bool                       wait_for_reusing;
} SPIDER_TRX_HA;

#ifdef HA_CAN_BULK_ACCESS
typedef struct st_spider_bulk_access_link
{
  ha_spider                  *spider;
  uint                       sequence_num;
  bool                       used;
  bool                       called;
  MEM_ROOT                   mem_root;
  st_spider_bulk_access_link *next;
} SPIDER_BULK_ACCESS_LINK;
#endif

#define SPIDER_INT_HLD_TGT_SIZE 100
typedef struct st_spider_int_hld
{
  uint tgt_num;
  int tgt[SPIDER_INT_HLD_TGT_SIZE];
  st_spider_int_hld *next;
} SPIDER_INT_HLD;

typedef struct st_spider_item_hld
{
  uint               tgt_num;
  Item               *item;
#ifdef SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY_AND_THDPTR
  bool               init_mem_root;
  MEM_ROOT           mem_root;
#endif
  st_spider_item_hld *next;
} SPIDER_ITEM_HLD;

char *spider_create_string(
  const char *str,
  uint length
);
