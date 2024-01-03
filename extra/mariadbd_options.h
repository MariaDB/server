#include <my_global.h>
static const char *mariadbd_valid_options[]= {
"allow_suspicious_udfs",
"alter_algorithm",
"analyze_sample_percentage",
"aria_block_size",
"aria_checkpoint_interval",
"aria_checkpoint_log_activity",
"aria_encrypt_tables",
"aria_force_start_after_recovery_failures",
"aria_group_commit",
"aria_group_commit_interval",
"aria_log_dir_path",
"aria_log_file_size",
"aria_log_purge_type",
"aria_max_sort_file_size",
"aria_page_checksum",
"aria_pagecache_age_threshold",
"aria_pagecache_buffer_size",
"aria_pagecache_division_limit",
"aria_pagecache_file_hash_size",
"aria_recover_options",
"aria_repair_threads",
"aria_sort_buffer_size",
"aria_stats_method",
"aria_sync_log_dir",
"auto_increment_increment",
"auto_increment_offset",
"autocommit",
"automatic_sp_privileges",
"back_log",
"big_tables",
"bind_address",
"binlog_alter_two_phase",
"binlog_annotate_row_events",
"binlog_cache_size",
"binlog_checksum",
"binlog_commit_wait_count",
"binlog_commit_wait_usec",
"binlog_direct_non_transactional_updates",
"binlog_do_db",
"binlog_expire_logs_seconds",
"binlog_file_cache_size",
"binlog_format",
"binlog_ignore_db",
"binlog_optimize_thread_scheduling",
"binlog_row_event_max_size",
"binlog_row_image",
"binlog_row_metadata",
"binlog_stmt_cache_size",
"block_encryption_mode",
"bootstrap",
"bulk_insert_buffer_size",
"character_set_client_handshake",
"character_set_collations",
"character_set_filesystem",
"character_sets_dir",
"collation_server",
"column_compression_threshold",
"column_compression_zlib_level",
"column_compression_zlib_strategy",
"column_compression_zlib_wrap",
"completion_type",
"concurrent_insert",
"connect_timeout",
"console",
"core_file",
"date_format",
"datetime_format",
"deadlock_search_depth_long",
"deadlock_search_depth_short",
"deadlock_timeout_long",
"deadlock_timeout_short",
"debug_abort_slave_event_count",
"debug_disconnect_slave_event_count",
"debug_gdb",
"debug_max_binlog_dump_events",
"debug_no_sync",
"debug_no_thread_alarm",
"debug_sporadic_binlog_dump_fail",
"default_password_lifetime",
"default_regex_flags",
"default_storage_engine",
"default_time_zone",
"default_tmp_storage_engine",
"default_week_format",
"delay_key_write",
"delayed_insert_limit",
"delayed_insert_timeout",
"delayed_queue_size",
"des_key_file",
"disconnect_on_expired_password",
"div_precision_increment",
"encrypt_binlog",
"encrypt_tmp_disk_tables",
"encrypt_tmp_files",
"enforce_storage_engine",
"eq_range_index_dive_limit",
"event_scheduler",
"expensive_subquery_limit",
"expire_logs_days",
"explicit_defaults_for_timestamp",
"external_locking",
"extra_max_connections",
"extra_port",
"flashback",
"flush",
"flush_time",
"ft_boolean_syntax",
"ft_max_word_len",
"ft_min_word_len",
"ft_query_expansion_limit",
"ft_stopword_file",
"gdb",
"general_log",
"general_log_file",
"getopt_prefix_matching",
"group_concat_max_len",
"gtid_cleanup_batch_size",
"gtid_domain_id",
"gtid_ignore_duplicates",
"gtid_pos_auto_engines",
"gtid_strict_mode",
"histogram_size",
"histogram_type",
"host_cache_size",
"idle_readonly_transaction_timeout",
"idle_transaction_timeout",
"idle_write_transaction_timeout",
"ignore_builtin_innodb",
"ignore_db_dirs",
"in_predicate_conversion_threshold",
"init_connect",
"init_file",
"init_rpl_role",
"init_slave",
"innodb",
"innodb_adaptive_flushing",
"innodb_adaptive_flushing_lwm",
"innodb_adaptive_hash_index",
"innodb_adaptive_hash_index_parts",
"innodb_autoextend_increment",
"innodb_autoinc_lock_mode",
"innodb_buf_dump_status_frequency",
"innodb_buffer_page",
"innodb_buffer_page_lru",
"innodb_buffer_pool_chunk_size",
"innodb_buffer_pool_dump_at_shutdown",
"innodb_buffer_pool_dump_now",
"innodb_buffer_pool_dump_pct",
"innodb_buffer_pool_filename",
"innodb_buffer_pool_load_abort",
"innodb_buffer_pool_load_at_startup",
"innodb_buffer_pool_load_now",
"innodb_buffer_pool_size",
"innodb_buffer_pool_stats",
"innodb_checksum_algorithm",
"innodb_cmp",
"innodb_cmp_per_index",
"innodb_cmp_per_index_enabled",
"innodb_cmp_per_index_reset",
"innodb_cmp_reset",
"innodb_cmpmem",
"innodb_cmpmem_reset",
"innodb_compression_algorithm",
"innodb_compression_default",
"innodb_compression_failure_threshold_pct",
"innodb_compression_level",
"innodb_compression_pad_pct_max",
"innodb_data_file_buffering",
"innodb_data_file_path",
"innodb_data_file_write_through",
"innodb_data_home_dir",
"innodb_deadlock_detect",
"innodb_deadlock_report",
"innodb_default_encryption_key_id",
"innodb_default_row_format",
"innodb_disable_sort_file_cache",
"innodb_doublewrite",
"innodb_encrypt_log",
"innodb_encrypt_tables",
"innodb_encrypt_temporary_tables",
"innodb_encryption_rotate_key_age",
"innodb_encryption_rotation_iops",
"innodb_encryption_threads",
"innodb_fast_shutdown",
"innodb_fatal_semaphore_wait_threshold",
"innodb_file_per_table",
"innodb_fill_factor",
"innodb_flush_log_at_timeout",
"innodb_flush_log_at_trx_commit",
"innodb_flush_method",
"innodb_flush_neighbors",
"innodb_flush_sync",
"innodb_flushing_avg_loops",
"innodb_force_primary_key",
"innodb_force_recovery",
"innodb_ft_aux_table",
"innodb_ft_being_deleted",
"innodb_ft_cache_size",
"innodb_ft_config",
"innodb_ft_default_stopword",
"innodb_ft_deleted",
"innodb_ft_enable_diag_print",
"innodb_ft_enable_stopword",
"innodb_ft_index_cache",
"innodb_ft_index_table",
"innodb_ft_max_token_size",
"innodb_ft_min_token_size",
"innodb_ft_num_word_optimize",
"innodb_ft_result_cache_limit",
"innodb_ft_server_stopword_table",
"innodb_ft_sort_pll_degree",
"innodb_ft_total_cache_size",
"innodb_ft_user_stopword_table",
"innodb_immediate_scrub_data_uncompressed",
"innodb_instant_alter_column_allowed",
"innodb_io_capacity",
"innodb_io_capacity_max",
"innodb_lock_wait_timeout",
"innodb_lock_waits",
"innodb_locks",
"innodb_log_buffer_size",
"innodb_log_file_buffering",
"innodb_log_file_size",
"innodb_log_file_write_through",
"innodb_log_group_home_dir",
"innodb_lru_flush_size",
"innodb_lru_scan_depth",
"innodb_max_dirty_pages_pct",
"innodb_max_dirty_pages_pct_lwm",
"innodb_max_purge_lag",
"innodb_max_purge_lag_delay",
"innodb_max_purge_lag_wait",
"innodb_max_undo_log_size",
"innodb_metrics",
"innodb_monitor_disable",
"innodb_monitor_enable",
"innodb_monitor_reset",
"innodb_monitor_reset_all",
"innodb_old_blocks_pct",
"innodb_old_blocks_time",
"innodb_online_alter_log_max_size",
"innodb_open_files",
"innodb_optimize_fulltext_only",
"innodb_page_size",
"innodb_prefix_index_cluster_optimization",
"innodb_print_all_deadlocks",
"innodb_purge_batch_size",
"innodb_purge_rseg_truncate_frequency",
"innodb_purge_threads",
"innodb_random_read_ahead",
"innodb_read_ahead_threshold",
"innodb_read_io_threads",
"innodb_read_only",
"innodb_read_only_compressed",
"innodb_rollback_on_timeout",
"innodb_sort_buffer_size",
"innodb_spin_wait_delay",
"innodb_stats_auto_recalc",
"innodb_stats_include_delete_marked",
"innodb_stats_method",
"innodb_stats_modified_counter",
"innodb_stats_on_metadata",
"innodb_stats_persistent",
"innodb_stats_persistent_sample_pages",
"innodb_stats_traditional",
"innodb_stats_transient_sample_pages",
"innodb_status_file",
"innodb_status_output",
"innodb_status_output_locks",
"innodb_strict_mode",
"innodb_sync_spin_loops",
"innodb_sys_columns",
"innodb_sys_fields",
"innodb_sys_foreign",
"innodb_sys_foreign_cols",
"innodb_sys_indexes",
"innodb_sys_tables",
"innodb_sys_tablespaces",
"innodb_sys_tablestats",
"innodb_sys_virtual",
"innodb_table_locks",
"innodb_tablespaces_encryption",
"innodb_temp_data_file_path",
"innodb_tmpdir",
"innodb_trx",
"innodb_undo_directory",
"innodb_undo_log_truncate",
"innodb_undo_tablespaces",
"innodb_use_atomic_writes",
"innodb_use_native_aio",
"innodb_write_io_threads",
"interactive_timeout",
"join_buffer_size",
"join_buffer_space_limit",
"join_cache_level",
"keep_files_on_create",
"key_buffer_size",
"key_cache_age_threshold",
"key_cache_block_size",
"key_cache_division_limit",
"key_cache_file_hash_size",
"key_cache_segments",
"large_pages",
"lc_messages",
"lc_time_names",
"local_infile",
"lock_wait_timeout",
"log_basename",
"log_bin",
"log_bin_compress",
"log_bin_compress_min_len",
"log_bin_index",
"log_bin_trust_function_creators",
"log_ddl_recovery",
"log_disabled_statements",
"log_error",
"log_isam",
"log_output",
"log_queries_not_using_indexes",
"log_short_format",
"log_slave_updates",
"log_slow_admin_statements",
"log_slow_disabled_statements",
"log_slow_filter",
"log_slow_max_warnings",
"log_slow_min_examined_row_limit",
"log_slow_query",
"log_slow_query_file",
"log_slow_query_time",
"log_slow_rate_limit",
"log_slow_slave_statements",
"log_slow_verbosity",
"log_tc",
"log_tc_size",
"long_query_time",
"low_priority_updates",
"lower_case_table_names",
"master_info_file",
"master_retry_count",
"master_verify_checksum",
"max_allowed_packet",
"max_binlog_cache_size",
"max_binlog_size",
"max_binlog_stmt_cache_size",
"max_connect_errors",
"max_connections",
"max_delayed_threads",
"max_digest_length",
"max_error_count",
"max_heap_table_size",
"max_join_size",
"max_length_for_sort_data",
"max_password_errors",
"max_prepared_stmt_count",
"max_recursive_iterations",
"max_relay_log_size",
"max_rowid_filter_size",
"max_seeks_for_key",
"max_session_mem_used",
"max_sort_length",
"max_sp_recursion_depth",
"max_statement_time",
"max_tmp_tables",
"max_user_connections",
"max_write_lock_count",
"memlock",
"metadata_locks_cache_size",
"metadata_locks_hash_instances",
"min_examined_row_limit",
"mrr_buffer_size",
"myisam_block_size",
"myisam_data_pointer_size",
"myisam_max_sort_file_size",
"myisam_mmap_size",
"myisam_recover_options",
"myisam_repair_threads",
"myisam_sort_buffer_size",
"myisam_stats_method",
"myisam_use_mmap",
"mysql56_temporal_format",
"net_buffer_length",
"net_read_timeout",
"net_retry_count",
"net_write_timeout",
"note_verbosity",
"old",
"old_mode",
"old_passwords",
"old_style_user_limits",
"open_files_limit",
"optimizer_disk_read_cost",
"optimizer_disk_read_ratio",
"optimizer_extra_pruning_depth",
"optimizer_index_block_copy_cost",
"optimizer_key_compare_cost",
"optimizer_key_copy_cost",
"optimizer_key_lookup_cost",
"optimizer_key_next_find_cost",
"optimizer_max_sel_arg_weight",
"optimizer_max_sel_args",
"optimizer_prune_level",
"optimizer_row_copy_cost",
"optimizer_row_lookup_cost",
"optimizer_row_next_find_cost",
"optimizer_rowid_compare_cost",
"optimizer_rowid_copy_cost",
"optimizer_scan_setup_cost",
"optimizer_search_depth",
"optimizer_selectivity_sampling_limit",
"optimizer_switch",
"optimizer_trace",
"optimizer_trace_max_mem_size",
"optimizer_use_condition_selectivity",
"optimizer_where_cost",
"partition",
"performance_schema",
"performance_schema_accounts_size",
"performance_schema_consumer_events_stages_current",
"performance_schema_consumer_events_stages_history",
"performance_schema_consumer_events_stages_history_long",
"performance_schema_consumer_events_statements_current",
"performance_schema_consumer_events_statements_history",
"performance_schema_consumer_events_statements_history_long",
"performance_schema_consumer_events_transactions_current",
"performance_schema_consumer_events_transactions_history",
"performance_schema_consumer_events_transactions_history_long",
"performance_schema_consumer_events_waits_current",
"performance_schema_consumer_events_waits_history",
"performance_schema_consumer_events_waits_history_long",
"performance_schema_consumer_global_instrumentation",
"performance_schema_consumer_statements_digest",
"performance_schema_consumer_thread_instrumentation",
"performance_schema_digests_size",
"performance_schema_events_stages_history_long_size",
"performance_schema_events_stages_history_size",
"performance_schema_events_statements_history_long_size",
"performance_schema_events_statements_history_size",
"performance_schema_events_transactions_history_long_size",
"performance_schema_events_transactions_history_size",
"performance_schema_events_waits_history_long_size",
"performance_schema_events_waits_history_size",
"performance_schema_hosts_size",
"performance_schema_instrument",
"performance_schema_max_cond_classes",
"performance_schema_max_cond_instances",
"performance_schema_max_digest_length",
"performance_schema_max_file_classes",
"performance_schema_max_file_handles",
"performance_schema_max_file_instances",
"performance_schema_max_index_stat",
"performance_schema_max_memory_classes",
"performance_schema_max_metadata_locks",
"performance_schema_max_mutex_classes",
"performance_schema_max_mutex_instances",
"performance_schema_max_prepared_statements_instances",
"performance_schema_max_program_instances",
"performance_schema_max_rwlock_classes",
"performance_schema_max_rwlock_instances",
"performance_schema_max_socket_classes",
"performance_schema_max_socket_instances",
"performance_schema_max_sql_text_length",
"performance_schema_max_stage_classes",
"performance_schema_max_statement_classes",
"performance_schema_max_statement_stack",
"performance_schema_max_table_handles",
"performance_schema_max_table_instances",
"performance_schema_max_table_lock_stat",
"performance_schema_max_thread_classes",
"performance_schema_max_thread_instances",
"performance_schema_session_connect_attrs_size",
"performance_schema_setup_actors_size",
"performance_schema_setup_objects_size",
"performance_schema_users_size",
"pid_file",
"plugin_dir",
"plugin_load",
"plugin_load_add",
"plugin_maturity",
"port_open_timeout",
"preload_buffer_size",
"profiling_history_size",
"progress_report_time",
"proxy_protocol_networks",
"query_alloc_block_size",
"query_cache_limit",
"query_cache_min_res_unit",
"query_cache_size",
"query_cache_strip_comments",
"query_cache_type",
"query_cache_wlock_invalidate",
"query_prealloc_size",
"range_alloc_block_size",
"read_binlog_speed_limit",
"read_buffer_size",
"read_only",
"read_rnd_buffer_size",
"relay_log",
"relay_log_index",
"relay_log_info_file",
"relay_log_purge",
"relay_log_recovery",
"relay_log_space_limit",
"replicate_annotate_row_events",
"replicate_do_db",
"replicate_do_table",
"replicate_events_marked_for_skip",
"replicate_ignore_db",
"replicate_ignore_table",
"replicate_rewrite_db",
"replicate_same_server_id",
"replicate_wild_do_table",
"replicate_wild_ignore_table",
"report_host",
"report_password",
"report_port",
"report_user",
"require_secure_transport",
"rowid_merge_buff_size",
"rpl_semi_sync_master_enabled",
"rpl_semi_sync_master_timeout",
"rpl_semi_sync_master_trace_level",
"rpl_semi_sync_master_wait_no_slave",
"rpl_semi_sync_master_wait_point",
"rpl_semi_sync_slave_delay_master",
"rpl_semi_sync_slave_enabled",
"rpl_semi_sync_slave_kill_conn_timeout",
"rpl_semi_sync_slave_trace_level",
"safe_mode",
"safe_user_create",
"secure_auth",
"secure_file_priv",
"secure_timestamp",
"sequence",
"server_id",
"session_track_schema",
"session_track_state_change",
"session_track_system_variables",
"session_track_transaction_info",
"show_slave_auth_info",
"silent_startup",
"skip_grant_tables",
"skip_host_cache",
"skip_name_resolve",
"skip_networking",
"skip_show_database",
"skip_slave_start",
"slave_compressed_protocol",
"slave_ddl_exec_mode",
"slave_domain_parallel_threads",
"slave_exec_mode",
"slave_load_tmpdir",
"slave_max_allowed_packet",
"slave_max_statement_time",
"slave_net_timeout",
"slave_parallel_max_queued",
"slave_parallel_mode",
"slave_parallel_threads",
"slave_parallel_workers",
"slave_run_triggers_for_rbr",
"slave_skip_errors",
"slave_sql_verify_checksum",
"slave_transaction_retries",
"slave_transaction_retry_errors",
"slave_transaction_retry_interval",
"slave_type_conversions",
"slow_launch_time",
"slow_query_log",
"slow_query_log_file",
"socket",
"sort_buffer_size",
"sql_mode",
"sql_safe_updates",
"ssl",
"ssl_ca",
"ssl_capath",
"ssl_cert",
"ssl_cipher",
"ssl_crl",
"ssl_crlpath",
"ssl_key",
"stack_trace",
"standard_compliant_cte",
"stored_program_cache",
"strict_password_validation",
"sync_binlog",
"sync_frm",
"sync_master_info",
"sync_relay_log",
"sync_relay_log_info",
"sysdate_is_now",
"system_versioning_alter_history",
"system_versioning_insert_history",
"table_cache",
"table_definition_cache",
"table_open_cache",
"table_open_cache_instances",
"tc_heuristic_recover",
"tcp_keepalive_interval",
"tcp_keepalive_probes",
"tcp_keepalive_time",
"tcp_nodelay",
"temp_pool",
"thread_cache_size",
"thread_handling",
"thread_pool_dedicated_listener",
"thread_pool_exact_stats",
"thread_pool_groups",
"thread_pool_idle_timeout",
"thread_pool_max_threads",
"thread_pool_oversubscribe",
"thread_pool_prio_kickup_timer",
"thread_pool_priority",
"thread_pool_queues",
"thread_pool_size",
"thread_pool_stall_limit",
"thread_pool_stats",
"thread_pool_waits",
"thread_stack",
"time_format",
"tls_version",
"tmp_disk_table_size",
"tmp_memory_table_size",
"tmp_table_size",
"transaction_alloc_block_size",
"transaction_isolation",
"transaction_prealloc_size",
"transaction_read_only",
"unix_socket",
"updatable_views_with_limit",
"use_stat_tables",
"user_variables",
"userstat",
"wait_timeout",
"wsrep_OSU_method",
"wsrep_SR_store",
"wsrep_allowlist",
"wsrep_auto_increment_control",
"wsrep_causal_reads",
"wsrep_certification_rules",
"wsrep_certify_nonPK",
"wsrep_cluster_address",
"wsrep_cluster_name",
"wsrep_convert_LOCK_to_trx",
"wsrep_data_home_dir",
"wsrep_dbug_option",
"wsrep_debug",
"wsrep_desync",
"wsrep_dirty_reads",
"wsrep_drupal_282555_workaround",
"wsrep_forced_binlog_format",
"wsrep_gtid_domain_id",
"wsrep_gtid_mode",
"wsrep_ignore_apply_errors",
"wsrep_load_data_splitting",
"wsrep_log_conflicts",
"wsrep_max_ws_rows",
"wsrep_max_ws_size",
"wsrep_mode",
"wsrep_mysql_replication_bundle",
"wsrep_new_cluster",
"wsrep_node_address",
"wsrep_node_incoming_address",
"wsrep_node_name",
"wsrep_notify_cmd",
"wsrep_on",
"wsrep_provider",
"wsrep_provider",
"wsrep_provider_options",
"wsrep_recover",
"wsrep_reject_queries",
"wsrep_restart_slave",
"wsrep_retry_autocommit",
"wsrep_slave_FK_checks",
"wsrep_slave_UK_checks",
"wsrep_slave_threads",
"wsrep_sst_auth",
"wsrep_sst_donor",
"wsrep_sst_donor_rejects_queries",
"wsrep_sst_method",
"wsrep_sst_receive_address",
"wsrep_start_position",
"wsrep_status_file",
"wsrep_sync_wait",
"wsrep_trx_fragment_size",
"wsrep_trx_fragment_unit",
};

