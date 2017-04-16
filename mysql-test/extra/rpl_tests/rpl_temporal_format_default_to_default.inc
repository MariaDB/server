#
# This include file is used by more than one test suite
# (currently rpl and binlog_encryption).
# Please check all dependent tests after modifying it
#

--source include/master-slave.inc

if ($force_master_mysql56_temporal_format)
{
  connection master;
  eval SET @@global.mysql56_temporal_format=$force_master_mysql56_temporal_format;
}

if ($force_slave_mysql56_temporal_format)
{
  connection slave;
  eval SET @@global.mysql56_temporal_format=$force_slave_mysql56_temporal_format;
}

connection master;
SELECT @@global.mysql56_temporal_format AS on_master;
connection slave;
SELECT @@global.mysql56_temporal_format AS on_slave;
connection master;

CREATE TABLE t1
(
  c0 TIME(0),
  c1 TIME(1),
  c2 TIME(2),
  c3 TIME(3),
  c4 TIME(4),
  c5 TIME(5),
  c6 TIME(6)
);
CREATE TABLE t2
(
  c0 TIMESTAMP(0),
  c1 TIMESTAMP(1),
  c2 TIMESTAMP(2),
  c3 TIMESTAMP(3),
  c4 TIMESTAMP(4),
  c5 TIMESTAMP(5),
  c6 TIMESTAMP(6)
);

CREATE TABLE t3
(
  c0 DATETIME(0),
  c1 DATETIME(1),
  c2 DATETIME(2),
  c3 DATETIME(3),
  c4 DATETIME(4),
  c5 DATETIME(5),
  c6 DATETIME(6)
);
INSERT INTO t1 VALUES ('01:01:01','01:01:01.1','01:01:01.11','01:01:01.111','01:01:01.1111','01:01:01.11111','01:01:01.111111');
INSERT INTO t2 VALUES ('2001-01-01 01:01:01','2001-01-01 01:01:01.1','2001-01-01 01:01:01.11','2001-01-01 01:01:01.111','2001-01-01 01:01:01.1111','2001-01-01 01:01:01.11111','2001-01-01 01:01:01.111111');
INSERT INTO t3 VALUES ('2001-01-01 01:01:01','2001-01-01 01:01:01.1','2001-01-01 01:01:01.11','2001-01-01 01:01:01.111','2001-01-01 01:01:01.1111','2001-01-01 01:01:01.11111','2001-01-01 01:01:01.111111');
SELECT TABLE_NAME, TABLE_ROWS, AVG_ROW_LENGTH,DATA_LENGTH FROM INFORMATION_SCHEMA.TABLES
WHERE TABLE_NAME RLIKE 't[1-3]' ORDER BY TABLE_NAME;
sync_slave_with_master;

connection slave;
--query_vertical SELECT * FROM t1;
--query_vertical SELECT * FROM t2;
--query_vertical SELECT * FROM t3;
SELECT TABLE_NAME, TABLE_ROWS, AVG_ROW_LENGTH,DATA_LENGTH FROM INFORMATION_SCHEMA.TABLES
WHERE TABLE_NAME RLIKE 't[1-3]' ORDER BY TABLE_NAME;

connection master;
DROP TABLE t1;
DROP TABLE t2;
DROP TABLE t3;

connection slave;
SET @@global.mysql56_temporal_format=DEFAULT;
connection master;
SET @@global.mysql56_temporal_format=DEFAULT;

--source include/rpl_end.inc
