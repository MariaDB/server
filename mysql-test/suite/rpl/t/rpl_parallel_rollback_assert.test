--echo *** MDEV-8725: Assertion on ROLLBACK statement in the binary log ***
--source include/have_innodb.inc
--source include/master-slave.inc

--connection server_2
SET @old_parallel_threads=@@GLOBAL.slave_parallel_threads;
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=10;
CHANGE MASTER TO master_use_gtid=slave_pos;
--source include/start_slave.inc

--connection server_1
ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;
CREATE TABLE t1 (a int PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE t2 (a int PRIMARY KEY) ENGINE=InnoDB;
--save_master_pos

--connection server_2
--sync_with_master

--connection server_1
# Inject an event group terminated by ROLLBACK, by mixing MyISAM and InnoDB
# in a transaction. The bug was an assertion on the ROLLBACK due to
# mark_start_commit() being already called.
--disable_warnings
BEGIN;
INSERT INTO t2 VALUES (2000);
INSERT INTO t1 VALUES (2000);
INSERT INTO t2 VALUES (2001);
ROLLBACK;
--enable_warnings
SELECT * FROM t1 WHERE a>=2000 ORDER BY a;
SELECT * FROM t2 WHERE a>=2000 ORDER BY a;
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc

--connection server_1
INSERT INTO t2 VALUES (2020);
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
SELECT * FROM t2 WHERE a>=2000 ORDER BY a;
let $rows_in_t1= `SELECT COUNT(*) FROM t1 WHERE a>=2000 ORDER BY a`;
if ($rows_in_t1 == 0)
{
--query_vertical SHOW SLAVE STATUS
}
SELECT * FROM t1 WHERE a>=2000 ORDER BY a;

# Clean up.
--source include/stop_slave.inc
SET GLOBAL slave_parallel_threads=@old_parallel_threads;
--source include/start_slave.inc

--connection server_1
DROP TABLE t1,t2;

--source include/rpl_end.inc

