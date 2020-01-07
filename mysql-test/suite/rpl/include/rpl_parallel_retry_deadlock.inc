--echo *** MDEV-7326 Server deadlock in connection with parallel replication ***
--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode=@@GLOBAL.slave_parallel_mode;
--source include/stop_slave.inc
# Test assumes that 'conservative' mode is in effect. i.e
# Do not start parallel execution of this event group until all prior groups
# have reached the commit phase. Refer 'rpl_parallel_start_waiting_for_prior'
# debug simumation.
SET GLOBAL slave_parallel_mode='conservative';
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE t6 (a INT) ENGINE=MyISAM;
--save_master_pos

--connection server_2
--sync_with_master

--connection server_1
# Use a stored function to inject a debug_sync into the appropriate THD.
# The function does nothing on the master, and on the slave it injects the
# desired debug_sync action(s).
SET sql_log_bin=0;
--delimiter ||
CREATE FUNCTION foo(x INT, d1 VARCHAR(500), d2 VARCHAR(500))
  RETURNS INT DETERMINISTIC
  BEGIN
    RETURN x;
  END
||
--delimiter ;
SET sql_log_bin=1;

--connection server_2
SET sql_log_bin=0;
--delimiter ||
CREATE FUNCTION foo(x INT, d1 VARCHAR(500), d2 VARCHAR(500))
  RETURNS INT DETERMINISTIC
  BEGIN
    IF d1 != '' THEN
      SET debug_sync = d1;
    END IF;
    IF d2 != '' THEN
      SET debug_sync = d2;
    END IF;
    RETURN x;
  END
||
--delimiter ;
SET sql_log_bin=1;

# We use three transactions, each in a separate group commit.
#   T1 does mark_start_commit(), then gets a deadlock error.
#   T2 wakes up and starts running
#   T1 does unmark_start_commit()
#   T3 goes to wait for T2 to start its commit
#   T2 does mark_start_commit()
# The bug was that at this point, T3 got deadlocked. Because T1 has unmarked(),
# T3 did not yet see the count_committing_event_groups reach its target value
# yet. But when T1 later re-did mark_start_commit(), it failed to send a wakeup
# to T3.

--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=3;
SET GLOBAL debug_dbug="+d,rpl_parallel_simulate_temp_err_xid";
--source include/start_slave.inc

--connection server_1
SET @old_format= @@SESSION.binlog_format;
SET binlog_format= STATEMENT;
# This debug_sync will linger on and be used to control T3 later.
INSERT INTO t1 VALUES (foo(50,
   "rpl_parallel_start_waiting_for_prior SIGNAL t3_ready",
   "rpl_parallel_end_of_group SIGNAL prep_ready WAIT_FOR prep_cont"));
--save_master_pos
--connection  server_2
# Wait for the debug_sync point for T3 to be set. But let the preparation
# transaction remain hanging, so that T1 and T2 will be scheduled for the
# remaining two worker threads.
SET DEBUG_SYNC= "now WAIT_FOR prep_ready";

--connection server_1
INSERT INTO t2 VALUES (foo(50,
   "rpl_parallel_simulate_temp_err_xid SIGNAL t1_ready1 WAIT_FOR t1_cont1",
   "rpl_parallel_retry_after_unmark SIGNAL t1_ready2 WAIT_FOR t1_cont2"));
--save_master_pos

--connection server_2
SET DEBUG_SYNC= "now WAIT_FOR t1_ready1";
# T1 has now done mark_start_commit(). It will later do a rollback and retry.

--connection server_1
# Use a MyISAM table for T2 and T3, so they do not trigger the 
# rpl_parallel_simulate_temp_err_xid DBUG insertion on XID event.
INSERT INTO t1 VALUES (foo(51,
   "rpl_parallel_before_mark_start_commit SIGNAL t2_ready1 WAIT_FOR t2_cont1",
   "rpl_parallel_after_mark_start_commit SIGNAL t2_ready2"));

--connection server_2
SET DEBUG_SYNC= "now WAIT_FOR t2_ready1";
# T2 has now started running, but has not yet done mark_start_commit()
SET DEBUG_SYNC= "now SIGNAL t1_cont1";
SET DEBUG_SYNC= "now WAIT_FOR t1_ready2";
# T1 has now done unmark_start_commit() in preparation for its retry.

--connection server_1
INSERT INTO t1 VALUES (52);
SET BINLOG_FORMAT= @old_format;
SELECT * FROM t2 WHERE a>=50 ORDER BY a;
SELECT * FROM t1 WHERE a>=50 ORDER BY a;

--connection server_2
# Let the preparation transaction complete, so that the same worker thread
# can continue with the transaction T3.
SET DEBUG_SYNC= "now SIGNAL prep_cont";
SET DEBUG_SYNC= "now WAIT_FOR t3_ready";
# T3 has now gone to wait for T2 to start committing
SET DEBUG_SYNC= "now SIGNAL t2_cont1";
SET DEBUG_SYNC= "now WAIT_FOR t2_ready2";
# T2 has now done mark_start_commit().
# Let things run, and check that T3 does not get deadlocked.
SET DEBUG_SYNC= "now SIGNAL t1_cont2";
--sync_with_master

