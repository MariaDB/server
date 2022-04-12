/* Copyright (C) 2008-2020 Kentoku Shiba
   Copyright (C) 2019-2022 MariaDB corp

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

#define SPIDER_DETAIL_VERSION "3.3.15"
#define SPIDER_HEX_VERSION 0x0303

#define spider_my_free(A,B) my_free(A)
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
#define pthread_mutex_assert_owner(A) mysql_mutex_assert_owner(A)
#define pthread_mutex_assert_not_owner(A) mysql_mutex_assert_not_owner(A)
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

#define spider_stmt_da_message(A) thd_get_error_message(A)
#define spider_stmt_da_sql_errno(A) thd_get_error_number(A)
#define spider_user_defined_key_parts(A) (A)->user_defined_key_parts
#define spider_join_table_count(A) (A)->table_count
#define SPIDER_CAN_BG_UPDATE (1LL << 39)
#define SPIDER_ALTER_PARTITION_ADD         ALTER_PARTITION_ADD
#define SPIDER_ALTER_PARTITION_DROP        ALTER_PARTITION_DROP
#define SPIDER_ALTER_PARTITION_COALESCE    ALTER_PARTITION_COALESCE
#define SPIDER_ALTER_PARTITION_REORGANIZE  ALTER_PARTITION_REORGANIZE
#define SPIDER_ALTER_PARTITION_TABLE_REORG ALTER_PARTITION_TABLE_REORG
#define SPIDER_ALTER_PARTITION_REBUILD     ALTER_PARTITION_REBUILD
#define SPIDER_WARN_LEVEL_WARN            Sql_condition::WARN_LEVEL_WARN
#define SPIDER_WARN_LEVEL_NOTE            Sql_condition::WARN_LEVEL_NOTE
#define SPIDER_THD_KILL_CONNECTION        KILL_CONNECTION

#define SPIDER_HAS_EXPLAIN_QUERY

#define SPIDER_TEST(A) MY_TEST(A)

#define SPIDER_FIELD_FIELDPTR_REQUIRES_THDPTR
#define SPIDER_ENGINE_CONDITION_PUSHDOWN_IS_ALWAYS_ON
#define SPIDER_XID_USES_xid_cache_iterate

#define SPIDER_Item_args_arg_count_IS_PROTECTED

#define SPIDER_Item_func_conv_charset_conv_charset collation.collation

#define SPIDER_WITHOUT_HA_STATISTIC_INCREMENT
#define SPIDER_init_read_record(A,B,C,D,E,F,G,H) init_read_record(A,B,C,D,E,F,G,H)
#define SPIDER_HAS_NEXT_THREAD_ID
#define SPIDER_new_THD(A) (new THD(A))
#define SPIDER_order_direction_is_asc(A) (A->direction == ORDER::ORDER_ASC)

#define SPIDER_HAS_MY_CHARLEN
#define SPIDER_open_temporary_table

#define SPIDER_generate_partition_syntax(A,B,C,D,E,F,G,H) generate_partition_syntax(A,B,C,E,F,G)

#define SPIDER_create_partition_name(A,B,C,D,E,F) create_partition_name(A,B,C,D,E,F)
#define SPIDER_create_subpartition_name(A,B,C,D,E,F) create_subpartition_name(A,B,C,D,E,F)
#define SPIDER_free_part_syntax(A,B)

#define SPIDER_read_record_read_record(A) read_record()
#define SPIDER_has_Item_with_subquery
#define SPIDER_use_LEX_CSTRING_for_KEY_Field_name
#define SPIDER_use_LEX_CSTRING_for_Field_blob_constructor
#define SPIDER_use_LEX_CSTRING_for_database_tablename_alias
#define SPIDER_THD_db_str(A) (A)->db.str
#define SPIDER_THD_db_length(A) (A)->db.length
#define SPIDER_TABLE_LIST_db_str(A) (A)->db.str
#define SPIDER_TABLE_LIST_db_length(A) (A)->db.length
#define SPIDER_TABLE_LIST_table_name_str(A) (A)->table_name.str
#define SPIDER_TABLE_LIST_table_name_length(A) (A)->table_name.length
#define SPIDER_TABLE_LIST_alias_str(A) (A)->alias.str
#define SPIDER_TABLE_LIST_alias_length(A) (A)->alias.length
#define SPIDER_field_name_str(A) (A)->field_name.str
#define SPIDER_field_name_length(A) (A)->field_name.length
#define SPIDER_item_name_str(A) (A)->name.str
#define SPIDER_item_name_length(A) (A)->name.length
const LEX_CSTRING SPIDER_empty_string = {"", 0};

#define SPIDER_date_mode_t(A) date_mode_t(A)
#define SPIDER_str_to_datetime(A,B,C,D,E) str_to_datetime_or_date(A,B,C,D,E)
#define SPIDER_get_linkage(A) A->get_linkage()

typedef start_new_trans *SPIDER_Open_tables_backup;

#define SPIDER_reset_n_backup_open_tables_state(A,B,C) do { \
  if (!(*(B) = new start_new_trans(A))) \
  { \
    DBUG_RETURN(C); \
  } \
} while (0)
#define SPIDER_restore_backup_open_tables_state(A,B) do { \
  (*(B))->restore_old_transaction(); \
  delete *(B); \
} while (0)
#define SPIDER_sys_close_thread_tables(A) (A)->commit_whole_transaction_and_close_tables()

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

#define SPIDER_TMP_SHARE_CHAR_PTR_COUNT     23
#define SPIDER_TMP_SHARE_UINT_COUNT         SPIDER_TMP_SHARE_CHAR_PTR_COUNT
#define SPIDER_TMP_SHARE_LONG_COUNT         20
#define SPIDER_TMP_SHARE_LONGLONG_COUNT      3

#define SPIDER_MEM_CALC_LIST_NUM           314
#define SPIDER_CONN_META_BUF_LEN           64

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
typedef struct st_spider_table_mon_list SPIDER_TABLE_MON_LIST;
typedef struct st_spider_ip_port_conn SPIDER_IP_PORT_CONN;

typedef struct st_spider_thread
{
  uint                  thread_idx;
  THD                   *thd;
  volatile bool         killed;
  volatile bool         thd_wait;
  volatile bool         first_free_wait;
  volatile bool         init_command;
  volatile int          error;
  pthread_t             thread;
  pthread_cond_t        cond;
  pthread_mutex_t       mutex;
  pthread_cond_t        sync_cond;
  volatile SPIDER_SHARE *queue_first;
  volatile SPIDER_SHARE *queue_last;
} SPIDER_THREAD;

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
  my_hash_value_type db_table_str_hash_value;
} SPIDER_LINK_FOR_HASH;

/* alter table */
typedef struct st_spider_alter_table
{
  bool               now_create;
  char               *table_name;
  uint               table_name_length;
  char               *tmp_char;
  my_hash_value_type table_name_hash_value;
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
  char               **tmp_tgt_dsns;
  char               **tmp_tgt_filedsns;
  char               **tmp_tgt_drivers;
  char               **tmp_static_link_ids;
  long               *tmp_tgt_ports;
  long               *tmp_tgt_ssl_vscs;
  long               *tmp_monitoring_binlog_pos_at_failing;
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
  uint               *tmp_tgt_dsns_lengths;
  uint               *tmp_tgt_filedsns_lengths;
  uint               *tmp_tgt_drivers_lengths;
  uint               *tmp_static_link_ids_lengths;

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
  uint               tmp_tgt_dsns_charlen;
  uint               tmp_tgt_filedsns_charlen;
  uint               tmp_tgt_drivers_charlen;
  uint               tmp_static_link_ids_charlen;

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
  uint               tmp_tgt_dsns_length;
  uint               tmp_tgt_filedsns_length;
  uint               tmp_tgt_drivers_length;
  uint               tmp_static_link_ids_length;
  uint               tmp_tgt_ports_length;
  uint               tmp_tgt_ssl_vscs_length;
  uint               tmp_monitoring_binlog_pos_at_failing_length;
  uint               tmp_link_statuses_length;
} SPIDER_ALTER_TABLE;

