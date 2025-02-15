--echo #
--echo # MDEV-30048 Prefix keys for CHAR work differently for MyISAM vs InnoDB
--echo # 

SET NAMES utf8mb3;

#
# Engines have different conditions based on the column size
# determining when to use trailing space compressions in key values,
# so let's test different column sizes for better coverage.
#


#
# CHAR(10)
#

CREATE TABLE t1 (a CHAR(10) COLLATE utf8mb3_unicode_nopad_ci, UNIQUE KEY(a));
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('ss'),('ß');
DROP TABLE t1;

CREATE TABLE t1 (a CHAR(10) COLLATE utf8mb3_unicode_nopad_ci, UNIQUE KEY(a(2)));
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('ss'),('ß');
DROP TABLE t1;

#
# CHAR(120)
#

CREATE TABLE t1 (a CHAR(120) COLLATE utf8mb3_unicode_nopad_ci, UNIQUE KEY(a));
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('ss'),('ß');
DROP TABLE t1;

CREATE TABLE t1 (a CHAR(120) COLLATE utf8mb3_unicode_nopad_ci, UNIQUE KEY(a(100)));
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('ss'),('ß');
DROP TABLE t1;

--echo #
--echo # MDEV-30050 Inconsistent results of DISTINCT with NOPAD
--echo #

CREATE TABLE t1 (c CHAR(100) COLLATE utf8mb3_unicode_nopad_ci);
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('ss'),('ß');
SELECT DISTINCT c FROM t1;
set statement tmp_memory_table_size=0 for
SELECT DISTINCT c FROM t1;
DROP TABLE t1;
