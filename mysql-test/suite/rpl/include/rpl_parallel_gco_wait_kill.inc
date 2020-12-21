--echo *** Test killing thread that is waiting to start transaction until previous transaction commits ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/have_binlog_format_statement.inc
--source include/master-slave.inc

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode= @@GLOBAL.slave_parallel_mode;
--source include/stop_slave.inc
SET sql_log_bin=0;
CALL mtr.add_suppression("Query execution was interrupted");
CALL mtr.add_suppression("Slave: Connection was killed");
CALL mtr.add_suppression("Commit failed due to failure of an earlier commit on which this one depends");
SET sql_log_bin=1;
SET GLOBAL slave_parallel_threads=10;
SET GLOBAL slave_parallel_mode= 'conservative';
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
--connect (con_temp3,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--connect (con_temp4,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--connect (con_temp5,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
# MDEV-515 takes X-lock on the table for the first insert.
# So concurrent insert won't happen on the table
INSERT INTO t3 VALUES(100, 100);

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
# We need to restart all parallel threads for the new global setting to
# be copied to the session-level values.
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=4;
--source include/start_slave.inc


# We set up four transactions T1, T2, T3, and T4 on the master. T2, T3, and T4
# can run in parallel with each other (same group commit and commit id),
# but not in parallel with T1.
#
# We use four worker threads, each Ti will be queued on each their own
# worker thread. We will delay T1 commit, T3 will wait for T1 to begin
# commit before it can start. We will kill T3 during this wait, and
# check that everything works correctly.
#
# It is rather tricky to get the correct thread id of the worker to kill.
# We start by injecting four dummy transactions in a debug_sync-controlled
# manner to be able to get known thread ids for the workers in a pool with
# just 4 worker threads. Then we let in each of the real test transactions
# T1-T4 one at a time in a way which allows us to know which transaction
# ends up with which thread id.

--connection server_1
SET gtid_domain_id=2;
BEGIN;
# This debug_sync will linger on and be used to control T4 later.
INSERT INTO t3 VALUES (70, foo(70,
       'rpl_parallel_start_waiting_for_prior SIGNAL t4_waiting', ''));
INSERT INTO t3 VALUES (60, foo(60,
       'ha_write_row_end SIGNAL d2_query WAIT_FOR d2_cont2',
       'rpl_parallel_end_of_group SIGNAL d2_done WAIT_FOR d2_cont'));
COMMIT;
SET gtid_domain_id=0;

--connection server_2
SET debug_sync='now WAIT_FOR d2_query';
--let $d2_thd_id= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE INFO LIKE '%foo(60%' AND INFO NOT LIKE '%LIKE%'`

--connection server_1
SET gtid_domain_id=1;
BEGIN;
# These debug_sync's will linger on and be used to control T3 later.
INSERT INTO t3 VALUES (61, foo(61,
       'rpl_parallel_start_waiting_for_prior SIGNAL t3_waiting',
       'rpl_parallel_start_waiting_for_prior_killed SIGNAL t3_killed'));
INSERT INTO t3 VALUES (62, foo(62,
       'ha_write_row_end SIGNAL d1_query WAIT_FOR d1_cont2',
       'rpl_parallel_end_of_group SIGNAL d1_done WAIT_FOR d1_cont'));
COMMIT;
SET gtid_domain_id=0;

--connection server_2
SET debug_sync='now WAIT_FOR d1_query';
--let $d1_thd_id= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE INFO LIKE '%foo(62%' AND INFO NOT LIKE '%LIKE%'`

--connection server_1
SET gtid_domain_id=0;
INSERT INTO t3 VALUES (63, foo(63,
       'ha_write_row_end SIGNAL d0_query WAIT_FOR d0_cont2',
       'rpl_parallel_end_of_group SIGNAL d0_done WAIT_FOR d0_cont'));

--connection server_2
SET debug_sync='now WAIT_FOR d0_query';
--let $d0_thd_id= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE INFO LIKE '%foo(63%' AND INFO NOT LIKE '%LIKE%'`

--connection server_1
SET gtid_domain_id=3;
BEGIN;
# These debug_sync's will linger on and be used to control T2 later.
INSERT INTO t3 VALUES (68, foo(68,
       'rpl_parallel_start_waiting_for_prior SIGNAL t2_waiting', ''));
INSERT INTO t3 VALUES (69, foo(69,
       'ha_write_row_end SIGNAL d3_query WAIT_FOR d3_cont2',
       'rpl_parallel_end_of_group SIGNAL d3_done WAIT_FOR d3_cont'));
COMMIT;
SET gtid_domain_id=0;

--connection server_2
SET debug_sync='now WAIT_FOR d3_query';
--let $d3_thd_id= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE INFO LIKE '%foo(69%' AND INFO NOT LIKE '%LIKE%'`

SET debug_sync='now SIGNAL d2_cont2';
SET debug_sync='now WAIT_FOR d2_done';
SET debug_sync='now SIGNAL d1_cont2';
SET debug_sync='now WAIT_FOR d1_done';
SET debug_sync='now SIGNAL d0_cont2';
SET debug_sync='now WAIT_FOR d0_done';
SET debug_sync='now SIGNAL d3_cont2';
SET debug_sync='now WAIT_FOR d3_done';

# Now prepare the real transactions T1, T2, T3, T4 on the master.

--connection con_temp3
# Create transaction T1.
INSERT INTO t3 VALUES (64, foo(64,
       'rpl_parallel_before_mark_start_commit SIGNAL t1_waiting WAIT_FOR t1_cont', ''));

# Create transaction T2, as a group commit leader on the master.
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2 WAIT_FOR master_cont2';
send INSERT INTO t3 VALUES (65, foo(65, '', ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';

--connection con_temp4
# Create transaction T3, participating in T2's group commit.
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued3';
send INSERT INTO t3 VALUES (66, foo(66, '', ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued3';

--connection con_temp5
# Create transaction T4, participating in group commit with T2 and T3.
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued4';
send INSERT INTO t3 VALUES (67, foo(67, '', ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued4';
SET debug_sync='now SIGNAL master_cont2';

--connection con_temp3
REAP;
--connection con_temp4
REAP;
--connection con_temp5
REAP;

--connection server_1
SELECT * FROM t3 WHERE a >= 60 ORDER BY a;
SET debug_sync='RESET';

--connection server_2
# Now we have the four transactions pending for replication on the slave.
# Let them be queued for our three worker threads in a controlled fashion.
# We put them at a stage where T1 is delayed and T3 is waiting for T1 to
# commit before T3 can start. Then we kill T3.

# Make the worker D0 free, and wait for T1 to be queued in it.
SET debug_sync='now SIGNAL d0_cont';
SET debug_sync='now WAIT_FOR t1_waiting';

# Make the worker D3 free, and wait for T2 to be queued in it.
SET debug_sync='now SIGNAL d3_cont';
SET debug_sync='now WAIT_FOR t2_waiting';

# Now release worker D1, and wait for T3 to be queued in it.
# T3 will wait for T1 to commit before it can start.
SET debug_sync='now SIGNAL d1_cont';
SET debug_sync='now WAIT_FOR t3_waiting';

# Release worker D2. Wait for T4 to be queued, so we are sure it has
# received the debug_sync signal (else we might overwrite it with the
# next debug_sync).
SET debug_sync='now SIGNAL d2_cont';
SET debug_sync='now WAIT_FOR t4_waiting';

# Now we kill the waiting transaction T3 in worker D1.
--replace_result $d1_thd_id THD_ID
eval KILL $d1_thd_id;

# Wait until T3 has reacted on the kill.
SET debug_sync='now WAIT_FOR t3_killed';

# Now we can allow T1 to proceed.
SET debug_sync='now SIGNAL t1_cont';

--let $slave_sql_errno= 1317,1927,1964
--source include/wait_for_slave_sql_error.inc
STOP SLAVE IO_THREAD;
# Since T2, T3, and T4 run in parallel, we can not be sure if T2 will have time
# to commit or not before the stop. However, T1 should commit, and T3/T4 may
# not have committed. (After slave restart we check that all become committed
# eventually).
SELECT * FROM t3 WHERE a >= 60 AND a != 65 ORDER BY a;

# Now we have to disable the debug_sync statements, so they do not trigger
# when the events are retried.
SET debug_sync='RESET';
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
SET sql_log_bin=0;
DROP FUNCTION foo;
--delimiter ||
CREATE FUNCTION foo(x INT, d1 VARCHAR(500), d2 VARCHAR(500))
  RETURNS INT DETERMINISTIC
  BEGIN
    RETURN x;
  END
||
--delimiter ;
SET sql_log_bin=1;

--connection server_1
UPDATE t3 SET b=b+1 WHERE a=60;
--save_master_pos

--connection server_2
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t3 WHERE a >= 60 ORDER BY a;
# Restore the foo() function.
SET sql_log_bin=0;
DROP FUNCTION foo;
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

--connection server_2
# Respawn all worker threads to clear any left-over debug_sync or other stuff.
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc

--echo *** 5. Test killing thread that is waiting for queue of max length to shorten ***

# Find the thread id of the driver SQL thread that we want to kill.
--let $wait_condition= SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE STATE LIKE '%Slave has read all relay log%'
--source include/wait_condition.inc
--let $thd_id= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE STATE LIKE '%Slave has read all relay log%'`
SET @old_max_queued= @@GLOBAL.slave_parallel_max_queued;
SET GLOBAL slave_parallel_max_queued=9000;

--connection server_1
--let bigstring= `SELECT REPEAT('x', 10000)`
# Create an event that will wait to be signalled.
INSERT INTO t3 VALUES (80, foo(0,
       'ha_write_row_end SIGNAL query_waiting WAIT_FOR query_cont', ''));

--connection server_2
SET debug_sync='now WAIT_FOR query_waiting';
# Inject that the SQL driver thread will signal `wait_queue_ready' to debug_sync
# as it goes to wait for the event queue to become smaller than the value of
# @@slave_parallel_max_queued.
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,rpl_parallel_wait_queue_max";

--connection server_1
--disable_query_log
# Create an event that will fill up the queue.
# The Xid event at the end of the event group will have to wait for the Query
# event with the INSERT to drain so the queue becomes shorter. However that in
# turn waits for the prior event group to continue.
eval INSERT INTO t3 VALUES (81, LENGTH('$bigstring'));
--enable_query_log
SELECT * FROM t3 WHERE a >= 80 ORDER BY a;

--connection server_2
SET debug_sync='now WAIT_FOR wait_queue_ready';

--replace_result $thd_id THD_ID
eval KILL $thd_id;

SET debug_sync='now WAIT_FOR wait_queue_killed';
SET debug_sync='now SIGNAL query_cont';

--let $slave_sql_errno= 1317,1927,1964
--source include/wait_for_slave_sql_error.inc
STOP SLAVE IO_THREAD;

SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL slave_parallel_max_queued= @old_max_queued;

--connection server_1
INSERT INTO t3 VALUES (82,0);
--save_master_pos

--connection server_2
SET debug_sync='RESET';
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t3 WHERE a >= 80 ORDER BY a;

--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
DROP function foo;
DROP TABLE t3;
SET DEBUG_SYNC= 'RESET';

--source include/rpl_end.inc