typedef struct st_spider_conn_loop_check SPIDER_CONN_LOOP_CHECK;

/* database connection */
typedef struct st_spider_conn
{
  uint               conn_kind;
  char               *conn_key;
  uint               conn_key_length;
  my_hash_value_type conn_key_hash_value;
  int                link_idx;
  spider_db_conn     *db_conn;
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
  int                wait_timeout;
  sql_mode_t         sql_mode;
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
  char               *tgt_db; /* for not joinable tables on different db */
  char               *tgt_ssl_ca;
  char               *tgt_ssl_capath;
  char               *tgt_ssl_cert;
  char               *tgt_ssl_cipher;
  char               *tgt_ssl_key;
  char               *tgt_default_file;
  char               *tgt_default_group;
  char               *tgt_dsn;
  char               *tgt_filedsn;
  char               *tgt_driver;
  long               tgt_port;
  long               tgt_ssl_vsc;

  uint               tgt_host_length;
  uint               tgt_username_length;
  uint               tgt_password_length;
  uint               tgt_socket_length;
  uint               tgt_wrapper_length;
  uint               tgt_db_length;
  uint               tgt_ssl_ca_length;
  uint               tgt_ssl_capath_length;
  uint               tgt_ssl_cert_length;
  uint               tgt_ssl_cipher_length;
  uint               tgt_ssl_key_length;
  uint               tgt_default_file_length;
  uint               tgt_default_group_length;
  uint               tgt_dsn_length;
  uint               tgt_filedsn_length;
  uint               tgt_driver_length;
  uint               dbton_id;

  volatile
    void             *quick_target;
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
  volatile
    int              *need_mon;
  int                *conn_need_mon;

  bool               use_for_active_standby;
  bool               in_before_query;

  bool               queued_connect;
  bool               queued_ping;
  bool               queued_trx_isolation;
  bool               queued_semi_trx_isolation;
  bool               queued_wait_timeout;
  bool               queued_autocommit;
  bool               queued_sql_log_off;
  bool               queued_sql_mode;
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
  int                queued_wait_timeout_val;
  bool               queued_autocommit_val;
  bool               queued_sql_log_off_val;
  sql_mode_t         queued_sql_mode_val;
  Time_zone          *queued_time_zone_val;
  XID                *queued_xa_start_xid;


  bool               disable_connect_retry;  /* TRUE if it is unnecessary to
                                                retry to connect after a
                                                connection error */
  bool               connect_error_with_message;
  char               connect_error_msg[MYSQL_ERRMSG_SIZE];
  int                connect_error;
  THD                *connect_error_thd;
  query_id_t         connect_error_query_id;
  time_t             connect_error_time;

  SPIDER_CONN_HOLDER    *conn_holder_for_direct_join;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  SPIDER_IP_PORT_CONN *ip_port_conn;

  pthread_mutex_t    loop_check_mutex;
  HASH               loop_checked;
  uint               loop_checked_id;
  const char         *loop_checked_func_name;
  const char         *loop_checked_file_name;
  ulong              loop_checked_line_no;
  HASH               loop_check_queue;
  uint               loop_check_queue_id;
  const char         *loop_check_queue_func_name;
  const char         *loop_check_queue_file_name;
  ulong              loop_check_queue_line_no;
  SPIDER_CONN_LOOP_CHECK *loop_check_ignored_first;
  SPIDER_CONN_LOOP_CHECK *loop_check_ignored_last;
  SPIDER_CONN_LOOP_CHECK *loop_check_meraged_first;
  SPIDER_CONN_LOOP_CHECK *loop_check_meraged_last;
} SPIDER_CONN;

