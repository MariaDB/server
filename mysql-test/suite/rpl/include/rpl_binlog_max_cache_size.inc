########################################################################################
#    This test verifies if the binlog is not corrupted when the cache buffer is not
#    big enough to accommodate the changes and is divided in five steps:
#
#    1 - Single Statements:
#    1.1 - Single statement on transactional table.
#    1.2 - Single statement on non-transactional table. 
#    1.3 - Single statement on both transactional and non-transactional tables.
#    In both 1.2 and 1.3, an incident event is logged to notify the user that the
#    master and slave are diverging.
#
#    2 - Transactions ended by an implicit commit.
#
#    3 - Transactions ended by a COMMIT.
#
#    4 - Transactions ended by a ROLLBACK.
#
#    5 - Transactions with a failing statement that updates a non-transactional
#    table. In this case, a failure means that the statement does not get into
#    the cache and an incident event is logged to notify the user that the master
#    and slave are diverging.
#    
########################################################################################

--connection slave
--source include/stop_slave.inc
CHANGE MASTER TO MASTER_USE_GTID=NO;
--source include/start_slave.inc
--connection master

call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");
call mtr.add_suppression("Write to binary log failed: Multi-row statements required more than .max_binlog_stmt_cache_size");
call mtr.add_suppression("Write to binary log failed: Multi-statement transaction required more than .max_binlog_cache_size");

let $old_max_binlog_cache_size= query_get_value(SHOW VARIABLES LIKE "max_binlog_cache_size", Value, 1);
let $old_binlog_cache_size= query_get_value(SHOW VARIABLES LIKE "binlog_cache_size", Value, 1);
let $old_max_binlog_stmt_cache_size= query_get_value(SHOW VARIABLES LIKE "max_binlog_stmt_cache_size", Value, 1);
let $old_binlog_stmt_cache_size= query_get_value(SHOW VARIABLES LIKE "binlog_stmt_cache_size", Value, 1);

SET GLOBAL max_binlog_cache_size = 4096;
SET GLOBAL binlog_cache_size = 4096;
SET GLOBAL max_binlog_stmt_cache_size = 4096;
SET GLOBAL binlog_stmt_cache_size = 4096;
disconnect master;
connect (master,127.0.0.1,root,,test,$MASTER_MYPORT,);

CREATE TABLE t1(a INT PRIMARY KEY, data VARCHAR(30000)) ENGINE=Innodb CHARSET=latin1;
CREATE TABLE t2(a INT PRIMARY KEY, data VARCHAR(30000)) ENGINE=MyIsam CHARSET=latin1;
CREATE TABLE t3(a INT PRIMARY KEY, data VARCHAR(30000)) ENGINE=Innodb CHARSET=latin1;

let $data = `select concat('"', repeat('a',2000), '"')`;

--echo ########################################################################################
--echo #                                   1 - SINGLE STATEMENT
--echo ########################################################################################

connection master;

--echo *** Single statement on transactional table ***
--disable_query_log
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE, 1534
eval INSERT INTO t1 (a, data) VALUES (1,
     CONCAT($data, $data, $data, $data, $data));
--enable_query_log

--echo *** Single statement on non-transactional table ***
--disable_query_log
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE, 1534
eval INSERT INTO t2 (a, data) VALUES (2,
       CONCAT($data, $data, $data, $data, $data, $data));
--enable_query_log

# Incident event
# 1590=ER_SLAVE_INCIDENT
--let $slave_sql_errno= 1590
--source include/wait_for_slave_sql_error_and_skip.inc

--disable_query_log
eval INSERT INTO t1 (a, data) VALUES (3, $data);
eval INSERT INTO t1 (a, data) VALUES (4, $data);
eval INSERT INTO t1 (a, data) VALUES (5, $data);
eval INSERT INTO t2 (a, data) VALUES (3, $data);
eval INSERT INTO t2 (a, data) VALUES (4, $data);
eval INSERT INTO t2 (a, data) VALUES (5, $data);
--enable_query_log

--echo *** Single statement on both transactional and non-transactional tables. ***
--disable_query_log
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
eval UPDATE t2, t1 SET t2.data = CONCAT($data, $data, $data, $data),
                       t1.data = CONCAT($data, $data, $data, $data);
--enable_query_log

