# set slave_exec_mode.
connection slave;
set @saved_dbug = @@global.debug_dbug;
set @saved_exec_mode= @@global.slave_exec_mode;

# set debug sync point to block parent query on slave.
set @@global.debug_dbug= "+d,rows_log_event_after_open_table";

--source include/stop_slave.inc
--eval set global slave_exec_mode='$fk_slave_exec_mode'
--source include/start_slave.inc

--echo # INSERT on master so it is replicated to slave (where its execution will be blocked after taking locks)
connection master;
INSERT INTO t1 (f2) VALUES (0);

--connection slave
set debug_sync='now WAIT_FOR after_open_table';

--echo # test.t1 should always be locked with SHARED_WRITE
--let $wait_condition = SELECT COUNT(*)=1 FROM performance_schema.metadata_locks WHERE OBJECT_SCHEMA='test' AND OBJECT_NAME='t1' AND LOCK_STATUS='GRANTED' AND LOCK_TYPE='SHARED_WRITE'
--source include/wait_condition.inc

--let $t2_lock_type= SHARED_READ
if (`SELECT strcmp("$fk_slave_exec_mode", "IDEMPOTENT") = 0`)
{
  --let $t2_lock_type= SHARED_WRITE
}

--echo # test.t2 lock type should be SHARED_WRITE only when slave_exec_mode is IDEMPOTENT. Current mode: $fk_slave_exec_mode
--let $wait_condition = SELECT COUNT(*)=1 FROM performance_schema.metadata_locks WHERE OBJECT_SCHEMA='test' AND OBJECT_NAME='t2' AND LOCK_STATUS='GRANTED' AND LOCK_TYPE='$t2_lock_type'
--source include/wait_condition.inc

# Cleanup
connection slave;
SET DEBUG_SYNC= 'RESET';
set debug_sync='now SIGNAL continue_rows_ev';
SET @@GLOBAL.debug_dbug = @saved_dbug;
SET @@GLOBAL.slave_exec_mode = @saved_exec_mode;
