
# Truncate table multiple times
TRUNCATE TABLE t1;
TRUNCATE TABLE t1;
TRUNCATE TABLE t1;

# Truncate partition multiple times
ALTER TABLE t1 TRUNCATE PARTITION p0;
ALTER TABLE t1 TRUNCATE PARTITION p0;
ALTER TABLE t1 TRUNCATE PARTITION p0;

# TRUNCATE multiple times
ALTER TABLE t1 TRUNCATE PARTITION p1;
ALTER TABLE t1 TRUNCATE PARTITION p1;
ALTER TABLE t1 TRUNCATE PARTITION p1;

# TRUNCATE multiple times
ALTER TABLE t1 TRUNCATE PARTITION p2;
ALTER TABLE t1 TRUNCATE PARTITION p2;
ALTER TABLE t1 TRUNCATE PARTITION p2;

# TRUNCATE different partition
ALTER TABLE t1 TRUNCATE PARTITION p1;
ALTER TABLE t1 TRUNCATE PARTITION p0;
TRUNCATE TABLE t1;
ALTER TABLE t1 TRUNCATE PARTITION p2;

# Insert value once and truncate multiple times
INSERT INTO t1 (a,b) VALUES (1, 1), (2, 4), (3, 8);
SELECT a,b FROM t1 ORDER BY a;

ALTER TABLE t1 TRUNCATE PARTITION p2;
SELECT a,b FROM t1 ORDER BY a;
SELECT a FROM t1 WHERE b > 2;
SELECT b from t1 where a != 3;

ALTER TABLE t1 TRUNCATE PARTITION p1;
SELECT a,b FROM t1 ORDER BY b;
SELECT a FROM t1 WHERE b > 2;
SELECT b from t1 where a != 3;

ALTER TABLE t1 TRUNCATE PARTITION p0;
SELECT a,b FROM t1 ORDER BY a;

TRUNCATE TABLE t1;
SELECT a,b FROM t1;

# Insert value multiple times and truncate multiple times
INSERT INTO t1 (a,b) VALUES (4, 1), (5, 4), (6, 8);
--sorted_result
SELECT a,b FROM t1;
--sorted_result
SELECT a FROM t1 WHERE b < 5;

ALTER TABLE t1 TRUNCATE PARTITION p2;
--sorted_result
SELECT a,b FROM t1;
INSERT INTO t1(a,b) VALUES(7, 1);
--sorted_result
SELECT b from t1 WHERE a > 2;
--sorted_result
SELECT a,b FROM t1;

ALTER TABLE t1 TRUNCATE PARTITION p1;
--sorted_result
SELECT a,b FROM t1;
INSERT INTO t1(a,b) VALUES(8, 4);
--sorted_result
SELECT a,b FROM t1;
--sorted_result
SELECT b from t1 WHERE a < 9;

ALTER TABLE t1 TRUNCATE PARTITION p0;
--sorted_result
SELECT a,b FROM t1;
INSERT INTO t1(a,b) VALUES(9, 8);

TRUNCATE TABLE t1;
SELECT a,b FROM t1;

TRUNCATE TABLE t1;
SELECT a,b FROM t1;

# manual commpact
set global rocksdb_force_flush_memtable_now = true;
set global rocksdb_compact_cf = 'default';

--disable_query_log
let $i = 0;
while($i < 9)
{
  inc $i;
  eval insert t1 values($i, $i);
}
--enable_query_log
--sorted_result
SELECT b FROM t1 WHERE a < 5;

TRUNCATE TABLE t1;
SELECT b FROM t1 WHERE a < 5;

DROP TABLE t1;
