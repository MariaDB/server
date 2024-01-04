#ifndef _mariadbd_options_h
#define _mariadbd_options_h
#include <my_global.h>
static const char *mariadbd_valid_options[]= {
"allow_suspicious_udfs",
"alter_algorithm",
"analyze_sample_percentage",
"archive",
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
"audit_null",
"auth_0x0100",
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
"blackhole",
"block_encryption_mode",
"bootstrap",
"bulk_insert_buffer_size",
"character_set_client_handshake",
"character_set_collations",
"character_set_filesystem",
"character_sets_dir",
"cleartext_plugin_server",
"collation_server",
"column_compression_threshold",
"column_compression_zlib_level",
"column_compression_zlib_strategy",
"column_compression_zlib_wrap",
"completion_type",
"concurrent_insert",
"connect",
"connect_cond_push",
"connect_conv_size",
"connect_default_depth",
"connect_default_prec",
"connect_exact_info",
"connect_force_bson",
"connect_indx_map",
"connect_json_all_path",
"connect_json_grp_size",
"connect_json_null",
"connect_timeout",
"connect_type_conv",
"connect_use_tempfile",
"connect_work_size",
"connect_xtrace",
"console",
"core_file",
"cracklib_password_check",
"cracklib_password_check_dictionary",
"daemon_example",
"date_format",
"datetime_format",
"deadlock_search_depth_long",
"deadlock_search_depth_short",
"deadlock_timeout_long",
"deadlock_timeout_short",
"debug_abort_slave_event_count",
"debug_disconnect_slave_event_count",
"debug_gdb",
"debug_key_management",
"debug_key_management_version",
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
"disks",
"div_precision_increment",
"ed25519",
"encrypt_binlog",
"encrypt_tmp_disk_tables",
"encrypt_tmp_files",
"enforce_storage_engine",
"eq_range_index_dive_limit",
"event_scheduler",
"example_key_management",
"expensive_subquery_limit",
"expire_logs_days",
"explicit_defaults_for_timestamp",
"external_locking",
"extra_max_connections",
"extra_port",
"federated",
"federated_pushdown",
"file_key_management",
"file_key_management_encryption_algorithm",
"file_key_management_filekey",
"file_key_management_filename",
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
"gssapi",
"gssapi_keytab_path",
"gssapi_principal_name",
"gtid_cleanup_batch_size",
"gtid_domain_id",
"gtid_ignore_duplicates",
"gtid_pos_auto_engines",
"gtid_strict_mode",
"handlersocket",
"handlersocket_accept_balance",
"handlersocket_address",
"handlersocket_backlog",
"handlersocket_epoll",
"handlersocket_plain_secret",
"handlersocket_plain_secret_wr",
"handlersocket_port",
"handlersocket_port_wr",
"handlersocket_rcvbuf",
"handlersocket_readsize",
"handlersocket_sndbuf",
"handlersocket_threads",
"handlersocket_threads_wr",
"handlersocket_timeout",
"handlersocket_verbose",
"handlersocket_wrlock_timeout",
"hashicorp_key_management",
"hashicorp_key_management_cache_timeout",
"hashicorp_key_management_cache_version_timeout",
"hashicorp_key_management_caching_enabled",
"hashicorp_key_management_check_kv_version",
"hashicorp_key_management_max_retries",
"hashicorp_key_management_timeout",
"hashicorp_key_management_token",
"hashicorp_key_management_use_cache_on_timeout",
"hashicorp_key_management_vault_ca",
"hashicorp_key_management_vault_url",
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
"locales",
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
"metadata_lock_info",
"metadata_locks_cache_size",
"metadata_locks_hash_instances",
"min_examined_row_limit",
"mroonga",
"mroonga_action_on_fulltext_query_error",
"mroonga_boolean_mode_syntax_flags",
"mroonga_database_path_prefix",
"mroonga_default_parser",
"mroonga_default_tokenizer",
"mroonga_default_wrapper_engine",
"mroonga_dry_write",
"mroonga_enable_operations_recording",
"mroonga_enable_optimization",
"mroonga_libgroonga_embedded",
"mroonga_libgroonga_support_lz4",
"mroonga_libgroonga_support_zlib",
"mroonga_libgroonga_support_zstd",
"mroonga_lock_timeout",
"mroonga_log_file",
"mroonga_log_level",
"mroonga_match_escalation_threshold",
"mroonga_max_n_records_for_estimate",
"mroonga_query_log_file",
"mroonga_stats",
"mroonga_vector_column_delimiter",
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
"mysql_json",
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
"pam",
"pam_use_cleartext_plugin",
"pam_winbind_workaround",
"partition",
"password_reuse_check",
"password_reuse_check_interval",
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
"provider_bzip2",
"provider_lz4",
"provider_lzma",
"proxy_protocol_networks",
"qa_auth_interface",
"qa_auth_server",
"query_alloc_block_size",
"query_cache_info",
"query_cache_limit",
"query_cache_min_res_unit",
"query_cache_size",
"query_cache_strip_comments",
"query_cache_type",
"query_cache_wlock_invalidate",
"query_prealloc_size",
"query_response_time",
"query_response_time_audit",
"query_response_time_range_base",
"query_response_time_stats",
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
"rocksdb",
"rocksdb_access_hint_on_compaction_start",
"rocksdb_advise_random_on_open",
"rocksdb_allow_concurrent_memtable_write",
"rocksdb_allow_mmap_reads",
"rocksdb_allow_mmap_writes",
"rocksdb_allow_to_start_after_corruption",
"rocksdb_blind_delete_primary_key",
"rocksdb_block_cache_size",
"rocksdb_block_restart_interval",
"rocksdb_block_size",
"rocksdb_block_size_deviation",
"rocksdb_bulk_load",
"rocksdb_bulk_load_allow_sk",
"rocksdb_bulk_load_allow_unsorted",
"rocksdb_bulk_load_size",
"rocksdb_bytes_per_sync",
"rocksdb_cache_dump",
"rocksdb_cache_high_pri_pool_ratio",
"rocksdb_cache_index_and_filter_blocks",
"rocksdb_cache_index_and_filter_with_high_priority",
"rocksdb_cf_options",
"rocksdb_cfstats",
"rocksdb_checksums_pct",
"rocksdb_collect_sst_properties",
"rocksdb_commit_in_the_middle",
"rocksdb_commit_time_batch_for_recovery",
"rocksdb_compact_cf",
"rocksdb_compaction_readahead_size",
"rocksdb_compaction_sequential_deletes",
"rocksdb_compaction_sequential_deletes_count_sd",
"rocksdb_compaction_sequential_deletes_file_size",
"rocksdb_compaction_sequential_deletes_window",
"rocksdb_compaction_stats",
"rocksdb_create_checkpoint",
"rocksdb_create_if_missing",
"rocksdb_create_missing_column_families",
"rocksdb_datadir",
"rocksdb_db_write_buffer_size",
"rocksdb_dbstats",
"rocksdb_ddl",
"rocksdb_deadlock",
"rocksdb_deadlock_detect",
"rocksdb_deadlock_detect_depth",
"rocksdb_debug_manual_compaction_delay",
"rocksdb_debug_optimizer_n_rows",
"rocksdb_debug_optimizer_no_zero_cardinality",
"rocksdb_debug_ttl_ignore_pk",
"rocksdb_debug_ttl_read_filter_ts",
"rocksdb_debug_ttl_rec_ts",
"rocksdb_debug_ttl_snapshot_ts",
"rocksdb_default_cf_options",
"rocksdb_delayed_write_rate",
"rocksdb_delete_cf",
"rocksdb_delete_obsolete_files_period_micros",
"rocksdb_enable_2pc",
"rocksdb_enable_bulk_load_api",
"rocksdb_enable_insert_with_update_caching",
"rocksdb_enable_thread_tracking",
"rocksdb_enable_ttl",
"rocksdb_enable_ttl_read_filtering",
"rocksdb_enable_write_thread_adaptive_yield",
"rocksdb_error_if_exists",
"rocksdb_error_on_suboptimal_collation",
"rocksdb_flush_log_at_trx_commit",
"rocksdb_force_compute_memtable_stats",
"rocksdb_force_compute_memtable_stats_cachetime",
"rocksdb_force_flush_memtable_and_lzero_now",
"rocksdb_force_flush_memtable_now",
"rocksdb_force_index_records_in_range",
"rocksdb_git_hash",
"rocksdb_global_info",
"rocksdb_hash_index_allow_collision",
"rocksdb_ignore_datadic_errors",
"rocksdb_ignore_unknown_options",
"rocksdb_index_file_map",
"rocksdb_index_type",
"rocksdb_info_log_level",
"rocksdb_io_write_timeout",
"rocksdb_is_fd_close_on_exec",
"rocksdb_keep_log_file_num",
"rocksdb_large_prefix",
"rocksdb_lock_scanned_rows",
"rocksdb_lock_wait_timeout",
"rocksdb_locks",
"rocksdb_log_dir",
"rocksdb_log_file_time_to_roll",
"rocksdb_manifest_preallocation_size",
"rocksdb_manual_compaction_threads",
"rocksdb_manual_wal_flush",
"rocksdb_master_skip_tx_api",
"rocksdb_max_background_jobs",
"rocksdb_max_latest_deadlocks",
"rocksdb_max_log_file_size",
"rocksdb_max_manifest_file_size",
"rocksdb_max_manual_compactions",
"rocksdb_max_open_files",
"rocksdb_max_row_locks",
"rocksdb_max_subcompactions",
"rocksdb_max_total_wal_size",
"rocksdb_merge_buf_size",
"rocksdb_merge_combine_read_size",
"rocksdb_merge_tmp_file_removal_delay_ms",
"rocksdb_new_table_reader_for_compaction_inputs",
"rocksdb_no_block_cache",
"rocksdb_override_cf_options",
"rocksdb_paranoid_checks",
"rocksdb_pause_background_work",
"rocksdb_perf_context",
"rocksdb_perf_context_global",
"rocksdb_perf_context_level",
"rocksdb_persistent_cache_path",
"rocksdb_persistent_cache_size_mb",
"rocksdb_pin_l0_filter_and_index_blocks_in_cache",
"rocksdb_print_snapshot_conflict_queries",
"rocksdb_rate_limiter_bytes_per_sec",
"rocksdb_records_in_range",
"rocksdb_remove_mariabackup_checkpoint",
"rocksdb_reset_stats",
"rocksdb_rollback_on_timeout",
"rocksdb_seconds_between_stat_computes",
"rocksdb_signal_drop_index_thread",
"rocksdb_sim_cache_size",
"rocksdb_skip_bloom_filter_on_read",
"rocksdb_skip_fill_cache",
"rocksdb_skip_unique_check_tables",
"rocksdb_sst_mgr_rate_bytes_per_sec",
"rocksdb_sst_props",
"rocksdb_stats_dump_period_sec",
"rocksdb_stats_level",
"rocksdb_stats_recalc_rate",
"rocksdb_store_row_debug_checksums",
"rocksdb_strict_collation_check",
"rocksdb_strict_collation_exceptions",
"rocksdb_table_cache_numshardbits",
"rocksdb_table_stats_sampling_pct",
"rocksdb_tmpdir",
"rocksdb_trace_sst_api",
"rocksdb_trx",
"rocksdb_two_write_queues",
"rocksdb_unsafe_for_binlog",
"rocksdb_update_cf_options",
"rocksdb_use_adaptive_mutex",
"rocksdb_use_clock_cache",
"rocksdb_use_direct_io_for_flush_and_compaction",
"rocksdb_use_direct_reads",
"rocksdb_use_fsync",
"rocksdb_validate_tables",
"rocksdb_verify_row_debug_checksums",
"rocksdb_wal_bytes_per_sync",
"rocksdb_wal_dir",
"rocksdb_wal_recovery_mode",
"rocksdb_wal_size_limit_mb",
"rocksdb_wal_ttl_seconds",
"rocksdb_whole_key_filtering",
"rocksdb_write_batch_max_bytes",
"rocksdb_write_disable_wal",
"rocksdb_write_ignore_missing_column_families",
"rocksdb_write_policy",
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
"s3",
"s3_access_key",
"s3_block_size",
"s3_bucket",
"s3_debug",
"s3_host_name",
"s3_pagecache_age_threshold",
"s3_pagecache_buffer_size",
"s3_pagecache_division_limit",
"s3_pagecache_file_hash_size",
"s3_port",
"s3_protocol_version",
"s3_region",
"s3_replicate_alter_as_create_select",
"s3_secret_key",
"s3_slave_ignore_updates",
"s3_use_http",
"safe_mode",
"safe_user_create",
"secure_auth",
"secure_file_priv",
"secure_timestamp",
"sequence",
"server_audit",
"server_audit_events",
"server_audit_excl_users",
"server_audit_file_path",
"server_audit_file_rotate_now",
"server_audit_file_rotate_size",
"server_audit_file_rotations",
"server_audit_incl_users",
"server_audit_logging",
"server_audit_mode",
"server_audit_output_type",
"server_audit_query_log_limit",
"server_audit_syslog_facility",
"server_audit_syslog_ident",
"server_audit_syslog_info",
"server_audit_syslog_priority",
"server_id",
"session_track_schema",
"session_track_state_change",
"session_track_system_variables",
"session_track_transaction_info",
"show_slave_auth_info",
"silent_startup",
"simple_parser",
"simple_parser_simple_sysvar_one",
"simple_parser_simple_sysvar_two",
"simple_parser_simple_thdvar_one",
"simple_parser_simple_thdvar_two",
"simple_password_check",
"simple_password_check_digits",
"simple_password_check_letters_same_case",
"simple_password_check_minimal_length",
"simple_password_check_other_characters",
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
"sphinx",
"spider",
"spider_alloc_mem",
"spider_auto_increment_mode",
"spider_bgs_first_read",
"spider_bgs_mode",
"spider_bgs_second_read",
"spider_bka_engine",
"spider_bka_mode",
"spider_bka_table_name_type",
"spider_block_size",
"spider_buffer_size",
"spider_bulk_size",
"spider_bulk_update_mode",
"spider_bulk_update_size",
"spider_casual_read",
"spider_conn_recycle_mode",
"spider_conn_recycle_strict",
"spider_conn_wait_timeout",
"spider_connect_error_interval",
"spider_connect_mutex",
"spider_connect_retry_count",
"spider_connect_retry_interval",
"spider_connect_timeout",
"spider_crd_bg_mode",
"spider_crd_interval",
"spider_crd_mode",
"spider_crd_sync",
"spider_crd_type",
"spider_crd_weight",
"spider_delete_all_rows_type",
"spider_direct_aggregate",
"spider_direct_dup_insert",
"spider_direct_order_limit",
"spider_disable_group_by_handler",
"spider_dry_access",
"spider_error_read_mode",
"spider_error_write_mode",
"spider_first_read",
"spider_force_commit",
"spider_general_log",
"spider_index_hint_pushdown",
"spider_init_sql_alloc_size",
"spider_internal_limit",
"spider_internal_offset",
"spider_internal_optimize",
"spider_internal_optimize_local",
"spider_internal_sql_log_off",
"spider_internal_unlock",
"spider_internal_xa",
"spider_internal_xa_id_type",
"spider_internal_xa_snapshot",
"spider_load_crd_at_startup",
"spider_load_sts_at_startup",
"spider_local_lock_table",
"spider_lock_exchange",
"spider_log_result_error_with_sql",
"spider_log_result_errors",
"spider_low_mem_read",
"spider_max_connections",
"spider_max_order",
"spider_multi_split_read",
"spider_net_read_timeout",
"spider_net_write_timeout",
"spider_ping_interval_at_trx_start",
"spider_quick_mode",
"spider_quick_page_byte",
"spider_quick_page_size",
"spider_read_only_mode",
"spider_remote_access_charset",
"spider_remote_autocommit",
"spider_remote_default_database",
"spider_remote_sql_log_off",
"spider_remote_time_zone",
"spider_remote_trx_isolation",
"spider_remote_wait_timeout",
"spider_reset_sql_alloc",
"spider_same_server_link",
"spider_second_read",
"spider_select_column_mode",
"spider_selupd_lock_mode",
"spider_semi_split_read",
"spider_semi_split_read_limit",
"spider_semi_table_lock",
"spider_semi_table_lock_connection",
"spider_semi_trx",
"spider_semi_trx_isolation",
"spider_skip_default_condition",
"spider_skip_parallel_search",
"spider_slave_trx_isolation",
"spider_split_read",
"spider_store_last_crd",
"spider_store_last_sts",
"spider_strict_group_by",
"spider_sts_bg_mode",
"spider_sts_interval",
"spider_sts_mode",
"spider_sts_sync",
"spider_support_xa",
"spider_sync_autocommit",
"spider_sync_sql_mode",
"spider_sync_trx_isolation",
"spider_table_crd_thread_count",
"spider_table_init_error_interval",
"spider_table_sts_thread_count",
"spider_use_all_conns_snapshot",
"spider_use_cond_other_than_pk_for_update",
"spider_use_consistent_snapshot",
"spider_use_default_database",
"spider_use_flash_logs",
"spider_use_pushdown_udf",
"spider_use_snapshot_with_flush_tables",
"spider_use_table_charset",
"spider_wait_timeout",
"spider_wrapper_protocols",
"spider_xa_register_mode",
"sql_error_log",
"sql_error_log_filename",
"sql_error_log_rate",
"sql_error_log_rotate",
"sql_error_log_rotations",
"sql_error_log_size_limit",
"sql_error_log_warnings",
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
"sysconst_test",
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
"test_double",
"test_int8",
"test_plugin_server",
"test_sql_discovery",
"test_sql_discovery_statement",
"test_sql_discovery_write_frm",
"test_sql_service",
"test_sql_service_execute_sql_global",
"test_sql_service_execute_sql_local",
"test_sql_service_run_test",
"test_versioning",
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
"three_attempts",
"time_format",
"tls_version",
"tmp_disk_table_size",
"tmp_memory_table_size",
"tmp_table_size",
"transaction_alloc_block_size",
"transaction_isolation",
"transaction_prealloc_size",
"transaction_read_only",
"two_questions",
"type_mysql_timestamp",
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
"wsrep_membership",
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
"wsrep_status",
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

static const char *valid_archive_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_archive_values_typelib = {
array_elements(valid_archive_values)-1,
"", valid_archive_values, 0};

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

static const char *valid_audit_null_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_audit_null_values_typelib = {
array_elements(valid_audit_null_values)-1,
"", valid_audit_null_values, 0};

static const char *valid_auth_0x0100_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_auth_0x0100_values_typelib = {
array_elements(valid_auth_0x0100_values)-1,
"", valid_auth_0x0100_values, 0};

static const char *valid_binlog_checksum_values[] = {
"NONE",
"CRC32",
0
};
static TYPELIB valid_binlog_checksum_values_typelib = {
array_elements(valid_binlog_checksum_values)-1,
"", valid_binlog_checksum_values, 0};

static const char *valid_blackhole_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_blackhole_values_typelib = {
array_elements(valid_blackhole_values)-1,
"", valid_blackhole_values, 0};

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

static const char *valid_cleartext_plugin_server_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_cleartext_plugin_server_values_typelib = {
array_elements(valid_cleartext_plugin_server_values)-1,
"", valid_cleartext_plugin_server_values, 0};

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

static const char *valid_connect_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_connect_values_typelib = {
array_elements(valid_connect_values)-1,
"", valid_connect_values, 0};

static const char *valid_connect_type_conv_values[] = {
"NO",
"YES",
"FORCE",
"SKIP",
0
};
static TYPELIB valid_connect_type_conv_values_typelib = {
array_elements(valid_connect_type_conv_values)-1,
"", valid_connect_type_conv_values, 0};

static const char *valid_connect_use_tempfile_values[] = {
"NO",
"AUTO",
"YES",
"FORCE",
"TEST",
0
};
static TYPELIB valid_connect_use_tempfile_values_typelib = {
array_elements(valid_connect_use_tempfile_values)-1,
"", valid_connect_use_tempfile_values, 0};

static const char *valid_cracklib_password_check_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_cracklib_password_check_values_typelib = {
array_elements(valid_cracklib_password_check_values)-1,
"", valid_cracklib_password_check_values, 0};

static const char *valid_daemon_example_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_daemon_example_values_typelib = {
array_elements(valid_daemon_example_values)-1,
"", valid_daemon_example_values, 0};

static const char *valid_debug_key_management_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_debug_key_management_values_typelib = {
array_elements(valid_debug_key_management_values)-1,
"", valid_debug_key_management_values, 0};

static const char *valid_disks_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_disks_values_typelib = {
array_elements(valid_disks_values)-1,
"", valid_disks_values, 0};

static const char *valid_ed25519_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_ed25519_values_typelib = {
array_elements(valid_ed25519_values)-1,
"", valid_ed25519_values, 0};

static const char *valid_example_key_management_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_example_key_management_values_typelib = {
array_elements(valid_example_key_management_values)-1,
"", valid_example_key_management_values, 0};

static const char *valid_federated_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_federated_values_typelib = {
array_elements(valid_federated_values)-1,
"", valid_federated_values, 0};

static const char *valid_file_key_management_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_file_key_management_values_typelib = {
array_elements(valid_file_key_management_values)-1,
"", valid_file_key_management_values, 0};

static const char *valid_file_key_management_encryption_algorithm_values[] = {
"aes_cbc",
"aes_ctr",
0
};
static TYPELIB valid_file_key_management_encryption_algorithm_values_typelib = {
array_elements(valid_file_key_management_encryption_algorithm_values)-1,
"", valid_file_key_management_encryption_algorithm_values, 0};

static const char *valid_gssapi_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_gssapi_values_typelib = {
array_elements(valid_gssapi_values)-1,
"", valid_gssapi_values, 0};

static const char *valid_handlersocket_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_handlersocket_values_typelib = {
array_elements(valid_handlersocket_values)-1,
"", valid_handlersocket_values, 0};

static const char *valid_hashicorp_key_management_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_hashicorp_key_management_values_typelib = {
array_elements(valid_hashicorp_key_management_values)-1,
"", valid_hashicorp_key_management_values, 0};

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

static const char *valid_locales_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_locales_values_typelib = {
array_elements(valid_locales_values)-1,
"", valid_locales_values, 0};

static const char *valid_metadata_lock_info_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_metadata_lock_info_values_typelib = {
array_elements(valid_metadata_lock_info_values)-1,
"", valid_metadata_lock_info_values, 0};

static const char *valid_mroonga_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_mroonga_values_typelib = {
array_elements(valid_mroonga_values)-1,
"", valid_mroonga_values, 0};

static const char *valid_mroonga_action_on_fulltext_query_error_values[] = {
"ERROR",
"ERROR_AND_LOG",
"IGNORE",
"IGNORE_AND_LOG",
0
};
static TYPELIB valid_mroonga_action_on_fulltext_query_error_values_typelib = {
array_elements(valid_mroonga_action_on_fulltext_query_error_values)-1,
"", valid_mroonga_action_on_fulltext_query_error_values, 0};

static const char *valid_mroonga_log_level_values[] = {
"NONE",
"EMERG",
"ALERT",
"CRIT",
"ERROR",
"WARNING",
"NOTICE",
"INFO",
"DEBUG",
"DUMP",
0
};
static TYPELIB valid_mroonga_log_level_values_typelib = {
array_elements(valid_mroonga_log_level_values)-1,
"", valid_mroonga_log_level_values, 0};

static const char *valid_mroonga_stats_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_mroonga_stats_values_typelib = {
array_elements(valid_mroonga_stats_values)-1,
"", valid_mroonga_stats_values, 0};

static const char *valid_mysql_json_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_mysql_json_values_typelib = {
array_elements(valid_mysql_json_values)-1,
"", valid_mysql_json_values, 0};

static const char *valid_pam_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_pam_values_typelib = {
array_elements(valid_pam_values)-1,
"", valid_pam_values, 0};

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

static const char *valid_password_reuse_check_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_password_reuse_check_values_typelib = {
array_elements(valid_password_reuse_check_values)-1,
"", valid_password_reuse_check_values, 0};

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

static const char *valid_provider_bzip2_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_provider_bzip2_values_typelib = {
array_elements(valid_provider_bzip2_values)-1,
"", valid_provider_bzip2_values, 0};

static const char *valid_provider_lz4_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_provider_lz4_values_typelib = {
array_elements(valid_provider_lz4_values)-1,
"", valid_provider_lz4_values, 0};

static const char *valid_provider_lzma_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_provider_lzma_values_typelib = {
array_elements(valid_provider_lzma_values)-1,
"", valid_provider_lzma_values, 0};

static const char *valid_qa_auth_interface_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_qa_auth_interface_values_typelib = {
array_elements(valid_qa_auth_interface_values)-1,
"", valid_qa_auth_interface_values, 0};

static const char *valid_qa_auth_server_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_qa_auth_server_values_typelib = {
array_elements(valid_qa_auth_server_values)-1,
"", valid_qa_auth_server_values, 0};

static const char *valid_query_cache_info_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_query_cache_info_values_typelib = {
array_elements(valid_query_cache_info_values)-1,
"", valid_query_cache_info_values, 0};

static const char *valid_query_response_time_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_query_response_time_values_typelib = {
array_elements(valid_query_response_time_values)-1,
"", valid_query_response_time_values, 0};

static const char *valid_query_response_time_audit_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_query_response_time_audit_values_typelib = {
array_elements(valid_query_response_time_audit_values)-1,
"", valid_query_response_time_audit_values, 0};

static const char *valid_rocksdb_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_values_typelib = {
array_elements(valid_rocksdb_values)-1,
"", valid_rocksdb_values, 0};

static const char *valid_rocksdb_cf_options_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_cf_options_values_typelib = {
array_elements(valid_rocksdb_cf_options_values)-1,
"", valid_rocksdb_cf_options_values, 0};

static const char *valid_rocksdb_cfstats_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_cfstats_values_typelib = {
array_elements(valid_rocksdb_cfstats_values)-1,
"", valid_rocksdb_cfstats_values, 0};

static const char *valid_rocksdb_compaction_stats_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_compaction_stats_values_typelib = {
array_elements(valid_rocksdb_compaction_stats_values)-1,
"", valid_rocksdb_compaction_stats_values, 0};

static const char *valid_rocksdb_dbstats_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_dbstats_values_typelib = {
array_elements(valid_rocksdb_dbstats_values)-1,
"", valid_rocksdb_dbstats_values, 0};

static const char *valid_rocksdb_ddl_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_ddl_values_typelib = {
array_elements(valid_rocksdb_ddl_values)-1,
"", valid_rocksdb_ddl_values, 0};

static const char *valid_rocksdb_deadlock_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_deadlock_values_typelib = {
array_elements(valid_rocksdb_deadlock_values)-1,
"", valid_rocksdb_deadlock_values, 0};

static const char *valid_rocksdb_global_info_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_global_info_values_typelib = {
array_elements(valid_rocksdb_global_info_values)-1,
"", valid_rocksdb_global_info_values, 0};

static const char *valid_rocksdb_index_file_map_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_index_file_map_values_typelib = {
array_elements(valid_rocksdb_index_file_map_values)-1,
"", valid_rocksdb_index_file_map_values, 0};

static const char *valid_rocksdb_index_type_values[] = {
"kBinarySearch",
"kHashSearch",
0
};
static TYPELIB valid_rocksdb_index_type_values_typelib = {
array_elements(valid_rocksdb_index_type_values)-1,
"", valid_rocksdb_index_type_values, 0};

static const char *valid_rocksdb_locks_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_locks_values_typelib = {
array_elements(valid_rocksdb_locks_values)-1,
"", valid_rocksdb_locks_values, 0};

static const char *valid_rocksdb_perf_context_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_perf_context_values_typelib = {
array_elements(valid_rocksdb_perf_context_values)-1,
"", valid_rocksdb_perf_context_values, 0};

static const char *valid_rocksdb_perf_context_global_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_perf_context_global_values_typelib = {
array_elements(valid_rocksdb_perf_context_global_values)-1,
"", valid_rocksdb_perf_context_global_values, 0};

static const char *valid_rocksdb_sst_props_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_sst_props_values_typelib = {
array_elements(valid_rocksdb_sst_props_values)-1,
"", valid_rocksdb_sst_props_values, 0};

static const char *valid_rocksdb_trx_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_rocksdb_trx_values_typelib = {
array_elements(valid_rocksdb_trx_values)-1,
"", valid_rocksdb_trx_values, 0};

static const char *valid_rocksdb_write_policy_values[] = {
"write_committed",
"write_prepared",
"write_unprepared",
0
};
static TYPELIB valid_rocksdb_write_policy_values_typelib = {
array_elements(valid_rocksdb_write_policy_values)-1,
"", valid_rocksdb_write_policy_values, 0};

static const char *valid_rpl_semi_sync_master_wait_point_values[] = {
"AFTER_SYNC",
"AFTER_COMMIT",
0
};
static TYPELIB valid_rpl_semi_sync_master_wait_point_values_typelib = {
array_elements(valid_rpl_semi_sync_master_wait_point_values)-1,
"", valid_rpl_semi_sync_master_wait_point_values, 0};

static const char *valid_s3_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_s3_values_typelib = {
array_elements(valid_s3_values)-1,
"", valid_s3_values, 0};

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

static const char *valid_server_audit_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_server_audit_values_typelib = {
array_elements(valid_server_audit_values)-1,
"", valid_server_audit_values, 0};

static const char *valid_server_audit_syslog_priority_values[] = {
"LOG_EMERG",
"LOG_ALERT",
"LOG_CRIT",
"LOG_ERR",
"LOG_WARNING",
"LOG_NOTICE",
"LOG_INFO",
"LOG_DEBUG",
0
};
static TYPELIB valid_server_audit_syslog_priority_values_typelib = {
array_elements(valid_server_audit_syslog_priority_values)-1,
"", valid_server_audit_syslog_priority_values, 0};

static const char *valid_simple_parser_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_simple_parser_values_typelib = {
array_elements(valid_simple_parser_values)-1,
"", valid_simple_parser_values, 0};

static const char *valid_simple_password_check_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_simple_password_check_values_typelib = {
array_elements(valid_simple_password_check_values)-1,
"", valid_simple_password_check_values, 0};

static const char *valid_sphinx_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_sphinx_values_typelib = {
array_elements(valid_sphinx_values)-1,
"", valid_sphinx_values, 0};

static const char *valid_spider_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_spider_values_typelib = {
array_elements(valid_spider_values)-1,
"", valid_spider_values, 0};

static const char *valid_spider_alloc_mem_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_spider_alloc_mem_values_typelib = {
array_elements(valid_spider_alloc_mem_values)-1,
"", valid_spider_alloc_mem_values, 0};

static const char *valid_spider_wrapper_protocols_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_spider_wrapper_protocols_values_typelib = {
array_elements(valid_spider_wrapper_protocols_values)-1,
"", valid_spider_wrapper_protocols_values, 0};

static const char *valid_sql_error_log_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_sql_error_log_values_typelib = {
array_elements(valid_sql_error_log_values)-1,
"", valid_sql_error_log_values, 0};

static const char *valid_sysconst_test_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_sysconst_test_values_typelib = {
array_elements(valid_sysconst_test_values)-1,
"", valid_sysconst_test_values, 0};

static const char *valid_tc_heuristic_recover_values[] = {
"OFF",
"COMMIT",
"ROLLBACK",
0
};
static TYPELIB valid_tc_heuristic_recover_values_typelib = {
array_elements(valid_tc_heuristic_recover_values)-1,
"", valid_tc_heuristic_recover_values, 0};

static const char *valid_test_double_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_test_double_values_typelib = {
array_elements(valid_test_double_values)-1,
"", valid_test_double_values, 0};

static const char *valid_test_int8_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_test_int8_values_typelib = {
array_elements(valid_test_int8_values)-1,
"", valid_test_int8_values, 0};

static const char *valid_test_plugin_server_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_test_plugin_server_values_typelib = {
array_elements(valid_test_plugin_server_values)-1,
"", valid_test_plugin_server_values, 0};

static const char *valid_test_sql_discovery_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_test_sql_discovery_values_typelib = {
array_elements(valid_test_sql_discovery_values)-1,
"", valid_test_sql_discovery_values, 0};

static const char *valid_test_sql_service_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_test_sql_service_values_typelib = {
array_elements(valid_test_sql_service_values)-1,
"", valid_test_sql_service_values, 0};

static const char *valid_test_versioning_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_test_versioning_values_typelib = {
array_elements(valid_test_versioning_values)-1,
"", valid_test_versioning_values, 0};

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

static const char *valid_three_attempts_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_three_attempts_values_typelib = {
array_elements(valid_three_attempts_values)-1,
"", valid_three_attempts_values, 0};

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

static const char *valid_two_questions_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_two_questions_values_typelib = {
array_elements(valid_two_questions_values)-1,
"", valid_two_questions_values, 0};

static const char *valid_type_mysql_timestamp_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_type_mysql_timestamp_values_typelib = {
array_elements(valid_type_mysql_timestamp_values)-1,
"", valid_type_mysql_timestamp_values, 0};

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

static const char *valid_wsrep_membership_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_wsrep_membership_values_typelib = {
array_elements(valid_wsrep_membership_values)-1,
"", valid_wsrep_membership_values, 0};

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

static const char *valid_wsrep_status_values[] = {
"ON",
"OFF",
"FORCE",
"FORCE_PLUS_PERMANENT",
0
};
static TYPELIB valid_wsrep_status_values_typelib = {
array_elements(valid_wsrep_status_values)-1,
"", valid_wsrep_status_values, 0};

static const char *mariadbd_enum_options[] = {
"alter_algorithm",
"archive",
"aria_log_purge_type",
"aria_stats_method",
"aria_sync_log_dir",
"audit_null",
"auth_0x0100",
"binlog_checksum",
"blackhole",
"block_encryption_mode",
"cleartext_plugin_server",
"completion_type",
"concurrent_insert",
"connect",
"connect_type_conv",
"connect_use_tempfile",
"cracklib_password_check",
"daemon_example",
"debug_key_management",
"disks",
"ed25519",
"example_key_management",
"federated",
"file_key_management",
"file_key_management_encryption_algorithm",
"gssapi",
"handlersocket",
"hashicorp_key_management",
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
"locales",
"metadata_lock_info",
"mroonga",
"mroonga_action_on_fulltext_query_error",
"mroonga_log_level",
"mroonga_stats",
"mysql_json",
"pam",
"partition",
"password_reuse_check",
"plugin_maturity",
"provider_bzip2",
"provider_lz4",
"provider_lzma",
"qa_auth_interface",
"qa_auth_server",
"query_cache_info",
"query_response_time",
"query_response_time_audit",
"rocksdb",
"rocksdb_cf_options",
"rocksdb_cfstats",
"rocksdb_compaction_stats",
"rocksdb_dbstats",
"rocksdb_ddl",
"rocksdb_deadlock",
"rocksdb_global_info",
"rocksdb_index_file_map",
"rocksdb_index_type",
"rocksdb_locks",
"rocksdb_perf_context",
"rocksdb_perf_context_global",
"rocksdb_sst_props",
"rocksdb_trx",
"rocksdb_write_policy",
"rpl_semi_sync_master_wait_point",
"s3",
"sequence",
"server_audit",
"server_audit_syslog_priority",
"simple_parser",
"simple_password_check",
"sphinx",
"spider",
"spider_alloc_mem",
"spider_wrapper_protocols",
"sql_error_log",
"sysconst_test",
"tc_heuristic_recover",
"test_double",
"test_int8",
"test_plugin_server",
"test_sql_discovery",
"test_sql_service",
"test_versioning",
"thread_handling",
"thread_pool_groups",
"thread_pool_queues",
"thread_pool_stats",
"thread_pool_waits",
"three_attempts",
"transaction_isolation",
"two_questions",
"type_mysql_timestamp",
"unix_socket",
"use_stat_tables",
"user_variables",
"wsrep_OSU_method",
"wsrep_SR_store",
"wsrep_debug",
"wsrep_forced_binlog_format",
"wsrep_membership",
"wsrep_provider",
"wsrep_reject_queries",
"wsrep_status",
};

static TYPELIB *mariadbd_enum_typelibs[] = {
&valid_alter_algorithm_values_typelib,
&valid_archive_values_typelib,
&valid_aria_log_purge_type_values_typelib,
&valid_aria_stats_method_values_typelib,
&valid_aria_sync_log_dir_values_typelib,
&valid_audit_null_values_typelib,
&valid_auth_0x0100_values_typelib,
&valid_binlog_checksum_values_typelib,
&valid_blackhole_values_typelib,
&valid_block_encryption_mode_values_typelib,
&valid_cleartext_plugin_server_values_typelib,
&valid_completion_type_values_typelib,
&valid_concurrent_insert_values_typelib,
&valid_connect_values_typelib,
&valid_connect_type_conv_values_typelib,
&valid_connect_use_tempfile_values_typelib,
&valid_cracklib_password_check_values_typelib,
&valid_daemon_example_values_typelib,
&valid_debug_key_management_values_typelib,
&valid_disks_values_typelib,
&valid_ed25519_values_typelib,
&valid_example_key_management_values_typelib,
&valid_federated_values_typelib,
&valid_file_key_management_values_typelib,
&valid_file_key_management_encryption_algorithm_values_typelib,
&valid_gssapi_values_typelib,
&valid_handlersocket_values_typelib,
&valid_hashicorp_key_management_values_typelib,
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
&valid_locales_values_typelib,
&valid_metadata_lock_info_values_typelib,
&valid_mroonga_values_typelib,
&valid_mroonga_action_on_fulltext_query_error_values_typelib,
&valid_mroonga_log_level_values_typelib,
&valid_mroonga_stats_values_typelib,
&valid_mysql_json_values_typelib,
&valid_pam_values_typelib,
&valid_partition_values_typelib,
&valid_password_reuse_check_values_typelib,
&valid_plugin_maturity_values_typelib,
&valid_provider_bzip2_values_typelib,
&valid_provider_lz4_values_typelib,
&valid_provider_lzma_values_typelib,
&valid_qa_auth_interface_values_typelib,
&valid_qa_auth_server_values_typelib,
&valid_query_cache_info_values_typelib,
&valid_query_response_time_values_typelib,
&valid_query_response_time_audit_values_typelib,
&valid_rocksdb_values_typelib,
&valid_rocksdb_cf_options_values_typelib,
&valid_rocksdb_cfstats_values_typelib,
&valid_rocksdb_compaction_stats_values_typelib,
&valid_rocksdb_dbstats_values_typelib,
&valid_rocksdb_ddl_values_typelib,
&valid_rocksdb_deadlock_values_typelib,
&valid_rocksdb_global_info_values_typelib,
&valid_rocksdb_index_file_map_values_typelib,
&valid_rocksdb_index_type_values_typelib,
&valid_rocksdb_locks_values_typelib,
&valid_rocksdb_perf_context_values_typelib,
&valid_rocksdb_perf_context_global_values_typelib,
&valid_rocksdb_sst_props_values_typelib,
&valid_rocksdb_trx_values_typelib,
&valid_rocksdb_write_policy_values_typelib,
&valid_rpl_semi_sync_master_wait_point_values_typelib,
&valid_s3_values_typelib,
&valid_sequence_values_typelib,
&valid_server_audit_values_typelib,
&valid_server_audit_syslog_priority_values_typelib,
&valid_simple_parser_values_typelib,
&valid_simple_password_check_values_typelib,
&valid_sphinx_values_typelib,
&valid_spider_values_typelib,
&valid_spider_alloc_mem_values_typelib,
&valid_spider_wrapper_protocols_values_typelib,
&valid_sql_error_log_values_typelib,
&valid_sysconst_test_values_typelib,
&valid_tc_heuristic_recover_values_typelib,
&valid_test_double_values_typelib,
&valid_test_int8_values_typelib,
&valid_test_plugin_server_values_typelib,
&valid_test_sql_discovery_values_typelib,
&valid_test_sql_service_values_typelib,
&valid_test_versioning_values_typelib,
&valid_thread_handling_values_typelib,
&valid_thread_pool_groups_values_typelib,
&valid_thread_pool_queues_values_typelib,
&valid_thread_pool_stats_values_typelib,
&valid_thread_pool_waits_values_typelib,
&valid_three_attempts_values_typelib,
&valid_transaction_isolation_values_typelib,
&valid_two_questions_values_typelib,
&valid_type_mysql_timestamp_values_typelib,
&valid_unix_socket_values_typelib,
&valid_use_stat_tables_values_typelib,
&valid_user_variables_values_typelib,
&valid_wsrep_OSU_method_values_typelib,
&valid_wsrep_SR_store_values_typelib,
&valid_wsrep_debug_values_typelib,
&valid_wsrep_forced_binlog_format_values_typelib,
&valid_wsrep_membership_values_typelib,
&valid_wsrep_provider_values_typelib,
&valid_wsrep_reject_queries_values_typelib,
&valid_wsrep_status_values_typelib,
};

static const char *valid_aria_recover_options_values[] = {
"NORMAL",
"BACKUP",
"FORCE",
"QUICK",
"OFF",
0
};
static TYPELIB valid_aria_recover_options_values_typelib = {
array_elements(valid_aria_recover_options_values)-1,
"", valid_aria_recover_options_values, 0};

static const char *valid_connect_xtrace_values[] = {
"YES",
"MORE",
"INDEX",
"MEMORY",
"SUBALLOC",
"QUERY",
"STMT",
"HANDLER",
"BLOCK",
"MONGO",
0
};
static TYPELIB valid_connect_xtrace_values_typelib = {
array_elements(valid_connect_xtrace_values)-1,
"", valid_connect_xtrace_values, 0};

static const char *valid_default_regex_flags_values[] = {
"DOTALL",
"DUPNAMES",
"EXTENDED",
"EXTENDED_MORE",
"EXTRA",
"MULTILINE",
"UNGREEDY",
0
};
static TYPELIB valid_default_regex_flags_values_typelib = {
array_elements(valid_default_regex_flags_values)-1,
"", valid_default_regex_flags_values, 0};

static const char *valid_log_disabled_statements_values[] = {
"slave",
"sp",
0
};
static TYPELIB valid_log_disabled_statements_values_typelib = {
array_elements(valid_log_disabled_statements_values)-1,
"", valid_log_disabled_statements_values, 0};

static const char *valid_log_output_values[] = {
"NONE",
"FILE",
"TABLE",
0
};
static TYPELIB valid_log_output_values_typelib = {
array_elements(valid_log_output_values)-1,
"", valid_log_output_values, 0};

static const char *valid_log_slow_disabled_statements_values[] = {
"admin",
"call",
"slave",
"sp",
0
};
static TYPELIB valid_log_slow_disabled_statements_values_typelib = {
array_elements(valid_log_slow_disabled_statements_values)-1,
"", valid_log_slow_disabled_statements_values, 0};

static const char *valid_log_slow_filter_values[] = {
"admin",
"filesort",
"filesort_on_disk",
"filesort_priority_queue",
"full_join",
"full_scan",
"not_using_index",
"query_cache",
"query_cache_miss",
"tmp_table",
"tmp_table_on_disk",
0
};
static TYPELIB valid_log_slow_filter_values_typelib = {
array_elements(valid_log_slow_filter_values)-1,
"", valid_log_slow_filter_values, 0};

static const char *valid_log_slow_verbosity_values[] = {
"innodb",
"query_plan",
"explain",
"engine",
"warnings",
"full",
0
};
static TYPELIB valid_log_slow_verbosity_values_typelib = {
array_elements(valid_log_slow_verbosity_values)-1,
"", valid_log_slow_verbosity_values, 0};

static const char *valid_myisam_recover_options_values[] = {
"DEFAULT",
"BACKUP",
"FORCE",
"QUICK",
"BACKUP_ALL",
"OFF",
0
};
static TYPELIB valid_myisam_recover_options_values_typelib = {
array_elements(valid_myisam_recover_options_values)-1,
"", valid_myisam_recover_options_values, 0};

static const char *valid_note_verbosity_values[] = {
"basic",
"unusable_keys",
"explain",
0
};
static TYPELIB valid_note_verbosity_values_typelib = {
array_elements(valid_note_verbosity_values)-1,
"", valid_note_verbosity_values, 0};

static const char *valid_old_mode_values[] = {
"NO_DUP_KEY_WARNINGS_WITH_IGNORE",
"NO_PROGRESS_INFO",
"ZERO_DATE_TIME_CAST",
"UTF8_IS_UTF8MB3",
"IGNORE_INDEX_ONLY_FOR_JOIN",
"COMPAT_5_1_CHECKSUM",
"LOCK_ALTER_TABLE_COPY",
0
};
static TYPELIB valid_old_mode_values_typelib = {
array_elements(valid_old_mode_values)-1,
"", valid_old_mode_values, 0};

static const char *valid_slave_type_conversions_values[] = {
"ALL_LOSSY",
"ALL_NON_LOSSY",
0
};
static TYPELIB valid_slave_type_conversions_values_typelib = {
array_elements(valid_slave_type_conversions_values)-1,
"", valid_slave_type_conversions_values, 0};

static const char *valid_sql_mode_values[] = {
"REAL_AS_FLOAT",
"PIPES_AS_CONCAT",
"ANSI_QUOTES",
"IGNORE_SPACE",
"IGNORE_BAD_TABLE_OPTIONS",
"ONLY_FULL_GROUP_BY",
"NO_UNSIGNED_SUBTRACTION",
"NO_DIR_IN_CREATE",
"POSTGRESQL",
"ORACLE",
"MSSQL",
"DB2",
"MAXDB",
"NO_KEY_OPTIONS",
"NO_TABLE_OPTIONS",
"NO_FIELD_OPTIONS",
"MYSQL323",
"MYSQL40",
"ANSI",
"NO_AUTO_VALUE_ON_ZERO",
"NO_BACKSLASH_ESCAPES",
"STRICT_TRANS_TABLES",
"STRICT_ALL_TABLES",
"NO_ZERO_IN_DATE",
"NO_ZERO_DATE",
"ALLOW_INVALID_DATES",
"ERROR_FOR_DIVISION_BY_ZERO",
"TRADITIONAL",
"NO_AUTO_CREATE_USER",
"HIGH_NOT_PRECEDENCE",
"NO_ENGINE_SUBSTITUTION",
"PAD_CHAR_TO_FULL_LENGTH",
"EMPTY_STRING_IS_NULL",
"SIMULTANEOUS_ASSIGNMENT",
"TIME_ROUND_FRACTIONAL",
0
};
static TYPELIB valid_sql_mode_values_typelib = {
array_elements(valid_sql_mode_values)-1,
"", valid_sql_mode_values, 0};

static const char *valid_tls_version_values[] = {
"TLSv1",
"TLSv1",
"1",
"TLSv1",
"2",
"TLSv1",
"3",
0
};
static TYPELIB valid_tls_version_values_typelib = {
array_elements(valid_tls_version_values)-1,
"", valid_tls_version_values, 0};

static const char *valid_wsrep_mode_values[] = {
"STRICT_REPLICATION",
"BINLOG_ROW_FORMAT_ONLY",
"REQUIRED_PRIMARY_KEY",
"REPLICATE_MYISAM",
"REPLICATE_ARIA",
"DISALLOW_LOCAL_GTID",
"BF_ABORT_MARIABACKUP",
0
};
static TYPELIB valid_wsrep_mode_values_typelib = {
array_elements(valid_wsrep_mode_values)-1,
"", valid_wsrep_mode_values, 0};

static const char *mariadbd_set_options[] = {
"aria_recover_options",
"connect_xtrace",
"default_regex_flags",
"log_disabled_statements",
"log_output",
"log_slow_disabled_statements",
"log_slow_filter",
"log_slow_verbosity",
"myisam_recover_options",
"note_verbosity",
"old_mode",
"slave_type_conversions",
"sql_mode",
"tls_version",
"wsrep_mode",
};

static TYPELIB *mariadbd_set_typelibs[] = {
&valid_aria_recover_options_values_typelib,
&valid_connect_xtrace_values_typelib,
&valid_default_regex_flags_values_typelib,
&valid_log_disabled_statements_values_typelib,
&valid_log_output_values_typelib,
&valid_log_slow_disabled_statements_values_typelib,
&valid_log_slow_filter_values_typelib,
&valid_log_slow_verbosity_values_typelib,
&valid_myisam_recover_options_values_typelib,
&valid_note_verbosity_values_typelib,
&valid_old_mode_values_typelib,
&valid_slave_type_conversions_values_typelib,
&valid_sql_mode_values_typelib,
&valid_tls_version_values_typelib,
&valid_wsrep_mode_values_typelib,
};
#endif /* _mariadbd_options_h */
