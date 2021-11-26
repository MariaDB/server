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


if ($case == 1)
{
  SET DEBUG_SYNC= "commit_after_release_LOCK_after_binlog_sync SIGNAL con1_ready WAIT_FOR con1_go";
  --send_eval $query_to_crash
  --connection server_$server_to_crash
  SET DEBUG_SYNC= "now WAIT_FOR con1_ready";
  --source include/kill_mysqld.inc
}

# complicate recovery with an extra binlog file
if ($case == 2)
{
  SET DEBUG_SYNC= "commit_before_get_LOCK_commit_ordered SIGNAL con1_ready WAIT_FOR con1_go";
  --send_eval $query_to_crash
  --connect (conn_client_2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
  # use the same signal with $query_to_crash
  SET DEBUG_SYNC= "now WAIT_FOR con1_ready";
  SET GLOBAL debug_dbug="d,Notify_binlog_EOF";
  --send_eval $query2_to_crash
  --connection server_$server_to_crash
  SET DEBUG_SYNC= "now WAIT_FOR eof_reached";
  --source include/kill_mysqld.inc
}

# complicate recovery with an extra binlog file
if ($case == 3)
{
  SET DEBUG_SYNC= "commit_before_get_LOCK_commit_ordered SIGNAL con1_ready WAIT_FOR con1_go";
  --send_eval $query_to_crash
  --connect (conn_client_3,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
  SET DEBUG_SYNC= "now WAIT_FOR con1_ready";
  # use the same signal with $query_to_crash
  SET DEBUG_SYNC= "commit_before_update_binlog_end_pos SIGNAL con3_ready WAIT_FOR con1_go";
  --send_eval $query2_to_crash
  --connection server_$server_to_crash
  SET DEBUG_SYNC= "now WAIT_FOR con3_ready";
  --source include/kill_mysqld.inc
}

--connection server_$server_to_promote
--let $slave_param= Slave_SQL_Running_State
--let $slave_param_value= Slave has read all relay log; waiting for more updates
source include/wait_for_slave_param.inc;

--error 2003
--source include/stop_slave.inc

--let $assert_cond= COUNT(*) = $expected_rows_on_slave FROM t1
--let $assert_text= Table t1 should have $expected_rows_on_slave rows.
--source include/assert.inc

SELECT @@GLOBAL.gtid_current_pos;

--let $restart_parameters=--skip-slave-start=1 --rpl-semi-sync-slave-enabled=1
--let $allow_rpl_inited=1
--source include/start_mysqld.inc
--connection server_$server_to_crash
--enable_reconnect
--source include/wait_until_connected_again.inc

--let $assert_cond= COUNT(*) = $expected_rows_on_master FROM t1
--let $assert_text= Table t1 should have $expected_rows_on_master rows.
--source include/assert.inc

# Check error log for correct messages.
let $log_error_ = $MYSQLTEST_VARDIR/log/mysqld.$server_to_crash.err;
--let SEARCH_FILE=$log_error_
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
evalp CHANGE MASTER TO master_host='127.0.0.1', master_port=$new_master_port, master_user='root', master_use_gtid=SLAVE_POS;
set global rpl_semi_sync_slave_enabled = 1;
set @@global.gtid_slave_pos=@@global.gtid_binlog_pos;
--source include/start_slave.inc
