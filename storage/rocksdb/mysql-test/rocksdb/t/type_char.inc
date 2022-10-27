#
# CHAR column types
#

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

eval CREATE TABLE t1 (
  c CHAR $extra_col_opts,
  c0 CHAR(0) $extra_col_opts,
  c1 CHAR(1) $extra_col_opts,
  c20 CHAR(20) $extra_col_opts,
  c255 CHAR(255) $extra_col_opts,
  PRIMARY KEY (c255)
) ENGINE=rocksdb;

SHOW COLUMNS IN t1;

# Valid values 

INSERT INTO t1 (c,c0,c1,c20,c255) VALUES ('','','','','');
INSERT INTO t1 (c,c0,c1,c20,c255) VALUES ('a','','b','abcdefghi klmnopqrst', 'Creating an article for the Knowledgebase is similar to asking questions. First, navigate to the category where you feel the article should be. Once there, double check that an article doesn\'t already exist which would work.');

--sorted_result
SELECT c,c0,c1,c20,c255 FROM t1;

# Invalid values

INSERT INTO t1 (c,c0,c1,c20,c255) VALUES ('abc', 'a', 'abc', REPEAT('a',21), REPEAT('x',256));
INSERT INTO t1 (c,c0,c1,c20,c255) SELECT c255, c255, c255, c255, CONCAT('a',c255,c1) FROM t1;

--sorted_result
SELECT c,c0,c1,c20,c255 FROM t1;

--sorted_result
SELECT DISTINCT c20, REPEAT('a',LENGTH(c20)), COUNT(*) FROM t1 GROUP BY c1, c20;

--error ER_TOO_BIG_FIELDLENGTH
eval ALTER TABLE t1 ADD COLUMN c257 CHAR(257) $extra_col_opts;

DROP TABLE t1;



