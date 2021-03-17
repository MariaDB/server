# Change history for the MySQL sys schema

## 1.5.1 (07/07/16)

### Improvements

* A `quote_identifier` function was added, which can be used to properly backtick identifier names
* The `Tls_version` column was added to the output from the `mysql.slave_master_info` table, from the `diagnostics` procedure (backported from 5.7 upstream change)

### Bug Fixes

* MySQL Bug #77853 / Oracle Bug #21512106 - The `format_path` function did not consider directory boundaries when comparing variables to paths - it now does. Also fixed to no longer translate backslashes within Windows paths to forward slash
* Oracle Bug #21663578 - Fixed an instability within the sysschema.v_schema_tables_with_full_table_scans test
* Oracle Bug #21970078 - The host_summary view could fail with a division by zero error
* MySQL Bug #78874 / Oracle Bug #22066096 - The `ps_setup_show_enabled` procedure showed all rows for the `performance_schema.setup_objects` table, rather than only those that are enabled
* MySQL Bug #80569 / Oracle Bug #22848110 - The `max_latency` column for the `host_summary_by_statement_latency` view incorrectly showed the SUM of latency
* MySQL Bug #80833 / Oracle Bug #22988461 - The `pages_hashed` and `pages_old` columns within the `innodb_buffer_stats_by_schema` and `innodb_buffer_stats_by_table` views were calculated incorrectly (**Contributed by Tsubasa Tanaka**)
* MySQL Bug #78823 / Oracle Bug #22011361 - The `create_synonym_db` procedure failed when using reserved words as the synonym name (this change also introduced the `quote_identifier` function mentioned above **Contriubuted by Paul Dubois**)
* MySQL Bug #81564 / Oracle Bug #23335880 - The `ps_setup_show_enabled` and `ps_setup_show_disabled` procedures were fixed to:
** Show `user@host` instead of `host@user` for accounts
** Fixed the column header for `disabled_users` within `ps_setup_show_disabled`
** Explicitly ordered all output for test stability
** Show disabled users for 5.7.6+
* Oracle Bug #21970806 - The `sysschema.fn_ps_thread_trx_info` test was unstable
* Oracle Bug #23621189 - The `ps_trace_statement_digest` procedure ran EXPLAIN incorrectly in certain cases (such as on a SHOW statement, no query being specified, or not having a full qualified table), the procedure now catches these issues and ignores them

## 1.5.0 (11/09/15)

### Improvements

* The `format_bytes` function now shows no decimal places when outputting a simple bytes value
* The `processlist`/`x$processlist` views where improved, changes include:
 * The `pid` and `program_name` of the connection are shown, if set within the `performance_schema.session_connect_attrs` table (**Contributed by Daniël van Eeden**)
 * Issue #50 - The current statement progress is reported via the new stage progress reporting within Performance Schema stages within 5.7 (such as ALTER TABLE progress reporting)
 * Issue #60 - A new `statement_latency` column was added to all versions, which reports the current statement latency with picosecond precision from the `performance_schema.events_statements_current` table, when enabled
 * Some transaction information was exposed, with the `trx_latency` (for the current or last transaction depending on `trx_state`), `trx_state` (ACTIVE, COMMITTED, ROLLED BACK), and `trx_autocommit` (YES/NO) columns
