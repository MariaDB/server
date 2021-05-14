#
# Execute a transaction and crash the server after binlog commit.
#
# @@input  $exec_query The query which needs to be executed.

--connection master
--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait
EOF

SET GLOBAL DEBUG_DBUG="d,simulate_crash_after_binlog_commit_or_rollback";
--error 2013 # CR_SERVER_LOST
--eval $exec_query

--source include/wait_until_disconnected.inc

# Server restart
--append_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
restart
EOF

connection default;
--enable_reconnect
--source include/wait_until_connected_again.inc

# rpl_end.inc needs to use the connection server_1
connection server_1;
--enable_reconnect
--source include/wait_until_connected_again.inc

--connection master
--enable_reconnect
--source include/wait_until_connected_again.inc

