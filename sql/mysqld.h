/* Copyright (c) 2006, 2016, Oracle and/or its affiliates.
   Copyright (c) 2010, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef MYSQLD_INCLUDED
#define MYSQLD_INCLUDED

#include "sql_basic_types.h"			/* query_id_t */
#include "sql_mode.h"                           /* Sql_mode_dependency */
#include "sql_plugin.h"
#include "sql_bitmap.h"                         /* Bitmap */
#include "my_decimal.h"                         /* my_decimal */
#include "mysql_com.h"                     /* SERVER_VERSION_LENGTH */
#include "my_counter.h"
#include "mysql/psi/mysql_file.h"          /* MYSQL_FILE */
#include "mysql/psi/mysql_socket.h"        /* MYSQL_SOCKET */
#include "sql_list.h"                      /* I_List */
#include "sql_cmd.h"
#include <my_rnd.h>
#include "my_pthread.h"
#include "my_rdtsc.h"

class THD;
class CONNECT;
struct handlerton;
class Time_zone;

struct scheduler_functions;

typedef struct st_mysql_show_var SHOW_VAR;

/* Bits from testflag */
#define TEST_PRINT_CACHED_TABLES 1U
#define TEST_NO_KEY_GROUP	 2U
#define TEST_MIT_THREAD		4U
#define TEST_BLOCKING		8U
#define TEST_KEEP_TMP_TABLES	16U
#define TEST_READCHECK		64U	/**< Force use of readcheck */
#define TEST_NO_EXTRA		128U
#define TEST_CORE_ON_SIGNAL	256U	/**< Give core if signal */
#define TEST_SIGINT		1024U	/**< Allow sigint on threads */
#define TEST_SYNCHRONIZATION    2048U   /**< get server to do sleep in
                                           some places */

/* Keep things compatible */
#define OPT_DEFAULT SHOW_OPT_DEFAULT
#define OPT_SESSION SHOW_OPT_SESSION
#define OPT_GLOBAL SHOW_OPT_GLOBAL

extern MYSQL_PLUGIN_IMPORT MY_TIMER_INFO sys_timer_info;

/*
  Values for --slave-parallel-mode
  Must match order in slave_parallel_mode_typelib in sys_vars.cc.
*/
enum enum_slave_parallel_mode {
  SLAVE_PARALLEL_NONE,
  SLAVE_PARALLEL_MINIMAL,
  SLAVE_PARALLEL_CONSERVATIVE,
  SLAVE_PARALLEL_OPTIMISTIC,
  SLAVE_PARALLEL_AGGRESSIVE
};

/* Function prototypes */
void kill_mysql(THD *thd);
void close_connection(THD *thd, uint sql_errno= 0);
void handle_connection_in_main_thread(CONNECT *thd);
void create_thread_to_handle_connection(CONNECT *connect);
void unlink_thd(THD *thd);
void refresh_status(THD *thd);
bool is_secure_file_path(char *path);
extern void init_net_server_extension(THD *thd);
extern void handle_accepted_socket(MYSQL_SOCKET new_sock, MYSQL_SOCKET sock);
extern void create_new_thread(CONNECT *connect);

extern void ssl_acceptor_stats_update(int sslaccept_ret);
extern int reinit_ssl();

extern "C" MYSQL_PLUGIN_IMPORT CHARSET_INFO *system_charset_info;
extern MYSQL_PLUGIN_IMPORT CHARSET_INFO *files_charset_info ;
extern MYSQL_PLUGIN_IMPORT CHARSET_INFO *national_charset_info;
extern MYSQL_PLUGIN_IMPORT CHARSET_INFO *table_alias_charset;

/**
  Character set of the buildin error messages loaded from errmsg.sys.
*/
extern CHARSET_INFO *error_message_charset_info;

extern CHARSET_INFO *character_set_filesystem;

void temp_pool_clear_bit(uint bit);
uint temp_pool_set_next();

extern bool opt_large_files;
extern bool opt_update_log, opt_bin_log, opt_error_log, opt_bin_log_compress; 
extern uint opt_bin_log_compress_min_len;
extern my_bool opt_log, opt_bootstrap;
extern my_bool opt_backup_history_log;
extern my_bool opt_backup_progress_log;
extern my_bool opt_support_flashback;
extern ulonglong log_output_options;
extern ulong log_backup_output_options;
extern bool opt_disable_networking, opt_skip_show_db;
extern bool opt_skip_name_resolve;
extern bool opt_ignore_builtin_innodb;
extern my_bool opt_character_set_client_handshake;
extern my_bool debug_assert_on_not_freed_memory;
extern MYSQL_PLUGIN_IMPORT bool volatile abort_loop;
extern my_bool opt_safe_user_create;
extern my_bool opt_safe_show_db, opt_local_infile, opt_myisam_use_mmap;
extern my_bool opt_slave_compressed_protocol, use_temp_pool;
extern ulong slave_exec_mode_options, slave_ddl_exec_mode_options;
extern ulong slave_retried_transactions;
extern ulong transactions_multi_engine;
extern ulong rpl_transactions_multi_engine;
extern ulong transactions_gtid_foreign_engine;
extern ulong slave_run_triggers_for_rbr;
extern ulonglong slave_type_conversions_options;
extern my_bool read_only, opt_readonly;
extern MYSQL_PLUGIN_IMPORT my_bool lower_case_file_system;
extern my_bool opt_enable_named_pipe, opt_sync_frm, opt_allow_suspicious_udfs;
extern my_bool opt_secure_auth;
extern my_bool opt_require_secure_transport;
extern const char *current_dbug_option;
extern char* opt_secure_file_priv;
extern char* opt_secure_backup_file_priv;
extern size_t opt_secure_backup_file_priv_len;
extern my_bool sp_automatic_privileges, opt_noacl;
extern ulong use_stat_tables;
extern my_bool opt_old_style_user_limits, trust_function_creators;
extern uint opt_crash_binlog_innodb;
extern const char *shared_memory_base_name;
extern MYSQL_PLUGIN_IMPORT char *mysqld_unix_port;
extern my_bool opt_enable_shared_memory;
extern ulong opt_replicate_events_marked_for_skip;
extern char *default_tz_name;
extern Time_zone *default_tz;
extern char *my_bind_addr_str;
extern char *default_storage_engine, *default_tmp_storage_engine;
extern char *enforced_storage_engine;
extern char *gtid_pos_auto_engines;
extern plugin_ref *opt_gtid_pos_auto_plugins;
extern bool opt_endinfo, using_udf_functions;
extern my_bool locked_in_memory;
extern bool opt_using_transactions;
extern ulong current_pid;
extern double expire_logs_days;
extern ulong binlog_expire_logs_seconds;
extern my_bool relay_log_recovery;
extern uint sync_binlog_period, sync_relaylog_period, 
            sync_relayloginfo_period, sync_masterinfo_period;
