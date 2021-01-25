--echo *** Test STOP SLAVE in parallel mode ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/have_binlog_format_statement.inc
--source include/master-slave.inc

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode=@@GLOBAL.slave_parallel_mode;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;
SET GLOBAL slave_parallel_mode='conservative';

CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc
--connect (con_temp1,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
--connect (con_temp2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
# MDEV-515 takes X-lock on the table for the first insert.
# So concurrent insert won't happen on the table
INSERT INTO t2 VALUES(100);
INSERT INTO t3 VALUES(100, 100);
--save_master_pos

--connection server_2
--sync_with_master
--source include/stop_slave.inc

--connection server_1
# Set up a couple of transactions. The first will be blocked halfway
# through on a lock, and while it is blocked we initiate STOP SLAVE.
# We then test that the halfway-initiated transaction is allowed to
# complete, but no subsequent ones.
# We have to use statement-based mode and set
# binlog_direct_non_transactional_updates=0; otherwise the binlog will
# be split into two event groups, one for the MyISAM part and one for the
# InnoDB part.
SET binlog_direct_non_transactional_updates=0;
SET sql_log_bin=0;
CALL mtr.add_suppression("Statement is unsafe because it accesses a non-transactional table after accessing a transactional table within the same transaction");
SET sql_log_bin=1;
BEGIN;
INSERT INTO t2 VALUES (20);
--disable_warnings
INSERT INTO t1 VALUES (20);
--enable_warnings
INSERT INTO t2 VALUES (21);
INSERT INTO t3 VALUES (20, 20);
COMMIT;
INSERT INTO t3 VALUES(21, 21);
INSERT INTO t3 VALUES(22, 22);
--save_master_pos

# Start a connection that will block the replicated transaction halfway.
--connection con_temp1
BEGIN;
INSERT INTO t2 VALUES (21);

--connection server_2
START SLAVE;
# Wait for the MyISAM change to be visible, after which replication will wait
# for con_temp1 to roll back.
--let $wait_condition= SELECT COUNT(*) = 1 FROM t1 WHERE a=20
--source include/wait_condition.inc

--connection con_temp2
# Initiate slave stop. It will have to wait for the current event group
# to complete.
# The dbug injection causes debug_sync to signal 'wait_for_done_waiting'
# when the SQL driver thread is ready.
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,rpl_parallel_wait_for_done_trigger";
send STOP SLAVE;

--connection con_temp1
SET debug_sync='now WAIT_FOR wait_for_done_waiting';
ROLLBACK;

--connection con_temp2
reap;
SET GLOBAL debug_dbug=@old_dbug;
SET debug_sync='RESET';

--connection server_2
--source include/wait_for_slave_to_stop.inc
# We should see the first transaction applied, but not the two others.
SELECT * FROM t1 WHERE a >= 20 ORDER BY a;
SELECT * FROM t2 WHERE a >= 20 ORDER BY a;
SELECT * FROM t3 WHERE a >= 20 ORDER BY a;

--source include/start_slave.inc
--sync_with_master
SELECT * FROM t1 WHERE a >= 20 ORDER BY a;
SELECT * FROM t2 WHERE a >= 20 ORDER BY a;
SELECT * FROM t3 WHERE a >= 20 ORDER BY a;

--connection server_2
# Respawn all worker threads to clear any left-over debug_sync or other stuff.
--source include/stop_slave.inc
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';
--disconnect con_temp1
--disconnect con_temp2

--connection server_1
DROP TABLE t1,t2,t3;
SET DEBUG_SYNC= 'RESET';

--source include/rpl_end.inc
