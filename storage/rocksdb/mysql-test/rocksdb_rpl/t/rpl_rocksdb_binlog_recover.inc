#
# Execute given queries and crash(and restart) the server after  binary log
#  commit
# @@input $exec_query_1
# @@input $exec_query_2

--connection master
set autocommit= 0;
--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait
EOF

start transaction;
 --eval $exec_query_1
 --eval $exec_query_2
SET GLOBAL DEBUG_DBUG="d,simulate_crash_after_binlog_commit_or_rollback";
--error 2013 # CR_SERVER_LOST
commit;

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
set autocommit= 1;
SET GLOBAL DEBUG_DBUG="reset";