typedef struct st_spider_lgtm_tblhnd_share
{
  char               *table_name;
  uint               table_name_length;
  my_hash_value_type table_path_hash_value;
  pthread_mutex_t    auto_increment_mutex;
  volatile bool      auto_increment_init;
  volatile ulonglong auto_increment_lclval;
  ulonglong          auto_increment_value;
} SPIDER_LGTM_TBLHND_SHARE;

typedef struct st_spider_patition_handler
{
  bool               clone_bitmap_init;
  query_id_t         parallel_search_query_id;
  uint               no_parts;
  TABLE              *table;
  ha_spider          *owner;
  ha_spider          **handlers;
} SPIDER_PARTITION_HANDLER;

typedef struct st_spider_wide_share
{
  char               *table_name;
  uint               table_name_length;
  my_hash_value_type table_path_hash_value;
  uint               use_count;
  THR_LOCK           lock;
  pthread_mutex_t    sts_mutex;
  pthread_mutex_t    crd_mutex;

  volatile bool      sts_init;
  volatile bool      crd_init;
  volatile time_t    sts_get_time;
  volatile time_t    crd_get_time;
  ha_statistics      stat;

  longlong           *cardinality;
} SPIDER_WIDE_SHARE;

enum spider_hnd_stage {
  SPD_HND_STAGE_NONE,
  SPD_HND_STAGE_STORE_LOCK,
  SPD_HND_STAGE_EXTERNAL_LOCK,
  SPD_HND_STAGE_START_STMT,
  SPD_HND_STAGE_EXTRA,
  SPD_HND_STAGE_COND_PUSH,
  SPD_HND_STAGE_COND_POP,
  SPD_HND_STAGE_INFO_PUSH,
  SPD_HND_STAGE_SET_TOP_TABLE_AND_FIELDS,
  SPD_HND_STAGE_CLEAR_TOP_TABLE_FIELDS
};

