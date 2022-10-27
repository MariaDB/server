#
# NOT NULL attribute in TIMESTAMP columns
#
# This is a copy of col_not_null.inc, except that 
# instead of getting an error on inserting NULL into a non-NULL column,
# we are getting the current timestamp (see MySQL:68472).
# If the bug is ever fixed, this include file won't be needed anymore.


--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

--echo #----------------------------------
--echo # $col_type NOT NULL column without a default
--echo #----------------------------------

eval CREATE TABLE t1 (pk INT AUTO_INCREMENT PRIMARY KEY, c $col_type NOT NULL) ENGINE=rocksdb;
SHOW COLUMNS IN t1;

# Here where the non-standard behavior strikes: 
# instead of an error we are getting the current timestamp

# As of mysql-5.6.11, this no longer works, and we get an error:
#  (MariaDB doesn't have this patch, so it doesnt produce an error):
# --error ER_BAD_NULL_ERROR
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

# Here where the non-standard behavior strikes: 
# instead of an error we are getting the current timestamp

# As of mysql-5.6.11, this no longer works, and we get an error:
#  (MariaDB doesn't have this patch, so it doesnt produce an error):
# --error ER_BAD_NULL_ERROR

# Since we don't produce an error, the row will get inserted. Make it
# deterministic:
set @save_ts=@@timestamp;
set timestamp=1478923914;

INSERT INTO t1 (c) VALUES (NULL);
set timestamp=@save_ts;

eval INSERT INTO t1 (c) VALUES ($col_default);
eval INSERT INTO t1 () VALUES ();

# HEX should be universal for all column types
SELECT pk, HEX(c) FROM t1 ORDER BY pk;

DROP TABLE t1;


