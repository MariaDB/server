#
# Issue an INSERT for gap between 1 and 3 to node_2 and wait until it hits
# apply monitor sync point on node_1
#

--connection node_1a
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

--connection node_2
--eval $galera_sp_bf_abort_conflict

--connection node_1a
--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc

# Send a procedure to node_1 which should take a gap lock between
# rows 1 and 3. It does not conflict with INSERT from node_2 in
# certification. Park the UPDATE after replicate and let INSERT to
# continue applying, generating a BF abort.

--let $galera_sync_point = after_replicate_sync
--source include/galera_set_sync_point.inc

--connection node_1
--send_eval CALL $galera_sp_bf_abort_proc

--connection node_1a
--let $galera_sync_point = after_replicate_sync apply_monitor_slave_enter_sync
--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc

--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_signal_sync_point.inc
--let $galera_sync_point = after_replicate_sync
--source include/galera_signal_sync_point.inc
