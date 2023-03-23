# ==== Purpose ====
#
# Test verifies that, in parallel replication, transaction failure notification
# is propagated to all the workers. Workers should abort the execution of
# transaction event groups, whose event positions are higher than the failing
# transaction group.
#
# ==== Implementation ====
#
# Steps:
#    0 - Create a table t1 on master which has a primary key. Enable parallel
#        replication on slave with slave_parallel_mode='optimistic' and
#        slave_parallel_threads=3.
#    1 - On slave start a transaction and execute a local INSERT statement
#        which will insert value 32. This is done to block the INSERT coming
#        from master.
#    2 - On master execute an INSERT statement with value 32, so that it is
#        blocked on slave.
#    3 - On slave enable a debug sync point such that it holds the worker thread
#        execution as soon as work is scheduled to it.
#    4 - INSERT value 33 on master. It will be held on slave by other worker
#        thread due to debug simulation.
#    5 - INSERT value 34 on master.
#    6 - On slave, enusre that INSERT 34 has reached a state where it waits for
#        its prior transactions to commit.
#    7 - Commit the local INSERT 32 on slave server so that first worker will
#        error out.
#    8 - Now send a continue signal to second worker processing 33. It should
#        wakeup and propagate the error to INSERT 34.
#    9 - Upon slave stop due to error, check that no rows are found after the
#        failed INSERT 32.
#
# ==== References ====
#
# MDEV-20645: Replication consistency is broken as workers miss the error
#             notification from an earlier failed group.
#

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/have_binlog_format_statement.inc
--source include/master-slave.inc

--enable_connect_log
--connection server_2
--source include/stop_slave.inc
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode=@@GLOBAL.slave_parallel_mode;
SET @old_debug= @@GLOBAL.debug_dbug;
SET GLOBAL slave_parallel_mode='optimistic';
SET GLOBAL slave_parallel_threads= 3;
CHANGE MASTER TO master_use_gtid=slave_pos;
CALL mtr.add_suppression("Commit failed due to failure of an earlier commit on which this one depends");
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=InnoDB;
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc

--connect (con_temp2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
BEGIN;
INSERT INTO t1 VALUES (32);

--connection server_1
INSERT INTO t1 VALUES (32);

--connection server_2
--let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist WHERE info like "INSERT INTO t1 VALUES (32)"
--source include/wait_condition.inc
SET GLOBAL debug_dbug="+d,hold_worker_on_schedule";
SET debug_sync="debug_sync_action SIGNAL reached_pause WAIT_FOR continue_worker";

--connection server_1
SET gtid_seq_no=100;
INSERT INTO t1 VALUES (33);

--connection server_2
SET debug_sync='now WAIT_FOR reached_pause';

--connection server_1
INSERT INTO t1 VALUES (34);

--connection server_2
--let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist WHERE state like "Waiting for prior transaction to commit"
--source include/wait_condition.inc
--connection con_temp2
COMMIT;

# Clean up.
--connection server_2
--source include/stop_slave.inc
--let $assert_cond= COUNT(*) = 0 FROM t1 WHERE a>32
--let $assert_text= table t1 should have zero rows where a>32
--source include/assert.inc
SELECT * FROM t1 WHERE a>32;
DELETE FROM t1 WHERE a=32;

SET GLOBAL slave_parallel_threads=@old_parallel_threads;
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
SET GLOBAL debug_dbug=@old_debug;
SET DEBUG_SYNC= 'RESET';
--source include/start_slave.inc

--connection server_1
DROP TABLE t1;
--disable_connect_log
--source include/rpl_end.inc
