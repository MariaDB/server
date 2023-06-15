--disable_query_log
SET GLOBAL log_slow_query=0;
--echo [log_slow_stop.inc] $log_file
--enable_query_log
