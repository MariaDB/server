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
# $mw_369_child_query - will be blocked on node_1 in wsrep_apply_cb
# node_1:
# COMMIT;             - will be blocked on node_1 in after_wsrep_before_prepare
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
SET GLOBAL DEBUG_DBUG = "d,sync.wsrep_apply_cb";

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
SET SESSION DEBUG_SYNC = "now WAIT_FOR sync.wsrep_apply_cb_reached";

--connection node_1
SET SESSION DEBUG_SYNC = "wsrep_before_certification SIGNAL before_certification_reached WAIT_FOR continue_before_certification";
--send COMMIT

#
# Wait until both sync points have been reached
#
--connection node_1a
SET SESSION DEBUG_SYNC = "now WAIT_FOR before_certification_reached";

#
# both threads are now parked in sync points, signal them to continue
#

--let $wait_condition = SELECT COUNT(*) = 2 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE STATE LIKE 'debug sync point:%'
--let $wait_condition_on_error_output = SELECT STATE FROM INFORMATION_SCHEMA.PROCESSLIST
--source include/wait_condition_with_debug.inc

SET SESSION DEBUG_SYNC = 'now SIGNAL continue_before_certification';

--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE STATE LIKE 'debug sync point:%'
--let $wait_condition_on_error_output = SELECT STATE FROM INFORMATION_SCHEMA.PROCESSLIST
--source include/wait_condition_with_debug.inc

SET SESSION DEBUG_SYNC = 'now SIGNAL signal.wsrep_apply_cb';

--let $wait_condition = SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE STATE LIKE 'debug sync point:%'
--let $wait_condition_on_error_output = SELECT STATE FROM INFORMATION_SCHEMA.PROCESSLIST
--source include/wait_condition_with_debug.inc

SET GLOBAL DEBUG_DBUG = "";
SET DEBUG_SYNC = 'RESET';
