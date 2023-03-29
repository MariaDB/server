--echo *** MDEV-7929: record_gtid() for non-transactional event group calls wakeup_subsequent_commits() too early, causing slave hang. ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
--save_master_pos

--connection server_2
--sync_with_master
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

SET @old_dbug= @@SESSION.debug_dbug;
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

SET SESSION debug_dbug=@old_dbug;

SELECT * FROM t3 WHERE a >= 130 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t3 WHERE a >= 130 ORDER BY a;

# Clean up.
--source include/stop_slave.inc
SET GLOBAL debug_dbug= @old_dbug;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc

--connection server_1
DROP TABLE t1,t3;

--source include/rpl_end.inc