extern ulong opt_tc_log_size, tc_log_max_pages_used, tc_log_page_size;
extern ulong tc_log_page_waits;
extern my_bool relay_log_purge, opt_innodb_safe_binlog, opt_innodb;
extern my_bool relay_log_recovery;
extern uint select_errors,ha_open_options;
extern ulonglong test_flags;
extern uint protocol_version, dropping_tables;
extern MYSQL_PLUGIN_IMPORT uint mysqld_port;
extern ulong delay_key_write_options;
extern char *opt_logname, *opt_slow_logname, *opt_bin_logname, 
            *opt_relay_logname;
extern char *opt_binlog_index_name;
extern char *opt_backup_history_logname, *opt_backup_progress_logname,
            *opt_backup_settings_name;
extern const char *log_output_str;
extern const char *log_backup_output_str;

/* System Versioning begin */
enum vers_system_time_t
{
  SYSTEM_TIME_UNSPECIFIED = 0,
  SYSTEM_TIME_AS_OF,
  SYSTEM_TIME_FROM_TO,
  SYSTEM_TIME_BETWEEN,
  SYSTEM_TIME_BEFORE,  // used for DELETE HISTORY ... BEFORE
  SYSTEM_TIME_HISTORY, // used for DELETE HISTORY
  SYSTEM_TIME_ALL
};

struct vers_asof_timestamp_t
{
  ulong type;
  my_time_t unix_time;
  ulong second_part;
};

enum vers_alter_history_enum
{
  VERS_ALTER_HISTORY_ERROR= 0,
  VERS_ALTER_HISTORY_KEEP
};
/* System Versioning end */

extern char *mysql_home_ptr, *pidfile_name_ptr;
extern MYSQL_PLUGIN_IMPORT char glob_hostname[FN_REFLEN];
extern char mysql_home[FN_REFLEN];
extern char pidfile_name[FN_REFLEN], system_time_zone[30], *opt_init_file;
extern char default_logfile_name[FN_REFLEN];
extern char log_error_file[FN_REFLEN], *opt_tc_log_file, *opt_ddl_recovery_file;
extern const double log_10[309];
extern ulonglong keybuff_size;
extern ulonglong thd_startup_options;
extern my_thread_id global_thread_id;
extern ulong binlog_cache_use, binlog_cache_disk_use;
extern ulong binlog_stmt_cache_use, binlog_stmt_cache_disk_use;
extern ulong aborted_threads, aborted_connects, aborted_connects_preauth;
extern ulong delayed_insert_timeout;
extern ulong delayed_insert_limit, delayed_queue_size;
extern ulong delayed_insert_threads, delayed_insert_writes;
extern ulong delayed_rows_in_use,delayed_insert_errors;
extern Atomic_counter<uint32_t> slave_open_temp_tables;
extern ulonglong query_cache_size;
extern ulong query_cache_limit;
extern ulong query_cache_min_res_unit;
extern ulong slow_launch_threads, slow_launch_time;
extern MYSQL_PLUGIN_IMPORT ulong max_connections;
extern uint max_digest_length;
extern ulong max_connect_errors, connect_timeout;
extern uint max_password_errors;
extern my_bool slave_allow_batching;
extern my_bool allow_slave_start;
extern LEX_CSTRING reason_slave_blocked;
extern ulong slave_trans_retries;
extern ulong slave_trans_retry_interval;
extern uint  slave_net_timeout;
extern int max_user_connections;
extern ulong what_to_log,flush_time;
extern uint max_prepared_stmt_count, prepared_stmt_count;
extern MYSQL_PLUGIN_IMPORT ulong open_files_limit;
extern ulonglong binlog_cache_size, binlog_stmt_cache_size, binlog_file_cache_size;
extern ulonglong max_binlog_cache_size, max_binlog_stmt_cache_size;
extern ulong max_binlog_size;
extern ulong slave_max_allowed_packet;
extern ulonglong slave_max_statement_time;
extern double slave_max_statement_time_double;
extern ulong opt_binlog_rows_event_max_size;
extern ulong binlog_row_metadata;
extern ulong thread_cache_size;
extern ulong stored_program_cache_size;
extern ulong opt_slave_parallel_threads;
extern ulong opt_slave_domain_parallel_threads;
extern ulong opt_slave_parallel_max_queued;
extern ulong opt_slave_parallel_mode;
extern ulong opt_binlog_commit_wait_count;
extern ulong opt_binlog_commit_wait_usec;
extern my_bool opt_gtid_ignore_duplicates;
extern uint opt_gtid_cleanup_batch_size;
extern ulong back_log;
extern ulong executed_events;
extern char language[FN_REFLEN];
extern "C" MYSQL_PLUGIN_IMPORT ulong server_id;
extern ulong concurrency;
extern time_t server_start_time, flush_status_time;
extern char *opt_mysql_tmpdir, mysql_charsets_dir[];
extern size_t mysql_unpacked_real_data_home_len;
extern MYSQL_PLUGIN_IMPORT MY_TMPDIR mysql_tmpdir_list;
extern const char *first_keyword, *delayed_user;
extern MYSQL_PLUGIN_IMPORT const char  *my_localhost;
extern MYSQL_PLUGIN_IMPORT const char **errmesg;			/* Error messages */
extern const char *myisam_recover_options_str;
extern const LEX_CSTRING in_left_expr_name, in_additional_cond, in_having_cond;
extern const LEX_CSTRING NULL_clex_str;
extern const LEX_CSTRING error_clex_str;
extern SHOW_VAR status_vars[];
extern struct system_variables max_system_variables;
extern struct system_status_var global_status_var;
extern struct my_rnd_struct sql_rand;
extern const char *opt_date_time_formats[];
extern handlerton *partition_hton;
extern handlerton *myisam_hton;
extern handlerton *heap_hton;
extern const char *load_default_groups[];
extern struct my_option my_long_options[];
int handle_early_options();
extern int MYSQL_PLUGIN_IMPORT mysqld_server_started;
extern int mysqld_server_initialized;
extern "C" MYSQL_PLUGIN_IMPORT int orig_argc;
extern "C" MYSQL_PLUGIN_IMPORT char **orig_argv;
extern pthread_attr_t connection_attrib;
extern my_bool old_mode;
extern LEX_STRING opt_init_connect, opt_init_slave;
extern char err_shared_dir[];
extern ulong connection_errors_select;
extern ulong connection_errors_accept;
extern ulong connection_errors_tcpwrap;
extern ulong connection_errors_internal;
extern ulong connection_errors_max_connection;
extern ulong connection_errors_peer_addr;
extern ulong log_warnings;
extern my_bool encrypt_binlog;
extern my_bool encrypt_tmp_disk_tables, encrypt_tmp_files;
extern ulong encryption_algorithm;
extern const char *encryption_algorithm_names[];
extern long opt_secure_timestamp;
extern uint default_password_lifetime;
extern my_bool disconnect_on_expired_password;