* A new `metrics` view has been added. On 5.7 this provides a union view of the performance_schema.global_status and information_schema.innodb_metrics tables, along with P_S memory and the current time, as a single metrics output. On 5.6 it provides a union view of the information_schema.global_status and information_schema.innodb_metrics tables, along with the current time. (**Contributed by Jesper Wisborg Krogh**)
* New `session`/`x$session` views have been added, which give the same output as the `processlist` view counterparts, but filtered to only show foreground connections (**Contributed by Morgan Tocker**)
* A new `session_ssl_status` view was added, which shows the SSL version, ciper and session resuse statistics for each connection (**Contributed by Daniël van Eeden**)
* A new `schema_auto_increment_columns` view was added, that shows statistics on each auto_incrment within the instance, including the `auto_increment_ratio`, so you can easily monitor how full specific auto_increment columns are (**Contributed by Shlomi Noach**)
* A new `schema_redundant_indexes` view was added, that shows indexes made redundant (or duplicated) by other more dominant indexes. Also includes the the helper view `x$schema_flattened_keys`. (**Contributed by Shlomi Noach**)
* New `schema_table_lock_waits`/`x$schema_table_lock_waits` views have been added, which show any sessions that are waiting for table level metadata locks, and the sessions that are blocking them. Resolves Git Issue #57, inspired by the suggestion from Daniël van Eeden
* The `innodb_lock_waits` view had the following columns added to it, following a manually merged contribution from Shlomi Noach for a similar view
 * `wait_age_secs` - the current row lock wait time in seconds
 * `sql_kill_blocking_query` - the "KILL QUERY <connection_id>" command to run to kill the blocking session current statement
 * `sql_kill_blocking_connection` - the "KILL <connection_id" command to run to kill the blocking session
* A new `table_exists` procedure was added, which checks for the existence of table, and if it exists, returns the type (BASE TABLE, VIEW, TEMPORARY) (**Contributed by Jesper Wisborg Krogh**)
* A new `execute_prepared_stmt()` procedure was added, which takes a SQL statement as an input variable and executes it as a prepared statement (**Contributed by Jesper Wisborg Krogh**)
* A new `statement_performance_analyzer()` procedure was added, that allows reporting on the statements that are have been running over snapshot periods (**Contributed by Jesper Wisborg Krogh**)
* A new `diagnostics()` procedure was added, which creates a large diagnostics report based upon most of the new instrumentation now available, computed over a configurable number of snapshot intervals (**Contributed by Jesper Wisborg Krogh**)
* A 5.7 specific `ps_trace_thread()` procedure was added, which now shows the hierarchy of transactions and stored routines, as well as statements, stages and waits, if enabled
* Added a new `ps_thread_account()` stored function, that returns the "user@host" account for a given Performance Schema thread id
* Added a new `ps_thread_trx_info()` stored function which outputs, for a given thread id, the transactions, and statements that those transactions have executed, as a JSON object
* Added new `list_add()` and `list_drop()` stored functions, that take a string csv list, and either add or remove items from that list respectively. Can be used to easily update variables that take such lists, like `sql_mode`.
* The `ps_thread_id` stored function now returns the thread id for the current connection if NULL is passed for the in_connection_id parameter
* Added a new `version_major()` stored function, which returns the major version of MySQL Server (**Contributed by Jesper Wisborg Krogh**)
* Added a new `version_minor()` stored function, which returns the minor (release series) version of MySQL Server (**Contributed by Jesper Wisborg Krogh**)
* Added a new `version_patch()` stored function, which returns the patch release version of MySQL Server (**Contributed by Jesper Wisborg Krogh**)
* The `ps_is_account_enabled` function was updated to take a VARCHAR(32) user input on 5.7, as a part of WL#2284
* The generate_sql_file.sh script had a number of improvements:
 * Generated files are now output in to a "gen" directory, that is ignored by git
 * Added using a new default "mysql.sys@localhost" user (that has the account locked) for the MySQL 5.7+ integration as the DEFINER for all objects
 * Added a warning to the top of the generated integration file to also submit changes to the sys project
 * Improved the the option of skipping binary logs, so that all routines can load as well - those that used SET sql_log_bin will now select a warning when being used instead of setting the option

### Bug Fixes

