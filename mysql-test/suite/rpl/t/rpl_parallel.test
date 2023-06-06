#
# This include file is used by more than one test suite
# (currently rpl and binlog_encryption).
# Please check all dependent tests after modifying it
#

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

# Test various aspects of parallel replication.

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
--error ER_SLAVE_MUST_STOP
SET GLOBAL slave_parallel_threads=10;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;

# Check that we do not spawn any worker threads when no slave is running.
SELECT IF(COUNT(*) < 10, "OK", CONCAT("Found too many system user processes: ", COUNT(*))) FROM information_schema.processlist WHERE user = "system user";

CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

# Check that worker threads get spawned when slave starts.
SELECT IF(COUNT(*) >= 10, "OK", CONCAT("Found too few system user processes: ", COUNT(*))) FROM information_schema.processlist WHERE user = "system user";
# ... and that worker threads get removed when slave stops.
--source include/stop_slave.inc
SELECT IF(COUNT(*) < 10, "OK", CONCAT("Found too many system user processes: ", COUNT(*))) FROM information_schema.processlist WHERE user = "system user";
--source include/start_slave.inc
SELECT IF(COUNT(*) >= 10, "OK", CONCAT("Found too few system user processes: ", COUNT(*))) FROM information_schema.processlist WHERE user = "system user";

--echo *** Test long-running query in domain 1 can run in parallel with short queries in domain 0 ***

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t1 VALUES (1);
INSERT INTO t2 VALUES (1);
--save_master_pos

--connection server_2
--sync_with_master