enum secure_timestamp { SECTIME_NO, SECTIME_SUPER, SECTIME_REPL, SECTIME_YES };
bool is_set_timestamp_forbidden(THD *thd);

#ifdef HAVE_MMAP
extern PSI_mutex_key key_PAGE_lock, key_LOCK_sync, key_LOCK_active,
       key_LOCK_pool, key_LOCK_pending_checkpoint;
#endif /* HAVE_MMAP */

#ifdef HAVE_OPENSSL
extern PSI_mutex_key key_LOCK_des_key_file;
#endif

extern PSI_mutex_key key_BINLOG_LOCK_index, key_BINLOG_LOCK_xid_list,
  key_BINLOG_LOCK_binlog_background_thread,
  key_LOCK_binlog_end_pos,
  key_delayed_insert_mutex, key_hash_filo_lock, key_LOCK_active_mi,
  key_LOCK_crypt, key_LOCK_delayed_create,
  key_LOCK_delayed_insert, key_LOCK_delayed_status, key_LOCK_error_log,
  key_LOCK_gdl, key_LOCK_global_system_variables,
  key_LOCK_logger, key_LOCK_manager,
  key_LOCK_prepared_stmt_count,
  key_LOCK_rpl_status, key_LOCK_server_started,
  key_LOCK_status,
  key_LOCK_thd_data, key_LOCK_thd_kill,
  key_LOCK_user_conn, key_LOG_LOCK_log,
  key_master_info_data_lock, key_master_info_run_lock,
  key_master_info_sleep_lock, key_master_info_start_stop_lock,
  key_master_info_start_alter_lock,
  key_master_info_start_alter_list_lock,
  key_mutex_slave_reporting_capability_err_lock, key_relay_log_info_data_lock,
  key_relay_log_info_log_space_lock, key_relay_log_info_run_lock,
  key_rpl_group_info_sleep_lock,
  key_structure_guard_mutex, key_TABLE_SHARE_LOCK_ha_data,
  key_LOCK_start_thread,
  key_LOCK_error_messages,
  key_PARTITION_LOCK_auto_inc;
extern PSI_mutex_key key_RELAYLOG_LOCK_index;
extern PSI_mutex_key key_LOCK_relaylog_end_pos;
extern PSI_mutex_key key_LOCK_slave_state, key_LOCK_binlog_state,
  key_LOCK_rpl_thread, key_LOCK_rpl_thread_pool, key_LOCK_parallel_entry;

extern PSI_mutex_key key_TABLE_SHARE_LOCK_share, key_LOCK_stats,
  key_LOCK_global_user_client_stats, key_LOCK_global_table_stats,
  key_LOCK_global_index_stats, key_LOCK_wakeup_ready, key_LOCK_wait_commit,
  key_TABLE_SHARE_LOCK_rotation;
extern PSI_mutex_key key_LOCK_gtid_waiting;

extern PSI_rwlock_key key_rwlock_LOCK_grant, key_rwlock_LOCK_logger,
  key_rwlock_LOCK_sys_init_connect, key_rwlock_LOCK_sys_init_slave,
  key_rwlock_LOCK_system_variables_hash, key_rwlock_query_cache_query_lock,
  key_LOCK_SEQUENCE,
  key_rwlock_LOCK_vers_stats, key_rwlock_LOCK_stat_serial,
  key_rwlock_THD_list;

#ifdef HAVE_MMAP
extern PSI_cond_key key_PAGE_cond, key_COND_active, key_COND_pool;
#endif /* HAVE_MMAP */

extern PSI_cond_key key_BINLOG_COND_xid_list, key_BINLOG_update_cond,
  key_BINLOG_COND_binlog_background_thread,
  key_BINLOG_COND_binlog_background_thread_end,
  key_COND_cache_status_changed, key_COND_manager,
  key_COND_rpl_status, key_COND_server_started,
  key_delayed_insert_cond, key_delayed_insert_cond_client,
  key_item_func_sleep_cond, key_master_info_data_cond,
  key_master_info_start_cond, key_master_info_stop_cond,
  key_master_info_sleep_cond,
  key_relay_log_info_data_cond, key_relay_log_info_log_space_cond,
  key_relay_log_info_start_cond, key_relay_log_info_stop_cond,
  key_rpl_group_info_sleep_cond,
  key_TABLE_SHARE_cond, key_user_level_lock_cond,
  key_COND_start_thread;
extern PSI_cond_key key_RELAYLOG_COND_relay_log_updated,
  key_RELAYLOG_COND_bin_log_updated, key_COND_wakeup_ready,
  key_COND_wait_commit;
extern PSI_cond_key key_RELAYLOG_COND_queue_busy;
extern PSI_cond_key key_TC_LOG_MMAP_COND_queue_busy;
extern PSI_cond_key key_COND_rpl_thread, key_COND_rpl_thread_queue,
  key_COND_rpl_thread_stop, key_COND_rpl_thread_pool,
  key_COND_parallel_entry, key_COND_group_commit_orderer;
