--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/mysqld.1.1.expect

# shutdown_mysqld
--exec echo "wait" > $_expect_file_name
--shutdown_server
--source include/wait_until_disconnected.inc

# start_mysqld
--exec echo "restart: $restart_parameters" > $_expect_file_name
--enable_reconnect
--source include/wait_until_connected_again.inc
--disable_reconnect
