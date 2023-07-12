--echo #
--echo # MDEV-28190 sql_mode makes MDEV-371 virtual column expressions nondeterministic
--echo #

CREATE TABLE t1 (a INT,b CHAR(20));
SHOW CREATE TABLE t1;
CREATE UNIQUE INDEX bi USING HASH ON t1 (b);
INSERT INTO t1 VALUES (0,0);
SET sql_mode='pad_char_to_full_length';
DELETE FROM t1;
DROP TABLE t1;


SET sql_mode='';
CREATE TABLE t1 (a INT,b CHAR(20));
SHOW CREATE TABLE t1;
CREATE UNIQUE INDEX bi USING HASH ON t1 (b);
SET sql_mode='pad_char_to_full_length';
INSERT INTO t1 VALUES (0,0);
DELETE FROM t1;
DROP TABLE t1;


SET sql_mode='';
CREATE OR REPLACE TABLE t1 (a CHAR(20),b CHAR(20));
SHOW CREATE TABLE t1;
CREATE UNIQUE INDEX bi USING HASH ON t1 (b);
INSERT INTO t1 VALUES (0,0);
SET sql_mode='pad_char_to_full_length';
DELETE FROM t1;
DROP TABLE t1;