typedef struct st_spider_wide_handler
{
  spider_hnd_stage   stage;
  handler            *stage_executor;
  THR_LOCK_DATA      lock;
  SPIDER_TRX         *trx;
  uchar              *searched_bitmap;
  uchar              *ft_discard_bitmap;
  uchar              *position_bitmap;
  uchar              *idx_read_bitmap;
  uchar              *idx_write_bitmap;
  uchar              *rnd_read_bitmap;
  uchar              *rnd_write_bitmap;
  SPIDER_CONDITION   *condition;
  void               *owner;
  SPIDER_PARTITION_HANDLER *partition_handler;
  List<Item>         *direct_update_fields;
  List<Item>         *direct_update_values;
  TABLE_SHARE        *top_share;
  enum thr_lock_type lock_type;
  uchar              lock_table_type;
  int                lock_mode;
  int                external_lock_type;
  int                cond_check_error;
  uint               sql_command;
  uint               top_table_fields;
#ifdef INFO_KIND_FORCE_LIMIT_BEGIN
  longlong           info_limit;
#endif
  bool               between_flg;
  bool               idx_bitmap_is_set;
  bool               rnd_bitmap_is_set;
  bool               position_bitmap_init;
  bool               semi_trx_isolation_chk;
  bool               semi_trx_chk;
  bool               low_priority;
  bool               high_priority;
  bool               insert_delayed;
  bool               consistent_snapshot;
  bool               quick_mode;
  bool               keyread;
  bool               update_request;
  bool               ignore_dup_key;
  bool               write_can_replace;
  bool               insert_with_update;
  bool               cond_check;
  bool               semi_table_lock;
} SPIDER_WIDE_HANDLER;

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

  bool               updated_in_this_trx;

  THD                *thd;
  my_hash_value_type thd_hash_value;
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
  uint               locked_connections;

  ulonglong          direct_update_count;
  ulonglong          direct_delete_count;
  ulonglong          direct_order_limit_count;
  ulonglong          direct_aggregate_count;
  ulonglong          parallel_search_count;


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
  TABLE_SHARE        *table_share;
  SPIDER_LGTM_TBLHND_SHARE *lgtm_tblhnd_share;
  my_hash_value_type table_name_hash_value;
  my_hash_value_type table_path_hash_value;

  volatile bool      init;
  volatile bool      init_error;
  volatile time_t    init_error_time;
  volatile bool      link_status_init;
  uchar              *table_mon_mutex_bitmap;
  volatile bool      sts_init;
  volatile time_t    sts_get_time;
  volatile time_t    bg_sts_try_time;
  volatile double    bg_sts_interval;
  volatile int       bg_sts_mode;
  volatile int       bg_sts_sync;
  volatile bool      bg_sts_init;
  volatile bool      bg_sts_kill;
  volatile bool      bg_sts_thd_wait;
  THD                *bg_sts_thd;
  pthread_t          bg_sts_thread;
  pthread_cond_t     bg_sts_cond;
  pthread_cond_t     bg_sts_sync_cond;
  volatile bool      crd_init;
  volatile time_t    crd_get_time;
  volatile time_t    bg_crd_try_time;
  volatile double    bg_crd_interval;
  volatile int       bg_crd_mode;
  volatile int       bg_crd_sync;
  volatile bool      bg_crd_init;
  volatile bool      bg_crd_kill;
  volatile bool      bg_crd_thd_wait;
  THD                *bg_crd_thd;
  pthread_t          bg_crd_thread;
  pthread_cond_t     bg_crd_cond;
  pthread_cond_t     bg_crd_sync_cond;
  volatile bool      bg_mon_init;
  volatile bool      bg_mon_kill;
  THD                **bg_mon_thds;
  pthread_t          *bg_mon_threads;
  pthread_mutex_t    *bg_mon_mutexes;
  pthread_cond_t     *bg_mon_conds;
  pthread_cond_t     *bg_mon_sleep_conds;
  /* static bg thread for sts and crd */
  TABLE                 table;
  ha_spider             *sts_spider;
  ha_spider             *crd_spider;
  SPIDER_THREAD         *sts_thread;
  SPIDER_THREAD         *crd_thread;
  volatile bool         sts_spider_init;
  volatile bool         sts_working;
  volatile bool         sts_wait;
  volatile bool         crd_spider_init;
  volatile bool         crd_working;
  volatile bool         crd_wait;
  volatile SPIDER_SHARE *sts_prev;
  volatile SPIDER_SHARE *sts_next;
  volatile SPIDER_SHARE *crd_prev;
  volatile SPIDER_SHARE *crd_next;

  MEM_ROOT           mem_root;

