--echo ***MDEV-5914: Parallel replication deadlock due to InnoDB lock conflicts ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

--connection server_2
SET sql_log_bin=0;
CALL mtr.add_suppression("Commit failed due to failure of an earlier commit on which this one depends");
SET sql_log_bin=1;
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t4 (a INT PRIMARY KEY, b INT, KEY b_idx(b)) ENGINE=InnoDB;
INSERT INTO t4 VALUES (1,NULL), (2,2), (3,NULL), (4,4), (5, NULL), (6, 6);
--connect (con1,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--connect (con2,127.0.0.1,root,,test,$SERVER_MYPORT_1,)

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


# Clean up.
--connection server_2
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
--disconnect con1
--disconnect con2
DROP TABLE t4;
SET DEBUG_SYNC= 'RESET';

--source include/rpl_end.inc