static const char *valid_alter_algorithm_values[] = {
"DEFAULT",
"COPY",
"INPLACE",
"NOCOPY",
"INSTANT",
0
};
static TYPELIB valid_alter_algorithm_values_typelib = {
array_elements(valid_alter_algorithm_values)-1,
"", valid_alter_algorithm_values, 0};

static const char *valid_aria_log_purge_type_values[] = {
"immediate",
"external",
"at_flush",
0
};
static TYPELIB valid_aria_log_purge_type_values_typelib = {
array_elements(valid_aria_log_purge_type_values)-1,
"", valid_aria_log_purge_type_values, 0};

static const char *valid_aria_stats_method_values[] = {
"nulls_unequal",
"nulls_equal",
"nulls_ignored",
0
};
static TYPELIB valid_aria_stats_method_values_typelib = {
array_elements(valid_aria_stats_method_values)-1,
"", valid_aria_stats_method_values, 0};

static const char *valid_aria_sync_log_dir_values[] = {
"NEVER",
"NEWFILE",
"ALWAYS",
0
};
static TYPELIB valid_aria_sync_log_dir_values_typelib = {
array_elements(valid_aria_sync_log_dir_values)-1,
"", valid_aria_sync_log_dir_values, 0};

static const char *valid_binlog_checksum_values[] = {
"NONE",
"CRC32",
0
};
static TYPELIB valid_binlog_checksum_values_typelib = {
array_elements(valid_binlog_checksum_values)-1,
"", valid_binlog_checksum_values, 0};

