#
# BOOLEAN column type
#

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

eval CREATE TABLE t1 (
  pk INT AUTO_INCREMENT PRIMARY KEY,
  b1 BOOL $extra_col_opts,
  b2 BOOLEAN $extra_col_opts
) ENGINE=rocksdb;

SHOW COLUMNS IN t1;

# Valid values 

INSERT INTO t1 (b1,b2) VALUES (1,TRUE);
SELECT b1,b2 FROM t1;

INSERT INTO t1 (b1,b2) VALUES (FALSE,0);
--sorted_result
SELECT b1,b2 FROM t1;

INSERT INTO t1 (b1,b2) VALUES (2,3);
--sorted_result
SELECT b1,b2 FROM t1;

INSERT INTO t1 (b1,b2) VALUES (-1,-2);
--sorted_result
SELECT b1,b2 FROM t1;

--sorted_result
SELECT IF(b1,'true','false') AS a, IF(b2,'true','false') AS b FROM t1;

--sorted_result
SELECT b1,b2 FROM t1 WHERE b1 = TRUE;

--sorted_result
SELECT b1,b2 FROM t1 WHERE b2 = FALSE;

# Invalid values

INSERT INTO t1 (b1,b2) VALUES ('a','b');
--sorted_result
SELECT b1,b2 FROM t1;

INSERT INTO t1 (b1,b2) VALUES (128,-129);
--sorted_result
SELECT b1,b2 FROM t1;

# This is why we don't have zerofill and unsigned tests
# for boolean columns:
--error ER_PARSE_ERROR
eval ALTER TABLE t1 ADD COLUMN b3 BOOLEAN UNSIGNED $extra_col_opts;

--error ER_PARSE_ERROR
eval ALTER TABLE ADD COLUMN b3 BOOL ZEROFILL $extra_col_opts;

DROP TABLE t1;