extern PSI_cond_key key_COND_wait_gtid, key_COND_gtid_ignore_duplicates;
extern PSI_cond_key key_TABLE_SHARE_COND_rotation;

extern PSI_thread_key key_thread_delayed_insert,
  key_thread_handle_manager, key_thread_kill_server, key_thread_main,
  key_thread_one_connection, key_thread_signal_hand,
  key_thread_slave_background, key_rpl_parallel_thread;

extern PSI_file_key key_file_binlog, key_file_binlog_cache,
       key_file_binlog_index, key_file_binlog_index_cache, key_file_casetest,
  key_file_dbopt, key_file_des_key_file, key_file_ERRMSG, key_select_to_file,
  key_file_fileparser, key_file_frm, key_file_global_ddl_log, key_file_load,
  key_file_loadfile, key_file_log_event_data, key_file_log_event_info,
  key_file_master_info, key_file_misc, key_file_partition_ddl_log,
  key_file_pid, key_file_relay_log_info, key_file_send_file, key_file_tclog,
  key_file_trg, key_file_trn, key_file_init, key_file_log_ddl;
extern PSI_file_key key_file_query_log, key_file_slow_log;
extern PSI_file_key key_file_relaylog, key_file_relaylog_index,
                    key_file_relaylog_cache, key_file_relaylog_index_cache;
extern PSI_socket_key key_socket_tcpip, key_socket_unix,
  key_socket_client_connection;
extern PSI_file_key key_file_binlog_state;

#ifdef HAVE_PSI_INTERFACE
void init_server_psi_keys();
#endif /* HAVE_PSI_INTERFACE */

extern PSI_memory_key key_memory_locked_table_list;
extern PSI_memory_key key_memory_locked_thread_list;
extern PSI_memory_key key_memory_thd_transactions;
extern PSI_memory_key key_memory_delegate;
extern PSI_memory_key key_memory_acl_mem;
extern PSI_memory_key key_memory_acl_memex;
extern PSI_memory_key key_memory_acl_cache;
extern PSI_memory_key key_memory_thd_main_mem_root;
extern PSI_memory_key key_memory_help;
extern PSI_memory_key key_memory_frm;
extern PSI_memory_key key_memory_table_share;
extern PSI_memory_key key_memory_gdl;
extern PSI_memory_key key_memory_table_triggers_list;
extern PSI_memory_key key_memory_prepared_statement_map;
extern PSI_memory_key key_memory_prepared_statement_main_mem_root;
extern PSI_memory_key key_memory_protocol_rset_root;
extern PSI_memory_key key_memory_warning_info_warn_root;
extern PSI_memory_key key_memory_sp_cache;
extern PSI_memory_key key_memory_sp_head_main_root;
extern PSI_memory_key key_memory_sp_head_execute_root;
extern PSI_memory_key key_memory_sp_head_call_root;
extern PSI_memory_key key_memory_table_mapping_root;
extern PSI_memory_key key_memory_quick_range_select_root;
extern PSI_memory_key key_memory_quick_index_merge_root;
extern PSI_memory_key key_memory_quick_ror_intersect_select_root;
extern PSI_memory_key key_memory_quick_ror_union_select_root;
extern PSI_memory_key key_memory_quick_group_min_max_select_root;
extern PSI_memory_key key_memory_test_quick_select_exec;
extern PSI_memory_key key_memory_prune_partitions_exec;
extern PSI_memory_key key_memory_binlog_recover_exec;
extern PSI_memory_key key_memory_blob_mem_storage;

