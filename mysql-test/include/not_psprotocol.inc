# Disable in mtr --ps-protocol
--disable_abort_on_error
--disable_result_log
--disable_query_log
/*M!100202 execute immediate 'select 1' */;
--enable_query_log
--enable_result_log
--enable_abort_on_error
let $error = $mysql_errno;
# ER_UNSUPPORTED_PS
if ($error == 1295)
{
--skip Test is not supported in the prepared statement protocol yet
}