static const char *valid_block_encryption_mode_values[] = {
"aes-128-ecb",
"aes-192-ecb",
"aes-256-ecb",
"aes-128-cbc",
"aes-192-cbc",
"aes-256-cbc",
"aes-128-ctr",
"aes-192-ctr",
"aes-256-ctr",
0
};
static TYPELIB valid_block_encryption_mode_values_typelib = {
array_elements(valid_block_encryption_mode_values)-1,
"", valid_block_encryption_mode_values, 0};

static const char *valid_completion_type_values[] = {
"NO_CHAIN",
"CHAIN",
"RELEASE",
0
};
static TYPELIB valid_completion_type_values_typelib = {
array_elements(valid_completion_type_values)-1,
"", valid_completion_type_values, 0};

static const char *valid_concurrent_insert_values[] = {
"NEVER",
"AUTO",
"ALWAYS",
0
};
static TYPELIB valid_concurrent_insert_values_typelib = {
array_elements(valid_concurrent_insert_values)-1,
"", valid_concurrent_insert_values, 0};

static const char *valid_init_rpl_role_values[] = {
"MASTER",
"SLAVE",
0
};
static TYPELIB valid_init_rpl_role_values_typelib = {
array_elements(valid_init_rpl_role_values)-1,
"", valid_init_rpl_role_values, 0};

