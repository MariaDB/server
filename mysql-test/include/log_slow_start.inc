--disable_query_log
--let log_file_full_path=$MYSQLTEST_VARDIR/tmp/$log_file.slog
--echo [slow_log_start.inc] $log_file
EVAL SET GLOBAL slow_query_log_file="$log_file_full_path";
SET GLOBAL slow_query_log=1;
--enable_query_log
