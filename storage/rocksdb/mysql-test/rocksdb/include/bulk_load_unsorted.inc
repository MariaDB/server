--source include/have_partition.inc
--source include/count_sessions.inc

SET rocksdb_bulk_load_size=3;
SET rocksdb_bulk_load_allow_unsorted=1;

### Test individual INSERTs ###

# A table with only a PK won't have rows until the bulk load is finished
eval CREATE TABLE t1(a INT, b INT, PRIMARY KEY(a) COMMENT "$pk_cf")
     ENGINE=ROCKSDB;
SET rocksdb_bulk_load=1;
--disable_query_log
let $sign = 1;
let $max = 5;
let $i = 1;
while ($i <= $max) {
  let $a = 1 + $sign * $i;
  let $b = 1 - $sign * $i;
  let $sign = -$sign;
  let $insert = INSERT INTO t1 VALUES ($a, $b);
  eval $insert;
  inc $i;
}
--enable_query_log
SELECT * FROM t1 FORCE INDEX (PRIMARY);
SET rocksdb_bulk_load=0;
SELECT * FROM t1 FORCE INDEX (PRIMARY);
DROP TABLE t1;

# A table with a PK and a SK shows rows immediately
eval CREATE TABLE t1(a INT, b INT, PRIMARY KEY(a) COMMENT "$pk_cf", KEY(b))
     ENGINE=ROCKSDB;
SET rocksdb_bulk_load=1;
--disable_query_log
let $sign = 1;
let $max = 5;
let $i = 1;
while ($i <= $max) {
  let $a = 1 + $sign * $i;
  let $b = 1 - $sign * $i;
  let $sign = -$sign;
  let $insert = INSERT INTO t1 VALUES ($a, $b);
  eval $insert;
  inc $i;
}
--enable_query_log

SELECT * FROM t1 FORCE INDEX (PRIMARY);
SET rocksdb_bulk_load=0;
SELECT * FROM t1 FORCE INDEX (PRIMARY);
DROP TABLE t1;

# Inserting into another table finishes bulk load to the previous table
eval CREATE TABLE t1(a INT, b INT, PRIMARY KEY(a) COMMENT "$pk_cf")
     ENGINE=ROCKSDB;
eval CREATE TABLE t2(a INT, b INT, PRIMARY KEY(a) COMMENT "$pk_cf")
     ENGINE=ROCKSDB;

SET rocksdb_bulk_load=1;
INSERT INTO t1 VALUES (1,1);
INSERT INTO t2 VALUES (1,1);
SELECT * FROM t1 FORCE INDEX (PRIMARY);
INSERT INTO t1 VALUES (2,2);
SELECT * FROM t2 FORCE INDEX (PRIMARY);
SELECT * FROM t1 FORCE INDEX (PRIMARY);
SET rocksdb_bulk_load=0;
SELECT * FROM t1 FORCE INDEX (PRIMARY);
DROP TABLE t1, t2;

### Test bulk load from a file ###
eval CREATE TABLE t1(a INT, b INT, PRIMARY KEY(a) COMMENT "$pk_cf")
     ENGINE=ROCKSDB;
eval CREATE TABLE t2(a INT, b INT, PRIMARY KEY(b) COMMENT "$pk_cf")
     ENGINE=ROCKSDB;
eval CREATE TABLE t3(a INT, b INT, PRIMARY KEY(a) COMMENT "$pk_cf")
     ENGINE=ROCKSDB PARTITION BY KEY() PARTITIONS 4;

--let $file = `SELECT CONCAT(@@datadir, "test_loadfile.txt")`
# Create a text file with data to import into the table.
# PK and SK are not in any order
--let ROCKSDB_INFILE = $file
perl;
my $fn = $ENV{'ROCKSDB_INFILE'};
open(my $fh, '>', $fn) || die "perl open($fn): $!";
binmode $fh;
my $max = 2500000;
my $sign = 1;
for (my $ii = 0; $ii < $max; $ii++)
{
  my $a = 1 + $sign * $ii;
  my $b = 1 - $sign * $ii;
  $sign = -$sign;
  print $fh "$a\t$b\n";
}
close($fh);
EOF
--file_exists $file

# Make sure a snapshot held by another user doesn't block the bulk load
connect (other,localhost,root,,);
set session transaction isolation level repeatable read;
start transaction with consistent snapshot;

# Assert that there is a pending snapshot
select VALUE > 0 as 'Has opened snapshots' from information_schema.rocksdb_dbstats where stat_type='DB_NUM_SNAPSHOTS';

connection default;
set rocksdb_bulk_load=1;
set rocksdb_bulk_load_size=100000;
--disable_query_log
--echo LOAD DATA INFILE <input_file> INTO TABLE t1;
eval LOAD DATA INFILE '$file' INTO TABLE t1;
--echo LOAD DATA INFILE <input_file> INTO TABLE t2;
eval LOAD DATA INFILE '$file' INTO TABLE t2;
--echo LOAD DATA INFILE <input_file> INTO TABLE t3;
eval LOAD DATA INFILE '$file' INTO TABLE t3;
--enable_query_log
set rocksdb_bulk_load=0;

--remove_file $file

# Make sure row count index stats are correct
--replace_column 6 # 7 # 8 # 9 #
SHOW TABLE STATUS WHERE name LIKE 't%';

ANALYZE TABLE t1, t2, t3;

--replace_column 6 # 7 # 8 # 9 #
SHOW TABLE STATUS WHERE name LIKE 't%';

# Make sure all the data is there.
select count(a),count(b) from t1;
select count(a),count(b) from t2;
select count(a),count(b) from t3;

SELECT * FROM t1 FORCE INDEX (PRIMARY) LIMIT 3;
SELECT * FROM t2 FORCE INDEX (PRIMARY) LIMIT 3;

disconnect other;
DROP TABLE t1, t2, t3;

--source include/wait_until_count_sessions.inc
