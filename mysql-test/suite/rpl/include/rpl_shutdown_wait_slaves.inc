--connection server_1

CREATE TABLE t1 (a INT) ENGINE=innodb;

--save_master_pos

--connection server_2
--sync_with_master

--connection server_3
--sync_with_master

--connection server_4
--source include/stop_slave.inc

--connection server_1
--disable_query_log
--let $count=1000
while ($count)
{
 INSERT INTO t1 SET a=1;
 --dec $count
}
--enable_query_log
--save_master_pos

# Shutdown master and restart server_4 who will be waiting for the master
# to start replication at its shutdown beginning phase.
# The being forked out server_4 dump thread must relate to a record
# in slave_list, and it won't start sending out binlog events
# until has received a signal from the shutdown thread.
# This also proves delivery to a  started-in-middle-of-shutdown slave.
--connection server_1
SET @@GLOBAL.debug_dbug="+d,simulate_delay_at_shutdown";

--connection server_4
--source include/start_slave.inc

--connection server_1
--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait
EOF
# --shutdown_server
--send SHUTDOWN WAIT FOR ALL SLAVES
--reap
--source include/wait_until_disconnected.inc
#
# MDEV-18450 liveness condition:
# Despite shutdown even "late" slave #4 is in sync
#
--connection server_4
--sync_with_master

--connection server_3
--sync_with_master

--connection server_2
--sync_with_master

--connection server_1
--append_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
restart
EOF

--connection default
--enable_reconnect
--source include/wait_until_connected_again.inc
--connection server_1
--enable_reconnect
--source include/wait_until_connected_again.inc

#
# Cleanup
#
--connection server_1
DROP TABLE t1;

--connection server_2
--disable_warnings
--source include/start_slave.inc
--enable_warnings

--connection server_3
--disable_warnings
--source include/start_slave.inc
--enable_warnings

--connection server_4
--disable_warnings
--source include/start_slave.inc
--enable_warnings