# 1590=ER_SLAVE_INCIDENT
--let $slave_sql_errno= 1590
--let $slave_skip_counter= `SELECT IF(@@binlog_format = 'ROW', 2, 1)`
--source include/wait_for_slave_sql_error_and_skip.inc

--let $diff_tables= master:t1,slave:t1
--source include/diff_tables.inc

--echo ########################################################################################
--echo #                             2 - BEGIN - IMPLICIT COMMIT by DDL
--echo ########################################################################################

connection master;
TRUNCATE TABLE t1;
TRUNCATE TABLE t2;
TRUNCATE TABLE t3;
set default_storage_engine=innodb;

BEGIN;
--disable_query_log
--eval INSERT INTO t1 (a, data) VALUES (1, $data);
--eval INSERT INTO t1 (a, data) VALUES (2, $data);
--eval INSERT INTO t1 (a, data) VALUES (3, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (4, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (5, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (6, $data);
--enable_query_log
--eval INSERT INTO t1 (a, data) VALUES (7, 's');
--eval INSERT INTO t2 (a, data) VALUES (8, 's');
--eval INSERT INTO t1 (a, data) VALUES (9, 's');

ALTER TABLE t3 ADD COLUMN d int;

--disable_query_log
--eval INSERT INTO t2 (a, data) VALUES (10, $data);
--eval INSERT INTO t2 (a, data) VALUES (11, $data);
--eval INSERT INTO t2 (a, data) VALUES (12, $data);
--eval INSERT INTO t2 (a, data) VALUES (13, $data);
--enable_query_log

BEGIN;
--disable_query_log
--eval INSERT INTO t1 (a, data) VALUES (14, $data);
--eval INSERT INTO t1 (a, data) VALUES (15, $data);
--eval INSERT INTO t1 (a, data) VALUES (16, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (17, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (18, $data);
--enable_query_log
--eval INSERT INTO t1 (a, data) VALUES (19, 's');
--eval INSERT INTO t2 (a, data) VALUES (20, 's');
--eval INSERT INTO t1 (a, data) VALUES (21, 's');

if (`SELECT @@binlog_format = 'STATEMENT' || @@binlog_format = 'MIXED'`)
{
  CREATE TABLE t4 SELECT * FROM t1;
}
if (`SELECT @@binlog_format = 'ROW'`)
{
  --error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
  CREATE TABLE t4 SELECT * FROM t1;
}

--disable_query_log
--eval INSERT INTO t2 (a, data) VALUES (15, $data);
--enable_query_log

BEGIN;
--disable_query_log
--eval INSERT INTO t1 (a, data) VALUES (22, $data);
--eval INSERT INTO t1 (a, data) VALUES (23, $data);
--eval INSERT INTO t1 (a, data) VALUES (24, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (25, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (26, $data);
--enable_query_log
--eval INSERT INTO t1 (a, data) VALUES (27, 's');
--eval INSERT INTO t2 (a, data) VALUES (28, 's');
--eval INSERT INTO t1 (a, data) VALUES (29, 's');

CREATE TABLE t5 (a int);

--sync_slave_with_master
--let $diff_tables= master:t1,slave:t1
--source include/diff_tables.inc

--echo ########################################################################################
--echo #                                     3 - BEGIN - COMMIT
--echo ########################################################################################

connection master;
TRUNCATE TABLE t1;
TRUNCATE TABLE t2;
TRUNCATE TABLE t3;

BEGIN;
--disable_query_log
--eval INSERT INTO t1 (a, data) VALUES (1, $data);
--eval INSERT INTO t1 (a, data) VALUES (2, $data);
--eval INSERT INTO t1 (a, data) VALUES (3, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (4, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (5, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (6, $data);
--eval INSERT INTO t1 (a, data) VALUES (7, 's');
--eval INSERT INTO t2 (a, data) VALUES (8, 's');
--eval INSERT INTO t1 (a, data) VALUES (9, 's');
--enable_query_log
COMMIT;

--sync_slave_with_master
--let $diff_tables= master:t1,slave:t1
--source include/diff_tables.inc

--echo ########################################################################################
--echo #                                      4 - BEGIN - ROLLBACK
--echo ########################################################################################

connection master;
TRUNCATE TABLE t1;
TRUNCATE TABLE t2;
TRUNCATE TABLE t3;

BEGIN;
--disable_query_log
--eval INSERT INTO t1 (a, data) VALUES (1, $data);
--eval INSERT INTO t1 (a, data) VALUES (2, $data);
--eval INSERT INTO t1 (a, data) VALUES (3, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (4, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (5, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (6, $data);
--eval INSERT INTO t1 (a, data) VALUES (7, 's');
--eval INSERT INTO t2 (a, data) VALUES (8, 's');
--eval INSERT INTO t1 (a, data) VALUES (9, 's');
--enable_query_log
ROLLBACK;

--sync_slave_with_master
--let $diff_tables= master:t1,slave:t1
--source include/diff_tables.inc

--echo ########################################################################################
--echo #                                         5 - PROCEDURE 
--echo ########################################################################################

connection master;
TRUNCATE TABLE t1;
TRUNCATE TABLE t2;
TRUNCATE TABLE t3;

DELIMITER //;

CREATE PROCEDURE p1(pd VARCHAR(30000) CHARACTER SET latin1)
BEGIN
  INSERT INTO t1 (a, data) VALUES (1, pd);
  INSERT INTO t1 (a, data) VALUES (2, pd);
  INSERT INTO t1 (a, data) VALUES (3, pd);
  INSERT INTO t1 (a, data) VALUES (4, pd);
  INSERT INTO t1 (a, data) VALUES (5, 's');
END//

DELIMITER ;//

TRUNCATE TABLE t1;

--disable_query_log
eval CALL p1($data);
--enable_query_log

TRUNCATE TABLE t1;

BEGIN;
--disable_query_log
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
eval CALL p1($data);
--enable_query_log
COMMIT;

TRUNCATE TABLE t1;

BEGIN;
--disable_query_log
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
eval CALL p1($data);
--enable_query_log
ROLLBACK;

--sync_slave_with_master
--let $diff_tables= master:t1,slave:t1
--source include/diff_tables.inc

--echo ########################################################################################
--echo #                                           6 - XID
--echo ########################################################################################

connection master;
TRUNCATE TABLE t1;
TRUNCATE TABLE t2;
TRUNCATE TABLE t3;

BEGIN;
--disable_query_log
--eval INSERT INTO t1 (a, data) VALUES (1, $data);
--eval INSERT INTO t1 (a, data) VALUES (2, $data);
--eval INSERT INTO t1 (a, data) VALUES (3, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (4, $data);
SAVEPOINT sv;
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (5, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (6, $data);
--enable_query_log
--eval INSERT INTO t1 (a, data) VALUES (7, 's');
--eval INSERT INTO t2 (a, data) VALUES (8, 's');
--eval INSERT INTO t1 (a, data) VALUES (9, 's');
ROLLBACK TO sv;
COMMIT;

--sync_slave_with_master
--let $diff_tables= master:t1,slave:t1
--source include/diff_tables.inc

--echo ########################################################################################
--echo #                                        7 - NON-TRANS TABLE
--echo ########################################################################################

connection master;
TRUNCATE TABLE t1;
TRUNCATE TABLE t2;
TRUNCATE TABLE t3;

BEGIN;
--disable_query_log
--eval INSERT INTO t1 (a, data) VALUES (1, $data);
--eval INSERT INTO t1 (a, data) VALUES (2, $data);
--eval INSERT INTO t2 (a, data) VALUES (3, $data);
--eval INSERT INTO t1 (a, data) VALUES (4, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (5, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (6, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (7, $data);
--eval UPDATE t2 SET data= CONCAT($data, $data);
--enable_query_log
--eval INSERT INTO t1 (a, data) VALUES (8, 's');
--eval INSERT INTO t1 (a, data) VALUES (9, 's');
--eval INSERT INTO t2 (a, data) VALUES (10, 's');
--eval INSERT INTO t1 (a, data) VALUES (11, 's');
COMMIT;

BEGIN;
--disable_query_log
--eval INSERT INTO t1 (a, data) VALUES (15, $data);
--eval INSERT INTO t1 (a, data) VALUES (16, $data);
--eval INSERT INTO t2 (a, data) VALUES (17, $data);
--eval INSERT INTO t1 (a, data) VALUES (18, $data);
--error ER_TRANS_CACHE_FULL, ER_STMT_CACHE_FULL, ER_ERROR_ON_WRITE
--eval INSERT INTO t1 (a, data) VALUES (19, $data);
--enable_query_log
COMMIT;

--sync_slave_with_master
--let $diff_tables= master:t1,slave:t1
--source include/diff_tables.inc

--echo ########################################################################
--echo #      8 - Bug#55375(Regression Bug) Transaction bigger than
--echo #          max_binlog_cache_size crashes slave
--echo ########################################################################

--echo # [ On Slave ]
SET GLOBAL max_binlog_cache_size = 4096;
SET GLOBAL binlog_cache_size = 4096;
SET GLOBAL max_binlog_stmt_cache_size = 4096;
SET GLOBAL binlog_stmt_cache_size = 4096;

source include/stop_slave.inc;
source include/start_slave.inc;
CALL mtr.add_suppression("Multi-statement transaction required more than 'max_binlog_cache_size' bytes of storage.*");
CALL mtr.add_suppression("Multi-statement transaction required more than 'max_binlog_stmt_cache_size' bytes of storage.*");
CALL mtr.add_suppression("Writing one row to the row-based binary log failed.*");
CALL mtr.add_suppression("Slave SQL.*The incident LOST_EVENTS occurred on the master. Message: error writing to the binary log");

connection master;
TRUNCATE t1;

sync_slave_with_master;
--let binlog_start= query_get_value(SHOW MASTER STATUS, Position, 1)
--let binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)

connection master;
--replace_result $old_max_binlog_cache_size ORIGINAL_VALUE
--eval SET GLOBAL max_binlog_cache_size= $old_max_binlog_cache_size
--replace_result $old_binlog_cache_size ORIGINAL_VALUE
--eval SET GLOBAL binlog_cache_size= $old_binlog_cache_size
--replace_result $old_max_binlog_stmt_cache_size ORIGINAL_VALUE
--eval SET GLOBAL max_binlog_stmt_cache_size= $old_max_binlog_stmt_cache_size
--replace_result $old_binlog_stmt_cache_size ORIGINAL_VALUE
--eval SET GLOBAL binlog_stmt_cache_size= $old_binlog_stmt_cache_size
disconnect master;
connect (master,127.0.0.1,root,,test,$MASTER_MYPORT,);

--let $n=128
BEGIN;
--disable_query_log
--echo Repeat statement 'INSERT INTO t1 VALUES(\$n, repeat("a", 32))' $n times
while ($n)
{
  --eval INSERT INTO t1 VALUES ($n, repeat("a", 32))
  --dec $n
}
--enable_query_log
COMMIT;

--connection slave
# Multi-statement transaction raised ER_TRANS_CACHE_FULL (4096=128x32)
# that will stop SQL slave thread
--let $slave_sql_errno= 1197
if (`SELECT @@binlog_format = 'ROW'`)
{
  --let $slave_sql_errno= 1534
}
source include/wait_for_slave_sql_error.inc;

SELECT count(*) FROM t1;
source include/show_binlog_events.inc;

--replace_result $old_max_binlog_cache_size ORIGINAL_VALUE
--eval SET GLOBAL max_binlog_cache_size= $old_max_binlog_cache_size
--replace_result $old_binlog_cache_size ORIGINAL_VALUE
--eval SET GLOBAL binlog_cache_size= $old_binlog_cache_size
--replace_result $old_max_binlog_stmt_cache_size ORIGINAL_VALUE
--eval SET GLOBAL max_binlog_stmt_cache_size= $old_max_binlog_stmt_cache_size
--replace_result $old_binlog_stmt_cache_size ORIGINAL_VALUE
--eval SET GLOBAL binlog_stmt_cache_size= $old_binlog_stmt_cache_size

# SQL slave is stopped, stop only IO thread
source include/stop_slave_io.inc;
source include/start_slave.inc;

connection master;
sync_slave_with_master;
SELECT count(*) FROM t1;

--echo ########################################################################################
--echo #                                        CLEAN
--echo ########################################################################################

connection master;
DROP TABLE t1;
DROP TABLE t2;
DROP TABLE t3;
# t4 exists only if binlog_format!=row, so so a warning is generated
# if binog_format=row
--disable_warnings
DROP TABLE IF EXISTS t4;
--enable_warnings
DROP TABLE t5;
DROP PROCEDURE p1;
