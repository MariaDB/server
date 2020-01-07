--echo *** MDEV-7888: ANALYZE TABLE does wakeup_subsequent_commits(), causing wrong binlog order and parallel replication hang ***
--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode=@@GLOBAL.slave_parallel_mode;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_mode='conservative';
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
--save_master_pos

--connection server_2
--sync_with_master
--source include/stop_slave.inc
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

SET @old_dbug= @@SESSION.debug_dbug;
SET SESSION debug_dbug="+d,binlog_force_commit_id";

# Group commit with cid=10000, two event groups.
SET @commit_id= 10000;
ANALYZE TABLE t2;
INSERT INTO t3 VALUES (120, 0);

# Group commit with cid=10001, one event group.
SET @commit_id= 10001;
INSERT INTO t3 VALUES (121, 0);

SET SESSION debug_dbug=@old_dbug;

SELECT * FROM t3 WHERE a >= 120 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t3 WHERE a >= 120 ORDER BY a;

# Clean up.
--source include/stop_slave.inc
SET GLOBAL debug_dbug= @old_dbug;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
--source include/start_slave.inc

--connection server_1
DROP TABLE t2,t3;

--source include/rpl_end.inc

