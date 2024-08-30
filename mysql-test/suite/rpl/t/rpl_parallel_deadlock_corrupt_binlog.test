--echo *** MDEV-7335: Potential parallel slave deadlock with specific binlog corruption ***
--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

--connection server_2
--source include/stop_slave.inc
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
--save_master_pos

--connection server_2
--sync_with_master
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

# Cleanup
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
DROP TABLE t2;
SET DEBUG_SYNC= 'RESET';

--source include/rpl_end.inc
