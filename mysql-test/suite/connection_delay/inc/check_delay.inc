let $step=`SELECT GREATEST(1000,FLOOR(($timeout*1000)/($max_password_errors-$password_errors_before_delay)))`;
--source ../inc/check_connection_delay_step.inc
inc $failed_count;
let $SERVER_RESPONSE_TIME= `SELECT ($failed_count-$password_errors_before_delay) * $step`;
--source ../inc/check_response_delay_time.inc