--connection server_1
--save_master_pos
--connection server_2
--sync_with_master
SELECT * FROM t2 WHERE a>=50 ORDER BY a;
SELECT * FROM t1 WHERE a>=50 ORDER BY a;
SET DEBUG_SYNC="reset";

# Re-spawn the worker threads to remove any DBUG injections or DEBUG_SYNC.
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc


--echo *** MDEV-7326 Server deadlock in connection with parallel replication ***
# Similar to the previous test, but with T2 and T3 in the same GCO.
# We use three transactions, T1 in one group commit and T2/T3 in another.
#   T1 does mark_start_commit(), then gets a deadlock error.
#   T2 wakes up and starts running
#   T1 does unmark_start_commit()
#   T3 goes to wait for T1 to start its commit
#   T2 does mark_start_commit()
# The bug was that at this point, T3 got deadlocked. T2 increments the
# count_committing_event_groups but does not signal T3, as they are in
# the same GCO. Then later when T1 increments, it would also not signal
# T3, because now the count_committing_event_groups is not equal to the
# wait_count of T3 (it is one larger).

--connect (con_temp3,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--connect (con_temp4,127.0.0.1,root,,test,$SERVER_MYPORT_1,)

--connection server_2
--source include/stop_slave.inc
SET @old_parallel_mode= @@GLOBAL.slave_parallel_mode;
SET GLOBAL slave_parallel_mode='conservative';
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=3;
SET GLOBAL debug_dbug="+d,rpl_parallel_simulate_temp_err_xid";
--source include/start_slave.inc

--connection server_1
SET @old_format= @@SESSION.binlog_format;
SET binlog_format= STATEMENT;
# This debug_sync will linger on and be used to control T3 later.
INSERT INTO t1 VALUES (foo(60,
   "rpl_parallel_start_waiting_for_prior SIGNAL t3_ready",
   "rpl_parallel_end_of_group SIGNAL prep_ready WAIT_FOR prep_cont"));
--save_master_pos
--connection  server_2
# Wait for the debug_sync point for T3 to be set. But let the preparation
# transaction remain hanging, so that T1 and T2 will be scheduled for the
# remaining two worker threads.
SET DEBUG_SYNC= "now WAIT_FOR prep_ready";

--connection server_1
INSERT INTO t2 VALUES (foo(60,
   "rpl_parallel_simulate_temp_err_xid SIGNAL t1_ready1 WAIT_FOR t1_cont1",
   "rpl_parallel_retry_after_unmark SIGNAL t1_ready2 WAIT_FOR t1_cont2"));
--save_master_pos

--connection server_2
SET DEBUG_SYNC= "now WAIT_FOR t1_ready1";
# T1 has now done mark_start_commit(). It will later do a rollback and retry.

# Do T2 and T3 in a single group commit.
# Use a MyISAM table for T2 and T3, so they do not trigger the 
# rpl_parallel_simulate_temp_err_xid DBUG insertion on XID event.
--connection con_temp3
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
SET binlog_format=statement;
send INSERT INTO t1 VALUES (foo(61,
   "rpl_parallel_before_mark_start_commit SIGNAL t2_ready1 WAIT_FOR t2_cont1",
   "rpl_parallel_after_mark_start_commit SIGNAL t2_ready2"));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con_temp4
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send INSERT INTO t6 VALUES (62);

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';
SET debug_sync='now SIGNAL master_cont1';

--connection con_temp3
REAP;
--connection con_temp4
REAP;

--connection server_1
SET debug_sync='RESET';
SET BINLOG_FORMAT= @old_format;
SELECT * FROM t2 WHERE a>=60 ORDER BY a;
SELECT * FROM t1 WHERE a>=60 ORDER BY a;
SELECT * FROM t6 WHERE a>=60 ORDER BY a;

--connection server_2
SET DEBUG_SYNC= "now WAIT_FOR t2_ready1";
# T2 has now started running, but has not yet done mark_start_commit()
SET DEBUG_SYNC= "now SIGNAL t1_cont1";
SET DEBUG_SYNC= "now WAIT_FOR t1_ready2";
# T1 has now done unmark_start_commit() in preparation for its retry.

--connection server_2
# Let the preparation transaction complete, so that the same worker thread
# can continue with the transaction T3.
SET DEBUG_SYNC= "now SIGNAL prep_cont";
SET DEBUG_SYNC= "now WAIT_FOR t3_ready";
# T3 has now gone to wait for T2 to start committing
SET DEBUG_SYNC= "now SIGNAL t2_cont1";
SET DEBUG_SYNC= "now WAIT_FOR t2_ready2";
# T2 has now done mark_start_commit().
# Let things run, and check that T3 does not get deadlocked.
SET DEBUG_SYNC= "now SIGNAL t1_cont2";
--sync_with_master

--connection server_1
--save_master_pos
--connection server_2
--sync_with_master
SELECT * FROM t2 WHERE a>=60 ORDER BY a;
SELECT * FROM t1 WHERE a>=60 ORDER BY a;
SELECT * FROM t6 WHERE a>=60 ORDER BY a;
SET DEBUG_SYNC="reset";

# Clean up.
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc

--connection server_1
DROP function foo;
DROP TABLE t1,t2,t6;
--disconnect con_temp3
--disconnect con_temp4
SET DEBUG_SYNC= 'RESET';

--source include/rpl_end.inc