/*
  volatile bool      auto_increment_init;
  volatile ulonglong auto_increment_lclval;
*/
  ha_statistics      stat;

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

  int                sts_bg_mode;
  double             sts_interval;
  int                sts_mode;
  int                sts_sync;
  int                store_last_sts;
  int                load_sts_at_startup;
  int                crd_bg_mode;
  double             crd_interval;
  int                crd_mode;
  int                crd_sync;
  int                store_last_crd;
  int                load_crd_at_startup;
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
  int                buffer_size;
  int                internal_optimize;
  int                internal_optimize_local;
  double             scan_rate;
  double             read_rate;
  longlong           priority;
  int                quick_mode;
  longlong           quick_page_size;
  longlong           quick_page_byte;
  int                low_mem_read;
  int                table_count_mode;
  int                select_column_mode;
  int                bgs_mode;
  longlong           bgs_first_read;
  longlong           bgs_second_read;
  longlong           first_read;
  longlong           second_read;
  int                auto_increment_mode;
  int                use_table_charset;
  int                use_pushdown_udf;
  int                skip_default_condition;
  int                skip_parallel_search;
  int                direct_dup_insert;
  longlong           direct_order_limit;
  int                read_only_mode;
  int                error_read_mode;
  int                error_write_mode;
  int                active_link_count;
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

  my_hash_value_type *conn_keys_hash_value;
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
  char               **tgt_dsns;
  char               **tgt_filedsns;
  char               **tgt_drivers;
  char               **static_link_ids;
  char               **tgt_pk_names;
  char               **tgt_sequence_names;
  char               **conn_keys;
  long               *tgt_ports;
  long               *tgt_ssl_vscs;
  long               *link_statuses;
  long               *monitoring_bg_flag;
  long               *monitoring_bg_kind;
  long               *monitoring_binlog_pos_at_failing;
  long               *monitoring_flag;
  long               *monitoring_kind;
  longlong           *monitoring_bg_interval;
  longlong           *monitoring_limit;
  longlong           *monitoring_sid;
  long               *use_handlers;
  long               *connect_timeouts;
  long               *net_read_timeouts;
  long               *net_write_timeouts;
  long               *access_balances;
  long               *bka_table_name_types;
  long               *strict_group_bys;

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
  uint               *tgt_dsns_lengths;
  uint               *tgt_filedsns_lengths;
  uint               *tgt_drivers_lengths;
  uint               *static_link_ids_lengths;
  uint               *tgt_pk_names_lengths;
  uint               *tgt_sequence_names_lengths;
  uint               *conn_keys_lengths;
  uint               *sql_dbton_ids;

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
  uint               tgt_dsns_charlen;
  uint               tgt_filedsns_charlen;
  uint               tgt_drivers_charlen;
  uint               static_link_ids_charlen;
  uint               tgt_pk_names_charlen;
  uint               tgt_sequence_names_charlen;
  uint               conn_keys_charlen;

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
  uint               tgt_dsns_length;
  uint               tgt_filedsns_length;
  uint               tgt_drivers_length;
  uint               static_link_ids_length;
  uint               tgt_pk_names_length;
  uint               tgt_sequence_names_length;
  uint               conn_keys_length;
  uint               tgt_ports_length;
  uint               tgt_ssl_vscs_length;
  uint               link_statuses_length;
  uint               monitoring_bg_flag_length;
  uint               monitoring_bg_kind_length;
  uint               monitoring_binlog_pos_at_failing_length;
  uint               monitoring_flag_length;
  uint               monitoring_kind_length;
  uint               monitoring_bg_interval_length;
  uint               monitoring_limit_length;
  uint               monitoring_sid_length;
  uint               use_handlers_length;
  uint               connect_timeouts_length;
  uint               net_read_timeouts_length;
  uint               net_write_timeouts_length;
  uint               access_balances_length;
  uint               bka_table_name_types_length;
  uint               strict_group_bys_length;

  /* for dbton */
  uchar              dbton_bitmap[spider_bitmap_size(SPIDER_DBTON_SIZE)];
  spider_db_share    *dbton_share[SPIDER_DBTON_SIZE];
  uint               use_dbton_count;
  uint               use_dbton_ids[SPIDER_DBTON_SIZE];
  uint               dbton_id_to_seq[SPIDER_DBTON_SIZE];
  uint               use_sql_dbton_count;
  uint               use_sql_dbton_ids[SPIDER_DBTON_SIZE];
  uint               sql_dbton_id_to_seq[SPIDER_DBTON_SIZE];

  SPIDER_ALTER_TABLE alter_table;
  SPIDER_WIDE_SHARE  *wide_share;
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
  my_hash_value_type table_name_hash_value;
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

  /* for using real table */
  bool                 real_table_used;
  TABLE_LIST           *table_list_first;
  TABLE_LIST           *table_list;
  uchar                *real_table_bitmap;
  SPIDER_Open_tables_backup open_tables_backup;
  THD                  *open_tables_thd;

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
  int                  use_real_table;
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
  char                 *tgt_dsn;
  char                 *tgt_filedsn;
  char                 *tgt_driver;
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
  uint                 tgt_dsn_length;
  uint                 tgt_filedsn_length;
  uint                 tgt_driver_length;
  uint                 conn_key_length;
  uint                 dbton_id;
  my_hash_value_type   conn_key_hash_value;

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
  st_spider_table_mon_list   *parent;
  st_spider_table_mon        *next;
} SPIDER_TABLE_MON;

typedef struct st_spider_table_mon_list
{
  char                       *key;
  uint                       key_length;
  my_hash_value_type         key_hash_value;

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
  int                        bg_error_num;
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
  int                        bg_mode;

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


typedef struct st_spider_ip_port_conn {
  char               *key;
  size_t             key_len;
  my_hash_value_type key_hash_value;
  char               *remote_ip_str;
  long               remote_port;
  ulong              ip_port_count;
  volatile ulong     waiting_count;
  pthread_mutex_t    mutex;
  pthread_cond_t     cond;
  ulonglong          conn_id; /* each conn has it's own conn_id */
} SPIDER_IP_PORT_CONN;
