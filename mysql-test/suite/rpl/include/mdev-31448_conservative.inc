--connection master
create table t1 (a int) engine=innodb;
create table t2 (a int) engine=innodb;
insert into t1 values (1);
--source include/save_master_gtid.inc

--connection slave
call mtr.add_suppression("Slave: Commit failed due to failure of an earlier commit on which this one depends");

--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
set @save.slave_parallel_threads= @@global.slave_parallel_threads;
set @save.slave_parallel_mode= @@global.slave_parallel_mode;
set @@global.slave_parallel_threads= 3;
set @@global.slave_parallel_mode= CONSERVATIVE;
--connection slave1
BEGIN;
update t1 set a=2 where a=1;

--connection master
SET @old_dbug= @@SESSION.debug_dbug;
SET @@SESSION.debug_dbug="+d,binlog_force_commit_id";

# GCO 1
SET @commit_id= 10000;
# T1
update t1 set a=2 where a=1;
# T2
insert into t2 values (1);

# GCO 2
SET @commit_id= 10001;
# T3
insert into t1 values (3);

--connection slave
--source include/start_slave.inc

--let $wait_condition= SELECT count(*)=1 FROM information_schema.processlist WHERE state LIKE 'Update_rows_log_event::find_row(-1)%' and  command LIKE 'Slave_worker';
--source include/wait_condition.inc
--let $wait_condition= SELECT count(*)=1 FROM information_schema.processlist WHERE state LIKE 'Waiting for prior transaction to commit%' and  command LIKE 'Slave_worker';
--source include/wait_condition.inc
--let $wait_condition= SELECT count(*)=1 FROM information_schema.processlist WHERE state LIKE 'Waiting for prior transaction to start commit%' and  command LIKE 'Slave_worker';
--source include/wait_condition.inc

--let $t3_tid= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE STATE LIKE 'Waiting for prior transaction to start commit%'`
--evalp kill $t3_tid

--connection slave1
commit;

--connection slave
--let $slave_timeout=1032
--source include/wait_for_slave_sql_to_stop.inc

update t1 set a=1 where a=2;
set @@global.slave_parallel_threads = @save.slave_parallel_threads;
set @@global.slave_parallel_mode = @save.slave_parallel_mode;
--source include/start_slave.inc

--echo #
--echo # Cleanup
--connection master
DROP TABLE t1, t2;
--source include/save_master_gtid.inc

--connection slave
--source include/sync_with_master_gtid.inc
