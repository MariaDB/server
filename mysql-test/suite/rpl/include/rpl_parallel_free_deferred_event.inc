--echo *** MDEV-5788 Incorrect free of rgi->deferred_events in parallel replication  ***

--source include/have_innodb.inc
--source include/master-slave.inc

--connection server_2
--source include/stop_slave.inc
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
--save_master_pos

--connection server_2
--sync_with_master
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
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


# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc

--connection server_1
DROP TABLE t3;

--source include/rpl_end.inc
