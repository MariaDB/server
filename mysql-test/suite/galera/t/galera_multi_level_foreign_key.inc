#
# Execute parent query on node_1 and wait for it to reach node_2
#
--connection node_2
SET GLOBAL DEBUG_DBUG = '+d,sync.wsrep_apply_toi';

--connection node_1
--eval $fk_parent_query

--connection node_2
SET DEBUG_SYNC = "now WAIT_FOR sync.wsrep_apply_toi_reached";

SET SESSION wsrep_sync_wait = 0;
--let $expected_apply_waits = query_get_value("SHOW STATUS LIKE 'wsrep_apply_waits'", Value, 1)
--let $expected_apply_waits = `select $expected_apply_waits + 1`


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
SET GLOBAL DEBUG_DBUG = '+d,wsrep_print_foreign_keys_table';
START TRANSACTION;
--eval $fk_child_query
--let $wait_condition = SELECT COUNT(*) = $fk_mdl_lock_num FROM performance_schema.metadata_locks WHERE OBJECT_SCHEMA='test' AND LOCK_STATUS="GRANTED"
--source include/wait_condition.inc
COMMIT;


#
# Expect the child query to depend on the parent query,
# therefore it should wait for the parent query to
# finish before it can be applied.
#
--connection node_2
--let $status_var = wsrep_apply_waits
--let $status_var_value = $expected_apply_waits
--source include/wait_for_status_var.inc

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