extern PSI_memory_key key_memory_Sys_var_charptr_value;
extern PSI_memory_key key_memory_THD_db;
extern PSI_memory_key key_memory_user_var_entry;
extern PSI_memory_key key_memory_user_var_entry_value;
extern PSI_memory_key key_memory_Slave_job_group_group_relay_log_name;
extern PSI_memory_key key_memory_Relay_log_info_group_relay_log_name;
extern PSI_memory_key key_memory_binlog_cache_mngr;
extern PSI_memory_key key_memory_Row_data_memory_memory;
extern PSI_memory_key key_memory_errmsgs;
extern PSI_memory_key key_memory_Event_queue_element_for_exec_names;
extern PSI_memory_key key_memory_Event_scheduler_scheduler_param;
extern PSI_memory_key key_memory_Gis_read_stream_err_msg;
extern PSI_memory_key key_memory_Geometry_objects_data;
extern PSI_memory_key key_memory_host_cache_hostname;
extern PSI_memory_key key_memory_User_level_lock;
extern PSI_memory_key key_memory_Filesort_info_record_pointers;
extern PSI_memory_key key_memory_Sort_param_tmp_buffer;
extern PSI_memory_key key_memory_Filesort_info_merge;
extern PSI_memory_key key_memory_Filesort_buffer_sort_keys;
extern PSI_memory_key key_memory_handler_errmsgs;
extern PSI_memory_key key_memory_handlerton;
extern PSI_memory_key key_memory_XID;
extern PSI_memory_key key_memory_MYSQL_LOCK;
extern PSI_memory_key key_memory_MYSQL_LOG_name;
extern PSI_memory_key key_memory_TC_LOG_MMAP_pages;
extern PSI_memory_key key_memory_my_str_malloc;
extern PSI_memory_key key_memory_MYSQL_BIN_LOG_basename;
extern PSI_memory_key key_memory_MYSQL_BIN_LOG_index;
extern PSI_memory_key key_memory_MYSQL_RELAY_LOG_basename;
extern PSI_memory_key key_memory_MYSQL_RELAY_LOG_index;
extern PSI_memory_key key_memory_rpl_filter;
extern PSI_memory_key key_memory_Security_context;
extern PSI_memory_key key_memory_NET_buff;
extern PSI_memory_key key_memory_NET_compress_packet;
extern PSI_memory_key key_memory_my_bitmap_map;
extern PSI_memory_key key_memory_QUICK_RANGE_SELECT_mrr_buf_desc;
extern PSI_memory_key key_memory_TABLE_RULE_ENT;
extern PSI_memory_key key_memory_Mutex_cond_array_Mutex_cond;
extern PSI_memory_key key_memory_Owned_gtids_sidno_to_hash;
extern PSI_memory_key key_memory_Sid_map_Node;
extern PSI_memory_key key_memory_bison_stack;
extern PSI_memory_key key_memory_TABLE_sort_io_cache;
extern PSI_memory_key key_memory_DATE_TIME_FORMAT;
extern PSI_memory_key key_memory_DDL_LOG_MEMORY_ENTRY;
extern PSI_memory_key key_memory_ST_SCHEMA_TABLE;
extern PSI_memory_key key_memory_ignored_db;
extern PSI_memory_key key_memory_SLAVE_INFO;
extern PSI_memory_key key_memory_log_event_old;
extern PSI_memory_key key_memory_HASH_ROW_ENTRY;
extern PSI_memory_key key_memory_table_def_memory;
extern PSI_memory_key key_memory_MPVIO_EXT_auth_info;
extern PSI_memory_key key_memory_LOG_POS_COORD;
extern PSI_memory_key key_memory_XID_STATE;
extern PSI_memory_key key_memory_Rpl_info_file_buffer;
extern PSI_memory_key key_memory_Rpl_info_table;
extern PSI_memory_key key_memory_binlog_pos;
extern PSI_memory_key key_memory_db_worker_hash_entry;
extern PSI_memory_key key_memory_rpl_slave_command_buffer;
extern PSI_memory_key key_memory_binlog_ver_1_event;
extern PSI_memory_key key_memory_rpl_slave_check_temp_dir;
extern PSI_memory_key key_memory_TABLE;
extern PSI_memory_key key_memory_binlog_statement_buffer;
extern PSI_memory_key key_memory_user_conn;
extern PSI_memory_key key_memory_dboptions_hash;
extern PSI_memory_key key_memory_dbnames_cache;
extern PSI_memory_key key_memory_hash_index_key_buffer;
extern PSI_memory_key key_memory_THD_handler_tables_hash;
extern PSI_memory_key key_memory_JOIN_CACHE;
extern PSI_memory_key key_memory_READ_INFO;
extern PSI_memory_key key_memory_partition_syntax_buffer;
extern PSI_memory_key key_memory_global_system_variables;
extern PSI_memory_key key_memory_THD_variables;
extern PSI_memory_key key_memory_PROFILE;
extern PSI_memory_key key_memory_LOG_name;
extern PSI_memory_key key_memory_string_iterator;
extern PSI_memory_key key_memory_frm_extra_segment_buff;
extern PSI_memory_key key_memory_frm_form_pos;
extern PSI_memory_key key_memory_frm_string;
extern PSI_memory_key key_memory_Unique_sort_buffer;
extern PSI_memory_key key_memory_Unique_merge_buffer;
extern PSI_memory_key key_memory_shared_memory_name;
extern PSI_memory_key key_memory_opt_bin_logname;
extern PSI_memory_key key_memory_Query_cache;
extern PSI_memory_key key_memory_READ_RECORD_cache;
extern PSI_memory_key key_memory_Quick_ranges;
extern PSI_memory_key key_memory_File_query_log_name;
extern PSI_memory_key key_memory_Table_trigger_dispatcher;
extern PSI_memory_key key_memory_show_slave_status_io_gtid_set;
extern PSI_memory_key key_memory_write_set_extraction;
extern PSI_memory_key key_memory_thd_timer;
extern PSI_memory_key key_memory_THD_Session_tracker;
extern PSI_memory_key key_memory_THD_Session_sysvar_resource_manager;
extern PSI_memory_key key_memory_get_all_tables;
extern PSI_memory_key key_memory_fill_schema_schemata;
extern PSI_memory_key key_memory_native_functions;
extern PSI_memory_key key_memory_JSON;