* Git Issue #51 - Fixed the `generate_sql_file.sh` script to also replace the definer in the before_setup.sql output
* Git Issue #52 - Removed apostrophe from the `format_statement` function comment because TOAD no likey
* Git Issue #56 - Installation failed on 5.6 with ONLY_FULL_GROUP_BY enabled
* Git Issue #76 - Fixes for the new show_compatibility_56 variable. 5.7 versions of the `format_path()` function and `ps_check_lost_instrumentation` view were added, that use performance_schema.global_status/global_variables instead of information_schema.global_status/global_variables
* Git Issue #79 - Fixed grammar within `statements_with_runtimes_in_95th_percentile` view descriptions
* Oracle Bug #21484593 / Oracle Bug #21281955 - The `format_path()` function incorrectly took and returned a VARCHAR(260) instead of VARCHAR(512) (as the underlying is exposed as in Performance Schema) causing sporadic test failures
* Oracle Bug #21550271 - Fixed the `ps_setup_reset_to_default` for 5.7 with the addition of the new `history` column on the `performance_schema.setup_actors` table
* Oracle Bug #21550054 - It is possible that the views can show data that overflows when aggregating very large values, reset all statistics before each test to ensure no overflows
* Oracle Bug #21647101 - Fixed the `ps_is_instrument_default_enabed` and `ps_is_instrument_default_timed` to take in to account the new instruments added within 5.7
* MySQL Bug #77848 - Added the missing ps_setup_instruments_cleanup.inc
* Fixed the `ps_setup_reset_to_default()` procedure to also set the new `ENABLED` column within `performance_schema.setup_actors` within 5.7
* The `user_summary_by_file_io`/`x$user_summary_by_file_io` and `host_summary_by_file_io`/`x$host_summary_by_file_io` tables were incorrectly aggregating all wait events, not just `wait/io/file/%`

### Implementation Details

* Tests were improved via 5.7 integration
* Template files were added for stored procedures and functions
* Improved the sys_config_cleanup.inc procedure in tests to be able to reset the sys_config table completely (including the set_by column to NULL). The triggers can now be set to not update the column by setting the @sys.ignore_sys_config_triggers user variable to true

## 1.4.0 (09/03/2015)

### Backwards Incompatible Changes

* The `memory_global_by_current_allocated` views were renamed to `memory_global_by_current_bytes` for consistency with the other memory views
* The `ps_setup_enable_consumers` procedure was renamed to `ps_setup_disable_consumer` for naming consistency (everything is now singular, not plural)
* The `format_time` function displayed values in minutes incorrectly, it now rounds to minutes, and uses an 'm' suffix, like the rest of the units

### Improvements

* The beginnings of a mysql-test suite have been added
* The `innodb_lock_waits`/`x$innodb_lock_waits` views were improved (**Contributions by both Jesper Wisborg Krogh and Mark Matthews**)
 * Added the `wait_started`, `wait_age`, `waiting_trx_started` `waiting_trx_age`, `waiting_trx_rows_locked` and `waiting_trx_rows_modified` columns for waiting transactions
 * Added the `blocking_trx_started`, `blocking_trx_age`, `blocking_trx_rows_locked` and `blocking_trx_rows_modified` for blocking transactions
 * Order the result set so the oldest lock waits are first
 * The `waiting_table` and `waiting_index` were always the same as the `blocking_table` and `blocking_index`. So the blocking_% columns have been removed and the waiting_% columns have been renamed to locked_%
 * The `waiting_lock_type` and `blocking_lock_type` were also always the same. So these were removed and replaced with a single `locked_type` column
 * Renamed the `waiting_thread` and `blocking_thread` to `waiting_pid` and `blocking_pid` respectively to avoid confusion with the threads from the Performance Schema.
* Added the `sys_get_config` function, used to get configuration parameters from the `sys_config` table - primarily from other sys objects, but can be used individually (**Contributed by Jesper Wisborg Krogh**)
* Add an option to generate_sql_file.sh to generate a mysql_install_db / mysqld --initialize format friendly file
* Added the `ps_is_thread_instrumented` function, to check whether a specified thread is instrumented within Performance Schema
* Added the `ps_is_consumer_enabled` function, to check whether a specified consumer is enabled within Performance Schema (**Contributed by Jesper Wisborg Krogh**)
* Added some further replacements to the `format_path` function (`slave_load_tmpdir`, `innodb_data_home_dir`, `innodb_log_group_home_dir` and `innodb_undo_directory`)

