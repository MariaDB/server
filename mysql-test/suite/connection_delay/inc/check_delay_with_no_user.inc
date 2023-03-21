let $step=`SELECT FLOOR(@@global.connect_timeout*1000*1000000/(@@global.max_password_errors-@@global.password_errors_before_delay))`;
--source ../inc/check_connection_delay_step.inc
let $SERVER_RESPONSE_TIME= `SELECT ($failed_count-@@global.password_errors_before_delay) * $step`;
--let SEARCH_PATTERN=@$HOST wait $SERVER_RESPONSE_TIME nanoseconds
--source include/search_pattern_in_file.inc
