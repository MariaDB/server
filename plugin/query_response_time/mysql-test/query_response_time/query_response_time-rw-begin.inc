--disable_query_log
--disable_result_log
SET GLOBAL QUERY_RESPONSE_TIME_STATS = 0;
SET SESSION QUERY_RESPONSE_TIME_EXEC_TIME_DEBUG = 500000;
FLUSH QUERY_RESPONSE_TIME;
--enable_query_log
--enable_result_log
--source query_response_time-check.inc
--disable_query_log
--disable_result_log
SET GLOBAL QUERY_RESPONSE_TIME_STATS = 1;
FLUSH QUERY_RESPONSE_TIME;
--enable_query_log
--enable_result_log
--echo ------------------Test body begin--------------------
