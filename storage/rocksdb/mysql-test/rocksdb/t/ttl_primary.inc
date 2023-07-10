#
# Specify the timestamp column type using $coltype
# and the timestamp generator using $timegen
#

# Basic TTL test
eval CREATE TABLE t1 (
`a` binary(8) NOT NULL,
`b` varbinary(64) NOT NULL,
`c` varbinary(256) NOT NULL,
`ts` $coltype NOT NULL,
`value` mediumblob NOT NULL,
PRIMARY KEY (`b`,`a`,`c`)
) ENGINE=ROCKSDB DEFAULT CHARSET=latin1
COMMENT='ttl_duration=1;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -100;
eval INSERT INTO t1 values ('a', 'b', 'c', $timegen, 'd');
eval INSERT INTO t1 values ('d', 'e', 'f', $timegen, 'g');
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;
set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# column before TTL in value
eval CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  b int NOT NULL,
  ts $coltype NOT NULL,
  c int NOT NULL,
  PRIMARY KEY (a)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -100;
eval INSERT INTO t1 values (1, 3, $timegen, 5);
eval INSERT INTO t1 values (2, 4, $timegen, 6);
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# multi-part PK w/ TTL
eval CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  b int NOT NULL,
  c int NOT NULL,
  ts $coltype NOT NULL,
  PRIMARY KEY (a,c)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -100;
eval INSERT INTO t1 values (1, 3, 5, $timegen);
eval INSERT INTO t1 values (2, 4, 6, $timegen);
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# multi-part PK w/ TTL
eval CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  b int NOT NULL,
  c int NOT NULL,
  ts $coltype NOT NULL,
  PRIMARY KEY (a,c)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -100;
eval INSERT INTO t1 values (1, 3, 5, $timegen);
eval INSERT INTO t1 values (2, 4, 6, $timegen);
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# nullable column(s) before TTL
eval CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  b int,
  c int,
  ts $coltype NOT NULL,
  PRIMARY KEY (a)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -100;
eval INSERT INTO t1 values (1, NULL, NULL, $timegen);
eval INSERT INTO t1 values (2, NULL, NULL, $timegen);
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# variable len columns + null column(s) before TTL
eval CREATE TABLE t1 (
`a` binary(8) NOT NULL,
`b` varbinary(64),
`c` varbinary(256),
`ts` $coltype NOT NULL,
`value` mediumblob NOT NULL,
PRIMARY KEY (`a`)
) ENGINE=ROCKSDB DEFAULT CHARSET=latin1
COMMENT='ttl_duration=1;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -100;
eval INSERT INTO t1 values ('a', NULL, 'bc', $timegen, 'd');
eval INSERT INTO t1 values ('d', 'efghijk', NULL, $timegen, 'l');
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# TTL implicitly generated (no ttl column)
CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  b int NOT NULL,
  c int NOT NULL,
  PRIMARY KEY (a)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;';

set global rocksdb_debug_ttl_rec_ts = -100;
INSERT INTO t1 values (1, 3, 5);
INSERT INTO t1 values (2, 4, 6);
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# TTL field as the PK
eval CREATE TABLE t1 (
	a int,
  ts $coltype NOT NULL,
  PRIMARY KEY (a, ts)
) ENGINE=rocksdb
COMMENT='ttl_duration=5;ttl_col=ts;';

eval INSERT INTO t1 values (1, $timegen);
eval INSERT INTO t1 values (2, $timegen);
SELECT COUNT(*) FROM t1;

set global rocksdb_debug_ttl_snapshot_ts = -10;
set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';
set global rocksdb_debug_ttl_snapshot_ts = 0;
# should all still be there..
SELECT COUNT(*) FROM t1;

set global rocksdb_debug_ttl_snapshot_ts = 10;
set global rocksdb_compact_cf='default';
set global rocksdb_debug_ttl_snapshot_ts = 0;
# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;


# TTL field inside multi-part pk
eval CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  b int NOT NULL,
  ts $coltype NOT NULL,
  c int NOT NULL,
  PRIMARY KEY (a, ts)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -100;
eval INSERT INTO t1 values (1, 3, $timegen, 5);
eval INSERT INTO t1 values (2, 4, $timegen, 6);
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# TTL field inside key with variable length things..
eval CREATE TABLE t1 (
`a` binary(8) NOT NULL,
`b` varbinary(64),
`c` varbinary(256),
`ts` $coltype NOT NULL,
`value` mediumblob NOT NULL,
PRIMARY KEY (`a`, `ts`)
) ENGINE=ROCKSDB DEFAULT CHARSET=latin1
COMMENT='ttl_duration=1;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -100;
eval INSERT INTO t1 values ('a', NULL, 'bc', $timegen, 'd');
eval INSERT INTO t1 values ('de', 'fghijk', NULL, $timegen, 'l');
set global rocksdb_debug_ttl_rec_ts = 0;
SELECT COUNT(*) FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# should have filtered the rows out since ttl is passed in compaction filter
SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# TTL test where you compact (values still exist), real_sleep, then compact again,
# values should now be gone.
eval CREATE TABLE t1 (
a INT NOT NULL,
b varbinary(64) NOT NULL,
c varbinary(256) NOT NULL,
ts $coltype NOT NULL,
value mediumblob NOT NULL,
PRIMARY KEY (b,a,c)
) ENGINE=ROCKSDB DEFAULT CHARSET=latin1
COMMENT='ttl_duration=10;ttl_col=ts;';

