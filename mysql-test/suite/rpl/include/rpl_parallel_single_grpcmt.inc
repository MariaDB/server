--echo *** Test that group-committed transactions on the master can replicate in parallel on the slave. ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/have_binlog_format_statement.inc
--source include/master-slave.inc

# Test various aspects of parallel replication.

--connection server_1
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
--source include/stop_slave.inc
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
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
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

--connect (con_temp1,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
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
--let $binlog_file= master-bin.000001
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
--let $binlog_file= slave-bin.000001
--source include/show_binlog_events.inc


# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
DROP function foo;
DROP TABLE t3;
SET DEBUG_SYNC= 'RESET';

--disable_connect_log
--source include/rpl_end.inc
