--echo *** MDEV-7237: Parallel replication: incorrect relaylog position after stop/start the slave ***

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

--connection server_2
--source include/stop_slave.inc
CALL mtr.add_suppression("Commit failed due to failure of an earlier commit on which this one depends");
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
SET @old_parallel_mode=@@GLOBAL.slave_parallel_mode;
SET GLOBAL slave_parallel_threads=10;
# Test assumes that 'conservative' mode is in effect. i.e
# Do not start parallel execution of this event group until all prior groups
# have reached the commit phase. Upon execution of STOP SLAVE there can be one
# group which is executing and the rest are doing group commit order wait.
SET GLOBAL slave_parallel_mode='conservative';
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t2 VALUES (40);
--save_master_pos

--connection server_2
--sync_with_master
--connect (con_temp2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
--source include/stop_slave.inc
CHANGE MASTER TO master_use_gtid=no;
SET @old_dbug= @@GLOBAL.debug_dbug;
# This DBUG injection causes a DEBUG_SYNC signal "scheduled_gtid_0_x_100" when
# GTID 0-1-100 has been scheduled for and fetched by a worker thread.
SET GLOBAL debug_dbug="+d,rpl_parallel_scheduled_gtid_0_x_100";
# This DBUG injection causes a DEBUG_SYNC signal "wait_for_done_waiting" when
# STOP SLAVE has signalled all worker threads to stop.
SET GLOBAL debug_dbug="+d,rpl_parallel_wait_for_done_trigger";
# Reset worker threads to make DBUG setting catch on.
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;


--connection server_1
# Setup some transaction for the slave to replicate.
INSERT INTO t2 VALUES (41);
INSERT INTO t2 VALUES (42);
# Need to log the DELETE in statement format, so we can see it in processlist.
SET @old_format= @@binlog_format;
SET binlog_format= statement;
DELETE FROM t2 WHERE a=40;
SET binlog_format= @old_format;
INSERT INTO t2 VALUES (43);
INSERT INTO t2 VALUES (44);
# Force the slave to switch to a new relay log file.
FLUSH LOGS;
INSERT INTO t2 VALUES (45);
# Inject a GTID 0-1-100, which will trigger a DEBUG_SYNC signal when this
# transaction has been fetched by a worker thread.
SET gtid_seq_no=100;
INSERT INTO t2 VALUES (46);
--save_master_pos

--connection con_temp2
# Temporarily block the DELETE on a=40 from completing.
BEGIN;
SELECT * FROM t2 WHERE a=40 FOR UPDATE;

--connection server_2
--source include/start_slave.inc

# Wait for a worker thread to start on the DELETE that will be blocked
# temporarily by the SELECT FOR UPDATE.
--let $wait_condition= SELECT count(*) > 0 FROM information_schema.processlist WHERE state='updating' and info LIKE '%DELETE FROM t2 WHERE a=40%'
--source include/wait_condition.inc

# The DBUG injection set above will make the worker thread signal the following
# debug_sync when the GTID 0-1-100 has been reached by a worker thread.
# Thus, at this point, the SQL driver thread has reached the next
# relay log file name, while a worker thread is still processing a
# transaction in the previous relay log file, blocked on the SELECT FOR
# UPDATE.
SET debug_sync= 'now WAIT_FOR scheduled_gtid_0_x_100';
# At this point, the SQL driver thread is in the new relay log file, while
# the DELETE from the old relay log file is not yet complete. We will stop
# the slave at this point. The bug was that the DELETE statement would
# update the slave position to the _new_ relay log file name instead of
# its own old file name. Thus, by stoping and restarting the slave at this
# point, we would get an error at restart due to incorrect position. (If
# we would let the slave catch up before stopping, the incorrect position
# would be corrected by a later transaction).

send STOP SLAVE;

--connection con_temp2
# Wait for STOP SLAVE to have proceeded sufficiently that it has signalled
# all worker threads to stop; this ensures that we will stop after the DELETE
# transaction (and not after a later transaction that might have been able
# to set a fixed position).
SET debug_sync= 'now WAIT_FOR wait_for_done_waiting';
# Now release the row lock that was blocking the replication of DELETE.
ROLLBACK;

--connection server_2
reap;
--source include/wait_for_slave_sql_to_stop.inc
SELECT * FROM t2 WHERE a >= 40 ORDER BY a;
# Now restart the slave. With the bug present, this would start at an
# incorrect relay log position, causing relay log read error (or if unlucky,
# silently skip a number of events).
--source include/start_slave.inc
--sync_with_master
SELECT * FROM t2 WHERE a >= 40 ORDER BY a;

# Clean up.
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET DEBUG_SYNC= 'RESET';
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
SET GLOBAL slave_parallel_mode=@old_parallel_mode;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
DROP TABLE t2;

--source include/rpl_end.inc