set global rocksdb_debug_ttl_rec_ts = -300;
eval INSERT INTO t1 values (1, 'b', 'c', $timegen, 'd');
eval INSERT INTO t1 values (2, 'e', 'f', $timegen, 'g');
set global rocksdb_debug_ttl_rec_ts = 300;
eval INSERT INTO t1 values (3, 'i', 'j', $timegen, 'k');
eval INSERT INTO t1 values (4, 'm', 'n', $timegen, 'o');
set global rocksdb_debug_ttl_rec_ts = 0;

# Nothing should get removed here.
set global rocksdb_debug_ttl_snapshot_ts = -3600;
set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';
set global rocksdb_debug_ttl_snapshot_ts = 0;
--sorted_result
SELECT a FROM t1;

# 1 and 2 should get removed here.
set global rocksdb_compact_cf='default';
--sorted_result
SELECT a FROM t1;

# 3 and 4 should get removed here.
set global rocksdb_debug_ttl_snapshot_ts = 3600;
set global rocksdb_compact_cf='default';
set global rocksdb_debug_ttl_snapshot_ts = 0;
--sorted_result
SELECT a FROM t1;

DROP TABLE t1;

# TTL field with nullable ttl column (should fail)
--error ER_RDB_TTL_COL_FORMAT
eval CREATE TABLE t1 (
  a bigint(20) UNSIGNED NOT NULL,
  b int NOT NULL,
  c int NOT NULL,
  ts $coltype,
  PRIMARY KEY (a,c)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;ttl_col=ts;';

# TTL field with non 8-bit integer column (should fail)
--error ER_RDB_TTL_COL_FORMAT
CREATE TABLE t1 (
  a bigint(20) UNSIGNED NOT NULL,
  b int NOT NULL,
  c int NOT NULL,
  ts int,
  PRIMARY KEY (a,c)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;ttl_col=ts;';

# TTL duration as some random garbage value
--error ER_RDB_TTL_DURATION_FORMAT
CREATE TABLE t1 (
  a bigint(20) UNSIGNED NOT NULL,
  b int NOT NULL,
  c int NOT NULL,
  PRIMARY KEY (a,c)
) ENGINE=rocksdb
COMMENT='ttl_duration=abc;';

# TTL col is some column outside of the table
--error ER_RDB_TTL_COL_FORMAT
CREATE TABLE t1 (
  a bigint(20) UNSIGNED NOT NULL,
  b int NOT NULL,
  c int NOT NULL,
  PRIMARY KEY (a,c)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;ttl_col=abc;';

# TTL col must have accompanying duration
--error ER_RDB_TTL_COL_FORMAT
CREATE TABLE t1 (
  a bigint(20) UNSIGNED NOT NULL,
  b int NOT NULL,
  c int NOT NULL,
  PRIMARY KEY (a,c)
) ENGINE=rocksdb
COMMENT='ttl_col=abc;';

# Make sure it doesn't filter out things early
CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  PRIMARY KEY (a)
) ENGINE=rocksdb
COMMENT='ttl_duration=500;';

INSERT INTO t1 values (1);
SELECT COUNT(*) FROM t1;
set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# Testing altering table comment with updated TTL duration
# This should trigger a rebuild of the table
CREATE TABLE t1 (
  a INT PRIMARY KEY
) ENGINE=rocksdb
COMMENT='ttl_duration=100;';

INSERT INTO t1 values (1);
SELECT * FROM t1;

set global rocksdb_debug_ttl_rec_ts = -300;
ALTER TABLE t1 COMMENT = 'ttl_duration=1';
set global rocksdb_debug_ttl_rec_ts = 0;
SHOW CREATE TABLE t1;
set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

SELECT COUNT(*) FROM t1;
DROP TABLE t1;

# Tables with hidden PK and SK disabled
CREATE TABLE t1 (
  a INT PRIMARY KEY,
  b INT
) ENGINE=rocksdb
COMMENT='ttl_duration=100;';

--error ER_RDB_TTL_UNSUPPORTED
ALTER TABLE t1 DROP PRIMARY KEY;

DROP TABLE t1;

# Test replacing PK, ttl should still work after
CREATE TABLE t1 (
  a INT PRIMARY KEY,
  b INT
) ENGINE=rocksdb
COMMENT='ttl_duration=5;';

INSERT INTO t1 VALUES (1,1);
INSERT INTO t1 VALUES (2,2);

ALTER TABLE t1 DROP PRIMARY KEY, ADD PRIMARY KEY(b);
set global rocksdb_debug_ttl_snapshot_ts = -3600;
set global rocksdb_force_flush_memtable_now=1;
set @@global.rocksdb_compact_cf = 'default';
set global rocksdb_debug_ttl_snapshot_ts = 0;

--sorted_result
SELECT COUNT(*) FROM t1;

set global rocksdb_debug_ttl_snapshot_ts = 3600;
set @@global.rocksdb_compact_cf = 'default';
set global rocksdb_debug_ttl_snapshot_ts = 0;

--sorted_result
SELECT COUNT(*) FROM t1;

DROP TABLE t1;

# Make sure table comment filled with other text before/after will work
# (basically, it needs semicolon before and after)
eval CREATE TABLE t1 (
  a $coltype NOT NULL,
  b int,
  PRIMARY KEY (a,b)
) ENGINE=rocksdb
COMMENT='asdadfasdfsadfadf ;ttl_duration=1; asfasdfasdfadfa';
eval INSERT INTO t1 values ($timegen, 1);
SELECT COUNT(*) FROM t1;

set global rocksdb_debug_ttl_snapshot_ts = 3600;
set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';
set global rocksdb_debug_ttl_snapshot_ts = 0;

SELECT COUNT(*) FROM t1;

ALTER TABLE t1 COMMENT = 'adsf;;ttl_duration=5;asfasdfa;ttl_col=a;asdfasdf;';

set global rocksdb_debug_ttl_rec_ts = 300;
eval INSERT INTO t1 values ($timegen, 2);
set global rocksdb_debug_ttl_rec_ts = 0;
set global rocksdb_force_flush_memtable_now=1;

# nothing removed here
set global rocksdb_compact_cf='default';
SELECT COUNT(*) FROM t1;

# all removed here
set global rocksdb_debug_ttl_snapshot_ts = 3600;
set global rocksdb_compact_cf='default';
set global rocksdb_debug_ttl_snapshot_ts = 0;
SELECT COUNT(*) FROM t1;

DROP TABLE t1;

# Test to make sure that TTL retains original timestamp during update
CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  PRIMARY KEY (a)
) ENGINE=rocksdb
COMMENT='ttl_duration=5;';

set global rocksdb_debug_ttl_rec_ts = -300;
INSERT INTO t1 values (1);
INSERT INTO t1 values (3);
INSERT INTO t1 values (5);
set global rocksdb_debug_ttl_rec_ts = 300;
INSERT INTO t1 values (7);
INSERT INTO t1 values (9);
set global rocksdb_debug_ttl_rec_ts = 0;

UPDATE t1 SET a=a+1;
--sorted_result
SELECT * FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# 1,3,5 should be dropped
--sorted_result
SELECT * FROM t1;
DROP TABLE t1;

# test behaviour on update with TTL column, TTL time can be updated here.
eval CREATE TABLE t1 (
  a INT,
  b $coltype NOT NULL,
  PRIMARY KEY (a)
) ENGINE=rocksdb
COMMENT='ttl_duration=5;ttl_col=b;';

set global rocksdb_debug_ttl_rec_ts = -300;
eval INSERT INTO t1 values (1, $timegen);
eval INSERT INTO t1 values (3, $timegen);
eval INSERT INTO t1 values (5, $timegen);
eval INSERT INTO t1 values (7, $timegen);

set global rocksdb_debug_ttl_rec_ts = 300;
eval UPDATE t1 SET b=$timegen WHERE a < 4;
set global rocksdb_debug_ttl_rec_ts = 0;

--sorted_result
SELECT a FROM t1;

set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

# 5 and 7 should be gone here
--sorted_result
SELECT a FROM t1;
DROP TABLE t1;

# Test rows expired stat variable and disable ttl variable
CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  PRIMARY KEY (a)
) ENGINE=rocksdb
COMMENT='ttl_duration=1;';

