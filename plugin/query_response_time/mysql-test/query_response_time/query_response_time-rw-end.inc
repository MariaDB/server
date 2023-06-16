--echo ------------------Test body end----------------------
--disable_query_log
SET GLOBAL QUERY_RESPONSE_TIME_STATS = 0;
SET SESSION QUERY_RESPONSE_TIME_EXEC_TIME_DEBUG = DEFAULT;
--enable_query_log
--source query_response_time-check.inc
