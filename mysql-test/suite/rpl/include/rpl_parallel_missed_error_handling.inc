--echo *** MDEV-5921: In parallel replication, an error is not correctly signalled to the next transaction ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

--connection server_2
--source include/stop_slave.inc
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
CALL mtr.add_suppression("Commit failed due to failure of an earlier commit on which this one depends");
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
--save_master_pos

--connection server_2
--sync_with_master

--connection server_1
INSERT INTO t3 VALUES (110, 1);
--save_master_pos

--connection server_2
--sync_with_master
SELECT * FROM t3 WHERE a >= 110 ORDER BY a;
# Inject a duplicate key error.
SET sql_log_bin=0;
INSERT INTO t3 VALUES (111, 666);
SET sql_log_bin=1;

--connection server_1

# Create a group commit with two inserts, the first one conflicts with a row on the slave
--connect (con1,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send INSERT INTO t3 VALUES (111, 2);
--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connect (con2,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send INSERT INTO t3 VALUES (112, 3);

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
--let $slave_sql_errno= 1062
--source include/wait_for_slave_sql_error.inc
--source include/wait_for_slave_sql_to_stop.inc
# We should not see the row (112,3) here, it should be rolled back due to
# error signal from the prior transaction.
SELECT * FROM t3 WHERE a >= 110 ORDER BY a;
SET sql_log_bin=0;
DELETE FROM t3 WHERE a=111 AND b=666;
SET sql_log_bin=1;
START SLAVE SQL_THREAD;
--sync_with_master
SELECT * FROM t3 WHERE a >= 110 ORDER BY a;

# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
--disconnect con1
--disconnect con2
DROP TABLE t3;
SET DEBUG_SYNC= 'RESET';

--source include/rpl_end.inc
