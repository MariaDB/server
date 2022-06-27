# ==== Purpose ====
#
# Show selected columns of output from SHOW SLAVE STATUS.
#
# Note: test cases should never call SHOW SLAVE STATUS directly,
# because that outputs more information to the query log than what is
# needed for the property that is being tested.  That would cause
# maintenance problems, because (1) it is hard for a human to
# understand what property is being tested; (2) the output of many of
# the fields is non-deterministic (e.g., Slave_IO_State) or changes
# frequently (e.g., binlog positions).
#
# Instead, what you want most of the time is to source one of the
# following scripts:
#
#   include/check_slave_no_error.inc
#     Assert that Slave_SQL_Errno = Slave_IO_Errno = 0.
#
#   include/check_slave_is_running.inc
#     Assert that Slave_IO_Running = Slave_SQL_Running = 'Yes'.
#
#   include/wait_for_slave_sql_error.inc
#     Wait for the SQL thread to get a given error (and assert that
#     it has this error).
#
#   include/wait_for_slave_io_error.inc
#     Wait for the IO thread to get a given error (and assert that
#     it has this error).
#
#   include/wait_for_slave_sql_to_stop.inc
#   include/wait_for_slave_io_to_stop.inc
#   include/wait_for_slave_to_stop.inc
#     Wait for the SQL thread, the IO thread, or both to stop (and
#     assert they stop).
#
# When none of the above applies, you may use this script instead.
# However, take care so that the test never contains explicit binlog
# coordinates. Usually you can read the binlog coordinates into a
# variable and compare it to some other coordinates.
#
#
# ==== Usage ====
#
# --let $status_items= Column_Name[, Column_Name[, ...]]
# --source include/show_slave_status.inc
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
#   $all_slaves_status
#     If set, use SHOW ALL SLAVES STATUS instead of SHOW SLAVE STATUS
#     and get the column values from all rows. Example:
#
#       --let $all_slaves_status= 1
#
#   $slave_name
#     If set, use SHOW SLAVE '<slave_name>' STATUS instead of SHOW SLAVE STATUS.
#     The name must be quoted (can be a quoted empty string).
#     Example:
#
#       --let $slave_name= 'm1'
#
#   Note: $all_slaves_status and $slave_name are mutually exclusive.
#

--let $_show_slave_status_items=$status_items
if (!$status_items)
{
  --die Bug in test case: The mysqltest variable $status_items is not set.
}

--let $_show_query=SHOW SLAVE STATUS

if ($all_slaves_status)
{
  if ($slave_name)
  {
    --die Bug in test case: Both $all_slaves_status and $slave_name are set.
  }
  --let $_show_query=SHOW ALL SLAVES STATUS
}
if ($slave_name)
{
  --let $_show_query=SHOW SLAVE $slave_name STATUS
  if ($for_channel)
  {
    --let $_show_query=SHOW SLAVE STATUS for channel $slave_name
  }
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

--let $_slave_field_result_replace= /[\\\\]/\// $slave_field_result_replace

while ($_show_slave_status_items) 
{
  --let $_show_slave_status_name= `SELECT SUBSTRING_INDEX('$_show_slave_status_items', ',', 1)`
  --let $_show_slave_status_items= `SELECT LTRIM(SUBSTRING('$_show_slave_status_items', LENGTH('$_show_slave_status_name') + 2))`

  --let $num= 1
  --let $_show_slave_status_value=
  while ($_show_slave_status_value != 'No such row')
  {
    --replace_regex /\.[\\\/]master/master/
    --let $_show_slave_status_value= query_get_value($_show_query, $_show_slave_status_name, $num)
    if ($_show_slave_status_value != 'No such row')
    {
      --replace_regex $_slave_field_result_replace
      --let $_show_slave_status_value= `SELECT REPLACE("$_show_slave_status_value", '$MYSQL_TEST_DIR', 'MYSQL_TEST_DIR')`
      --echo $_show_slave_status_name = '$_show_slave_status_value'
      --inc $num
    }
  }
}


--disable_query_log
eval SET sql_mode= '$_previous_slave_sql_mode';
--enable_query_log
