#
# NOT NULL attribute in columns
#
# Usage:
# let $col_type = <column type>;
# let $col_default = <default value for a column>;
# --source col_not_null.inc
#
# We will add NOT NULL to the column options;
#

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

--echo #----------------------------------
--echo # $col_type NOT NULL columns without a default
--echo #----------------------------------

eval CREATE TABLE t1 (pk INT AUTO_INCREMENT PRIMARY KEY, c $col_type NOT NULL) ENGINE=rocksdb;
SHOW COLUMNS IN t1;

--error ER_BAD_NULL_ERROR
INSERT INTO t1 (c) VALUES (NULL);
eval INSERT INTO t1 (c) VALUES ($col_default);
SELECT HEX(c) FROM t1;

DROP TABLE t1;

--echo #----------------------------------
--echo # $col_type NOT NULL columns with a default
--echo #----------------------------------

eval CREATE TABLE t1 (
  pk INT AUTO_INCREMENT PRIMARY KEY,
  c $col_type NOT NULL DEFAULT $col_default
) ENGINE=rocksdb;

SHOW COLUMNS IN t1;

--error ER_INVALID_DEFAULT
eval ALTER TABLE t1 ADD COLUMN err $col_type NOT NULL DEFAULT NULL;

--error ER_BAD_NULL_ERROR
INSERT INTO t1 (c) VALUES (NULL);

eval INSERT INTO t1 (c) VALUES ($col_default);
eval INSERT INTO t1 () VALUES ();

# HEX should be universal for all column types
SELECT pk, HEX(c) FROM t1 ORDER BY pk;

DROP TABLE t1;