static const char *valid_innodb_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_values_typelib = {
array_elements(valid_innodb_values)-1,
"", valid_innodb_values, 0};

static const char *valid_innodb_buffer_page_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_buffer_page_values_typelib = {
array_elements(valid_innodb_buffer_page_values)-1,
"", valid_innodb_buffer_page_values, 0};

static const char *valid_innodb_buffer_page_lru_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_buffer_page_lru_values_typelib = {
array_elements(valid_innodb_buffer_page_lru_values)-1,
"", valid_innodb_buffer_page_lru_values, 0};

static const char *valid_innodb_buffer_pool_stats_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_buffer_pool_stats_values_typelib = {
array_elements(valid_innodb_buffer_pool_stats_values)-1,
"", valid_innodb_buffer_pool_stats_values, 0};

static const char *valid_innodb_cmp_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_cmp_values_typelib = {
array_elements(valid_innodb_cmp_values)-1,
"", valid_innodb_cmp_values, 0};

static const char *valid_innodb_cmp_per_index_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_cmp_per_index_values_typelib = {
array_elements(valid_innodb_cmp_per_index_values)-1,
"", valid_innodb_cmp_per_index_values, 0};

static const char *valid_innodb_cmp_per_index_reset_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_cmp_per_index_reset_values_typelib = {
array_elements(valid_innodb_cmp_per_index_reset_values)-1,
"", valid_innodb_cmp_per_index_reset_values, 0};