/*
  MAINTAINER: Please keep this list in order, to limit merge collisions.
  Hint: grep PSI_stage_info | sort -u
*/
extern PSI_stage_info stage_apply_event;
extern PSI_stage_info stage_after_create;
extern PSI_stage_info stage_after_opening_tables;
extern PSI_stage_info stage_after_table_lock;
extern PSI_stage_info stage_allocating_local_table;
extern PSI_stage_info stage_alter_inplace_prepare;
extern PSI_stage_info stage_alter_inplace;
extern PSI_stage_info stage_alter_inplace_commit;
extern PSI_stage_info stage_after_apply_event;
extern PSI_stage_info stage_changing_master;
extern PSI_stage_info stage_checking_master_version;
extern PSI_stage_info stage_checking_permissions;
extern PSI_stage_info stage_checking_privileges_on_cached_query;
extern PSI_stage_info stage_checking_query_cache_for_query;
extern PSI_stage_info stage_cleaning_up;
extern PSI_stage_info stage_closing_tables;
extern PSI_stage_info stage_connecting_to_master;
extern PSI_stage_info stage_converting_heap_to_myisam;
extern PSI_stage_info stage_copying_to_group_table;
extern PSI_stage_info stage_copying_to_tmp_table;
extern PSI_stage_info stage_copy_to_tmp_table;
extern PSI_stage_info stage_creating_delayed_handler;
extern PSI_stage_info stage_creating_sort_index;
extern PSI_stage_info stage_creating_table;
extern PSI_stage_info stage_creating_tmp_table;
extern PSI_stage_info stage_deleting_from_main_table;
extern PSI_stage_info stage_deleting_from_reference_tables;
extern PSI_stage_info stage_discard_or_import_tablespace;
extern PSI_stage_info stage_end;
extern PSI_stage_info stage_enabling_keys;
extern PSI_stage_info stage_executing;
extern PSI_stage_info stage_execution_of_init_command;
extern PSI_stage_info stage_explaining;
extern PSI_stage_info stage_finding_key_cache;
extern PSI_stage_info stage_finished_reading_one_binlog_switching_to_next_binlog;
extern PSI_stage_info stage_flushing_relay_log_and_master_info_repository;
extern PSI_stage_info stage_flushing_relay_log_info_file;
extern PSI_stage_info stage_freeing_items;
extern PSI_stage_info stage_fulltext_initialization;
extern PSI_stage_info stage_got_handler_lock;
extern PSI_stage_info stage_got_old_table;
extern PSI_stage_info stage_init;
extern PSI_stage_info stage_init_update;
extern PSI_stage_info stage_insert;
extern PSI_stage_info stage_invalidating_query_cache_entries_table;
extern PSI_stage_info stage_invalidating_query_cache_entries_table_list;
extern PSI_stage_info stage_killing_slave;
extern PSI_stage_info stage_logging_slow_query;
extern PSI_stage_info stage_making_temp_file_append_before_load_data;
extern PSI_stage_info stage_making_temp_file_create_before_load_data;
extern PSI_stage_info stage_manage_keys;
extern PSI_stage_info stage_master_has_sent_all_binlog_to_slave;
extern PSI_stage_info stage_opening_tables;
extern PSI_stage_info stage_optimizing;
extern PSI_stage_info stage_preparing;
extern PSI_stage_info stage_purging_old_relay_logs;
extern PSI_stage_info stage_query_end;
extern PSI_stage_info stage_starting_cleanup;
extern PSI_stage_info stage_rollback;
extern PSI_stage_info stage_rollback_implicit;
extern PSI_stage_info stage_commit;
extern PSI_stage_info stage_commit_implicit;
extern PSI_stage_info stage_queueing_master_event_to_the_relay_log;
extern PSI_stage_info stage_reading_event_from_the_relay_log;
extern PSI_stage_info stage_recreating_table;
extern PSI_stage_info stage_registering_slave_on_master;
extern PSI_stage_info stage_removing_duplicates;
extern PSI_stage_info stage_removing_tmp_table;
extern PSI_stage_info stage_rename;
extern PSI_stage_info stage_rename_result_table;
extern PSI_stage_info stage_requesting_binlog_dump;
extern PSI_stage_info stage_reschedule;
extern PSI_stage_info stage_searching_rows_for_update;
extern PSI_stage_info stage_sending_binlog_event_to_slave;
extern PSI_stage_info stage_sending_cached_result_to_client;
extern PSI_stage_info stage_sending_data;
extern PSI_stage_info stage_setup;
extern PSI_stage_info stage_slave_has_read_all_relay_log;
extern PSI_stage_info stage_show_explain;
extern PSI_stage_info stage_sorting;
extern PSI_stage_info stage_sorting_for_group;
extern PSI_stage_info stage_sorting_for_order;
extern PSI_stage_info stage_sorting_result;
extern PSI_stage_info stage_sql_thd_waiting_until_delay;
extern PSI_stage_info stage_statistics;
extern PSI_stage_info stage_storing_result_in_query_cache;
extern PSI_stage_info stage_storing_row_into_queue;
extern PSI_stage_info stage_system_lock;
extern PSI_stage_info stage_unlocking_tables;
extern PSI_stage_info stage_table_lock;
extern PSI_stage_info stage_filling_schema_table;
extern PSI_stage_info stage_update;
extern PSI_stage_info stage_updating;
extern PSI_stage_info stage_updating_main_table;
extern PSI_stage_info stage_updating_reference_tables;
extern PSI_stage_info stage_upgrading_lock;
extern PSI_stage_info stage_user_lock;
extern PSI_stage_info stage_user_sleep;
extern PSI_stage_info stage_verifying_table;
extern PSI_stage_info stage_waiting_for_ddl;
extern PSI_stage_info stage_waiting_for_delay_list;
extern PSI_stage_info stage_waiting_for_flush;
extern PSI_stage_info stage_waiting_for_gtid_to_be_written_to_binary_log;
extern PSI_stage_info stage_waiting_for_handler_insert;
extern PSI_stage_info stage_waiting_for_handler_lock;
extern PSI_stage_info stage_waiting_for_handler_open;
extern PSI_stage_info stage_waiting_for_insert;
extern PSI_stage_info stage_waiting_for_master_to_send_event;
extern PSI_stage_info stage_waiting_for_master_update;
extern PSI_stage_info stage_waiting_for_relay_log_space;
extern PSI_stage_info stage_waiting_for_slave_mutex_on_exit;
extern PSI_stage_info stage_waiting_for_slave_thread_to_start;
extern PSI_stage_info stage_waiting_for_query_cache_lock;
extern PSI_stage_info stage_waiting_for_table_flush;
extern PSI_stage_info stage_waiting_for_the_next_event_in_relay_log;
extern PSI_stage_info stage_waiting_for_the_slave_thread_to_advance_position;
extern PSI_stage_info stage_waiting_to_finalize_termination;
extern PSI_stage_info stage_binlog_waiting_background_tasks;
extern PSI_stage_info stage_binlog_write;
extern PSI_stage_info stage_binlog_processing_checkpoint_notify;
extern PSI_stage_info stage_binlog_stopping_background_thread;
extern PSI_stage_info stage_waiting_for_work_from_sql_thread;
extern PSI_stage_info stage_waiting_for_prior_transaction_to_commit;
extern PSI_stage_info stage_waiting_for_prior_transaction_to_start_commit;
extern PSI_stage_info stage_waiting_for_room_in_worker_thread;
extern PSI_stage_info stage_waiting_for_workers_idle;
extern PSI_stage_info stage_waiting_for_ftwrl;
extern PSI_stage_info stage_waiting_for_ftwrl_threads_to_pause;
extern PSI_stage_info stage_waiting_for_rpl_thread_pool;
extern PSI_stage_info stage_master_gtid_wait_primary;
extern PSI_stage_info stage_master_gtid_wait;
extern PSI_stage_info stage_gtid_wait_other_connection;
extern PSI_stage_info stage_slave_background_process_request;
extern PSI_stage_info stage_slave_background_wait_request;
extern PSI_stage_info stage_waiting_for_deadlock_kill;
extern PSI_stage_info stage_starting;
#ifdef WITH_WSREP
// Aditional Galera thread states
extern PSI_stage_info stage_waiting_isolation;
extern PSI_stage_info stage_waiting_certification;
extern PSI_stage_info stage_waiting_ddl;
extern PSI_stage_info stage_waiting_flow;
#endif /* WITH_WSREP */

#ifdef HAVE_PSI_STATEMENT_INTERFACE
/**
  Statement instrumentation keys (sql).
  The last entry, at [SQLCOM_END], is for parsing errors.
*/
extern PSI_statement_info sql_statement_info[(uint) SQLCOM_END + 1];

/**
  Statement instrumentation keys (com).
  The last entry, at [COM_END], is for packet errors.
*/
extern PSI_statement_info com_statement_info[(uint) COM_END + 1];

