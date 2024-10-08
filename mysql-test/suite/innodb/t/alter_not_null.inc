# This test is run from alter_not_null.test with different combinations of
# alter_algorithm and strict mode

let $sql_mode = `SELECT @@SQL_MODE`;
let $error_code = 0;

if ($sql_mode == "STRICT_TRANS_TABLES") {
 let $error_code = WARN_DATA_TRUNCATED;
}

--echo #
--echo # sql_mode: $sql_mode   alter_algorithm: $alter_algorithm
--echo #

CREATE TABLE t1(f1 INT)ENGINE=INNODB;
INSERT INTO t1 VALUES(NULL);
SELECT * FROM t1;
--enable_info
--error $error_code
--eval ALTER TABLE t1 CHANGE f1 f1 INT NOT NULL, ALGORITHM=$alter_algorithm
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1(f1 CHAR(10))ENGINE=INNODB;
INSERT INTO t1 VALUES(NULL);
SELECT * FROM t1;
--enable_info
--error $error_code
--eval ALTER TABLE t1 CHANGE f1 f1 CHAR(10) NOT NULL, ALGORITHM=$alter_algorithm
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1(f1 VARCHAR(10))ENGINE=INNODB;
INSERT INTO t1 VALUES(NULL);
SELECT * FROM t1;
--enable_info
--error $error_code
--eval ALTER TABLE t1 CHANGE f1 f1 VARCHAR(20) NOT NULL, ALGORITHM=$alter_algorithm
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1(f1 TEXT)ENGINE=INNODB;
INSERT INTO t1 VALUES(NULL);
SELECT * FROM t1;
--enable_info
--error $error_code
--eval ALTER TABLE t1 CHANGE f1 f1 TEXT NOT NULL DEFAULT 'abc', ALGORITHM=$alter_algorithm
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1(f1 INT NOT NULL, f2 INT NOT NULL, f3 INT)ENGINE=INNODB;
INSERT INTO t1 VALUES(2, 2, NULL);
SELECT * FROM t1;
--enable_info
--error $error_code
--eval ALTER TABLE t1 CHANGE f3 f3 INT NOT NULL DEFAULT (f1 + f2), ALGORITHM=$alter_algorithm
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1(f1 INT NOT NULL DEFAULT 0, b TINYINT)ENGINE=InnoDB;
INSERT INTO t1 VALUES(10, NULL);
SELECT * FROM t1;
--enable_info
--error $error_code
--eval ALTER TABLE t1 CHANGE b b TINYINT NOT NULL DEFAULT if(unix_timestamp()>1,1000,0), ALGORITHM=$alter_algorithm
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1(a INT, v INT AS (a), c INT, d INT NOT NULL, e INT) ENGINE=InnoDB;
--enable_info
--eval ALTER TABLE t1 DROP COLUMN c, CHANGE COLUMN e e INT NOT NULL, ALGORITHM=$alter_algorithm
--disable_info
DROP TABLE t1;

CREATE TABLE t1 (a INT, v INT AS (a), d INT NOT NULL, e INT) ENGINE=InnoDB;
--enable_info
--eval ALTER TABLE t1 FORCE, ALGORITHM=$alter_algorithm
--disable_info
DROP TABLE t1;

# Alter ignore should work irrespective of sql mode

CREATE TABLE t1(c1 INT NOT NULL, c2 INT, PRIMARY KEY(c1))ENGINE=INNODB;
INSERT INTO t1 VALUES(1, NULL);
--enable_info
ALTER IGNORE TABLE t1 CHANGE c2 c2 INT NOT NULL DEFAULT 2;
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

--echo #
--echo # MDEV-16126 Crash or ASAN heap-buffer-overflow in
--echo # mach_read_from_n_little_endian upon ALTER TABLE with blob
--echo #

CREATE TABLE t1(a INT, v INT AS (a), b INT, c BLOB) ENGINE=InnoDB;
--enable_info
--eval ALTER TABLE t1 ADD PRIMARY KEY(b), ALGORITHM=$alter_algorithm
--disable_info
DROP TABLE t1;
