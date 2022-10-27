#
# Check concurrent locking issues:
#   Rows scanned but are not in the updated table should be locked when
#   rocksdb_lock_scanned_rows is on but not locked otherwise.
#
# To call this, set $isolation_level and $lock_scanned_rows and call this file
#
# let $isolation_level = REPEATABLE READ;
# let $lock_scanned_rows = 0 (or 1)
# --source suite/rocksdb/include/locking_issues_case7.inc
#

--echo
--echo -----------------------------------------------------------------------
--echo - Locking issues case 7:
--echo -   Rows that are scanned as part of a query but not in the table being
--echo -   updated should not be locked unless rocksdb_lock_scanned_rows is on
--echo -----------------------------------------------------------------------

--disable_warnings
DROP TABLE IF EXISTS t1, t2;
--enable_warnings

SELECT @@global.rocksdb_lock_scanned_rows;

if ($lock_scanned_rows)
{
  let $original_val=query_get_value(
      select @@global.rocksdb_lock_scanned_rows as val, val, 1);
  SET GLOBAL rocksdb_lock_scanned_rows=ON;
}

CREATE TABLE t1(id INT PRIMARY KEY, value INT);
CREATE TABLE t2(id INT PRIMARY KEY, value INT);
INSERT INTO t1 VALUES (1,1), (2,2), (3,3);
INSERT INTO t2 VALUES (1,1), (2,2), (3,3), (4,4), (5,5);

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;

connection con2;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;

--echo lock_scanned_rows is $lock_scanned_rows
if ($lock_scanned_rows == 1)
{
  connection con1;
  # This is expected to leave a lock id=3 in t2;
  UPDATE t1 JOIN t2 ON t1.id = t2.id SET t1.value=t1.value+100 WHERE t2.id=3;

  connection con2;
  --error ER_LOCK_WAIT_TIMEOUT
  UPDATE t2 SET value=value+100 WHERE id=3;

  # No other row in t2 should be locked;
  UPDATE t2 SET value=value+100 WHERE id IN (1,2,4,5);
  SELECT * FROM t2;
}

if ($lock_scanned_rows == 0)
{
  connection con1;
  # This should leave no locks on any row in t2;
  UPDATE t1 JOIN t2 ON t1.id = t2.id SET t1.value=t1.value+100 WHERE t2.id=3;

  connection con2;
  UPDATE t2 SET value=value+100;
  SELECT * FROM t2;
}

connection con1;
COMMIT;

connection default;
disconnect con1;
disconnect con2;

DROP TABLE t1;
DROP TABLE t2;

if ($lock_scanned_rows == 1)
{
  eval SET GLOBAL rocksdb_lock_scanned_rows=$original_val;
}
