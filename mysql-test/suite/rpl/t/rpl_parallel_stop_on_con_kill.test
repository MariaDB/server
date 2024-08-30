--echo *** MDEV-8031: Parallel replication stops on "connection killed" error (probably incorrectly handled deadlock kill) ***

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
CREATE TABLE t3 (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
INSERT INTO t3 VALUES (201,0), (202,0);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug= '+d,inject_mdev8031';

--connection server_1
# We artificially create a situation that hopefully resembles the original
# bug which was only seen "in the wild", and only once.
# Setup a fake group commit with lots of conflicts that will lead to deadloc
# kill. The slave DBUG injection causes the slave to be deadlock killed at
# a particular point during the retry, and then later do a small sleep at
# another critical point where the prior transaction then has a chance to
# complete. Finally an extra KILL check catches an unhandled, lingering
# deadlock kill. So rather artificial, but at least it exercises the
# relevant code paths.
SET @old_dbug= @@SESSION.debug_dbug;
SET SESSION debug_dbug="+d,binlog_force_commit_id";

SET @commit_id= 10200;
INSERT INTO t3 VALUES (203, 1);
INSERT INTO t3 VALUES (204, 1);
INSERT INTO t3 VALUES (205, 1);
UPDATE t3 SET b=b+1 WHERE a=201;
UPDATE t3 SET b=b+1 WHERE a=201;
UPDATE t3 SET b=b+1 WHERE a=201;
UPDATE t3 SET b=b+1 WHERE a=202;
UPDATE t3 SET b=b+1 WHERE a=202;
UPDATE t3 SET b=b+1 WHERE a=202;
UPDATE t3 SET b=b+1 WHERE a=202;
UPDATE t3 SET b=b+1 WHERE a=203;
UPDATE t3 SET b=b+1 WHERE a=203;
UPDATE t3 SET b=b+1 WHERE a=204;
UPDATE t3 SET b=b+1 WHERE a=204;
UPDATE t3 SET b=b+1 WHERE a=204;
UPDATE t3 SET b=b+1 WHERE a=203;
UPDATE t3 SET b=b+1 WHERE a=205;
UPDATE t3 SET b=b+1 WHERE a=205;
SET SESSION debug_dbug=@old_dbug;

SELECT * FROM t3 WHERE a>=200 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t3 WHERE a>=200 ORDER BY a;
--source include/stop_slave.inc
SET GLOBAL debug_dbug= @old_dbug;
--source include/start_slave.inc


--echo *** Check getting deadlock killed inside open_binlog() during retry. ***

--connection server_2
--source include/stop_slave.inc
SET @old_dbug= @@GLOBAL.debug_dbug;
SET GLOBAL debug_dbug= '+d,inject_retry_event_group_open_binlog_kill';
SET @old_max= @@GLOBAL.max_relay_log_size;
SET GLOBAL max_relay_log_size= 4096;

--connection server_1
SET @old_dbug= @@SESSION.debug_dbug;
SET SESSION debug_dbug="+d,binlog_force_commit_id";

--let $large= `SELECT REPEAT("*", 8192)`
SET @commit_id= 10210;
--echo Omit long queries that cause relaylog rotations and transaction retries...
--disable_query_log
eval UPDATE t3 SET b=b+1 WHERE a=201 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=201 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=201 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=202 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=202 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=202 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=202 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=203 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=203 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=204 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=204 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=204 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=203 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=205 /* $large */;
eval UPDATE t3 SET b=b+1 WHERE a=205 /* $large */;
--enable_query_log
SET SESSION debug_dbug=@old_dbug;

SELECT * FROM t3 WHERE a>=200 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc

SELECT * FROM t3 WHERE a>=200 ORDER BY a;

# Cleanup
--source include/stop_slave.inc
SET GLOBAL debug_dbug= @old_debg;
SET GLOBAL max_relay_log_size= @old_max;
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc

--connection server_1
DROP TABLE t3;

--source include/rpl_end.inc