static const char *valid_innodb_cmp_reset_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_cmp_reset_values_typelib = {
array_elements(valid_innodb_cmp_reset_values)-1,
"", valid_innodb_cmp_reset_values, 0};

static const char *valid_innodb_cmpmem_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_cmpmem_values_typelib = {
array_elements(valid_innodb_cmpmem_values)-1,
"", valid_innodb_cmpmem_values, 0};

static const char *valid_innodb_cmpmem_reset_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_cmpmem_reset_values_typelib = {
array_elements(valid_innodb_cmpmem_reset_values)-1,
"", valid_innodb_cmpmem_reset_values, 0};

static const char *valid_innodb_compression_algorithm_values[] = {
"none",
"zlib",
"lz4",
"lzo",
"lzma",
"bzip2",
"or",
"snappy",
0
};
static TYPELIB valid_innodb_compression_algorithm_values_typelib = {
array_elements(valid_innodb_compression_algorithm_values)-1,
"", valid_innodb_compression_algorithm_values, 0};

static const char *valid_innodb_deadlock_report_values[] = {
"off",
"basic",
"full",
0
};
static TYPELIB valid_innodb_deadlock_report_values_typelib = {
array_elements(valid_innodb_deadlock_report_values)-1,
"", valid_innodb_deadlock_report_values, 0};

