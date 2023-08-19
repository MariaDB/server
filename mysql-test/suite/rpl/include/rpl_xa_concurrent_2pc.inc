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
RESET SLAVE;
set @@global.gtid_slave_pos= "";

if ($is_xac)
{
--connection slave
RESET MASTER;

--connection master
RESET MASTER;
}

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
--echo # Test Case 1: Ensure that a 2-phase XA transaction has its XA PREPARE
--echo # and XA $xa_complete_sym run concurrently. That is, the
--echo # XA $xa_complete_sym will wait at group commit until the XA PREPARE
--echo # binlogs, and then it will wait again until the XA PREPARE finishes
--echo # preparing in all engines. At this point, the XA $xa_complete_sym will
--echo # run to completion.
--connection master
# For worker thread to hold XAP at dequeue time via debug_sync through
# `hold_worker_on_schedule`.
set @@session.gtid_seq_no= 100;
XA START 'x';
--eval insert into t1 values ($t1_ctr, 0)
--inc $t1_ctr
XA END 'x';
XA PREPARE 'x';
--eval XA $xa_complete_sym 'x'
--source include/save_master_gtid.inc

--connection slave
# For worker to stop at dequeue event time and after binlogging XA PREPARE
set @@global.debug_dbug= "+d,hold_worker_on_schedule,stop_after_binlog_prepare";
--source include/start_slave.inc

--echo # Waiting for XAP to pause when it is pulled from the queue
set debug_sync= "now wait_for reached_pause";

--echo # Before the XA PREPARE executes, the XA $xa_complete_sym should wait in group commit..
--let $wait_condition=SELECT count(*) = 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior transaction to commit"
--source include/wait_condition.inc
--echo # ..done


--echo # Execute the XA PREPARE
set debug_sync= "now signal continue_worker";

--echo # Wait for XA PREPARE to have binlogged, but hold it before it prepares in engines
set debug_sync= "now wait_for xa_prepare_binlogged";

--echo # The XA $xa_complete_sym should move on from binlog to wait for the XA PREPARE to complete in engines
--let $wait_condition=SELECT count(*) = 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior xa transaction"
--source include/wait_condition.inc
--echo # ..done

--echo # Signal the XAP to complete in engines (which will automatically signal XAC)
set debug_sync= "now signal continue_xap";

--source include/sync_with_master_gtid.inc

--let $diff_tables=master:test.t1, slave:test.t1
--source include/diff_tables.inc

--connection slave
--source include/stop_slave.inc
set @@global.debug_dbug= @save_debug;


--echo #
--echo # Test Case 2: If two XA $xa_complete_sym transactions have different
--echo # XIDs, ensure both phases of both transactions all execute concurrently.
--echo #

--echo # Ensure slave is stopped
--connection slave
--source include/wait_for_slave_to_stop.inc

# Stop both XAP after their binlogging and before their engine changing
set @@global.debug_dbug= "+d,stop_before_binlog_prepare,stop_after_binlog_prepare";

--connection master
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
--source include/save_master_gtid.inc

--connection slave
--source include/start_slave.inc

# This stage is necessary to avoid XAP_1 <-register-> XAC_2 race in that
# XAC_2 may get stuck in the below WFPT2C state all time until XAP_1 has finished.
# Prove the workers' status are like the following:
--let $count_wait= 2
--let $wait_condition=SELECT count(*) = $count_wait FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior transaction to commit"
--source include/wait_condition.inc

--let $count_wait= 2
--let $wait_condition=SELECT count(*) = $count_wait FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "debug sync point: now"
--source include/wait_condition.inc
set debug_sync= "now signal binlog_xap";

# wait for two XAP:s arrive at their stations (XAP_1 to next one after binlog is done)
--let $wait_condition=SELECT count(*) >= 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "debug sync point: now"
--source include/wait_condition.inc
set debug_sync= "now signal binlog_xap";

--echo # Ensuring both phases of both transactions all execute concurrently
# Waiting for both XA "COMPLETE"s to binlog proves this, as they would not pass
# group commit if their preceding XA PREPAREs had not also binlogged
--let $count_xa_wait_workers= 1

--let $wait_condition=SELECT count(*) = $count_xa_wait_workers FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior xa transaction"
--source include/wait_condition.inc
--echo # ..done

--echo # Verify XA PREPARE has binlogged
set debug_sync= "now wait_for xa_prepare_binlogged";

--echo # Signal the XAPs to complete in engines (which will automatically signal XACs)
set debug_sync= "now signal continue_xap";

--let $wait_condition=SELECT count(*) = 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "debug sync point: now"
--source include/wait_condition.inc
set debug_sync= "now signal continue_xap";

--source include/sync_with_master_gtid.inc

--let $diff_tables=master:test.t1, slave:test.t1
--source include/diff_tables.inc

