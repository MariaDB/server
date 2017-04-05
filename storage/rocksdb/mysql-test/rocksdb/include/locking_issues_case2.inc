#
# Check concurrent locking issues:
#   Rows that are scanned but do not match the WHERE clause are not locked.
#
# To call this, set $isolation_level and call this file
# If you want to enable rocksdb_lock_scanned_rows set $lock_scanned_rows=1
#
# let $isolation_level = REPEATABLE READ;
# let $lock_scanned_rows = 1 (optional)
# --source suite/rocksdb/include/locking_issues_case2.inc
#

--echo
--echo -----------------------------------------------------------------------
--echo - Locking issues case 2:
--echo -   Rows that are scanned but do not match the WHERE are not locked
--echo -   using $isolation_level transaction isolation level unless
--echo -   rocksdb_lock_scanned_rows is on
--echo -----------------------------------------------------------------------

--disable_warnings
DROP TABLE IF EXISTS t0;
--enable_warnings

SELECT @@global.rocksdb_lock_scanned_rows;

if ($lock_scanned_rows)
{
  let $original_val=query_get_value(
      select @@global.rocksdb_lock_scanned_rows as val, val, 1);
  SET GLOBAL rocksdb_lock_scanned_rows=ON;
}

CREATE TABLE t0(id INT PRIMARY KEY, value INT);
INSERT INTO t0 VALUES (1,0), (2,1), (3,0), (4,0), (5,1);

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;

connection con2;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;

if ($lock_scanned_rows == 1)
{
  connection con1;
  # This is expected to leave locks on all the rows in t0
  SELECT * FROM t0 WHERE value > 0 FOR UPDATE;

  connection con2;
  --error ER_LOCK_WAIT_TIMEOUT
  UPDATE t0 SET VALUE=10 WHERE id=1;
}

if ($lock_scanned_rows == 0)
{
  connection con1;
  # This is expected to release locks on rows with value=0
  SELECT * FROM t0 WHERE value > 0 FOR UPDATE;

  connection con2;
  # This should succeed as con1 should have released the lock on row (1,0)
  UPDATE t0 SET VALUE=10 WHERE id=1;

  # This should fail because lock on row (5,1) is still held.
  --error ER_LOCK_WAIT_TIMEOUT
  UPDATE t0 SET VALUE=10 WHERE id=5;

  connection con1;
  # Do another operation
  UPDATE t0 SET value=100 WHERE id in (4,5) and value>0;

  connection con2;
  # Check that row (4,0) is still not locked
  SELECT * FROM t0 WHERE id=4 FOR UPDATE;

  COMMIT;
  SELECT * FROM t0;
}

connection con1;
COMMIT;

connection default;
disconnect con1;
disconnect con2;

DROP TABLE t0;

if ($lock_scanned_rows == 1)
{
  eval SET GLOBAL rocksdb_lock_scanned_rows=$original_val;
}