# Block the table t1 to simulate a replicated query taking a long time.
--connect (con_temp1,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
LOCK TABLE t1 WRITE;

--connection server_1
SET gtid_domain_id=1;
# This query will be blocked on the slave until UNLOCK TABLES.
INSERT INTO t1 VALUES (2);
SET gtid_domain_id=0;
# These t2 queries can be replicated in parallel with the prior t1 query, as
# they are in a separate replication domain.
INSERT INTO t2 VALUES (2);
INSERT INTO t2 VALUES (3);
BEGIN;
INSERT INTO t2 VALUES (4);
INSERT INTO t2 VALUES (5);
COMMIT;
INSERT INTO t2 VALUES (6);

--connection server_2
--let $wait_condition= SELECT COUNT(*) = 6 FROM t2
--source include/wait_condition.inc

SELECT * FROM t2 ORDER by a;

--connection con_temp1
SELECT * FROM t1;
UNLOCK TABLES;

--connection server_2
--let $wait_condition= SELECT COUNT(*) = 2 FROM t1
--source include/wait_condition.inc

SELECT * FROM t1 ORDER BY a;


--echo *** Test two transactions in different domains committed in opposite order on slave but in a single group commit. ***
--connection server_2
--source include/stop_slave.inc

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

SET @old_format= @@SESSION.binlog_format;
SET binlog_format='statement';
SET gtid_domain_id=1;
INSERT INTO t2 VALUES (foo(10,
    'commit_before_enqueue SIGNAL ready1 WAIT_FOR cont1',
    'commit_after_release_LOCK_prepare_ordered SIGNAL ready2'));

--connection server_2
FLUSH LOGS;
--source include/wait_for_binlog_checkpoint.inc
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
SET @old_format=@@GLOBAL.binlog_format;
SET GLOBAL binlog_format=statement;
# We need to restart all parallel threads for the new global setting to
# be copied to the session-level values.
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc

# First make sure the first insert is ready to commit, but not queued yet.
SET debug_sync='now WAIT_FOR ready1';

--connection server_1
SET gtid_domain_id=2;
INSERT INTO t2 VALUES (foo(11,
    'commit_before_enqueue SIGNAL ready3 WAIT_FOR cont3',
    'commit_after_release_LOCK_prepare_ordered SIGNAL ready4 WAIT_FOR cont4'));
SET gtid_domain_id=0;
SELECT * FROM t2 WHERE a >= 10 ORDER BY a;

--connection server_2
# Now wait for the second insert to queue itself as the leader, and then
# wait for more commits to queue up.
SET debug_sync='now WAIT_FOR ready3';
SET debug_sync='now SIGNAL cont3';
SET debug_sync='now WAIT_FOR ready4';
# Now allow the first insert to queue up to participate in group commit.
SET debug_sync='now SIGNAL cont1';
SET debug_sync='now WAIT_FOR ready2';
# Finally allow the second insert to proceed and do the group commit.
SET debug_sync='now SIGNAL cont4';

--let $wait_condition= SELECT COUNT(*) = 2 FROM t2 WHERE a >= 10
--source include/wait_condition.inc
SELECT * FROM t2 WHERE a >= 10 ORDER BY a;
# The two INSERT transactions should have been committed in opposite order,
# but in the same group commit (seen by precense of cid=# in the SHOW
# BINLOG output).
--let $binlog_file= slave-bin.000002
--source include/show_binlog_events.inc
FLUSH LOGS;
--source include/wait_for_binlog_checkpoint.inc

# Restart all the slave parallel worker threads, to clear all debug_sync actions.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
SET debug_sync='RESET';
--source include/start_slave.inc


--echo *** Test that group-committed transactions on the master can replicate in parallel on the slave. ***
--connection server_1
SET debug_sync='RESET';
FLUSH LOGS;
--source include/wait_for_binlog_checkpoint.inc
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
# Create some sentinel rows so that the rows inserted in parallel fall into
# separate gaps and do not cause gap lock conflicts.
INSERT INTO t3 VALUES (1,1), (3,3), (5,5), (7,7);
--save_master_pos
--connection server_2
--sync_with_master

# We want to test that the transactions can execute out-of-order on
# the slave, but still end up committing in-order, and in a single
# group commit.
#
# The idea is to group-commit three transactions together on the master:
# A, B, and C. On the slave, C will execute the insert first, then A,
# and then B. But B manages to complete before A has time to commit, so
# all three end up committing together.
#
# So we start by setting up some row locks that will block transactions
# A and B from executing, allowing C to run first.

--connection con_temp1
BEGIN;
INSERT INTO t3 VALUES (2,102);
--connect (con_temp2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
BEGIN;
INSERT INTO t3 VALUES (4,104);

# On the master, queue three INSERT transactions as a single group commit.
--connect (con_temp3,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (2, foo(12,
    'commit_after_release_LOCK_prepare_ordered SIGNAL slave_queued1 WAIT_FOR slave_cont1',
    ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connect (con_temp4,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (4, foo(14,
    'commit_after_release_LOCK_prepare_ordered SIGNAL slave_queued2',
    ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';

--connect (con_temp5,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued3';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (6, foo(16,
    'group_commit_waiting_for_prior SIGNAL slave_queued3',
    ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued3';
SET debug_sync='now SIGNAL master_cont1';

--connection con_temp3
REAP;
--connection con_temp4
REAP;
--connection con_temp5
REAP;
SET debug_sync='RESET';

--connection server_1
SELECT * FROM t3 ORDER BY a;
--let $binlog_file= master-bin.000002
--source include/show_binlog_events.inc

# First, wait until insert 3 is ready to queue up for group commit, but is
# waiting for insert 2 to commit before it can do so itself.
--connection server_2
SET debug_sync='now WAIT_FOR slave_queued3';

# Next, let insert 1 proceed, and allow it to queue up as the group commit
# leader, but let it wait for insert 2 to also queue up before proceeding.
--connection con_temp1
ROLLBACK;
--connection server_2
SET debug_sync='now WAIT_FOR slave_queued1';

# Now let insert 2 proceed and queue up.
--connection con_temp2
ROLLBACK;
--connection server_2
SET debug_sync='now WAIT_FOR slave_queued2';
# And finally, we can let insert 1 proceed and do the group commit with all
# three insert transactions together.
SET debug_sync='now SIGNAL slave_cont1';

# Wait for the commit to complete and check that all three transactions
# group-committed together (will be seen in the binlog as all three having
# cid=# on their GTID event).
--let $wait_condition= SELECT COUNT(*) = 3 FROM t3 WHERE a IN (2,4,6)
--source include/wait_condition.inc
SELECT * FROM t3 ORDER BY a;
--let $binlog_file= slave-bin.000003
--source include/show_binlog_events.inc


--echo *** Test STOP SLAVE in parallel mode ***
--connection server_2
--source include/stop_slave.inc
# Respawn all worker threads to clear any left-over debug_sync or other stuff.
SET debug_sync='RESET';
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;

--connection server_1
# Set up a couple of transactions. The first will be blocked halfway
# through on a lock, and while it is blocked we initiate STOP SLAVE.
# We then test that the halfway-initiated transaction is allowed to
# complete, but no subsequent ones.
# We have to use statement-based mode and set
# binlog_direct_non_transactional_updates=0; otherwise the binlog will
# be split into two event groups, one for the MyISAM part and one for the
# InnoDB part.
SET binlog_direct_non_transactional_updates=0;
SET sql_log_bin=0;
CALL mtr.add_suppression("Statement is unsafe because it accesses a non-transactional table after accessing a transactional table within the same transaction");
SET sql_log_bin=1;
BEGIN;
INSERT INTO t2 VALUES (20);
--disable_warnings
INSERT INTO t1 VALUES (20);
--enable_warnings
INSERT INTO t2 VALUES (21);
INSERT INTO t3 VALUES (20, 20);
COMMIT;
INSERT INTO t3 VALUES(21, 21);
INSERT INTO t3 VALUES(22, 22);
SET binlog_format=@old_format;
--save_master_pos

# Start a connection that will block the replicated transaction halfway.
--connection con_temp1
BEGIN;
INSERT INTO t2 VALUES (21);

--connection server_2
START SLAVE;
# Wait for the MyISAM change to be visible, after which replication will wait
# for con_temp1 to roll back.
--let $wait_condition= SELECT COUNT(*) = 1 FROM t1 WHERE a=20
--source include/wait_condition.inc

--connection con_temp2
# Initiate slave stop. It will have to wait for the current event group
# to complete.
# The dbug injection causes debug_sync to signal 'wait_for_done_waiting'
# when the SQL driver thread is ready.
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,rpl_parallel_wait_for_done_trigger";
send STOP SLAVE;

--connection con_temp1
SET debug_sync='now WAIT_FOR wait_for_done_waiting';
ROLLBACK;

--connection con_temp2
reap;
SET GLOBAL debug_dbug=@old_dbug;
SET debug_sync='RESET';

--connection server_2
--source include/wait_for_slave_to_stop.inc
# We should see the first transaction applied, but not the two others.
SELECT * FROM t1 WHERE a >= 20 ORDER BY a;
SELECT * FROM t2 WHERE a >= 20 ORDER BY a;
SELECT * FROM t3 WHERE a >= 20 ORDER BY a;

--source include/start_slave.inc
--sync_with_master
SELECT * FROM t1 WHERE a >= 20 ORDER BY a;
SELECT * FROM t2 WHERE a >= 20 ORDER BY a;
SELECT * FROM t3 WHERE a >= 20 ORDER BY a;


--connection server_2
# Respawn all worker threads to clear any left-over debug_sync or other stuff.
--source include/stop_slave.inc
SET GLOBAL binlog_format=@old_format;
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc


--echo *** Test killing slave threads at various wait points ***
--echo *** 1. Test killing transaction waiting in commit for previous transaction to commit ***

# Set up three transactions on the master that will be group-committed
# together so they can be replicated in parallel on the slave.
--connection con_temp3
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (31, foo(31,
    'commit_before_prepare_ordered WAIT_FOR t2_waiting',
    'commit_after_prepare_ordered SIGNAL t1_ready WAIT_FOR t1_cont'));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con_temp4
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
SET binlog_format=statement;
BEGIN;
# This insert is just so we can get T2 to wait while a query is running that we
# can see in SHOW PROCESSLIST so we can get its thread_id to kill later.
INSERT INTO t3 VALUES (32, foo(32,
    'ha_write_row_end SIGNAL t2_query WAIT_FOR t2_cont',
    ''));
# This insert sets up debug_sync points so that T2 will tell when it is at its
# wait point where we want to kill it - and when it has been killed.
INSERT INTO t3 VALUES (33, foo(33,
    'group_commit_waiting_for_prior SIGNAL t2_waiting',
    'group_commit_waiting_for_prior_killed SIGNAL t2_killed'));
send COMMIT;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';

--connection con_temp5
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued3';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (34, foo(34,
    '',
    ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued3';
SET debug_sync='now SIGNAL master_cont1';

--connection con_temp3
REAP;
--connection con_temp4
REAP;
--connection con_temp5
REAP;

--connection server_1
SELECT * FROM t3 WHERE a >= 30 ORDER BY a;
SET debug_sync='RESET';

--connection server_2
SET sql_log_bin=0;
CALL mtr.add_suppression("Query execution was interrupted");
CALL mtr.add_suppression("Commit failed due to failure of an earlier commit on which this one depends");
CALL mtr.add_suppression("Slave: Connection was killed");
SET sql_log_bin=1;
# Wait until T2 is inside executing its insert of 32, then find it in SHOW
# PROCESSLIST to know its thread id for KILL later.
SET debug_sync='now WAIT_FOR t2_query';
--let $thd_id= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE INFO LIKE '%foo(32%' AND INFO NOT LIKE '%LIKE%'`
SET debug_sync='now SIGNAL t2_cont';

# Wait until T2 has entered its wait for T1 to commit, and T1 has
# progressed into its commit phase.
SET debug_sync='now WAIT_FOR t1_ready';

# Now kill the transaction T2.
--replace_result $thd_id THD_ID
eval KILL $thd_id;

# Wait until T2 has reacted on the kill.
SET debug_sync='now WAIT_FOR t2_killed';

# Now we can allow T1 to proceed.
SET debug_sync='now SIGNAL t1_cont';

--let $slave_sql_errno= 1317,1927,1964
--source include/wait_for_slave_sql_error.inc
STOP SLAVE IO_THREAD;
SELECT * FROM t3 WHERE a >= 30 ORDER BY a;

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
INSERT INTO t3 VALUES (39,0);
--save_master_pos

--connection server_2
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t3 WHERE a >= 30 ORDER BY a;
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
SET GLOBAL binlog_format=@old_format;
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc


--echo *** 2. Same as (1), but without restarting IO thread after kill of SQL threads ***

# Set up three transactions on the master that will be group-committed
# together so they can be replicated in parallel on the slave.
--connection con_temp3
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (41, foo(41,
    'commit_before_prepare_ordered WAIT_FOR t2_waiting',
    'commit_after_prepare_ordered SIGNAL t1_ready WAIT_FOR t1_cont'));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con_temp4
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
SET binlog_format=statement;
BEGIN;
# This insert is just so we can get T2 to wait while a query is running that we
# can see in SHOW PROCESSLIST so we can get its thread_id to kill later.
INSERT INTO t3 VALUES (42, foo(42,
    'ha_write_row_end SIGNAL t2_query WAIT_FOR t2_cont',
    ''));
# This insert sets up debug_sync points so that T2 will tell when it is at its
# wait point where we want to kill it - and when it has been killed.
INSERT INTO t3 VALUES (43, foo(43,
    'group_commit_waiting_for_prior SIGNAL t2_waiting',
    'group_commit_waiting_for_prior_killed SIGNAL t2_killed'));
send COMMIT;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';

--connection con_temp5
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued3';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (44, foo(44,
    '',
    ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued3';
SET debug_sync='now SIGNAL master_cont1';

--connection con_temp3
REAP;
--connection con_temp4
REAP;
--connection con_temp5
REAP;

--connection server_1
SELECT * FROM t3 WHERE a >= 40 ORDER BY a;
SET debug_sync='RESET';

--connection server_2
# Wait until T2 is inside executing its insert of 42, then find it in SHOW
# PROCESSLIST to know its thread id for KILL later.
SET debug_sync='now WAIT_FOR t2_query';
--let $thd_id= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE INFO LIKE '%foo(42%' AND INFO NOT LIKE '%LIKE%'`
SET debug_sync='now SIGNAL t2_cont';

# Wait until T2 has entered its wait for T1 to commit, and T1 has
# progressed into its commit phase.
SET debug_sync='now WAIT_FOR t1_ready';

# Now kill the transaction T2.
--replace_result $thd_id THD_ID
eval KILL $thd_id;

# Wait until T2 has reacted on the kill.
SET debug_sync='now WAIT_FOR t2_killed';

# Now we can allow T1 to proceed.
SET debug_sync='now SIGNAL t1_cont';

--let $slave_sql_errno= 1317,1927,1964
--source include/wait_for_slave_sql_error.inc

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
INSERT INTO t3 VALUES (49,0);
--save_master_pos

--connection server_2
START SLAVE SQL_THREAD;
--sync_with_master
SELECT * FROM t3 WHERE a >= 40 ORDER BY a;
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
SET GLOBAL binlog_format=@old_format;
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc


--echo *** 3. Same as (2), but not using gtid mode ***

--connection server_2
--source include/stop_slave.inc
CHANGE MASTER TO master_use_gtid=no;
--source include/start_slave.inc

--connection server_1
# Set up three transactions on the master that will be group-committed
# together so they can be replicated in parallel on the slave.
--connection con_temp3
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (51, foo(51,
    'commit_before_prepare_ordered WAIT_FOR t2_waiting',
    'commit_after_prepare_ordered SIGNAL t1_ready WAIT_FOR t1_cont'));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con_temp4
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
SET binlog_format=statement;
BEGIN;
# This insert is just so we can get T2 to wait while a query is running that we
# can see in SHOW PROCESSLIST so we can get its thread_id to kill later.
INSERT INTO t3 VALUES (52, foo(52,
    'ha_write_row_end SIGNAL t2_query WAIT_FOR t2_cont',
    ''));
# This insert sets up debug_sync points so that T2 will tell when it is at its
# wait point where we want to kill it - and when it has been killed.
INSERT INTO t3 VALUES (53, foo(53,
    'group_commit_waiting_for_prior SIGNAL t2_waiting',
    'group_commit_waiting_for_prior_killed SIGNAL t2_killed'));
send COMMIT;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';

--connection con_temp5
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued3';
SET binlog_format=statement;
send INSERT INTO t3 VALUES (54, foo(54,
    '',
    ''));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued3';
SET debug_sync='now SIGNAL master_cont1';

--connection con_temp3
REAP;
--connection con_temp4
REAP;
--connection con_temp5
REAP;

--connection server_1
SELECT * FROM t3 WHERE a >= 50 ORDER BY a;
SET debug_sync='RESET';

--connection server_2
# Wait until T2 is inside executing its insert of 52, then find it in SHOW
# PROCESSLIST to know its thread id for KILL later.
SET debug_sync='now WAIT_FOR t2_query';
--let $thd_id= `SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST WHERE INFO LIKE '%foo(52%' AND INFO NOT LIKE '%LIKE%'`
SET debug_sync='now SIGNAL t2_cont';

# Wait until T2 has entered its wait for T1 to commit, and T1 has
# progressed into its commit phase.
SET debug_sync='now WAIT_FOR t1_ready';

# Now kill the transaction T2.
--replace_result $thd_id THD_ID
eval KILL $thd_id;

# Wait until T2 has reacted on the kill.
SET debug_sync='now WAIT_FOR t2_killed';

# Now we can allow T1 to proceed.
SET debug_sync='now SIGNAL t1_cont';

--let $slave_sql_errno= 1317,1927,1964
--source include/wait_for_slave_sql_error.inc
SELECT * FROM t3 WHERE a >= 50 ORDER BY a;

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
INSERT INTO t3 VALUES (59,0);
--save_master_pos

--connection server_2
START SLAVE SQL_THREAD;
--sync_with_master
SELECT * FROM t3 WHERE a >= 50 ORDER BY a;
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


--source include/stop_slave.inc
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_2
# Respawn all worker threads to clear any left-over debug_sync or other stuff.
--source include/stop_slave.inc
SET GLOBAL binlog_format=@old_format;
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=4;
--source include/start_slave.inc


--echo *** 4. Test killing thread that is waiting to start transaction until previous transaction commits ***

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
SET binlog_format=statement;
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
SET binlog_format=statement;
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
SET GLOBAL binlog_format=@old_format;
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
SET binlog_format=statement;
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
SET binlog_format=@old_format;
--save_master_pos

--connection server_2
SET debug_sync='RESET';
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t3 WHERE a >= 80 ORDER BY a;


--connection server_2
--source include/stop_slave.inc
SET GLOBAL binlog_format=@old_format;
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc

--echo *** MDEV-5788 Incorrect free of rgi->deferred_events in parallel replication  ***

--connection server_2
# Use just two worker threads, so we are sure to get the rpl_group_info added
# to the free list, which is what triggered the bug.
--source include/stop_slave.inc
SET GLOBAL replicate_ignore_table="test.t3";
SET GLOBAL slave_parallel_threads=2;
--source include/start_slave.inc

--connection server_1
INSERT INTO t3 VALUES (100, rand());
INSERT INTO t3 VALUES (101, rand());

--save_master_pos

--connection server_2
--sync_with_master

--connection server_1
INSERT INTO t3 VALUES (102, rand());
INSERT INTO t3 VALUES (103, rand());
INSERT INTO t3 VALUES (104, rand());
INSERT INTO t3 VALUES (105, rand());

--save_master_pos

--connection server_2
--sync_with_master
--source include/stop_slave.inc
SET GLOBAL replicate_ignore_table="";
--source include/start_slave.inc

--connection server_1
INSERT INTO t3 VALUES (106, rand());
INSERT INTO t3 VALUES (107, rand());
--save_master_pos

--connection server_2
--sync_with_master
--replace_column 2 #
SELECT * FROM t3 WHERE a >= 100 ORDER BY a;


--echo *** MDEV-5921: In parallel replication, an error is not correctly signalled to the next transaction ***

--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc

--connection server_1
INSERT INTO t3 VALUES (110, 1);
--save_master_pos

--connection server_2
--sync_with_master
SELECT * FROM t3 WHERE a >= 110 ORDER BY a;
# Inject a duplicate key error.
SET sql_log_bin=0;
INSERT INTO t3 VALUES (111, 666);
SET sql_log_bin=1;

--connection server_1

# Create a group commit with two inserts, the first one conflicts with a row on the slave
--connect (con1,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send INSERT INTO t3 VALUES (111, 2);
--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connect (con2,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send INSERT INTO t3 VALUES (112, 3);

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';
SET debug_sync='now SIGNAL master_cont1';

--connection con1
REAP;
--connection con2
REAP;
SET debug_sync='RESET';
--save_master_pos

--connection server_2
--let $slave_sql_errno= 1062
--source include/wait_for_slave_sql_error.inc
--source include/wait_for_slave_sql_to_stop.inc
# We should not see the row (112,3) here, it should be rolled back due to
# error signal from the prior transaction.
SELECT * FROM t3 WHERE a >= 110 ORDER BY a;
SET sql_log_bin=0;
DELETE FROM t3 WHERE a=111 AND b=666;
SET sql_log_bin=1;
START SLAVE SQL_THREAD;
--sync_with_master
SELECT * FROM t3 WHERE a >= 110 ORDER BY a;


--echo ***MDEV-5914: Parallel replication deadlock due to InnoDB lock conflicts ***
--connection server_2
--source include/stop_slave.inc

--connection server_1
CREATE TABLE t4 (a INT PRIMARY KEY, b INT, KEY b_idx(b)) ENGINE=InnoDB;
INSERT INTO t4 VALUES (1,NULL), (2,2), (3,NULL), (4,4), (5, NULL), (6, 6);

# Create a group commit with UPDATE and DELETE, in that order.
# The bug was that while the UPDATE's row lock does not block the DELETE, the
# DELETE's gap lock _does_ block the UPDATE. This could cause a deadlock
# on the slave.
--connection con1
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send UPDATE t4 SET b=NULL WHERE a=6;
--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con2
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send DELETE FROM t4 WHERE b <= 3;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';
SET debug_sync='now SIGNAL master_cont1';

--connection con1
REAP;
--connection con2
REAP;
SET debug_sync='RESET';
--save_master_pos

--connection server_2
--source include/start_slave.inc
--sync_with_master
--source include/stop_slave.inc

SELECT * FROM t4 ORDER BY a;


# Another example, this one with INSERT vs. DELETE
--connection server_1
DELETE FROM t4;
INSERT INTO t4 VALUES (1,NULL), (2,2), (3,NULL), (4,4), (5, NULL), (6, 6);

# Create a group commit with INSERT and DELETE, in that order.
# The bug was that while the INSERT's insert intention lock does not block
# the DELETE, the DELETE's gap lock _does_ block the INSERT. This could cause
# a deadlock on the slave.
--connection con1
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send INSERT INTO t4 VALUES (7, NULL);
--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con2
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send DELETE FROM t4 WHERE b <= 3;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';
SET debug_sync='now SIGNAL master_cont1';

--connection con1
REAP;
--connection con2
REAP;
SET debug_sync='RESET';
--save_master_pos

--connection server_2
--source include/start_slave.inc
--sync_with_master
--source include/stop_slave.inc

SELECT * FROM t4 ORDER BY a;


# MDEV-6549, failing to update gtid_slave_pos for a transaction that was retried.
# The problem was that when a transaction updates the mysql.gtid_slave_pos
# table, it clears the flag that marks that there is a GTID position that
# needs to be updated. Then, if the transaction got killed after that due
# to a deadlock, the subsequent retry would fail to notice that the GTID needs
# to be recorded in gtid_slave_pos.
#
# (In the original bug report, the symptom was an assertion; this was however
# just a side effect of the missing update of gtid_slave_pos, which also
# happened to cause a missing clear of OPTION_GTID_BEGIN).
--connection server_1
DELETE FROM t4;
INSERT INTO t4 VALUES (1,NULL), (2,2), (3,NULL), (4,4), (5, NULL), (6, 6);

# Create two transactions that can run in parallel on the slave but cause
# a deadlock if the second runs before the first.
--connection con1
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send UPDATE t4 SET b=NULL WHERE a=6;
--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con2
# Must use statement-based binlogging. Otherwise the transaction will not be
# binlogged at all, as it modifies no rows.
SET @old_format= @@SESSION.binlog_format;
SET binlog_format='statement';
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send DELETE FROM t4 WHERE b <= 1;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';
SET debug_sync='now SIGNAL master_cont1';

--connection con1
REAP;
--connection con2
REAP;
SET @old_format=@@GLOBAL.binlog_format;
SET debug_sync='RESET';
--save_master_pos
--let $last_gtid= `SELECT @@last_gtid`

--connection server_2
# Disable the usual skip of gap locks for transactions that are run in
# parallel, using DBUG. This allows the deadlock to occur, and this in turn
# triggers a retry of the second transaction, and the code that was buggy and
# caused the gtid_slave_pos update to be skipped in the retry.
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,disable_thd_need_ordering_with";
--source include/start_slave.inc
--sync_with_master
SET GLOBAL debug_dbug=@old_dbug;

SELECT * FROM t4 ORDER BY a;
# Check that the GTID of the second transaction was correctly recorded in
# gtid_slave_pos, in the variable as well as in the table.
--replace_result $last_gtid GTID
eval SET @last_gtid= '$last_gtid';
SELECT IF(@@gtid_slave_pos LIKE CONCAT('%',@last_gtid,'%'), "GTID found ok",
    CONCAT("GTID ", @last_gtid, " not found in gtid_slave_pos=", @@gtid_slave_pos))
    AS result;
SELECT "ROW FOUND" AS `Is the row found?`
  FROM mysql.gtid_slave_pos
 WHERE CONCAT(domain_id, "-", server_id, "-", seq_no) = @last_gtid;


--echo *** MDEV-5938: Exec_master_log_pos not updated at log rotate in parallel replication ***
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=1;
SET DEBUG_SYNC= 'RESET';
--source include/start_slave.inc

--connection server_1
CREATE TABLE t5 (a INT PRIMARY KEY, b INT);
INSERT INTO t5 VALUES (1,1);
INSERT INTO t5 VALUES (2,2), (3,8);
INSERT INTO t5 VALUES (4,16);
--save_master_pos

--connection server_2
--sync_with_master
let $io_file= query_get_value(SHOW SLAVE STATUS, Master_Log_File, 1);
let $io_pos= query_get_value(SHOW SLAVE STATUS, Read_Master_Log_Pos, 1);
let $sql_file= query_get_value(SHOW SLAVE STATUS, Relay_Master_Log_File, 1);
let $sql_pos= query_get_value(SHOW SLAVE STATUS, Exec_Master_Log_Pos, 1);
--disable_query_log
eval SELECT IF('$io_file' = '$sql_file', "OK", "Not ok, $io_file <> $sql_file") AS test_check;
eval SELECT IF('$io_pos' = '$sql_pos', "OK", "Not ok, $io_pos <> $sql_pos") AS test_check;
--enable_query_log

--connection server_1
FLUSH LOGS;
--source include/wait_for_binlog_checkpoint.inc
--save_master_pos

--connection server_2
--sync_with_master
let $io_file= query_get_value(SHOW SLAVE STATUS, Master_Log_File, 1);
let $io_pos= query_get_value(SHOW SLAVE STATUS, Read_Master_Log_Pos, 1);
let $sql_file= query_get_value(SHOW SLAVE STATUS, Relay_Master_Log_File, 1);
let $sql_pos= query_get_value(SHOW SLAVE STATUS, Exec_Master_Log_Pos, 1);
--disable_query_log
eval SELECT IF('$io_file' = '$sql_file', "OK", "Not ok, $io_file <> $sql_file") AS test_check;
eval SELECT IF('$io_pos' = '$sql_pos', "OK", "Not ok, $io_pos <> $sql_pos") AS test_check;
--enable_query_log


--echo *** MDEV_6435: Incorrect error handling when query binlogged partially on master with "killed" error ***

--connection server_1
CREATE TABLE t6 (a INT) ENGINE=MyISAM;
CREATE TRIGGER tr AFTER INSERT ON t6 FOR EACH ROW SET @a = 1;

--connection con1
SET @old_format= @@binlog_format;
SET binlog_format= statement;
--let $conid = `SELECT CONNECTION_ID()`
SET debug_sync='sp_head_execute_before_loop SIGNAL ready WAIT_FOR cont';
send INSERT INTO t6 VALUES (1), (2), (3);

--connection server_1
SET debug_sync='now WAIT_FOR ready';
--replace_result $conid CONID
eval KILL QUERY $conid;
SET debug_sync='now SIGNAL cont';

--connection con1
--error ER_QUERY_INTERRUPTED
--reap
SET binlog_format= @old_format;
SET debug_sync='RESET';
--let $after_error_gtid_pos= `SELECT @@gtid_binlog_pos`

--connection server_1
SET debug_sync='RESET';


--connection server_2
--let $slave_sql_errno= 1317
--source include/wait_for_slave_sql_error.inc
STOP SLAVE IO_THREAD;
--replace_result $after_error_gtid_pos AFTER_ERROR_GTID_POS
eval SET GLOBAL gtid_slave_pos= '$after_error_gtid_pos';
--source include/start_slave.inc

--connection server_1
INSERT INTO t6 VALUES (4);
SELECT * FROM t6 ORDER BY a;
--save_master_pos

--connection server_2
--sync_with_master
SELECT * FROM t6 ORDER BY a;


--echo *** MDEV-6551: Some replication errors are ignored if slave_parallel_threads > 0 ***

--connection server_1
INSERT INTO t2 VALUES (31);
--let $gtid1= `SELECT @@LAST_GTID`
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads= 0;
--source include/start_slave.inc

# Force a duplicate key error on the slave.
SET sql_log_bin= 0;
INSERT INTO t2 VALUES (32);
SET sql_log_bin= 1;

--connection server_1
INSERT INTO t2 VALUES (32);
--let $gtid2= `SELECT @@LAST_GTID`
# Rotate the binlog; the bug is triggered when the master binlog file changes
# after the event group that causes the duplicate key error.
FLUSH LOGS;
INSERT INTO t2 VALUES (33);
INSERT INTO t2 VALUES (34);
SELECT * FROM t2 WHERE a >= 30 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--let $slave_sql_errno= 1062
--source include/wait_for_slave_sql_error.inc

--connection server_2
--source include/stop_slave_io.inc
SET GLOBAL slave_parallel_threads=10;
START SLAVE;

--let $slave_sql_errno= 1062
--source include/wait_for_slave_sql_error.inc

# Note: IO thread is still running at this point.
# The bug seems to have been that restarting the SQL thread after an error with
# the IO thread still running, somehow picks up a later relay log position and
# thus ends up skipping the failing event, rather than re-executing.

START SLAVE SQL_THREAD;
--let $slave_sql_errno= 1062
--source include/wait_for_slave_sql_error.inc

SELECT * FROM t2 WHERE a >= 30 ORDER BY a;

# Skip the duplicate error, so we can proceed.
--error ER_SLAVE_SKIP_NOT_IN_GTID
SET sql_slave_skip_counter= 1;
--source include/stop_slave_io.inc
--disable_query_log
eval SET GLOBAL gtid_slave_pos = REPLACE(@@gtid_slave_pos, "$gtid1", "$gtid2");
--enable_query_log
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t2 WHERE a >= 30 ORDER BY a;


--echo *** MDEV-6775: Wrong binlog order in parallel replication ***
--connection server_1
# A bit tricky bug to reproduce. On the master, we binlog in statement-mode
# two transactions, an UPDATE followed by a DELETE. On the slave, we replicate
# with binlog-mode set to ROW, which means the DELETE, which modifies no rows,
# is not binlogged. Then we inject a wait in the group commit code on the
# slave, shortly before the actual commit of the UPDATE. The bug was that the
# DELETE could wake up from wait_for_prior_commit() before the commit of the
# UPDATE. So the test could see the slave position updated to after DELETE,
# while the UPDATE was still not visible.
DELETE FROM t4;
INSERT INTO t4 VALUES (1,NULL), (3,NULL), (4,4), (5, NULL), (6, 6);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,inject_binlog_commit_before_get_LOCK_log";
SET @old_format=@@GLOBAL.binlog_format;
SET GLOBAL binlog_format=ROW;
# Re-spawn the worker threads to be sure they pick up the new binlog format
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;

--connection con1
SET @old_format= @@binlog_format;
SET binlog_format= statement;
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send UPDATE t4 SET b=NULL WHERE a=6;
--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con2
SET @old_format= @@binlog_format;
SET binlog_format= statement;
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send DELETE FROM t4 WHERE b <= 3;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';
SET debug_sync='now SIGNAL master_cont1';

--connection con1
REAP;
SET binlog_format= @old_format;
--connection con2
REAP;
SET binlog_format= @old_format;
SET debug_sync='RESET';
--save_master_pos
SELECT * FROM t4 ORDER BY a;

--connection server_2
--source include/start_slave.inc
SET debug_sync= 'now WAIT_FOR waiting';
--sync_with_master
SELECT * FROM t4 ORDER BY a;
SET debug_sync= 'now SIGNAL cont';

# Re-spawn the worker threads to remove any DBUG injections or DEBUG_SYNC.
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL binlog_format= @old_format;
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc


--echo *** MDEV-7237: Parallel replication: incorrect relaylog position after stop/start the slave ***
--connection server_1
INSERT INTO t2 VALUES (40);
--save_master_pos

--connection server_2
--sync_with_master
--source include/stop_slave.inc
CHANGE MASTER TO master_use_gtid=no;
SET @old_dbug= @@GLOBAL.debug_dbug;
# This DBUG injection causes a DEBUG_SYNC signal "scheduled_gtid_0_x_100" when
# GTID 0-1-100 has been scheduled for and fetched by a worker thread.
SET GLOBAL debug_dbug="+d,rpl_parallel_scheduled_gtid_0_x_100";
# This DBUG injection causes a DEBUG_SYNC signal "wait_for_done_waiting" when
# STOP SLAVE has signalled all worker threads to stop.
SET GLOBAL debug_dbug="+d,rpl_parallel_wait_for_done_trigger";
# Reset worker threads to make DBUG setting catch on.
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;


--connection server_1
# Setup some transaction for the slave to replicate.
INSERT INTO t2 VALUES (41);
INSERT INTO t2 VALUES (42);
# Need to log the DELETE in statement format, so we can see it in processlist.
SET @old_format= @@binlog_format;
SET binlog_format= statement;
DELETE FROM t2 WHERE a=40;
SET binlog_format= @old_format;
INSERT INTO t2 VALUES (43);
INSERT INTO t2 VALUES (44);
# Force the slave to switch to a new relay log file.
FLUSH LOGS;
INSERT INTO t2 VALUES (45);
# Inject a GTID 0-1-100, which will trigger a DEBUG_SYNC signal when this
# transaction has been fetched by a worker thread.
SET gtid_seq_no=100;
INSERT INTO t2 VALUES (46);
--save_master_pos

--connection con_temp2
# Temporarily block the DELETE on a=40 from completing.
BEGIN;
SELECT * FROM t2 WHERE a=40 FOR UPDATE;


--connection server_2
--source include/start_slave.inc

# Wait for a worker thread to start on the DELETE that will be blocked
# temporarily by the SELECT FOR UPDATE.
--let $wait_condition= SELECT count(*) > 0 FROM information_schema.processlist WHERE state='updating' and info LIKE '%DELETE FROM t2 WHERE a=40%'
--source include/wait_condition.inc

# The DBUG injection set above will make the worker thread signal the following
# debug_sync when the GTID 0-1-100 has been reached by a worker thread.
# Thus, at this point, the SQL driver thread has reached the next
# relay log file name, while a worker thread is still processing a
# transaction in the previous relay log file, blocked on the SELECT FOR
# UPDATE.
SET debug_sync= 'now WAIT_FOR scheduled_gtid_0_x_100';
# At this point, the SQL driver thread is in the new relay log file, while
# the DELETE from the old relay log file is not yet complete. We will stop
# the slave at this point. The bug was that the DELETE statement would
# update the slave position to the _new_ relay log file name instead of
# its own old file name. Thus, by stoping and restarting the slave at this
# point, we would get an error at restart due to incorrect position. (If
# we would let the slave catch up before stopping, the incorrect position
# would be corrected by a later transaction).

send STOP SLAVE;

--connection con_temp2
# Wait for STOP SLAVE to have proceeded sufficiently that it has signalled
# all worker threads to stop; this ensures that we will stop after the DELETE
# transaction (and not after a later transaction that might have been able
# to set a fixed position).
SET debug_sync= 'now WAIT_FOR wait_for_done_waiting';
# Now release the row lock that was blocking the replication of DELETE.
ROLLBACK;

--connection server_2
reap;
--source include/wait_for_slave_sql_to_stop.inc
SELECT * FROM t2 WHERE a >= 40 ORDER BY a;
# Now restart the slave. With the bug present, this would start at an
# incorrect relay log position, causing relay log read error (or if unlucky,
# silently skip a number of events).
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t2 WHERE a >= 40 ORDER BY a;
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET DEBUG_SYNC= 'RESET';
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc


--echo *** MDEV-7326 Server deadlock in connection with parallel replication ***
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

# Re-spawn the worker threads to remove any DBUG injections or DEBUG_SYNC.
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc

--echo *** MDEV-7335: Potential parallel slave deadlock with specific binlog corruption ***

--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=1;
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,slave_discard_xid_for_gtid_0_x_1000";
CALL mtr.add_suppression("Unexpected break of being relay-logged GTID");

--connection server_1
INSERT INTO t2 VALUES (101);
INSERT INTO t2 VALUES (102);
INSERT INTO t2 VALUES (103);
INSERT INTO t2 VALUES (104);
INSERT INTO t2 VALUES (105);
# Inject a partial event group (missing XID at the end). The bug was that such
# partial group was not handled appropriately, leading to server deadlock.
SET gtid_seq_no=1000;
INSERT INTO t2 VALUES (106);
INSERT INTO t2 VALUES (107);
INSERT INTO t2 VALUES (108);
INSERT INTO t2 VALUES (109);
INSERT INTO t2 VALUES (110);
INSERT INTO t2 VALUES (111);
INSERT INTO t2 VALUES (112);
INSERT INTO t2 VALUES (113);
INSERT INTO t2 VALUES (114);
INSERT INTO t2 VALUES (115);
INSERT INTO t2 VALUES (116);
INSERT INTO t2 VALUES (117);
INSERT INTO t2 VALUES (118);
INSERT INTO t2 VALUES (119);
INSERT INTO t2 VALUES (120);
INSERT INTO t2 VALUES (121);
INSERT INTO t2 VALUES (122);
INSERT INTO t2 VALUES (123);
INSERT INTO t2 VALUES (124);
INSERT INTO t2 VALUES (125);
INSERT INTO t2 VALUES (126);
INSERT INTO t2 VALUES (127);
INSERT INTO t2 VALUES (128);
INSERT INTO t2 VALUES (129);
INSERT INTO t2 VALUES (130);
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
# The partial event group (a=106) should be rolled back and thus missing.
SELECT * FROM t2 WHERE a >= 100 ORDER BY a;

--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc

--echo *** MDEV-6676 - test syntax of @@slave_parallel_mode ***
--connection server_2

--let $status_items= Parallel_Mode
--source include/show_slave_status.inc
--source include/stop_slave.inc
SET GLOBAL slave_parallel_mode='aggressive';
--let $status_items= Parallel_Mode
--source include/show_slave_status.inc
SET GLOBAL slave_parallel_mode='conservative';
--let $status_items= Parallel_Mode
--source include/show_slave_status.inc


--echo *** MDEV-6676 - test that empty parallel_mode does not replicate in parallel ***
--connection server_1
INSERT INTO t2 VALUES (1040);
--source include/save_master_gtid.inc

--connection server_2
SET GLOBAL slave_parallel_mode='none';
# Test that we do not use parallel apply, by injecting an unconditional
# crash in the parallel apply code.
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,slave_crash_if_parallel_apply";
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
SELECT * FROM t2 WHERE a >= 1040 ORDER BY a;
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;

--echo *** MDEV-6676 - test disabling domain-based parallel replication ***
--connection server_1
# Let's do a bunch of transactions that will conflict if run out-of-order in
# domain-based parallel replication mode.
SET gtid_domain_id = 1;
INSERT INTO t2 VALUES (1041);
INSERT INTO t2 VALUES (1042);
INSERT INTO t2 VALUES (1043);
INSERT INTO t2 VALUES (1044);
INSERT INTO t2 VALUES (1045);
INSERT INTO t2 VALUES (1046);
DELETE FROM t2 WHERE a >= 1041;
SET gtid_domain_id = 2;
INSERT INTO t2 VALUES (1041);
INSERT INTO t2 VALUES (1042);
INSERT INTO t2 VALUES (1043);
INSERT INTO t2 VALUES (1044);
INSERT INTO t2 VALUES (1045);
INSERT INTO t2 VALUES (1046);
SET gtid_domain_id = 0;
--source include/save_master_gtid.inc
--connection server_2
SET GLOBAL slave_parallel_mode=minimal;
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
SELECT * FROM t2 WHERE a >= 1040 ORDER BY a;

--echo *** MDEV-7888: ANALYZE TABLE does wakeup_subsequent_commits(), causing wrong binlog order and parallel replication hang ***

--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_mode='conservative';
SET GLOBAL slave_parallel_threads=10;

SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug= '+d,inject_analyze_table_sleep';

--connection server_1
# Inject two group commits. The bug was that ANALYZE TABLE would call
# wakeup_subsequent_commits() too early, allowing the following transaction
# in the same group to run ahead and binlog and free the GCO. Then we get
# wrong binlog order and later access freed GCO, which causes lost wakeup
# of following GCO and thus replication hang.
# We injected a small sleep in ANALYZE to make the race easier to hit (this
# can only cause false negatives in versions with the bug, not false positives,
# so sleep is ok here. And it's in general not possible to trigger reliably
# the race with debug_sync, since the bugfix makes the race impossible).

SET @old_dbug_slave= @@SESSION.debug_dbug;
SET SESSION debug_dbug="+d,binlog_force_commit_id";

# Group commit with cid=10000, two event groups.
SET @commit_id= 10000;
ANALYZE TABLE t2;
INSERT INTO t3 VALUES (120, 0);

# Group commit with cid=10001, one event group.
SET @commit_id= 10001;
INSERT INTO t3 VALUES (121, 0);

SET SESSION debug_dbug=@old_dbug_slave;

SELECT * FROM t3 WHERE a >= 120 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t3 WHERE a >= 120 ORDER BY a;

--source include/stop_slave.inc
SET GLOBAL debug_dbug= @old_dbug;
--source include/start_slave.inc


--echo *** MDEV-7929: record_gtid() for non-transactional event group calls wakeup_subsequent_commits() too early, causing slave hang. ***

--connection server_2
--source include/stop_slave.inc
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug= '+d,inject_record_gtid_serverid_100_sleep';

--connection server_1
# Inject two group commits. The bug was that record_gtid for a
# non-transactional event group would commit its own transaction, which would
# cause ha_commit_trans() to call wakeup_subsequent_commits() too early. This
# in turn lead to access to freed group_commit_orderer object, losing a wakeup
# and causing slave threads to hang.
# We inject a small sleep in the corresponding record_gtid() to make the race
# easier to hit.

SET @old_dbug_slave= @@SESSION.debug_dbug;
SET SESSION debug_dbug="+d,binlog_force_commit_id";

# Group commit with cid=10010, two event groups.
SET @old_server_id= @@SESSION.server_id;
SET SESSION server_id= 100;
SET @commit_id= 10010;
ALTER TABLE t1 COMMENT "Hulubulu!";
SET SESSION server_id= @old_server_id;
INSERT INTO t3 VALUES (130, 0);

# Group commit with cid=10011, one event group.
SET @commit_id= 10011;
INSERT INTO t3 VALUES (131, 0);

SET SESSION debug_dbug=@old_dbug_slave;
SELECT * FROM t3 WHERE a >= 130 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t3 WHERE a >= 130 ORDER BY a;

--source include/stop_slave.inc
SET GLOBAL debug_dbug= @old_dbug;
--source include/start_slave.inc


--echo *** MDEV-8031: Parallel replication stops on "connection killed" error (probably incorrectly handled deadlock kill) ***

--connection server_1
INSERT INTO t3 VALUES (201,0), (202,0);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug= '+d,inject_mdev8031';

--connection server_1
# We artificially create a situation that hopefully resembles the original
# bug which was only seen "in the wild", and only once.
# Setup a fake group commit with lots of conflicts that will lead to deadloc
# kill. The slave DBUG injection causes the slave to be deadlock killed at
# a particular point during the retry, and then later do a small sleep at
# another critical point where the prior transaction then has a chance to
# complete. Finally an extra KILL check catches an unhandled, lingering
# deadlock kill. So rather artificial, but at least it exercises the
# relevant code paths.
SET @old_dbug_slave= @@SESSION.debug_dbug;
SET SESSION debug_dbug="+d,binlog_force_commit_id";

SET @commit_id= 10200;
INSERT INTO t3 VALUES (203, 1);
INSERT INTO t3 VALUES (204, 1);
INSERT INTO t3 VALUES (205, 1);
UPDATE t3 SET b=b+1 WHERE a=201;
UPDATE t3 SET b=b+1 WHERE a=201;
UPDATE t3 SET b=b+1 WHERE a=201;
UPDATE t3 SET b=b+1 WHERE a=202;
UPDATE t3 SET b=b+1 WHERE a=202;
UPDATE t3 SET b=b+1 WHERE a=202;
UPDATE t3 SET b=b+1 WHERE a=202;
UPDATE t3 SET b=b+1 WHERE a=203;
UPDATE t3 SET b=b+1 WHERE a=203;
UPDATE t3 SET b=b+1 WHERE a=204;
UPDATE t3 SET b=b+1 WHERE a=204;
UPDATE t3 SET b=b+1 WHERE a=204;
UPDATE t3 SET b=b+1 WHERE a=203;
UPDATE t3 SET b=b+1 WHERE a=205;
UPDATE t3 SET b=b+1 WHERE a=205;
SET SESSION debug_dbug=@old_dbug_slave;

SELECT * FROM t3 WHERE a>=200 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t3 WHERE a>=200 ORDER BY a;
--source include/stop_slave.inc
SET GLOBAL debug_dbug= @old_dbug;
--source include/start_slave.inc


--echo *** Check getting deadlock killed inside open_binlog() during retry. ***

--connection server_2
--source include/stop_slave.inc
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug= '+d,inject_retry_event_group_open_binlog_kill';
SET @old_max= @@GLOBAL.max_relay_log_size;
SET GLOBAL max_relay_log_size= 4096;

--connection server_1
SET @old_dbug_slave= @@SESSION.debug_dbug;
SET SESSION debug_dbug="+d,binlog_force_commit_id";

--let $large= `SELECT REPEAT("*", 8192)`
SET @commit_id= 10210;
--echo Omit long queries that cause relaylog rotations and transaction retries...
--disable_query_log
eval UPDATE t3 SET b=b+1 WHERE a=201 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=201 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=201 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=202 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=202 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=202 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=202 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=203 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=203 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=204 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=204 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=204 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=203 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=205 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=205 /* $large */;
--enable_query_log
SET SESSION debug_dbug=@old_dbug_slave;

SELECT * FROM t3 WHERE a>=200 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t3 WHERE a>=200 ORDER BY a;
--source include/stop_slave.inc
SET GLOBAL debug_dbug= @old_dbug;
SET GLOBAL max_relay_log_size= @old_max;
--source include/start_slave.inc

--echo *** MDEV-8725: Assertion on ROLLBACK statement in the binary log ***
--connection server_1
# Inject an event group terminated by ROLLBACK, by mixing MyISAM and InnoDB
# in a transaction. The bug was an assertion on the ROLLBACK due to
# mark_start_commit() being already called.
--disable_warnings
BEGIN;
INSERT INTO t2 VALUES (2000);
INSERT INTO t1 VALUES (2000);
INSERT INTO t2 VALUES (2001);
ROLLBACK;
--enable_warnings
SELECT * FROM t1 WHERE a>=2000 ORDER BY a;
SELECT * FROM t2 WHERE a>=2000 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
SELECT * FROM t1 WHERE a>=2000 ORDER BY a;
SELECT * FROM t2 WHERE a>=2000 ORDER BY a;


# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
DROP function foo;
DROP TABLE t1,t2,t3,t4,t5,t6;
SET DEBUG_SYNC= 'RESET';

--source include/rpl_end.inc
