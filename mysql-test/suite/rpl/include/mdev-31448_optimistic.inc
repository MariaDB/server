--echo # MDEV-31448 OOO finish event group by killed worker
# The test demonstrates how a killed worker access gco lists
# in finish_event_group() out-of-order to fire
#       DBUG_ASSERT(!tmp_gco->next_gco || tmp_gco->last_sub_id > sub_id);
# in the buggy version.

--echo # Initialize test data
--connection master
create table t1 (a int) engine=innodb;
create table t2 (a int) engine=innodb;

insert into t1 values (1);
--source include/save_master_gtid.inc

--connection slave
call mtr.add_suppression("Connection was killed");
call mtr.add_suppression("Can.t find record");

--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
set @save.slave_parallel_threads= @@global.slave_parallel_threads;
set @save.slave_parallel_mode= @@global.slave_parallel_mode;
set @@global.slave_parallel_threads= 3;
set @@global.slave_parallel_mode= OPTIMISTIC;

--connection slave1
begin;
update t1 set a=2 where a=1;

--connection master
set @old_dbug= @@session.debug_dbug;
set @@session.debug_dbug="+d,binlog_force_commit_id";

# GCO 1
set @commit_id= 10000;
# T1
update t1 set a=2 where a=1;

if (!$killed_trx_commits)
{
set @commit_id= 10001;
# T2
set statement skip_parallel_replication=1 for  insert into t2 values (1);
}

if ($killed_trx_commits)
{
insert into t2 values (1);
}
# GCO 2
# T3
drop table t2;

--connection slave
--source include/start_slave.inc

--echo # wait for T1
--let $wait_condition= SELECT count(*)=1 FROM information_schema.processlist WHERE state LIKE 'Update_rows_log_event::find_row(-1)%' and  command LIKE 'Slave_worker';
--source include/wait_condition.inc

--echo # wait for T2
--let $wait_condition= SELECT count(*)=1 FROM information_schema.processlist WHERE state LIKE 'Waiting for prior transaction to commit%' and  command LIKE 'Slave_worker';
--source include/wait_condition.inc
--let $t2_tid= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE STATE LIKE 'Waiting for prior transaction to commit%' and  command LIKE 'Slave_worker'`
--echo # wait for T3
--let $wait_condition= SELECT count(*)=1 FROM information_schema.processlist WHERE state LIKE  'Waiting for prior transaction to start commit%' and  command LIKE 'Slave_worker';
--source include/wait_condition.inc

--evalp kill $t2_tid
# give some little time for T2 to re-sink into the same state
--let $slave_param=Last_Errno
--let $slave_param_value=1927
--source include/wait_for_slave_param.inc
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
drop table t1;
--source include/save_master_gtid.inc

--connection slave
--source include/sync_with_master_gtid.inc

