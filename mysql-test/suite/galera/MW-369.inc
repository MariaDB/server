#
# This file should be included from tests for MW-369 to run concurrent
# transaction from node_1 with autocommit query from node_2.
#
# The parameters:
# * $mw_369_parent_query - the parent query to be run inside transaction
# * $mw_369_child_query - the child query
#
# The operations are the following:
#
# node_1:
# START TRANSACTION;
# $mw_369_parent_query
# node_2
# $mw_369_child_query - will be blocked on node_1 in apply monitor
# node_1:
# COMMIT;             - will be blocked on node_1 in local monitor
#
# The $mw_369_child_query is always expected to succeed. The caller is
# responsible for checking if the final COMMIT on connection node_1
# succeeds.
#

--connection node_1
SET AUTOCOMMIT=ON;
START TRANSACTION;

--eval $mw_369_parent_query

#
# Block the $mw_369_child_query from node_2
#
# --connect node_1a, 127.0.0.1, root, , test, $NODE_MYPORT_1
--connection node_1a
SET SESSION wsrep_sync_wait = 0;
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

#
# insert client row, which will make it impossible to replay the
# delete on parent
#
--connection node_2
--eval $mw_369_child_query

#
# Wait until $mw_369_child_query from node_2 reaches the sync point and
# block the 'COMMIT' from node_1 before it certifies.
#
--connection node_1a
--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc

--let $galera_sync_point = local_monitor_enter_sync
--source include/galera_set_sync_point.inc

--connection node_1
--send COMMIT

#
# Wait until both sync points have been reached
#
--connection node_1a
--let $galera_sync_point = apply_monitor_slave_enter_sync local_monitor_enter_sync
--source include/galera_wait_sync_point.inc

#
# both threads are now parked in sync points, signal them to continue
#
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_signal_sync_point.inc

--let $galera_sync_point = local_monitor_enter_sync
--source include/galera_signal_sync_point.inc
--source include/galera_clear_sync_point.inc
