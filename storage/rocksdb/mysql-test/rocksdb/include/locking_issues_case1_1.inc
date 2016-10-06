#
# Check concurrent locking issues:
#   Locking rows that do not exist when using all primary key columns in a
#   WHERE clause
#
# To call this, set $isolation_level and call this file
#
# let $isolation_level = REPEATABLE READ;
# --source suite/rocksdb/include/locking_issues_case1_1.inc
#

--echo
--echo -----------------------------------------------------------------------
--echo - Locking issues case 1.1:
--echo -   Locking rows that do not exist when using all primary key columns in
--echo -   a WHERE clause
--echo -   using $isolation_level transaction isolation level
--echo -----------------------------------------------------------------------

--disable_warnings
DROP TABLE IF EXISTS t0;
--enable_warnings

CREATE TABLE t0(id1 INT, id2 INT, value INT, PRIMARY KEY(id1, id2));
INSERT INTO t0 VALUES (1,1,0), (3,3,0), (4,4,0), (6,6,0);

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;
SELECT * FROM t0 WHERE id1=1 AND id2=5 FOR UPDATE;

connection con2;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t0 VALUES (1,5,0);

--error ER_LOCK_WAIT_TIMEOUT
SELECT * FROM t0 WHERE id1=1 AND id2=5 FOR UPDATE;

connection con1;
COMMIT;

connection default;
disconnect con1;
disconnect con2;

DROP TABLE t0;
