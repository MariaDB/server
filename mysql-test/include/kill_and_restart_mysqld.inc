if (!$restart_parameters)
{
  let $restart_parameters = restart;
}

--let $_expect_file_name= `select regexp_replace(@@tmpdir, '^.*/','')`
--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/$_expect_file_name.expect

--echo # Kill and $restart_parameters
--exec echo "$restart_parameters" > $_expect_file_name
--shutdown_server 0
--source include/wait_until_disconnected.inc
--enable_reconnect
--source include/wait_until_connected_again.inc
--disable_reconnect
