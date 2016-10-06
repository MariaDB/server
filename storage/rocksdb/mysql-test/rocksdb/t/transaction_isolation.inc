#
# Basic check for transaction isolation. 
# The results should be different depending on the isolation level.
# For some isolation levels, some statements will end with a timeout.
# If the engine has its own timeout parameters, reduce them to minimum,
# otherwise the test will take very long.
# If the timeout value is greater than the testcase-timeout the test is run with,
# it might fail due to the testcase timeout.
#

--enable_connect_log

# Save the initial number of concurrent sessions
--source include/count_sessions.inc

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

connect (con1,localhost,root,,);
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;
connect (con2,localhost,root,,);
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;

connection con1;

CREATE TABLE t1 (a INT, pk INT AUTO_INCREMENT PRIMARY KEY) ENGINE=rocksdb;

START TRANSACTION; 
--sorted_result
SELECT a FROM t1; # First snapshot

connection con2;

BEGIN;
--error 0,ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 (a) VALUES(1); 

connection con1;
--sorted_result
SELECT a FROM t1; # Second snapshot

connection con2;
--error 0,ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 (a) VALUES (2); 

connection con1;
--sorted_result
SELECT a FROM t1; # Third snapshot

--error 0,ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 (a) SELECT a+100 FROM t1; 

--sorted_result
SELECT a FROM t1;

connection con2;
--sorted_result
SELECT a FROM t1; # Inside the transaction
COMMIT;
--sorted_result
SELECT a FROM t1; # Outside the transaction

connection con1;
--sorted_result
SELECT a FROM t1; # Inside the transaction

# Note: INSERT .. SELECT might be tricky, for example for InnoDB
# even with REPEATABLE-READ it works as if it is executed with READ COMMITTED.
# The test will have a 'logical' result for repeatable read, even although
# we currently don't have an engine which works this way.

--error 0,ER_LOCK_WAIT_TIMEOUT
INSERT INTO t1 (a) SELECT a+200 FROM t1; 

--sorted_result
SELECT a FROM t1;
COMMIT;
--sorted_result
SELECT a FROM t1; # Outside the transaction

connection con2;
--sorted_result
SELECT a FROM t1; # After both transactions have committed

# Now test with an error in one statement to make sure the snapshots are
# Held/released when expected
connection default;
CREATE TABLE t2 (a INT PRIMARY KEY) ENGINE=rocksdb;
INSERT INTO t2 (a) VALUES (1);
COMMIT;

connection con1;
BEGIN;
--sorted_result
SELECT a from t2;
--error ER_DUP_ENTRY
INSERT INTO t2 (a) VALUES (1), (3); # failure

connection con2;
--error 0,ER_LOCK_WAIT_TIMEOUT
INSERT INTO t2 (a) VALUES (2);
COMMIT;

connection con1;
--sorted_result
SELECT a from t2;
COMMIT;

connection default;
disconnect con1;
disconnect con2;
DROP TABLE t1;
DROP TABLE t2;

CREATE TABLE t3 (
  pk int unsigned PRIMARY KEY,
  count int unsigned DEFAULT '0'
) ENGINE=ROCKSDB;

connect (con1,localhost,root,,);
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;
connect (con2,localhost,root,,);
eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;

connection con1;
BEGIN;
SELECT * FROM t3;

connection con2;
BEGIN;
INSERT INTO t3 (pk) VALUES(1) ON DUPLICATE KEY UPDATE count=count+1;
COMMIT;

connection con1;
--error 0,ER_LOCK_DEADLOCK
INSERT INTO t3 (pk) VALUES(1) ON DUPLICATE KEY UPDATE count=count+1;
COMMIT;

# count will be 0 for repeatable read (because the last insert failed)
# and 1 for read committed
SELECT count FROM t3;

connection default;
disconnect con1;
disconnect con2;
DROP TABLE t3;

--source include/wait_until_count_sessions.inc