/**
  Statement instrumentation key for replication.
*/
extern PSI_statement_info stmt_info_rpl;

void init_sql_statement_info();
void init_com_statement_info();
#endif /* HAVE_PSI_STATEMENT_INTERFACE */

#ifndef _WIN32
extern pthread_t signal_thread;
#endif

#ifdef HAVE_OPENSSL
extern struct st_VioSSLFd * ssl_acceptor_fd;
#endif /* HAVE_OPENSSL */

/*
  The following variables were under INNODB_COMPABILITY_HOOKS
 */
extern my_bool opt_large_pages;
extern uint opt_large_page_size;
extern MYSQL_PLUGIN_IMPORT char lc_messages_dir[FN_REFLEN];
extern char *lc_messages_dir_ptr, *log_error_file_ptr;
extern MYSQL_PLUGIN_IMPORT char reg_ext[FN_EXTLEN];
extern MYSQL_PLUGIN_IMPORT uint reg_ext_length;
extern MYSQL_PLUGIN_IMPORT uint lower_case_table_names;
extern MYSQL_PLUGIN_IMPORT bool mysqld_embedded;
extern ulong specialflag;
extern uint mysql_data_home_len;
extern uint mysql_real_data_home_len;
extern const char *mysql_real_data_home_ptr;
extern ulong thread_handling;
extern "C" MYSQL_PLUGIN_IMPORT char server_version[SERVER_VERSION_LENGTH];
extern char *server_version_ptr;
extern bool using_custom_server_version;
extern MYSQL_PLUGIN_IMPORT char mysql_real_data_home[];
extern char mysql_unpacked_real_data_home[];
extern MYSQL_PLUGIN_IMPORT struct system_variables global_system_variables;
extern char default_logfile_name[FN_REFLEN];
extern char *my_proxy_protocol_networks;

#define mysql_tmpdir (my_tmpdir(&mysql_tmpdir_list))

extern MYSQL_PLUGIN_IMPORT const key_map key_map_empty;
extern MYSQL_PLUGIN_IMPORT key_map key_map_full;          /* Should be threaded as const */

/*
  Server mutex locks and condition variables.
 */
extern mysql_mutex_t
       LOCK_item_func_sleep, LOCK_status,
       LOCK_error_log, LOCK_delayed_insert, LOCK_short_uuid_generator,
       LOCK_delayed_status, LOCK_delayed_create, LOCK_crypt, LOCK_timezone,
       LOCK_active_mi, LOCK_manager, LOCK_user_conn,
       LOCK_prepared_stmt_count, LOCK_error_messages,  LOCK_backup_log;
extern MYSQL_PLUGIN_IMPORT mysql_mutex_t LOCK_global_system_variables;
extern mysql_rwlock_t LOCK_all_status_vars;
extern mysql_mutex_t LOCK_start_thread;
#ifdef HAVE_OPENSSL
extern char* des_key_file;
extern mysql_mutex_t LOCK_des_key_file;
#endif
extern MYSQL_PLUGIN_IMPORT mysql_mutex_t LOCK_server_started;
extern MYSQL_PLUGIN_IMPORT mysql_cond_t COND_server_started;
extern mysql_rwlock_t LOCK_grant, LOCK_sys_init_connect, LOCK_sys_init_slave;
extern mysql_rwlock_t LOCK_ssl_refresh;
extern mysql_prlock_t LOCK_system_variables_hash;
extern mysql_cond_t COND_start_thread;
extern mysql_cond_t COND_manager;

extern my_bool opt_use_ssl;
extern char *opt_ssl_ca, *opt_ssl_capath, *opt_ssl_cert, *opt_ssl_cipher,
  *opt_ssl_key, *opt_ssl_crl, *opt_ssl_crlpath;
extern ulonglong tls_version;

#ifdef MYSQL_SERVER

/**
  only options that need special treatment in get_one_option() deserve
  to be listed below
*/
enum options_mysqld
{
  OPT_to_set_the_start_number=256,
  OPT_BINLOG_DO_DB,
  OPT_BINLOG_FORMAT,
  OPT_BINLOG_IGNORE_DB,
  OPT_BIN_LOG,
  OPT_BOOTSTRAP,
  OPT_EXPIRE_LOGS_DAYS,
  OPT_BINLOG_EXPIRE_LOGS_SECONDS,
  OPT_CONSOLE,
  OPT_DEBUG_SYNC_TIMEOUT,
  OPT_REMOVED_OPTION,
  OPT_IGNORE_DB_DIRECTORY,
  OPT_ISAM_LOG,
  OPT_KEY_BUFFER_SIZE,
  OPT_KEY_CACHE_AGE_THRESHOLD,
  OPT_KEY_CACHE_BLOCK_SIZE,
  OPT_KEY_CACHE_DIVISION_LIMIT,
  OPT_KEY_CACHE_PARTITIONS,
  OPT_KEY_CACHE_CHANGED_BLOCKS_HASH_SIZE,
  OPT_LOG_BASENAME,
  OPT_LOG_ERROR,
  OPT_LOWER_CASE_TABLE_NAMES,
  OPT_PLUGIN_LOAD,
  OPT_PLUGIN_LOAD_ADD,
  OPT_PFS_INSTRUMENT,
  OPT_REPLICATE_DO_DB,
  OPT_REPLICATE_DO_TABLE,
  OPT_REPLICATE_IGNORE_DB,
  OPT_REPLICATE_IGNORE_TABLE,
  OPT_REPLICATE_REWRITE_DB,
  OPT_REPLICATE_WILD_DO_TABLE,
  OPT_REPLICATE_WILD_IGNORE_TABLE,
  OPT_SAFE,
  OPT_SERVER_ID,
  OPT_SILENT,
  OPT_SKIP_HOST_CACHE,
  OPT_SLAVE_PARALLEL_MODE,
  OPT_SSL_CA,
  OPT_SSL_CAPATH,
  OPT_SSL_CERT,
  OPT_SSL_CIPHER,
  OPT_SSL_CRL,
  OPT_SSL_CRLPATH,
  OPT_SSL_KEY,
  OPT_THREAD_CONCURRENCY,
  OPT_WANT_CORE,
#ifdef WITH_WSREP
  OPT_WSREP_CAUSAL_READS,
  OPT_WSREP_SYNC_WAIT,
#endif /* WITH_WSREP */
  OPT_MYSQL_COMPATIBILITY,
  OPT_TLS_VERSION,
  OPT_MYSQL_TO_BE_IMPLEMENTED,
  OPT_which_is_always_the_last
};
#endif