static const char *valid_innodb_default_row_format_values[] = {
"redundant",
"compact",
"dynamic",
0
};
static TYPELIB valid_innodb_default_row_format_values_typelib = {
array_elements(valid_innodb_default_row_format_values)-1,
"", valid_innodb_default_row_format_values, 0};

static const char *valid_innodb_encrypt_tables_values[] = {
"OFF",
"ON",
"FORCE",
0
};
static TYPELIB valid_innodb_encrypt_tables_values_typelib = {
array_elements(valid_innodb_encrypt_tables_values)-1,
"", valid_innodb_encrypt_tables_values, 0};

static const char *valid_innodb_flush_method_values[] = {
"fsync",
"O_DSYNC",
"littlesync",
"nosync",
"O_DIRECT",
"O_DIRECT_NO_FSYNC",
0
};
static TYPELIB valid_innodb_flush_method_values_typelib = {
array_elements(valid_innodb_flush_method_values)-1,
"", valid_innodb_flush_method_values, 0};

static const char *valid_innodb_ft_being_deleted_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_ft_being_deleted_values_typelib = {
array_elements(valid_innodb_ft_being_deleted_values)-1,
"", valid_innodb_ft_being_deleted_values, 0};

static const char *valid_innodb_ft_config_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_ft_config_values_typelib = {
array_elements(valid_innodb_ft_config_values)-1,
"", valid_innodb_ft_config_values, 0};

static const char *valid_innodb_ft_default_stopword_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_ft_default_stopword_values_typelib = {
array_elements(valid_innodb_ft_default_stopword_values)-1,
"", valid_innodb_ft_default_stopword_values, 0};

static const char *valid_innodb_ft_deleted_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_ft_deleted_values_typelib = {
array_elements(valid_innodb_ft_deleted_values)-1,
"", valid_innodb_ft_deleted_values, 0};

static const char *valid_innodb_ft_index_cache_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_ft_index_cache_values_typelib = {
array_elements(valid_innodb_ft_index_cache_values)-1,
"", valid_innodb_ft_index_cache_values, 0};

static const char *valid_innodb_ft_index_table_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_ft_index_table_values_typelib = {
array_elements(valid_innodb_ft_index_table_values)-1,
"", valid_innodb_ft_index_table_values, 0};

static const char *valid_innodb_instant_alter_column_allowed_values[] = {
"never",
"add_last",
"add_drop_reorder",
0
};
static TYPELIB valid_innodb_instant_alter_column_allowed_values_typelib = {
array_elements(valid_innodb_instant_alter_column_allowed_values)-1,
"", valid_innodb_instant_alter_column_allowed_values, 0};

static const char *valid_innodb_lock_waits_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_lock_waits_values_typelib = {
array_elements(valid_innodb_lock_waits_values)-1,
"", valid_innodb_lock_waits_values, 0};

static const char *valid_innodb_locks_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_locks_values_typelib = {
array_elements(valid_innodb_locks_values)-1,
"", valid_innodb_locks_values, 0};

static const char *valid_innodb_metrics_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_metrics_values_typelib = {
array_elements(valid_innodb_metrics_values)-1,
"", valid_innodb_metrics_values, 0};

static const char *valid_innodb_stats_method_values[] = {
"nulls_equal",
"nulls_unequal",
"nulls_ignored",
0
};
static TYPELIB valid_innodb_stats_method_values_typelib = {
array_elements(valid_innodb_stats_method_values)-1,
"", valid_innodb_stats_method_values, 0};

static const char *valid_innodb_sys_columns_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_columns_values_typelib = {
array_elements(valid_innodb_sys_columns_values)-1,
"", valid_innodb_sys_columns_values, 0};

static const char *valid_innodb_sys_fields_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_fields_values_typelib = {
array_elements(valid_innodb_sys_fields_values)-1,
"", valid_innodb_sys_fields_values, 0};

static const char *valid_innodb_sys_foreign_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_foreign_values_typelib = {
array_elements(valid_innodb_sys_foreign_values)-1,
"", valid_innodb_sys_foreign_values, 0};

static const char *valid_innodb_sys_foreign_cols_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_foreign_cols_values_typelib = {
array_elements(valid_innodb_sys_foreign_cols_values)-1,
"", valid_innodb_sys_foreign_cols_values, 0};

static const char *valid_innodb_sys_indexes_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_indexes_values_typelib = {
array_elements(valid_innodb_sys_indexes_values)-1,
"", valid_innodb_sys_indexes_values, 0};

static const char *valid_innodb_sys_tables_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_tables_values_typelib = {
array_elements(valid_innodb_sys_tables_values)-1,
"", valid_innodb_sys_tables_values, 0};

static const char *valid_innodb_sys_tablespaces_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_tablespaces_values_typelib = {
array_elements(valid_innodb_sys_tablespaces_values)-1,
"", valid_innodb_sys_tablespaces_values, 0};

static const char *valid_innodb_sys_tablestats_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_tablestats_values_typelib = {
array_elements(valid_innodb_sys_tablestats_values)-1,
"", valid_innodb_sys_tablestats_values, 0};

static const char *valid_innodb_sys_virtual_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_sys_virtual_values_typelib = {
array_elements(valid_innodb_sys_virtual_values)-1,
"", valid_innodb_sys_virtual_values, 0};

static const char *valid_innodb_tablespaces_encryption_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_tablespaces_encryption_values_typelib = {
array_elements(valid_innodb_tablespaces_encryption_values)-1,
"", valid_innodb_tablespaces_encryption_values, 0};

