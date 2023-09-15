--disable_query_log
--let slow_query_log_old= `select @@slow_query_log`
--let slow_query_log_file_old= `select @@slow_query_log_file`
--let log_output_old= `select @@log_output`

SET GLOBAL slow_query_log=0;
SET GLOBAL log_output=FILE;
--enable_query_log
