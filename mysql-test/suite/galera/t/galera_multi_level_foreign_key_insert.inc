
--connection node_2
SET GLOBAL DEBUG_DBUG = '+d,sync.wsrep_apply_toi';

--connection node_1
--eval $fk_parent_query

--connection node_2
SET DEBUG_SYNC = "now WAIT_FOR sync.wsrep_apply_toi_reached";
SET SESSION wsrep_sync_wait = 0;

#
# Execute child query on node_1.
# If bug is present, expect the wait condition
# to timeout and when the child query applies, it
# will be granted a MDL lock on parent table.
# When resumed, the parent query will
# also try to acquire MDL lock on parent table,
# causing a BF-BF conflict on that MDL lock.
#
--connection node_1
START TRANSACTION;
--eval $fk_child_query
--let $wait_condition = SELECT COUNT(*) = $fk_mdl_lock_num FROM performance_schema.metadata_locks WHERE OBJECT_SCHEMA='test' AND LOCK_STATUS="GRANTED"
--source include/wait_condition.inc
COMMIT;


--connection node_2
SET GLOBAL DEBUG_DBUG = '-d,sync.wsrep_apply_toi';
SET DEBUG_SYNC = "now SIGNAL signal.wsrep_apply_toi";


#
# Cleanup
#
SET DEBUG_SYNC = 'RESET';
SET GLOBAL DEBUG_DBUG = "";
SET GLOBAL wsrep_slave_threads=DEFAULT;

--connection node_1
SET DEBUG_SYNC = 'RESET';
SET GLOBAL DEBUG_DBUG = "";
SET GLOBAL wsrep_slave_threads=DEFAULT;