/**
   Query type constants (usable as bitmap flags).
*/
enum enum_query_type
{
  /// Nothing specific, ordinary SQL query.
  QT_ORDINARY= 0,
  /// In utf8.
  QT_TO_SYSTEM_CHARSET= (1 << 0),
  /// Without character set introducers.
  QT_WITHOUT_INTRODUCERS= (1 << 1),
  /// view internal representation (like QT_ORDINARY except ORDER BY clause)
  QT_VIEW_INTERNAL= (1 << 2),
  /// If identifiers should not include database names, where unambiguous
  QT_ITEM_IDENT_SKIP_DB_NAMES= (1 << 3),
  /// If identifiers should not include table names, where unambiguous
  QT_ITEM_IDENT_SKIP_TABLE_NAMES= (1 << 4),
  /// If Item_cache_wrapper should not print <expr_cache>
  QT_ITEM_CACHE_WRAPPER_SKIP_DETAILS= (1 << 5),
  /// If Item_subselect should print as just "(subquery#1)"
  /// rather than display the subquery body
  QT_ITEM_SUBSELECT_ID_ONLY= (1 << 6),
  /// If NULLIF(a,b) should print itself as
  /// CASE WHEN a_for_comparison=b THEN NULL ELSE a_for_return_value END
  /// when "a" was replaced to two different items
  /// (e.g. by equal fields propagation in optimize_cond())
  /// or always as NULLIF(a, b).
  /// The default behaviour is to use CASE syntax when
  /// a_for_return_value is not the same as a_for_comparison.
  /// SHOW CREATE {VIEW|PROCEDURE|FUNCTION} and other cases where the
  /// original representation is required, should set this flag.
  QT_ITEM_ORIGINAL_FUNC_NULLIF= (1 << 7),
  /// good for parsing
  QT_PARSABLE= (1 << 8),

  /// This value means focus on readability, not on ability to parse back, etc.
  QT_EXPLAIN=           QT_TO_SYSTEM_CHARSET |
                        QT_ITEM_IDENT_SKIP_DB_NAMES |
                        QT_ITEM_CACHE_WRAPPER_SKIP_DETAILS |
                        QT_ITEM_SUBSELECT_ID_ONLY,

  QT_SHOW_SELECT_NUMBER= (1<<10),

  /// Do not print database name or table name in the identifiers (even if
  /// this means the printout will be ambigous). It is assumed that the caller
  ///  passing this flag knows what they are doing.
  QT_ITEM_IDENT_DISABLE_DB_TABLE_NAMES= (1 <<11),

  /// This is used for EXPLAIN EXTENDED extra warnings / Be more detailed
  /// Be more detailed than QT_EXPLAIN.
  /// Perhaps we should eventually include QT_ITEM_IDENT_SKIP_CURRENT_DATABASE
  /// here, as it would give better readable results
  QT_EXPLAIN_EXTENDED=  QT_TO_SYSTEM_CHARSET|
                        QT_SHOW_SELECT_NUMBER,

  // If an expression is constant, print the expression, not the value
  // it evaluates to. Should be used for error messages, so that they
  // don't reveal values.
  QT_NO_DATA_EXPANSION= (1 << 9),

  // The temporary tables used by the query might be freed by the time
  // this print() call is made.
  QT_DONT_ACCESS_TMP_TABLES= (1 << 12)
};


/* query_id */
extern Atomic_counter<query_id_t> global_query_id;

/* increment query_id and return it.  */
inline __attribute__((warn_unused_result)) query_id_t next_query_id()
{
  return global_query_id++;
}

inline query_id_t get_query_id()
{
  return global_query_id;
}

/* increment global_thread_id and return it.  */
extern __attribute__((warn_unused_result)) my_thread_id next_thread_id(void);

/*
  TODO: Replace this with an inline function.
 */
#ifndef EMBEDDED_LIBRARY
extern "C" void unireg_abort(int exit_code) __attribute__((noreturn));
#else
extern "C" void unireg_clear(int exit_code);
#define unireg_abort(exit_code) do { unireg_clear(exit_code); DBUG_RETURN(exit_code); } while(0)
#endif

inline void table_case_convert(char * name, uint length)
{
  if (lower_case_table_names)
    files_charset_info->casedn(name, length, name, length);
}

extern void set_server_version(char *buf, size_t size);

#define current_thd _current_thd()
void set_current_thd(THD *thd);

/*
  @todo remove, make it static in ha_maria.cc
  currently it's needed for sql_select.cc
*/
extern handlerton *maria_hton;

extern uint64 global_gtid_counter;
extern my_bool opt_gtid_strict_mode;
extern my_bool opt_userstat_running, debug_assert_if_crashed_table;
extern uint mysqld_extra_port;
extern ulong opt_progress_report_time;
extern ulong extra_max_connections;
extern ulonglong denied_connections;
extern ulong thread_created;
extern scheduler_functions *thread_scheduler, *extra_thread_scheduler;
extern char *opt_log_basename;
extern my_bool opt_master_verify_checksum;
extern my_bool opt_stack_trace, disable_log_notes;
extern my_bool opt_expect_abort;
extern my_bool opt_slave_sql_verify_checksum;
extern my_bool opt_mysql56_temporal_format, strict_password_validation;
extern ulong binlog_checksum_options;
extern bool max_user_connections_checking;
extern ulong opt_binlog_dbug_fsync_sleep;

extern uint volatile global_disable_checkpoint;
extern my_bool opt_help;

extern int mysqld_main(int argc, char **argv);

#ifdef _WIN32
extern HANDLE hEventShutdown;
extern void mysqld_win_initiate_shutdown();
extern void mysqld_win_set_startup_complete();
extern void mysqld_win_extend_service_timeout(DWORD sec);
extern void mysqld_set_service_status_callback(void (*)(DWORD, DWORD, DWORD));
extern void mysqld_win_set_service_name(const char *name);
#endif

#endif /* MYSQLD_INCLUDED */
