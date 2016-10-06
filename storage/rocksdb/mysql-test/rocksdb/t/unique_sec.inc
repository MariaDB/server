# Save the initial number of concurrent sessions
--source include/count_sessions.inc

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;

CREATE TABLE t1 (id1 INT NOT NULL, id2 INT NOT NULL, id3 VARCHAR(32),
                 id4 INT, id5 VARCHAR(32),
                 value1 INT, value2 INT, value3 VARCHAR(32),
                 PRIMARY KEY (id1, id2) ##CF##, 
                 UNIQUE INDEX (id2, id1) ##CF##,
                 UNIQUE INDEX (id2, id3, id4) ##CF##,
                 INDEX (id1) ##CF##,
                 INDEX (id3, id1) ##CF##,
                 UNIQUE INDEX(id5) ##CF##, 
                 INDEX (id2, id5)) ENGINE=ROCKSDB;

--disable_query_log
let $max = 10;
let $i = 1;
while ($i <= $max) {
  let $insert = INSERT INTO t1 VALUES ($i, $i, $i, $i, $i, $i, $i, $i);
  inc $i;
  eval $insert;
}
--enable_query_log

SELECT COUNT(*) FROM t1;

--echo # Test inserting a key that returns duplicate error
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (1, 1, 11, 11, 11, 11, 11, 11);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (5, 5, 11, 11, 11, 11, 11, 11);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (10, 10, 11, 11, 11, 11, 11, 11);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (11, 1, 1, 1, 11, 11, 11, 11);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (11, 5, 5, 5, 11, 11, 11, 11);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (11, 10, 10, 10, 11, 11, 11, 11);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (11, 11, 11, 11, 1, 11, 11, 11);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (11, 11, 11, 11, 5, 11, 11, 11);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (11, 11, 11, 11, 10, 11, 11, 11);

--echo # Test updating a key that returns duplicate error
--error ER_DUP_ENTRY
UPDATE t1 SET id2=1, id3=1, id4=1 WHERE id1=2;
--error ER_DUP_ENTRY
UPDATE t1 SET id2=1, id3=1, id4=1;

SELECT COUNT(*) FROM t1;

--echo # Test updating a key to itself
UPDATE t1 set id2=id4; 
UPDATE t1 set id5=id3, value1=value2;
UPDATE t1 set value3=value1;

--echo # Test modifying values should not cause duplicates
UPDATE t1 SET value1=value3+1;
UPDATE t1 SET value3=value3 div 2;
UPDATE t1 SET value2=value3;

SELECT COUNT(*) FROM t1;

--echo # Test NULL values are considered unique
INSERT INTO t1 VALUES (20, 20, 20, NULL, NULL, 20, 20, 20);
INSERT INTO t1 VALUES (21, 20, 20, NULL, NULL, 20, 20, 20);
INSERT INTO t1 VALUES (22, 20, 20, NULL, NULL, 20, 20, 20);

SELECT COUNT(*) FROM t1;

--echo # Adding multiple rows where one of the rows fail the duplicate
--echo # check should fail the whole statement
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (23, 23, 23, 23, 23, 23, 23, 23),
                      (24, 24, 24, 24, 24, 24, 24, 24),
                      (25, 10, 10, 10, 25, 25, 25, 25),
                      (26, 26, 26, 26, 26, 26, 26, 26);
SELECT COUNT(*) FROM t1;

# Test open transactions can prevent duplicate keys
connection con1;
BEGIN;
INSERT INTO t1 VALUES (30, 31, 32, 33, 34, 30, 30, 30);

connection con2;
BEGIN;

SELECT COUNT(*) FROM t1;

--echo # Primary key should prevent duplicate on insert
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 VALUES (30, 31, 30, 30, 30, 30, 30, 30);

--echo # Primary key should prevent duplicate on update
--error ER_LOCK_WAIT_TIMEOUT
UPDATE t1 SET id1=30, id2=31 WHERE id2=10;

--echo # Unique secondary key should prevent duplicate on insert
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 VALUES (31, 31, 32, 33, 30, 30, 30, 30);
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 VALUES (32, 32, 32, 32, 34, 32, 32, 32);

--echo # Unique secondary key should prevent duplicate on update
--error ER_LOCK_WAIT_TIMEOUT
UPDATE t1 SET id2=31, id3=32, id4=33 WHERE id2=8;
--error ER_LOCK_WAIT_TIMEOUT
UPDATE t1 SET id5=34 WHERE id2=8;

--echo # Adding multiple rows where one of the rows fail the duplicate
--echo # check should fail the whole statement
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 VALUES (35, 35, 35, 35, 35, 35, 35, 35),
                      (36, 36, 36, 36, 36, 36, 36, 36),
                      (37, 31, 32, 33, 37, 37, 37, 37),
                      (38, 38, 38, 38, 38, 38, 38, 38);
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 VALUES (35, 35, 35, 35, 35, 35, 35, 35),
                      (36, 36, 36, 36, 36, 36, 36, 36),
                      (37, 37, 37, 37, 34, 37, 37, 37),
                      (38, 38, 38, 38, 38, 38, 38, 38);

--echo # NULL values are unique and duplicates in value fields are ignored
INSERT INTO t1 VALUES (37, 31, 32, NULL, 37, 37, 37, 37),
                      (38, 31, 32, NULL, 38, 37, 37, 37),
                      (39, 31, 32, NULL, 39, 37, 37, 37);

SELECT COUNT(*) FROM t1;

--echo # Fail on duplicate key update for row added in our transaction
--error ER_DUP_ENTRY
UPDATE t1 SET id5=37 WHERE id1=38;

--echo # Fail on lock timeout for row modified in another transaction
--error ER_LOCK_WAIT_TIMEOUT
UPDATE t1 SET id5=34 WHERE id1=38;

--echo # NULL values are unique
UPDATE t1 SET id5=NULL WHERE value1 > 37;

connection con1;
COMMIT;

connection con2;
COMMIT;

# Test transaction is reading from latest data
connection con2;
BEGIN;
SELECT COUNT(*) FROM t1;

connection con1;
BEGIN;
INSERT INTO t1 VALUES (40, 40, 40, 40, 40, 40, 40, 40);

connection con2;
--echo # When transaction is pending, fail on lock acquisition
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 VALUES (40, 40, 40, 40, 40, 40, 40, 40);
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 VALUES (41, 40, 40, 40, 40, 40, 40, 40);

SELECT COUNT(*) FROM t1;

connection con1;
COMMIT;

connection con2;
--echo # When transaction is committed, fail on duplicate key
--error ER_DUP_ENTRY,ER_LOCK_DEADLOCK
INSERT INTO t1 VALUES (40, 40, 40, 40, 40, 40, 40, 40);
--error ER_DUP_ENTRY
INSERT INTO t1 VALUES (41, 40, 40, 40, 40, 40, 40, 40);

ROLLBACK;

SELECT * FROM t1;

disconnect con1;
disconnect con2;

connection default;
DROP TABLE t1;

# Wait till we reached the initial number of concurrent sessions
--source include/wait_until_count_sessions.inc
