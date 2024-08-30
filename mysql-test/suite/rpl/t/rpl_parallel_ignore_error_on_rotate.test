--echo *** MDEV-6551: Some replication errors are ignored if slave_parallel_threads > 0 ***

--source include/have_innodb.inc
--source include/have_binlog_format_statement.inc
--source include/master-slave.inc

--connection server_2
--source include/stop_slave.inc
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode=@@GLOBAL.slave_parallel_mode;
SET GLOBAL slave_parallel_threads=1;
CHANGE MASTER TO master_use_gtid=slave_pos;
CALL mtr.add_suppression("Commit failed due to failure of an earlier commit on which this one depends");
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
SET gtid_domain_id=1;
INSERT INTO t2 VALUES (1);
SET gtid_domain_id=0;
SET gtid_domain_id=2;
INSERT INTO t2 VALUES (2);
SET gtid_domain_id=0;
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

# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
--source include/start_slave.inc

--connection server_1
DROP TABLE t2;

--source include/rpl_end.inc