static const char *valid_innodb_trx_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_innodb_trx_values_typelib = {
array_elements(valid_innodb_trx_values)-1,
"", valid_innodb_trx_values, 0};

static const char *valid_partition_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_partition_values_typelib = {
array_elements(valid_partition_values)-1,
"", valid_partition_values, 0};

static const char *valid_plugin_maturity_values[] = {
"unknown",
"experimental",
"alpha",
"beta",
"gamma",
"stable",
0
};
static TYPELIB valid_plugin_maturity_values_typelib = {
array_elements(valid_plugin_maturity_values)-1,
"", valid_plugin_maturity_values, 0};

static const char *valid_rpl_semi_sync_master_wait_point_values[] = {
"AFTER_SYNC",
"AFTER_COMMIT",
0
};
static TYPELIB valid_rpl_semi_sync_master_wait_point_values_typelib = {
array_elements(valid_rpl_semi_sync_master_wait_point_values)-1,
"", valid_rpl_semi_sync_master_wait_point_values, 0};

static const char *valid_sequence_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_sequence_values_typelib = {
array_elements(valid_sequence_values)-1,
"", valid_sequence_values, 0};

static const char *valid_tc_heuristic_recover_values[] = {
"OFF",
"COMMIT",
"ROLLBACK",
0
};
static TYPELIB valid_tc_heuristic_recover_values_typelib = {
array_elements(valid_tc_heuristic_recover_values)-1,
"", valid_tc_heuristic_recover_values, 0};

static const char *valid_thread_handling_values[] = {
"one-thread-per-connection",
"no-threads",
"pool-of-threads",
0
};
static TYPELIB valid_thread_handling_values_typelib = {
array_elements(valid_thread_handling_values)-1,
"", valid_thread_handling_values, 0};

static const char *valid_thread_pool_groups_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_thread_pool_groups_values_typelib = {
array_elements(valid_thread_pool_groups_values)-1,
"", valid_thread_pool_groups_values, 0};

static const char *valid_thread_pool_queues_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_thread_pool_queues_values_typelib = {
array_elements(valid_thread_pool_queues_values)-1,
"", valid_thread_pool_queues_values, 0};

static const char *valid_thread_pool_stats_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_thread_pool_stats_values_typelib = {
array_elements(valid_thread_pool_stats_values)-1,
"", valid_thread_pool_stats_values, 0};

static const char *valid_thread_pool_waits_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_thread_pool_waits_values_typelib = {
array_elements(valid_thread_pool_waits_values)-1,
"", valid_thread_pool_waits_values, 0};

static const char *valid_transaction_isolation_values[] = {
"READ-UNCOMMITTED",
"READ-COMMITTED",
"REPEATABLE-READ",
"SERIALIZABLE",
0
};
static TYPELIB valid_transaction_isolation_values_typelib = {
array_elements(valid_transaction_isolation_values)-1,
"", valid_transaction_isolation_values, 0};

static const char *valid_unix_socket_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_unix_socket_values_typelib = {
array_elements(valid_unix_socket_values)-1,
"", valid_unix_socket_values, 0};

static const char *valid_use_stat_tables_values[] = {
"NEVER",
"COMPLEMENTARY",
"PREFERABLY",
"COMPLEMENTARY_FOR_QUERIES",
"PREFERABLY_FOR_QUERIES",
0
};
static TYPELIB valid_use_stat_tables_values_typelib = {
array_elements(valid_use_stat_tables_values)-1,
"", valid_use_stat_tables_values, 0};

static const char *valid_user_variables_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_user_variables_values_typelib = {
array_elements(valid_user_variables_values)-1,
"", valid_user_variables_values, 0};

static const char *valid_wsrep_OSU_method_values[] = {
"TOI",
"RSU",
0
};
static TYPELIB valid_wsrep_OSU_method_values_typelib = {
array_elements(valid_wsrep_OSU_method_values)-1,
"", valid_wsrep_OSU_method_values, 0};

static const char *valid_wsrep_SR_store_values[] = {
"none",
"table",
0
};
static TYPELIB valid_wsrep_SR_store_values_typelib = {
array_elements(valid_wsrep_SR_store_values)-1,
"", valid_wsrep_SR_store_values, 0};

static const char *valid_wsrep_debug_values[] = {
"NONE",
"SERVER",
"TRANSACTION",
"STREAMING",
"CLIENT",
0
};
static TYPELIB valid_wsrep_debug_values_typelib = {
array_elements(valid_wsrep_debug_values)-1,
"", valid_wsrep_debug_values, 0};

static const char *valid_wsrep_forced_binlog_format_values[] = {
"MIXED",
"STATEMENT",
"ROW",
"NONE",
0
};
static TYPELIB valid_wsrep_forced_binlog_format_values_typelib = {
array_elements(valid_wsrep_forced_binlog_format_values)-1,
"", valid_wsrep_forced_binlog_format_values, 0};

static const char *valid_wsrep_provider_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_wsrep_provider_values_typelib = {
array_elements(valid_wsrep_provider_values)-1,
"", valid_wsrep_provider_values, 0};

static const char *valid_wsrep_reject_queries_values[] = {
"NONE",
"ALL",
"ALL_KILL",
0
};
static TYPELIB valid_wsrep_reject_queries_values_typelib = {
array_elements(valid_wsrep_reject_queries_values)-1,
"", valid_wsrep_reject_queries_values, 0};

