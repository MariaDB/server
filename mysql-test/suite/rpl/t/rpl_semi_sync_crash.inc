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
  SET DEBUG_SYNC= "commit_after_release_LOCK_after_binlog_sync WAIT_FOR con1_go1";
  --send_eval $query_to_crash
  --connect (conn_client_2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
  # use the same signal with $query_to_crash
  SET DEBUG_SYNC= "now WAIT_FOR con1_ready";
  SET DEBUG_SYNC= "commit_before_get_LOCK_after_binlog_sync SIGNAL con1_go";
  SET DEBUG_SYNC= "commit_before_get_LOCK_commit_ordered SIGNAL con2_ready";
  --send_eval $query2_to_crash
  --connection server_$server_to_crash
  SET DEBUG_SYNC= "now WAIT_FOR con2_ready";
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

--eval select count(*) 'on slave must be $expected_rows_on_slave' from t1

SELECT @@GLOBAL.gtid_current_pos;

--let $restart_parameters=--skip-slave-start=1 --rpl-semi-sync-slave-enabled=1 --init-rpl-role=SLAVE
--let $allow_rpl_inited=1
--source include/start_mysqld.inc
--connection server_$server_to_crash
--enable_reconnect
--source include/wait_until_connected_again.inc

--let $slave_semi_sync_enabled= query_get_value(SHOW VARIABLES LIKE 'Rpl_semi_sync_slave_enabled', Value, 1)
--echo # Ensuring variable rpl_semi_sync_slave_enabled is ON..
if (`SELECT strcmp("ON", "$slave_semi_sync_enabled") != 0`)
{
  --die Slave started with rpl_semi_sync_slave_enabled=1 yet it is OFF in the variable output
}

--let $slave_semi_sync_status= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_slave_status', Value, 1)
--echo # Ensuring status rpl_semi_sync_slave_status is OFF..
if (`SELECT strcmp("OFF", "$slave_semi_sync_status") != 0`)
{
  --die Slave started with skip-slave-start yet started with rpl_semi_sync_slave_status=ON
}

--eval select count(*) 'on master must be $expected_rows_on_master' from t1

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
evalp CHANGE MASTER TO master_host='127.0.0.1', master_port=$new_master_port, master_user='root', master_ssl_verify_server_cert=0, master_use_gtid=SLAVE_POS;
set global rpl_semi_sync_slave_enabled = 1;
set @@global.gtid_slave_pos=@@global.gtid_binlog_pos;
--source include/start_slave.inc
