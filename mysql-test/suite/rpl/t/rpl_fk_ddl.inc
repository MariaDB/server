# Set slave_exec_mode.
connection slave;
SET @saved_dbug = @@GLOBAL.debug_dbug;
--let $default_fk_slave_exec_mode= `SELECT @@global.slave_exec_mode`

# Set debug sync point to block parent query on slave. 
set @@global.debug_dbug= "+d,sync.rm_table_no_locks";

--eval set global slave_exec_mode='$fk_slave_exec_mode'


# Execute parent query.
connection master;
--eval $fk_parent_query


# Wait for parent query to take mdl lock.
--connection slave
set debug_sync='now WAIT_FOR after_lock_table';
--let $wait_condition = SELECT COUNT(*) = $fk_mdl_lock_parent_num FROM performance_schema.metadata_locks WHERE OBJECT_SCHEMA='test' AND LOCK_STATUS="GRANTED"
--source include/wait_condition.inc


# Now execute child query.
--connection master
START TRANSACTION;
--eval $fk_child_query
--let $wait_condition = SELECT COUNT(*) = $fk_mdl_lock_child_num FROM performance_schema.metadata_locks WHERE OBJECT_SCHEMA='test' AND LOCK_STATUS="GRANTED"
--source include/wait_condition.inc
COMMIT;


# Cleanup
connection slave;
SET DEBUG_SYNC= 'RESET';
set debug_sync='now SIGNAL continue';
SET @@GLOBAL.debug_dbug = @saved_dbug;
--eval set global slave_exec_mode='$default_fk_slave_exec_mode'
