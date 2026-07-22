--echo #
--echo # MDEV-7824 [Bug #68041] Zero date can be inserted in strict no-zero mode through a default value
--echo #

# Testing direct INSERT

SET sql_mode=DEFAULT;
eval CREATE TABLE t1 (a $type DEFAULT $defval);
SET sql_mode=TRADITIONAL;
--error ER_TRUNCATED_WRONG_VALUE
eval INSERT INTO t1 VALUES ($defval);
--error ER_INVALID_DEFAULT_VALUE_FOR_FIELD
INSERT INTO t1 VALUES ();
--error ER_INVALID_DEFAULT_VALUE_FOR_FIELD
INSERT INTO t1 VALUES (DEFAULT);
DROP TABLE t1;
SET sql_mode=DEFAULT;


# Testing INSERT .. SELECT

eval CREATE TABLE t1 (a $type NOT NULL DEFAULT $defval, b $type NOT NULL DEFAULT $defval);
eval CREATE TABLE t2 (a $type NOT NULL DEFAULT $defval);
eval INSERT INTO t2 VALUES ($defval);
SET sql_mode=TRADITIONAL;
--error ER_INVALID_DEFAULT_VALUE_FOR_FIELD
INSERT INTO t1 (a) SELECT a FROM t2;
DROP TABLE t1, t2;
SET sql_mode=DEFAULT;


# Testing LOAD

--eval CREATE TABLE t1 (a $type DEFAULT $defval, b $type DEFAULT $defval)
--eval INSERT INTO t1 VALUES (DEFAULT,DEFAULT);
--replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
--eval SELECT a INTO OUTFILE '$MYSQLTEST_VARDIR/tmp/mdev-7824.txt' FROM t1
DELETE FROM t1;
SET sql_mode=TRADITIONAL;
--replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
--error ER_INVALID_DEFAULT_VALUE_FOR_FIELD
--eval LOAD DATA INFILE '$MYSQLTEST_VARDIR/tmp/mdev-7824.txt' INTO TABLE t1 (a)
--remove_file $MYSQLTEST_VARDIR/tmp/mdev-7824.txt
DROP TABLE t1;
SET sql_mode=DEFAULT;

# Testing ALTER when an old field default becomes invalid
# Return an error, even if there is no STRICT_XXX_TABLES set
--eval CREATE TABLE t1 (a $type DEFAULT $defval);
SET sql_mode='NO_ZERO_DATE';
--error ER_INVALID_DEFAULT
ALTER TABLE t1 ADD b INT NOT NULL;
DROP TABLE t1;
SET sql_mode=DEFAULT;


--echo #
--echo # End of MDEV-7824 [Bug #68041] Zero date can be inserted in strict no-zero mode through a default value
--echo #

--echo #
--echo # MDEV-8373 Zero date can be inserted in strict no-zero mode through CREATE TABLE AS SELECT timestamp_field
--echo #

SET sql_mode=DEFAULT;
--eval CREATE TABLE t1 (a $type);
INSERT INTO t1 VALUES (0);
SET sql_mode='TRADITIONAL';
--error ER_TRUNCATED_WRONG_VALUE
CREATE TABLE t2 AS SELECT * FROM t1;
DROP TABLE t1;

--echo #
--echo # End of MDEV-8373 Zero date can be inserted in strict no-zero mode through CREATE TABLE AS SELECT timestamp_field
--echo #
