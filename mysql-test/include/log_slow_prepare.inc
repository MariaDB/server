--disable_query_log
--let log_slow_query_old= `select @@log_slow_query`
--let log_slow_query_file_old= `select @@log_slow_query_file`
--let log_output_old= `select @@log_output`

SET GLOBAL log_slow_query=0;
SET GLOBAL log_output=FILE;
--enable_query_log
