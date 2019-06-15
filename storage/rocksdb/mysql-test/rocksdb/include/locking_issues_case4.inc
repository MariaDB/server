#
# Check concurrent locking issues:
#   Phantom rows
#
# To call this, set $isolation_level and call this file
#
# let $isolation_level = REPEATABLE READ;
# --source suite/rocksdb/include/locking_issues_case4.inc
#

--echo
--echo -----------------------------------------------------------------------
--echo - Locking issues case 4:
--echo -   Phantom rows
--echo -   using $isolation_level transaction isolation level
--echo -----------------------------------------------------------------------

--disable_warnings
DROP TABLE IF EXISTS t0;
--enable_warnings

CREATE TABLE t0(id INT AUTO_INCREMENT PRIMARY KEY, value INT);

# Insert 200,000 rows, breaking it up into inserts of 1000 rows at a time
--echo Inserting 200,000 rows
--disable_query_log
SET @save_rocksdb_bulk_load=@@rocksdb_bulk_load;
SET rocksdb_bulk_load=1;
SET @save_rocksdb_write_disable_wal=@@rocksdb_write_disable_wal;
SET GLOBAL rocksdb_write_disable_wal=1;
let $i = 1;
while ($i <= 200) {
  eval BEGIN;
  let $j = 1;
  while ($j <= 100) {
    eval INSERT INTO t0(value) VALUES (0),(0),(0),(0),(0),(0),(0),(0),(0),(0);
    inc $j;
  }
  eval COMMIT;
  inc $i;
}
SET rocksdb_bulk_load=@save_rocksdb_bulk_load;
SET GLOBAL rocksdb_write_disable_wal=@save_rocksdb_write_disable_wal;
--enable_query_log

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
let $ID = `SELECT connection_id()`;
send SELECT * FROM t0 WHERE value > 0 FOR UPDATE;

connection con2;
let $wait_condition =
    SELECT 1 FROM information_schema.processlist
    WHERE (id = $ID/* OR srv_id = $ID*/) AND state = "Sending data";
--source include/wait_condition.inc
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
INSERT INTO t0 VALUES(200001,1), (-1,1);

connection con1;
reap;

connection default;
disconnect con1;
disconnect con2;

DROP TABLE t0;
