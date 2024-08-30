--remove_files_wildcard $MYSQLTEST_VARDIR/tmp $log_slow_prefix-*.slog
--disable_query_log
EVAL SET GLOBAL log_output= $log_output_old;
EVAL SET GLOBAL slow_query_log_file= "$slow_query_log_file_old";
EVAL SET GLOBAL slow_query_log= $slow_query_log_old;
--enable_query_log