### Bug Fixes

* The 5.6 `host_summary` and `x$host_summary` views incorrectly had the column with `COUNT(DISTINCT accounts.user)` named `unique_hosts` instead of `unique_users` (**Contributed by Jesper Wisborg Krogh**)
* Both the `format_time` and `format_bytes` took a BIGINT as input, and output VARCHAR, but BIGINT could be too small for aggregated values for the inputs. Now both functions both use TEXT as their input (Issue #34, Issue #38)
* The `format_time` function displayed values in minutes incorrectly, it now rounds to minutes, and uses an 'm' suffix, like the rest of the units
* The `sys_config` related triggers had no DEFINER clause set
* The `ps_setup_disable_thread` procedure always disabled the current thread and was ignoring the connection id given as an argument (**Contributed by Jesper Wisborg Krogh**)
* The `ps_trace_thread` procedure had an incorrect calculation of how long the procedure has been running (**Contributed by Jesper Wisborg Krogh**)

### Implementation Details

Various changes were made to allow better generation of integration sql files:

* The formatting for all comments has been standardized on -- line comments. C-style /* comments */ have been removed
 * Issue #35 had one instance of this resolved in this release (**Contributed by Joe Grasse**), but the entire code base has now been done
* Each object has been created within it's own file. No longer do x$ views live with their non-x$ counterparts
* DELIMITERs were standardized to $$

## 1.3.0 (23/10/2014)

### Improvements

* Added an `innodb_lock_waits` set of views, showing each thread that is waiting on a lock within InnoDB, and the blocking thread lock information (**Contributed by Jesper Wisborg Krogh**)

### Bug Fixes

* Fixed broken `host_summary_by_stages` views, broken with a last minute change before the 1.2.0 release that went unnoticed (facepalm)

## 1.2.0 (22/10/2014)

### Backwards Incompatible Changes

* The `host_summary_by_stages` and `user_summary_by_stages` `wait_sum` and `wait_avg` columns were renamed to `total_latency` and `avg_latency` respectively, for consistency.
* The `host_summary_by_file_io_type` and `user_summary_by_file_io_type `latency` column was renamed to `total_latency`, for consistency.

### Improvements

* Made the truncation length for the `format_statement` view configurable
  * This includes adding a new persistent `sys_config` table to store the new variable - `statement_truncate_len` - see the README for usage
* Added `total_latency` to the `schema_tables_with_full_table_scans` view, and added an x$ counterpart
* Added `innodb_buffer_free` to the `schema_table_statistics_with_buffer` view, to summarize how much free space is allocated per table in the buffer pool
* The `schema_unused_indexes` view now ignores indexes named `PRIMARY` (primary keys)
* Added `rows_affected` and `rows_affected_avg` stats to the `statement_analysis` views
* The `statements_with_full_table_scans` view now ignores any SQL that starts with `SHOW`
* Added a script, `generate_sql_file.sh`, that can be used to generate a single SQL file, also allowing substitution of the MySQL user to use, and/or whether the `SET sql_log_bin ...` statements should be omitted.
 * This is useful for those using RDS, where the root@localhost user is not accessible, and sql_log_bin is disabled (Issue #5)
* Added a set of `memory_by_thread_by_current_bytes` views, that summarize memory usage per thread with MySQL 5.7's memory instrumentation
* Improved each of the host specific views to return aggregate values for `background` threads, instead of ignoring them, in the same way as the user summary views

### Bug Fixes

* Added the missing `memory_by_host` view for MySQL 5.7
* Added missing space for hour notation within the `format_time` function
* Fixed views affected by MySQL 5.7 ONLY_FULL_GROUP_BY and functional dependency changes

## 1.1.0 (04/09/2014)

### Improvements

* Added host summary views, which have the same structure as the user summary views, but aggregated by host instead (**Contributed by Arnaud Adant**)
   * `host_summary`
   * `host_summary_by_file_io_type`
   * `host_summary_by_file_io`
   * `host_summary_by_statement_type`
   * `host_summary_by_statement_latency`
   * `host_summary_by_stages`
   * `waits_by_host_by_latency`

* Added functions which return instruments are either enabled, or timed by default (#15) (**Contributed by Jesper Wisborg Krogh**)
   * `ps_is_instrument_default_enabled`
   * `ps_is_instrument_default_timed`

* Added a `ps_thread_id` function, which returns the thread_id value exposed within performance_schema for the current connection (**Contributed by Jesper Wisborg Krogh**)
* Improved each of the user specific views to return aggregate values for `background` threads, instead of ignoring them (**Contributed by Joe Grasse**)
* Optimized the `schema_table_statistics` and `schema_table_statistics_with_buffer` views, to use a new view that will get materialized (`x$ps_schema_table_statistics_io`), along with the changes to the RETURN types for `extract_schema_from_file_name` and `extract_table_from_file_name`, this results in a significant performance improvement - in one test changing the run time from 14 minutes to 20 seconds. (**Conceived by Roy Lyseng, Mark Leith and Jesper Wisborg Krogh, implemented and contributed by Jesper Wisborg Krogh**)

### Bug Fixes

* Removed unintentially committed sys_56_rds.sql file (See Issue #5, which is still outstanding)
* Fixed the `ps_trace_statement_digest` and `ps_trace_thread` procedures to properly set sql_log_bin, and reset the thread INSTRUMENTED value correctly (**Contributed by Jesper Wisborg Krogh**)
* Removed various sql_log_bin disabling from other procedures that no longer require it - DML against the performance_schema data is no longer replicated (**Contributed by Jesper Wisborg Krogh**)
* Fixed EXPLAIN within `ps_trace_statement_digest` procedure (**Contributed by Jesper Wisborg Krogh**)
* Fixed the datatype for the `thd_id` variable within the `ps_thread_stack` procedure (**Contributed by Jesper Wisborg Krogh**)
* Fixed datatypes used for temporary tables within the `ps_trace_statement_digest` procedure (**Contributed by Jesper Wisborg Krogh**)
* Fixed the RETURN datatype `extract_schema_from_file_name` and `extract_table_from_file_name` to return a VARCHAR(64) (**Contributed by Jesper Wisborg Krogh**)
* Added events_transactions_current to the default enabled consumers in 5.7 (#25)

## 1.0.1 (23/05/2014)

### Improvements

* Added procedures to enable / disable Performance Schema consumers. (**Contributed by the MySQL QA Team**)
   * `ps_setup_disable_consumers(<LIKE string>)` allows disabling any consumers matching the LIKE string.
   * `ps_setup_enable_consumers(<LIKE string>)` allows enabling any consumers matching the LIKE string.

* Added procedures to show both enabled and disbled consumers or instruments individually, these are more useful for tooling than the `ps_setup_show_enabled`/`ps_setup_show_disabled` procedures which show all configuration in multiple result sets.  (**Contributed by the MySQL QA Team**)
   * `ps_setup_show_disabled_consumers` shows only disabled consumers.
   * `ps_setup_show_disabled_instruments` shows only disabled instruments.
   * `ps_setup_show_enabled_consumers` shows only enabled consumers.
   * `ps_setup_show_enabled_instruments` shows only enabled instruments.

### Bug Fixes

* Running the installation scripts sometimes failed because of the comment format. (#1) (**Contributed by Joe Grasse**)
* Some views did not work with the ERROR_FOR_DIVISION_BY_ZERO SQL mode. (#6) (**Contributed by Joe Grasse**)
* On Windows the `ps_thread_stack()` stored function failed to escape file path backslashes correctly within the JSON output.

## 1.0.0 (11/04/2014)
