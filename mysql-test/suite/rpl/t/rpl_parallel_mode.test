--echo *** MDEV-6676 - test syntax of @@slave_parallel_mode ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/master-slave.inc

--connection server_2
--source include/stop_slave.inc
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode=@@GLOBAL.slave_parallel_mode;
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
--save_master_pos

--connection server_2
--sync_with_master

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

# Cleanup
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
--source include/start_slave.inc

--connection server_1
DROP TABLE t2;
--source include/rpl_end.inc
