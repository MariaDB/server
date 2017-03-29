--let $_server_id= `SELECT @@server_id`
--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/mysqld.$_server_id.expect

if ($restart_parameters)
{
  --echo # Kill and restart: $restart_parameters
  --exec echo "restart: $restart_parameters" > $_expect_file_name
}
if (!$restart_parameters)
{
  --echo # Kill and restart
  --exec echo "restart" > $_expect_file_name
}

--shutdown_server 0
--source include/wait_until_disconnected.inc
--enable_reconnect
--source include/wait_until_connected_again.inc
--disable_reconnect
