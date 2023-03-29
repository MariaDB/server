--echo *** MDEV-6775: Wrong binlog order in parallel replication ***

# A bit tricky bug to reproduce. On the master, we binlog in statement-mode
# two transactions, an UPDATE followed by a DELETE. On the slave, we replicate
# with binlog-mode set to ROW, which means the DELETE, which modifies no rows,
# is not binlogged. Then we inject a wait in the group commit code on the
# slave, shortly before the actual commit of the UPDATE. The bug was that the
# DELETE could wake up from wait_for_prior_commit() before the commit of the
# UPDATE. So the test could see the slave position updated to after DELETE,
# while the UPDATE was still not visible.

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
CREATE TABLE t4 (a INT PRIMARY KEY, b INT, KEY b_idx(b)) ENGINE=InnoDB;
INSERT INTO t4 VALUES (1,NULL), (3,NULL), (4,4), (5, NULL), (6, 6);
--connect (con1,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--connect (con2,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug="+d,inject_binlog_commit_before_get_LOCK_log";
SET @old_format=@@GLOBAL.binlog_format;
SET GLOBAL binlog_format=ROW;
# Re-spawn the worker threads to be sure they pick up the new binlog format
SET GLOBAL slave_parallel_threads=0;
SET GLOBAL slave_parallel_threads=10;

--connection con1
SET @old_format= @@binlog_format;
SET binlog_format= statement;
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued1 WAIT_FOR master_cont1';
send UPDATE t4 SET b=NULL WHERE a=6;
--connection server_1
SET debug_sync='now WAIT_FOR master_queued1';

--connection con2
SET @old_format= @@binlog_format;
SET binlog_format= statement;
SET debug_sync='commit_after_release_LOCK_prepare_ordered SIGNAL master_queued2';
send DELETE FROM t4 WHERE b <= 3;

--connection server_1
SET debug_sync='now WAIT_FOR master_queued2';
SET debug_sync='now SIGNAL master_cont1';

--connection con1
REAP;
SET binlog_format= @old_format;
--connection con2
REAP;
SET binlog_format= @old_format;
SET debug_sync='RESET';
--save_master_pos
SELECT * FROM t4 ORDER BY a;

--connection server_2
--source include/start_slave.inc
SET debug_sync= 'now WAIT_FOR waiting';
--sync_with_master
SELECT * FROM t4 ORDER BY a;
SET debug_sync= 'now SIGNAL cont';

# Re-spawn the worker threads to remove any DBUG injections or DEBUG_SYNC.
--source include/stop_slave.inc
SET GLOBAL debug_dbug=@old_dbug;
SET GLOBAL binlog_format= @old_format;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc
SET DEBUG_SYNC= 'RESET';

--connection server_1
DROP TABLE t4;
SET DEBUG_SYNC= 'RESET';
--disconnect con1
--disconnect con2
--source include/rpl_end.inc
