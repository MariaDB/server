--disable_query_log
--let log_file_full_path=$MYSQLTEST_VARDIR/tmp/$log_file.slog
--echo [slow_log_start.inc] $log_file
EVAL SET GLOBAL log_slow_query_file="$log_file_full_path";
SET GLOBAL log_slow_query=1;
--enable_query_log
