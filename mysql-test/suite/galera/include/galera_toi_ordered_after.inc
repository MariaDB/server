# Run $local on node_1 so that it is ordered in TOI after $earlier from
# node_2, while the applier of $earlier is parked on node_1 before it
# applies it. Then release the applier and reap $local.
#
# Used to check that $local does not hold locks while waiting in TOI
# that the applier of $earlier needs.
#
# Parameters:
#   $earlier      statement run on node_2
#   $local        statement run on node_1
#   $local_error  expected error of $local, 0 for success (default)
#
# Connections node_1, node_1a, node_2 and node_2a are used, node_1a and
# node_2a must have wsrep_sync_wait = 0, and node_1 too if $local is a
# statement that would wait for causality.

if (!$local_error)
{
  --let $local_error = 0
}

--connection node_1a
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

--connection node_2
--eval $earlier

--connection node_1a
--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc

--connection node_2a
--let $galera_toi_last_committed = `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_last_committed'`

--connection node_1
--send_eval $local

--echo # Ordered in TOI after it: node_2 has processed it
--connection node_2a
--let $wait_condition = SELECT VARIABLE_VALUE > $galera_toi_last_committed FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_last_committed'
--source include/wait_condition.inc

--connection node_1a
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_signal_sync_point.inc

--connection node_1
--error $local_error
--reap

--let $local_error = 0
