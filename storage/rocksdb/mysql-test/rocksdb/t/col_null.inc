#
# NULL attribute and DEFAULT NULL in columns
#
# Usage:
# let $col_type = <column type>;
# let $col_default = <default non-null value for a column>;
# --source col_null.inc
#
# We will add NULL attribute to the column options.
#


--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

eval CREATE TABLE t1 (
  c $col_type NULL,
  c1 $col_type NULL DEFAULT NULL,
  c2 $col_type NULL DEFAULT $col_default,
  pk INT AUTO_INCREMENT PRIMARY KEY
) ENGINE=rocksdb;

SHOW COLUMNS IN t1;

INSERT INTO t1 (c,c1,c2) VALUES (NULL,NULL,NULL);
eval INSERT INTO t1 (c,c1,c2) VALUES ($col_default,$col_default,$col_default);
INSERT INTO t1 () VALUES ();

SELECT pk, HEX(c), HEX(c1), HEX(c2) FROM t1 ORDER BY pk;
SELECT pk, HEX(c2) FROM t1 ORDER BY pk;

DROP TABLE t1;

