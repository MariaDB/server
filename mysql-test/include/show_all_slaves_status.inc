# ==== Usage ====
#
# --let $status_items= Column_Name[, Column_Name[, ...]]
# --source include/show_all_slaves_status.inc
#
# Parameters:
#   $status_items
#     Set to the name of the column in the output of SHOW SLAVE STATUS
#     that you want to display. Example:
#
#       --let $status_items= Master_SSL_Allowed
#
#     You can show multiple columns by setting $status_items to a
#     comma-separated list. Example:
#
#       --let $status_items= Master_Log_File, Relay_Master_Log_File
#
#   $slave_field_result_replace
#     If set, one or more regex patterns for replacing variable
#     text in the error message. Syntax as --replace-regex
#
#   $slave_sql_mode
#     If set, change the slave sql mode during this macro, reverting
#     to the previous on exit. Default sql_mode is NO_BACKSLASH_ESCAPES
#     to allow replace '\' by '/' making paths OS independent. Example:
#
#       --let $slave_sql_mode= NO_BACKSLASH_ESCAPES
#


--let $_show_slave_status_items=$status_items
if (!$status_items)
{
  --die Bug in test case: The mysqltest variable $status_items is not set.
}


--let $_slave_sql_mode= NO_BACKSLASH_ESCAPES
if ($slave_sql_mode)
{
  --let $_slave_sql_mode= $slave_sql_mode
}
--let $_previous_slave_sql_mode = `SELECT @@sql_mode`
--disable_query_log
eval SET sql_mode= '$_slave_sql_mode';
--enable_query_log


while ($_show_slave_status_items) 
{
  --let $_show_slave_status_name= `SELECT SUBSTRING_INDEX('$_show_slave_status_items', ',', 1)`
  --let $_show_slave_status_items= `SELECT LTRIM(SUBSTRING('$_show_slave_status_items', LENGTH('$_show_slave_status_name') + 2))`

  --replace_regex /\.[\\\/]master/master/
  --let $_show_slave_status_value= query_get_value(SHOW ALL SLAVES STATUS, $_show_slave_status_name, 1)
  --let $_slave_field_result_replace= /[\\\\]/\// $slave_field_result_replace
  --replace_regex $_slave_field_result_replace
  --let $_show_slave_status_value= `SELECT REPLACE("$_show_slave_status_value", '$MYSQL_TEST_DIR', 'MYSQL_TEST_DIR')`
  --echo $_show_slave_status_name = '$_show_slave_status_value'
}


--disable_query_log
eval SET sql_mode= '$_previous_slave_sql_mode';
--enable_query_log
