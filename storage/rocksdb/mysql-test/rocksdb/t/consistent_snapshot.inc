#
# TRANSACTION WITH CONSISTENT SNAPSHOT
#

--enable_connect_log

# Save the initial number of concurrent sessions
--source include/count_sessions.inc

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;

CREATE TABLE t1 (a INT, pk INT AUTO_INCREMENT PRIMARY KEY) ENGINE=ROCKSDB;
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;

# While a consistent snapshot transaction is executed,
# no external inserts should be visible to the transaction.
# But it should only work this way for REPEATABLE-READ and SERIALIZABLE

--error 0,ER_ISOLATION_LEVEL_WITH_CONSISTENT_SNAPSHOT
START TRANSACTION WITH CONSISTENT SNAPSHOT;
--echo ERROR: $mysql_errno

connection con2;
select * from information_schema.rocksdb_dbstats where stat_type='DB_NUM_SNAPSHOTS';

connection con1;
COMMIT;

# verifying snapshot is released after finishing transaction
connection con2;
select * from information_schema.rocksdb_dbstats where stat_type='DB_NUM_SNAPSHOTS';

connection con1;
--error 0,ER_ISOLATION_LEVEL_WITH_CONSISTENT_SNAPSHOT
START TRANSACTION WITH CONSISTENT SNAPSHOT;
--echo ERROR: $mysql_errno

connection con2;
INSERT INTO t1 (a) VALUES (1);

connection con1;
--echo # If consistent read works on this isolation level ($trx_isolation), the following SELECT should not return the value we inserted (1)
SELECT a FROM t1; 
COMMIT;

connection default;
disconnect con1;
disconnect con2;
DROP TABLE t1;

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;
CREATE TABLE r1 (id int primary key, value int, value2 int) engine=ROCKSDB;
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;
insert into r1 values (1,1,1),(2,2,2),(3,3,3),(4,4,4);

BEGIN;

connection con2;
INSERT INTO r1 values (5,5,5);

connection con1;
SELECT * FROM r1; # 5

connection con2;
INSERT INTO r1 values (6,6,6);

connection con1;
SELECT * FROM r1; # 5
COMMIT;
SELECT * FROM r1; # 6

--error 0,ER_ISOLATION_LEVEL_WITH_CONSISTENT_SNAPSHOT
START TRANSACTION WITH CONSISTENT SNAPSHOT;
--echo ERROR: $mysql_errno

connection con2;
INSERT INTO r1 values (7,7,7);

connection con1;
SELECT * FROM r1; # 6

connection con2;
INSERT INTO r1 values (8,8,8);

connection con1;
SELECT * FROM r1; # 6
COMMIT;
SELECT * FROM r1; # 8

--error 0,ER_ISOLATION_LEVEL_WITH_CONSISTENT_SNAPSHOT
START TRANSACTION WITH CONSISTENT SNAPSHOT;
--echo ERROR: $mysql_errno

connection con2;
INSERT INTO r1 values (9,9,9);

connection con1;
--error 0,ER_ISOLATION_LEVEL_WITH_CONSISTENT_SNAPSHOT
START TRANSACTION WITH CONSISTENT SNAPSHOT;
--echo ERROR: $mysql_errno

connection con2;
INSERT INTO r1 values (10,10,10);

connection con1;
SELECT * FROM r1; # 9

--error 0,ER_ISOLATION_LEVEL_WITH_CONSISTENT_SNAPSHOT
START TRANSACTION WITH CONSISTENT SNAPSHOT;
--echo ERROR: $mysql_errno
# Succeeds with Read Committed, Fails with Repeatable Read
--error 0,ER_UPDATES_WITH_CONSISTENT_SNAPSHOT
INSERT INTO r1 values (11,11,11);
--echo ERROR: $mysql_errno
SELECT * FROM r1; # self changes should be visible


drop table r1;

connection default;
disconnect con1;
disconnect con2;


--source include/wait_until_count_sessions.inc

