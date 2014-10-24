SET SESSION query_response_time_exec_time_debug=100000;

SET GLOBAL QUERY_RESPONSE_TIME_STATS=0;
EVAL SET GLOBAL QUERY_RESPONSE_TIME_RANGE_BASE=$base;
FLUSH QUERY_RESPONSE_TIME;
# Following two queries check works of FLUSH and 
# respecting of "QUERY_RESPONSE_TIME_STATS" variable (see launchpad bug #855312)
SELECT * FROM INFORMATION_SCHEMA.QUERY_RESPONSE_TIME;
SET GLOBAL QUERY_RESPONSE_TIME_STATS=1;

SET SESSION query_response_time_exec_time_debug=310000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=320000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=330000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=340000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=350000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=360000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=370000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=380000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=390000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=400000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=1100000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=1200000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=1300000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=1500000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=1400000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=500000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=2100000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=2300000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=2500000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=3100000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=4100000; SELECT 1;
SET SESSION query_response_time_exec_time_debug=5100000; SELECT 1;

SET SESSION query_response_time_exec_time_debug=100000;

SET GLOBAL QUERY_RESPONSE_TIME_STATS=0;

SHOW GLOBAL VARIABLES where Variable_name like 'QUERY_RESPONSE_TIME_RANGE_BASE';
SHOW QUERY_RESPONSE_TIME;

SET SESSION query_response_time_exec_time_debug=default;
