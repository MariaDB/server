#
# BIT column type
#

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

# Valid values

eval CREATE TABLE t1 (
  a BIT $extra_col_opts,
  b BIT(20) $extra_col_opts,
  c BIT(64) $extra_col_opts,
  d BIT(1) $extra_col_opts,
  PRIMARY KEY (c)
) ENGINE=rocksdb;

SHOW COLUMNS IN t1;

ALTER TABLE t1 DROP COLUMN d;
eval ALTER TABLE t1 ADD COLUMN d BIT(0) $extra_col_opts;
SHOW COLUMNS IN t1;

INSERT INTO t1 (a,b,c,d) VALUES (0,POW(2,20)-1,b'1111111111111111111111111111111111111111111111111111111111111111',1);
SELECT BIN(a), HEX(b), c+0 FROM t1 WHERE d>0;

INSERT INTO t1 (a,b,c,d) VALUES (1,0,-2,0);
--sorted_result
SELECT a+0, b+0, c+0 FROM t1 WHERE d<100;

INSERT INTO t1 (a,b,c,d) VALUES (b'1', 'f', 0xFF, 0x0);
--sorted_result
SELECT a+0, b+0, c+0 FROM t1 WHERE d IN (0, 2);

DELETE FROM t1;

# Out of range values
# (should produce warnings)

INSERT INTO t1 (a,b,c,d) VALUES (0x10,0,0,1);
SELECT a+0,b+0,c+0,d+0 FROM t1;

INSERT INTO t1 (a,b,c,d) VALUES (0x01,0,0x10000000000000000,0);
--sorted_result
SELECT a+0,b+0,c+0,d+0 FROM t1;

DROP TABLE t1;

--error ER_TOO_BIG_DISPLAYWIDTH
eval CREATE TABLE t1 (pk INT PRIMARY KEY, a BIT(65) $extra_col_opts) ENGINE=rocksdb;


