# Test is independent of slave_parallel_mode
--echo *** Test two transactions in different domains committed in opposite order on slave but in a single group commit. ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

# Test various aspects of parallel replication.

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t1 VALUES (1);
INSERT INTO t2 VALUES (1);
--save_master_pos

--connection server_2
--sync_with_master
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


# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';
SET GLOBAL binlog_format=@old_format;

--connection server_1
DROP function foo;
DROP TABLE t1,t2;
SET DEBUG_SYNC= 'RESET';
SET GLOBAL binlog_format=@old_format;

--source include/rpl_end.inc
