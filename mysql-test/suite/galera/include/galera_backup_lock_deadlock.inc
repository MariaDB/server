# include/galera_backup_lock_deadlock.inc
#
# Check for a deadlock between a backup lock statement on node 1 and a
# write in the provider order on node 1: the lock statement holds its
# backup lock and waits for the provider pause, the pause waits for the
# write, and the write waits for the backup lock. Print one line:
#
#   BUG: <name>: deadlock: ...
#   <name>: no deadlock
#
# Then kill node 1 and start it again. This is the only way to end the
# deadlock. Connections other than node_1 and node_2 to node 1 are lost.
# Node 1 then runs with other options: source
# include/galera_restart_default.inc at the end of the test.
#
# Parameters:
#   $dl_name     label to print
#   $dl_lock_id  connection ID that runs the backup lock statement
#   $dl_write    PROCESSLIST condition that selects the write
#

--disable_query_log
--connection node_1
SET SESSION wsrep_sync_wait= 0;
--let $dl_lock_waits= ID = $dl_lock_id AND COMMAND = 'Query' AND STATE <> 'Waiting for backup lock'
--let $dl_write_waits= ($dl_write) AND STATE = 'Waiting for backup lock'
--let $dl_paused= SELECT VARIABLE_VALUE <> 'Synced' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment'
--let $dl_deadlock= SELECT ($dl_paused) AND (SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE $dl_lock_waits) AND (SELECT COUNT(*) > 0 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE $dl_write_waits)
--let $dl_lock_ended= SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $dl_lock_id AND COMMAND <> 'Query'
--let $wait_condition= SELECT ($dl_deadlock) OR ($dl_lock_ended)
--source include/wait_condition.inc
--disable_query_log
if (`$dl_deadlock`)
{
  --echo BUG: $dl_name: deadlock: the lock statement waits for the provider pause, the pause waits for the write, the write waits for the backup lock
}
if (!`$dl_deadlock`)
{
  --echo $dl_name: no deadlock
}

--source include/kill_galera.inc

--connection node_2
--let $wait_condition= SELECT VARIABLE_VALUE = 'non-Primary' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_status'
--source include/wait_condition.inc

# Node 1 is not safe to bootstrap after the kill. It joins node 2.
--connection node_1
--let $start_mysqld_params= --wsrep-cluster-address=gcomm://127.0.0.1:$NODE_GALERAPORT_2
--source include/start_mysqld.inc
--let $start_mysqld_params=

--connection node_2
--let $wait_condition= SELECT VARIABLE_VALUE = 2 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc
--let $wait_condition= SELECT VARIABLE_VALUE = 'Synced' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment'
--source include/wait_condition.inc
--enable_query_log
