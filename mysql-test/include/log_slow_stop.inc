--disable_query_log
SET GLOBAL slow_query_log=0;
--echo [log_slow_stop.inc] $log_file
--enable_query_log
