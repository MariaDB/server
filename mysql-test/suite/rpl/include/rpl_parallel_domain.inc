# Test should work with both conservative and optimistic modes

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/master-slave.inc

# Test various aspects of parallel replication.

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
--error ER_SLAVE_MUST_STOP
SET GLOBAL slave_parallel_threads=10;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;

# Check that we do not spawn any worker threads when no slave is running.
SELECT IF(COUNT(*) < 10, "OK", CONCAT("Found too many system user processes: ", COUNT(*))) FROM information_schema.processlist WHERE user = "system user";

CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

# Check that worker threads get spawned when slave starts.
SELECT IF(COUNT(*) >= 10, "OK", CONCAT("Found too few system user processes: ", COUNT(*))) FROM information_schema.processlist WHERE user = "system user";
# ... and that worker threads get removed when slave stops.
--source include/stop_slave.inc
SELECT IF(COUNT(*) < 10, "OK", CONCAT("Found too many system user processes: ", COUNT(*))) FROM information_schema.processlist WHERE user = "system user";
--source include/start_slave.inc
SELECT IF(COUNT(*) >= 10, "OK", CONCAT("Found too few system user processes: ", COUNT(*))) FROM information_schema.processlist WHERE user = "system user";

--echo *** Test long-running query in domain 1 can run in parallel with short queries in domain 0 ***

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t1 VALUES (1);
INSERT INTO t2 VALUES (1);
--save_master_pos

--connection server_2
--sync_with_master

# Block the table t1 to simulate a replicated query taking a long time.
--connect (con_temp1,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
LOCK TABLE t1 WRITE;

--connection server_1
SET gtid_domain_id=1;
# This query will be blocked on the slave until UNLOCK TABLES.
INSERT INTO t1 VALUES (2);
SET gtid_domain_id=0;
# These t2 queries can be replicated in parallel with the prior t1 query, as
# they are in a separate replication domain.
INSERT INTO t2 VALUES (2);
INSERT INTO t2 VALUES (3);
BEGIN;
INSERT INTO t2 VALUES (4);
INSERT INTO t2 VALUES (5);
COMMIT;
INSERT INTO t2 VALUES (6);

--connection server_2
--let $wait_condition= SELECT COUNT(*) = 6 FROM t2
--source include/wait_condition.inc

SELECT * FROM t2 ORDER by a;

--connection con_temp1
SELECT * FROM t1;
UNLOCK TABLES;

--connection server_2
--let $wait_condition= SELECT COUNT(*) = 2 FROM t1
--source include/wait_condition.inc

SELECT * FROM t1 ORDER BY a;

# Clean up.
--connection server_2
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc

--connection server_1
DROP TABLE t1,t2;
--source include/rpl_end.inc