--connection slave
--source include/stop_slave.inc
set @@global.debug_dbug= @save_debug;


--echo #
--echo # Test Case 3: Two current 2-phase XA transactions with matching XIDs
--echo # should run one after the other, while each transaction still allows
--echo # its XA PREPARE and XA $xa_complete_sym to run concurrently

--echo # Ensure slave is stopped
--connection slave
--source include/wait_for_slave_to_stop.inc

# Stop both XAP after their binlogging and before their engine changing
set @@global.debug_dbug= "+d,stop_before_binlog_prepare,stop_after_binlog_prepare,stop_after_binlog_cor_by_xid";

--connection master
XA START 'x';
--eval insert into t1 values ($t1_ctr, 0)
--inc $t1_ctr
XA END 'x';
XA PREPARE 'x';
--eval XA $xa_complete_sym 'x'

XA START 'x';
--eval insert into t1 values ($t1_ctr, 0)
--inc $t1_ctr
XA END 'x';
XA PREPARE 'x';
--eval XA $xa_complete_sym 'x'
--source include/save_master_gtid.inc

--connection slave
--source include/start_slave.inc

# This stage is necessary to avoid XAP_1 <-register-> XAC_2 race in that
# XAC_2 may get stuck in the below WFPT2C state all time until XAP_1 has finished.
--let $count_wait= 3
--let $wait_condition=SELECT count(*) = $count_wait FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior transaction to commit"

--source include/wait_condition.inc
--let $wait_condition=SELECT count(*) = 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "debug sync point: now"
--source include/wait_condition.inc
set debug_sync= "now signal binlog_xap";

--echo # Verify first XA PREPARE has binlogged
set debug_sync= "now wait_for xa_prepare_binlogged";

--echo # Ensure first XA transaction is running concurrently
--let $wait_condition=SELECT count(*) = 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior xa transaction" AND info LIKE "XA $xa_complete_sym%"
--source include/wait_condition.inc

--echo # The second XA transaction's XAP can *not* be in waiting for the first transaction
--let $wait_condition=SELECT count(*) = 0 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior xa transaction" AND info LIKE "XA START%"
#--source include/wait_condition.inc
--eval $wait_condition

--echo # Signal first XA PREPARE to complete
set debug_sync= "now signal continue_xap";

--echo # Wait for first XA $xa_complete_sym to binlog
set debug_sync= "now wait_for xa_cor_binlogged";

--echo # Ensure second XA PREPARE doesn't begin yet because the XAC hadn't released its XID
--let $wait_condition=SELECT count(*) = 2 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior transaction to commit"
--source include/wait_condition.inc

--echo # Signal first XA $xa_complete_sym to complete
set debug_sync= "now signal continue_xa_cor";

--echo # Wait for second XA PREPARE to binlogged
--echo # First pass through binlog_xap
--let $wait_condition=SELECT count(*) = 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "debug sync point: now"
--source include/wait_condition.inc
set debug_sync= "now signal binlog_xap";
set debug_sync= "now wait_for xa_prepare_binlogged";

--echo # Ensure second XA $xa_complete_sym is concurrent with XAP
--let $wait_condition=SELECT count(*) = 1 FROM information_schema.processlist WHERE command = 'Slave_worker' AND state LIKE "Waiting for prior xa transaction"
--source include/wait_condition.inc

--echo # Signal second XA transaction to complete
set debug_sync= "now signal continue_xap";
set debug_sync= "now wait_for xa_cor_binlogged";
set debug_sync= "now signal continue_xa_cor";

--source include/sync_with_master_gtid.inc

--let $diff_tables=master:test.t1, slave:test.t1
--source include/diff_tables.inc

--connection slave
--source include/stop_slave.inc
set @@global.debug_dbug= @save_debug;


--echo #
--echo # Test Case 4 (Error Case): If an XA PREPARE errors while its
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

--let $slave_sql_errno= 1213
--source include/wait_for_slave_sql_error.inc

--connection slave1
ROLLBACK;

--connection slave
# Stop the IO thread too
--source include/stop_slave_io.inc
set @@global.innodb_lock_wait_timeout= @save_lock_wait_timeout;
set @@global.slave_transaction_retries= @save_trans_retries;

--echo # Ensure on slave restart, we can re-execute the XA transaction
--source include/start_slave.inc
--source include/save_master_gtid.inc
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

--let $slave_sql_errno= 1213
--source include/wait_for_slave_sql_error.inc

--connection slave1
ROLLBACK;

--echo # There should not be any prepared rows seen by XA RECOVER
XA RECOVER;

--echo # Ensuring data from second XAP isn't visible..
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
--source include/save_master_gtid.inc
--source include/stop_slave.inc
set @@global.debug_dbug= @save_debug;
--source include/start_slave.inc

--echo # Ensuring data from second XAP is visible..
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

--echo #
--echo # Cleanup
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