static const char *mariadbd_enum_options[] = {
"alter_algorithm",
"aria_log_purge_type",
"aria_stats_method",
"aria_sync_log_dir",
"binlog_checksum",
"block_encryption_mode",
"completion_type",
"concurrent_insert",
"init_rpl_role",
"innodb",
"innodb_buffer_page",
"innodb_buffer_page_lru",
"innodb_buffer_pool_stats",
"innodb_cmp",
"innodb_cmp_per_index",
"innodb_cmp_per_index_reset",
"innodb_cmp_reset",
"innodb_cmpmem",
"innodb_cmpmem_reset",
"innodb_compression_algorithm",
"innodb_deadlock_report",
"innodb_default_row_format",
"innodb_encrypt_tables",
"innodb_flush_method",
"innodb_ft_being_deleted",
"innodb_ft_config",
"innodb_ft_default_stopword",
"innodb_ft_deleted",
"innodb_ft_index_cache",
"innodb_ft_index_table",
"innodb_instant_alter_column_allowed",
"innodb_lock_waits",
"innodb_locks",
"innodb_metrics",
"innodb_stats_method",
"innodb_sys_columns",
"innodb_sys_fields",
"innodb_sys_foreign",
"innodb_sys_foreign_cols",
"innodb_sys_indexes",
"innodb_sys_tables",
"innodb_sys_tablespaces",
"innodb_sys_tablestats",
"innodb_sys_virtual",
"innodb_tablespaces_encryption",
"innodb_trx",
"partition",
"plugin_maturity",
"rpl_semi_sync_master_wait_point",
"sequence",
"tc_heuristic_recover",
"thread_handling",
"thread_pool_groups",
"thread_pool_queues",
"thread_pool_stats",
"thread_pool_waits",
"transaction_isolation",
"unix_socket",
"use_stat_tables",
"user_variables",
"wsrep_OSU_method",
"wsrep_SR_store",
"wsrep_debug",
"wsrep_forced_binlog_format",
"wsrep_provider",
"wsrep_reject_queries",
};

static TYPELIB *mariadbd_enum_typelibs[] = {
&valid_alter_algorithm_values_typelib,
&valid_aria_log_purge_type_values_typelib,
&valid_aria_stats_method_values_typelib,
&valid_aria_sync_log_dir_values_typelib,
&valid_binlog_checksum_values_typelib,
&valid_block_encryption_mode_values_typelib,
&valid_completion_type_values_typelib,
&valid_concurrent_insert_values_typelib,
&valid_init_rpl_role_values_typelib,
&valid_innodb_values_typelib,
&valid_innodb_buffer_page_values_typelib,
&valid_innodb_buffer_page_lru_values_typelib,
&valid_innodb_buffer_pool_stats_values_typelib,
&valid_innodb_cmp_values_typelib,
&valid_innodb_cmp_per_index_values_typelib,
&valid_innodb_cmp_per_index_reset_values_typelib,
&valid_innodb_cmp_reset_values_typelib,
&valid_innodb_cmpmem_values_typelib,
&valid_innodb_cmpmem_reset_values_typelib,
&valid_innodb_compression_algorithm_values_typelib,
&valid_innodb_deadlock_report_values_typelib,
&valid_innodb_default_row_format_values_typelib,
&valid_innodb_encrypt_tables_values_typelib,
&valid_innodb_flush_method_values_typelib,
&valid_innodb_ft_being_deleted_values_typelib,
&valid_innodb_ft_config_values_typelib,
&valid_innodb_ft_default_stopword_values_typelib,
&valid_innodb_ft_deleted_values_typelib,
&valid_innodb_ft_index_cache_values_typelib,
&valid_innodb_ft_index_table_values_typelib,
&valid_innodb_instant_alter_column_allowed_values_typelib,
&valid_innodb_lock_waits_values_typelib,
&valid_innodb_locks_values_typelib,
&valid_innodb_metrics_values_typelib,
&valid_innodb_stats_method_values_typelib,
&valid_innodb_sys_columns_values_typelib,
&valid_innodb_sys_fields_values_typelib,
&valid_innodb_sys_foreign_values_typelib,
&valid_innodb_sys_foreign_cols_values_typelib,
&valid_innodb_sys_indexes_values_typelib,
&valid_innodb_sys_tables_values_typelib,
&valid_innodb_sys_tablespaces_values_typelib,
&valid_innodb_sys_tablestats_values_typelib,
&valid_innodb_sys_virtual_values_typelib,
&valid_innodb_tablespaces_encryption_values_typelib,
&valid_innodb_trx_values_typelib,
&valid_partition_values_typelib,
&valid_plugin_maturity_values_typelib,
&valid_rpl_semi_sync_master_wait_point_values_typelib,
&valid_sequence_values_typelib,
&valid_tc_heuristic_recover_values_typelib,
&valid_thread_handling_values_typelib,
&valid_thread_pool_groups_values_typelib,
&valid_thread_pool_queues_values_typelib,
&valid_thread_pool_stats_values_typelib,
&valid_thread_pool_waits_values_typelib,
&valid_transaction_isolation_values_typelib,
&valid_unix_socket_values_typelib,
&valid_use_stat_tables_values_typelib,
&valid_user_variables_values_typelib,
&valid_wsrep_OSU_method_values_typelib,
&valid_wsrep_SR_store_values_typelib,
&valid_wsrep_debug_values_typelib,
&valid_wsrep_forced_binlog_format_values_typelib,
&valid_wsrep_provider_values_typelib,
&valid_wsrep_reject_queries_values_typelib,
};
