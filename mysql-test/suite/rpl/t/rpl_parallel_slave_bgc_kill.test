--echo *** Test killing slave threads at various wait points ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/have_binlog_format_statement.inc
--source include/master-slave.inc

# Test various aspects of parallel replication.

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode=@@GLOBAL.slave_parallel_mode;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;
SET GLOBAL slave_parallel_mode='conservative';
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
--connect (con_temp3,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--connect (con_temp4,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--connect (con_temp5,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
# MDEV-515 takes X-lock on the table for the first insert.
# So concurrent insert won't happen on the table
INSERT INTO t2 VALUES(100);
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

--echo *** 1. Test killing transaction waiting in commit for previous transaction to commit ***

# Set up three transactions on the master that will be group-committed
# together so they can be replicated in parallel on the slave.
--connection con_temp3
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send INSERT INTO t3 VALUES (31, foo(31,
    'commit_before_prepare_ordered WAIT_FOR t2_waiting',
    'commit_after_prepare_ordered SIGNAL t1_ready WAIT_FOR t1_cont'));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con_temp4
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
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
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;
--source include/start_slave.inc


--echo *** 2. Same as (1), but without restarting IO thread after kill of SQL threads ***

# Set up three transactions on the master that will be group-committed
# together so they can be replicated in parallel on the slave.
--connection con_temp3
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send INSERT INTO t3 VALUES (41, foo(41,
    'commit_before_prepare_ordered WAIT_FOR t2_waiting',
    'commit_after_prepare_ordered SIGNAL t1_ready WAIT_FOR t1_cont'));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con_temp4
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
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
send INSERT INTO t3 VALUES (51, foo(51,
    'commit_before_prepare_ordered WAIT_FOR t2_waiting',
    'commit_after_prepare_ordered SIGNAL t1_ready WAIT_FOR t1_cont'));

--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con_temp4
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
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

# Clean up.
--connection server_2
--source include/stop_slave.inc
CHANGE MASTER TO master_use_gtid=slave_pos;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
DROP function foo;
DROP TABLE t1,t2,t3;
SET DEBUG_SYNC= 'RESET';

--source include/rpl_end.inc
