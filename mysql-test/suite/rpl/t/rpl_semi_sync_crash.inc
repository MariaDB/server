if ($failover_to_slave)
{
 --let $server_to_crash=1
 --let $server_to_promote=2
 --let $new_master_port=$SERVER_MYPORT_2
 --let $client_port=$SERVER_MYPORT_1

 --connect (conn_client,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
}
if (!$failover_to_slave)
{
 --let $server_to_crash=2
 --let $server_to_promote=1
 --let $new_master_port=$SERVER_MYPORT_1
 --let $client_port=$SERVER_MYPORT_2

 --connect (conn_client,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
}


# Hold insert after write to binlog and before "run_commit_ordered" in engine

SET DEBUG_SYNC= "commit_after_release_LOCK_after_binlog_sync SIGNAL con1_ready WAIT_FOR con1_go";
--send_eval $query_to_crash

# complicate recovery with an extra binlog file
if (!$failover_to_slave)
{
  --connect (conn_client_2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
  # use the same signal with $query_to_crash
  SET DEBUG_SYNC= "now WAIT_FOR con1_ready";
  SET DEBUG_SYNC= "commit_after_release_LOCK_lock SIGNAL con1_ready WAIT_FOR con2_go";
  --send_eval $query2_to_crash
}

--connection server_$server_to_crash
SET DEBUG_SYNC= "now WAIT_FOR con1_ready";
--source include/kill_mysqld.inc
--source include/wait_until_disconnected.inc

--connection server_$server_to_promote
--error 2003
--source include/stop_slave.inc
SELECT @@GLOBAL.gtid_current_pos;

--let $_expect_file_name=$MYSQLTEST_VARDIR/tmp/mysqld.$server_to_crash.expect
--let $restart_parameters=--rpl-semi-sync-slave-enabled=1
--let $allow_rpl_inited=1
--source include/start_mysqld.inc

--connection server_$server_to_crash
--enable_reconnect
--source include/wait_until_connected_again.inc

# Check error log for correct messages.
let $log_error_= `SELECT @@GLOBAL.log_error`;
if(!$log_error_)
{
    # MySQL Server on windows is started with --console and thus
    # does not know the location of its .err log, use default location
    let $log_error_ = $MYSQLTEST_VARDIR/log/mysqld.$server_to_crash.err;
}
--let SEARCH_FILE=$log_error_
--let SEARCH_RANGE=-50000
--let SEARCH_PATTERN=$log_search_pattern
--source include/search_pattern_in_file.inc

--disconnect conn_client

#
# FAIL OVER now to new master
#
--connection server_$server_to_promote
set global rpl_semi_sync_master_enabled = 1;
set global rpl_semi_sync_master_wait_point=AFTER_SYNC;

--connection server_$server_to_crash
--let $master_port=$SERVER_MYPORT_2
if (`select $server_to_crash = 2`)
{
 --let $master_port=$SERVER_MYPORT_1
}
--replace_result $SERVER_MYPORT_1 SERVER_MYPORT_1 $SERVER_MYPORT_2 SERVER_MYPORT_2
eval CHANGE MASTER TO master_host='127.0.0.1', master_port=$new_master_port, master_user='root', master_use_gtid=SLAVE_POS;
set global rpl_semi_sync_slave_enabled = 1;
set @@global.gtid_slave_pos=@@global.gtid_binlog_pos;
--source include/start_slave.inc
