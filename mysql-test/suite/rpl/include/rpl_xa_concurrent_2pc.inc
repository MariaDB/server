#
# Helper file to run the 1-4(a,b) test cases for rpl_xa_concurrent_2pc,
# with either XA COMMIT or XA ROLLBACK used to complete XA transactions.
#
# Parameters
#   $xa_complete_sym (string) : COMMIT or ROLLBACK, the action used to complete
#                               a prepared XA transaction
#

if (!$xa_complete_sym)
{
    die MTR variable xa_complete_sym not specified, must be either COMMIT or ROLLBACK;
}

--let $is_xac= 0
--let $is_xar= 0

if (`SELECT strcmp("COMMIT", "$xa_complete_sym") = 0`)
{
    --let $is_xac= 1
}

if (`SELECT strcmp("ROLLBACK", "$xa_complete_sym") = 0`)
{
    --let $is_xar= 1
}

if (`SELECT !$is_xar && !$is_xac`)
{
    die MTR variable xa_complete_sym invalid, must be either COMMIT or ROLLBACK;
}


--echo #
--echo # Initialize test data
--connection slave
--source include/stop_slave.inc

--connection master
create table t1 (a int primary key, b int) engine=innodb;

# Slave locks this row before updates to pause transaction progress
--let $hold_row= -1
--let $t1_ctr= 0
--eval insert into t1 values ($hold_row, 0)
--source include/save_master_gtid.inc

--connection slave
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
set @save_debug= @@GLOBAL.debug_dbug;
set @save_par_thds= @@GLOBAL.slave_parallel_threads;
set @save_par_mode= @@GLOBAL.slave_parallel_mode;
set @@GLOBAL.slave_parallel_threads= 4;
set @@GLOBAL.slave_parallel_mode= optimistic;

set statement sql_log_bin=0 for call mtr.add_suppression("Commit failed due to failure of an earlier commit on which this one depends");

--echo #
--echo # Test Case 1-$xa_complete_sym:
--echo # If two XA $xa_complete_sym transactions have different
--echo # XIDs, ensure both phases of both transactions all execute concurrently.
--echo #

--connection slave
# Stop both XAP after their binlogging and before their engine changing
set @@global.debug_dbug= "+d,stop_before_binlog_prepare,stop_after_binlog_prepare";

--connection master
--eval set @@session.gtid_seq_no= 200 + ($is_xar * 1000)
XA START 'x1';
--eval insert into t1 values ($t1_ctr, 0)
--inc $t1_ctr
XA END 'x1';
XA PREPARE 'x1';
--eval XA $xa_complete_sym 'x1'

XA START 'x2';
--eval insert into t1 values ($t1_ctr, 0)
--inc $t1_ctr
XA END 'x2';
XA PREPARE 'x2';
--eval XA $xa_complete_sym 'x2'

--connection slave
--source include/start_slave.inc

# Prove $count_wait number of the workers in W4PT2C.
# It's just 1 with MDEV-33668 that schedules XAC(xid) to a XAP(xid) worker
--let $count_wait= 1
--let $wait_condition=SELECT count(*) = $count_wait FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior transaction to commit"
--source include/wait_condition.inc
# .. while the seq_no 200 XAP is dsync-delayed
--let $count_wait= 1
--let $wait_condition=SELECT count(*) = $count_wait FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "debug sync point: now"
--source include/wait_condition.inc
--echo must be \Empty
XA RECOVER;
set debug_sync= "now signal binlog_xap";

--echo # Make a stop at a point where XA PREPARE (both) have completed (binlogged):
set debug_sync= "now wait_for xa_prepare_binlogged";

--echo # prove XA-$xa_complete_sym 'x1' has completed too
--let $xid = query_get_value("XA RECOVER", data, 1)
--eval SELECT '$xid' = 'x1'

--echo # cleanup: release the seq_no 200 XAP
--let $wait_condition=SELECT count(*) = 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "debug sync point: now"
--source include/wait_condition.inc
set debug_sync= "now signal continue_xap";

--echo # Proof of the full master-slave sync
--source include/sync_with_master_gtid.inc

--let $diff_tables=master:test.t1, slave:test.t1
--source include/diff_tables.inc

--connection slave
--source include/stop_slave.inc
set @@global.debug_dbug= @save_debug;


--echo #
--echo # Test Case 2-$xa_complete_sym:
--echo # Two concurrent 2-phase XA transactions with matching XIDs
--echo # should run one after the other.
--echo #
--connection slave
set @@global.debug_dbug= "+d,stop_before_binlog_prepare,stop_after_binlog_prepare";

--connection master
--eval set @@session.gtid_seq_no= 300 + ($is_xar * 1000)
XA START 'x';
--eval insert into t1 values ($t1_ctr, 1)
--inc $t1_ctr
XA END 'x';
XA PREPARE 'x';
--eval XA $xa_complete_sym 'x'

XA START 'x';
--eval insert into t1 values ($t1_ctr, 2)
--inc $t1_ctr
XA END 'x';
XA PREPARE 'x';
--eval XA $xa_complete_sym 'x'
--source include/save_master_gtid.inc

--connection slave
# remember the slave's initial binlog offset
--let $binlog_pos=query_get_value(SHOW MASTER STATUS, Position, 1)
--let $binlog_file=query_get_value(SHOW MASTER STATUS, File, 1)
--source include/start_slave.inc

--echo # states of workers at the first XAP near binlogging are expected to be as the following
--let $count_wait= 3
--let $wait_condition=SELECT count(*) = $count_wait FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior transaction to commit"
set debug_sync= "now signal binlog_xap";