set global rocksdb_debug_ttl_rec_ts = -100;
INSERT INTO t1 values (1);
INSERT INTO t1 values (2);
INSERT INTO t1 values (3);
set global rocksdb_debug_ttl_rec_ts = 0;

set global rocksdb_enable_ttl=0;
set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';

select variable_value into @c from information_schema.global_status where variable_name='rocksdb_rows_expired';
set global rocksdb_enable_ttl=1;
set global rocksdb_compact_cf='default';

select variable_value-@c from information_schema.global_status where variable_name='rocksdb_rows_expired';
SELECT COUNT(*) FROM t1;
DROP TABLE t1;


# Table with TTL won't increment rows expired when no records have been
# compacted
CREATE TABLE t1 (
  a bigint(20) NOT NULL,
  PRIMARY KEY (a)
) ENGINE=rocksdb
COMMENT='ttl_duration=100;';

INSERT INTO t1 values (1);
INSERT INTO t1 values (2);
INSERT INTO t1 values (3);

select variable_value into @c from information_schema.global_status where variable_name='rocksdb_rows_expired';
set global rocksdb_force_flush_memtable_now=1;
set global rocksdb_compact_cf='default';
select variable_value-@c from information_schema.global_status where variable_name='rocksdb_rows_expired';

DROP TABLE t1;