--echo # Once the first XA PREPARE has binlogged..
set debug_sync= "now wait_for xa_prepare_binlogged";

# with MDEV-33668 the first of the two XAP:s is done now
--let $xid = query_get_value("XA RECOVER", data, 1)
--eval SELECT '$xid' = 'x'

--echo # cleanup: signal XA PREPARE to leave the stage
set debug_sync= "now signal continue_xap";

--echo # Proof of the full master-slave sync
--source include/sync_with_master_gtid.inc
--let $diff_tables=master:test.t1, slave:test.t1
--source include/diff_tables.inc

--connection slave
--source include/stop_slave.inc
set @@global.debug_dbug= @save_debug;


--echo #
--echo # Test Case 3-$xa_complete_sym (Error Case):
--echo # If an XA PREPARE errors while its
--echo # XA $xa_complete_sym is waiting on it, both phases should rollback
--echo # successfully. Note this tests both:
--echo #    a) XA $xa_complete_sym is waiting in group commit (first phase
--echo #       times out in DMLs)
--echo #    b) XA $xa_complete_sym is waiting in group commit, with another XAP
--echo #       with a duplicate XID waiting on it.

--echo # Case a)
--echo # Ensure slave is stopped
--connection slave
--source include/wait_for_slave_to_stop.inc
set @save_lock_wait_timeout= @@GLOBAL.innodb_lock_wait_timeout;
set @save_trans_retries= @@GLOBAL.slave_transaction_retries;
set @@global.innodb_lock_wait_timeout= 1;
set @@global.slave_transaction_retries= 0;

--connection master
XA START 'x';
--eval update t1 set b=b+1 where a=$hold_row
XA END 'x';
XA PREPARE 'x';
--eval XA $xa_complete_sym 'x'
--source include/save_master_gtid.inc

--connection slave1
BEGIN;
--eval select * from t1 where a=$hold_row for update;

--connection slave
--source include/start_slave.inc

--let $slave_sql_errno= 1205
--source include/wait_for_slave_sql_error.inc

--connection slave1
ROLLBACK;

--connection slave
# Stop the IO thread too
--source include/stop_slave_io.inc
set @@global.innodb_lock_wait_timeout= @save_lock_wait_timeout;
set @@global.slave_transaction_retries= @save_trans_retries;

select @@global.gtid_slave_pos;

--echo # Ensure on slave restart, we can re-execute the XA transaction
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
let $diff_tables= master:t1, slave:t1;
--source include/diff_tables.inc
select * from t1;

--source include/stop_slave.inc
set @@global.debug_dbug= @save_debug;


--echo # Case b)
--echo # Ensure slave is stopped
--connection slave
--source include/wait_for_slave_to_stop.inc
set @save_lock_wait_timeout= @@GLOBAL.innodb_lock_wait_timeout;
set @save_trans_retries= @@GLOBAL.slave_transaction_retries;
set @@global.innodb_lock_wait_timeout= 1;
set @@global.slave_transaction_retries= 0;

--connection master
XA START 'x';
--eval update t1 set b=b+1 where a=$hold_row
--eval select * from t1    where a=$hold_row
XA END 'x';
XA PREPARE 'x';
--eval XA $xa_complete_sym 'x'

XA START 'x';
--eval insert into t1 values ($t1_ctr, 0)
--let $new_row_idx= $t1_ctr
--inc $t1_ctr
XA END 'x';
XA PREPARE 'x';
--source include/save_master_gtid.inc
--eval XA $xa_complete_sym 'x'

--connection slave1
BEGIN;
--eval select * from t1 where a=$hold_row for update;

--connection slave
--source include/start_slave.inc

--let $slave_sql_errno= 1205
--source include/wait_for_slave_sql_error.inc

--connection slave1
ROLLBACK;

--echo # There should not be any prepared rows seen by XA RECOVER
XA RECOVER;

--echo # Ensuring data from second XAP *isn't* visible..
if (`select count(*) from t1 where a=$new_row_idx`)
{
    --die Failed, row exists
}
--echo # ..done

--connection slave
--source include/stop_slave_io.inc
set @@global.innodb_lock_wait_timeout= @save_lock_wait_timeout;
set @@global.slave_transaction_retries= @save_trans_retries;

--echo # Ensure on slave restart, we can re-execute the XA transaction
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
let $diff_tables= master:t1, slave:t1;
--source include/diff_tables.inc
select * from t1;

--echo # Ensuring data from second XAP *is* visible..
if ($is_xac)
{
    --let $expected_row_count= 1
}
if ($is_xar)
{
    --let $expected_row_count= 0
}
if (`select count(*) != $expected_row_count from t1 where a=$new_row_idx`)
{
  --die Failed, XA $xa_complete_sym was not observed
}
--echo # ..done

#
--echo # Cleanup
#


--connection slave
set debug_sync='RESET';
set @@global.debug_dbug= @save_debug;
--source include/start_slave.inc

--connection master
DROP TABLE t1;
--source include/save_master_gtid.inc
--let $binlog_file=query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_binlog_events.inc

--connection slave
--source include/sync_with_master_gtid.inc
--let $binlog_file=query_get_value(SHOW MASTER STATUS, File, 1)
--let $filter_cid=1
--source include/show_binlog_events2.inc

--source include/stop_slave.inc
set @@GLOBAL.slave_parallel_threads= @save_par_thds;
set @@GLOBAL.slave_parallel_mode= @save_par_mode;
--source include/start_slave.inc
