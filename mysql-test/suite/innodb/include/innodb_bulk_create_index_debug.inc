#
# wl#7277: InnoDB: Bulk Load for Create Index
#

# Not supported in embedded
-- source include/not_embedded.inc
-- source include/have_debug.inc
-- source include/have_debug_sync.inc

# Create Insert Procedure
DELIMITER |;
CREATE PROCEDURE populate_t1()
BEGIN
	DECLARE i int DEFAULT 1;

	START TRANSACTION;
	WHILE (i <= 10000) DO
		INSERT INTO t1 VALUES (i, i, CONCAT('a', i));
		SET i = i + 1;
	END WHILE;
	COMMIT;
END|
DELIMITER ;|

# Test scenarios:
# 1. Test restart;
# 2. Test crash recovery.

# Test Restart
if ($row_format != 'COMPRESSED')
{
  eval CREATE TABLE t1(
        class   INT,
        id      INT,
        title   VARCHAR(100)
  ) ENGINE=InnoDB ROW_FORMAT=$row_format;
}

if ($row_format == 'COMPRESSED')
{
  SET GLOBAL innodb_file_per_table=1;

  eval CREATE TABLE t1(
        class   INT,
        id      INT,
        title   VARCHAR(100)
  ) ENGINE=InnoDB ROW_FORMAT=$row_format KEY_BLOCK_SIZE=4;
}

-- disable_query_log
CALL populate_t1();
-- enable_query_log

SELECT COUNT(*) FROM t1;

--enable_info
CREATE INDEX idx_title ON t1(title);
--disable_info

RENAME TABLE t1 TO t0;

-- echo # Test Blob

if ($row_format != 'COMPRESSED') {
  eval CREATE TABLE t1(
        a INT PRIMARY KEY,
        b TEXT,
        c TEXT) ENGINE=InnoDB ROW_FORMAT=$row_format;
}

if ($row_format == 'COMPRESSED') {
  SET GLOBAL innodb_file_per_table=1;

  eval CREATE TABLE t1(
        a INT PRIMARY KEY,
        b TEXT,
        c TEXT) ENGINE=InnoDB ROW_FORMAT=$row_format KEY_BLOCK_SIZE=4;
}

INSERT INTO t1 VALUES
	(1, REPEAT('a',10000), 'a'),
	(2, REPEAT('b',20000), 'b'),
	(3, REPEAT('c',40000), 'c'),
	(4, REPEAT('d',60000), 'd');

SELECT CHAR_LENGTH(b) FROM t1;

--enable_info
ALTER TABLE t1 DROP COLUMN c, FORCE;
--disable_info

--source include/restart_mysqld.inc

CHECK TABLE t0,t1;

SELECT CHAR_LENGTH(b) FROM t1;

DROP TABLE t1;

RENAME TABLE t0 to t1;

CHECK TABLE t1;

SELECT * FROM t1 WHERE title = 'a10';

SELECT * FROM t1 WHERE title = 'a5000';

SELECT * FROM t1 WHERE title = 'a10000';

SELECT * FROM t1 WHERE title = 'a10010';

DROP TABLE t1;

# Test Crash Recovery

if ($row_format != 'COMPRESSED')
{
  eval CREATE TABLE t1(
        class   INT,
        id      INT,
        title   VARCHAR(100)
  ) ENGINE=InnoDB ROW_FORMAT=$row_format;
}

if ($row_format == 'COMPRESSED')
{
  SET GLOBAL innodb_file_per_table=1;

  eval CREATE TABLE t1(
        class   INT,
        id      INT,
        title   VARCHAR(100)
  ) ENGINE=InnoDB ROW_FORMAT=$row_format KEY_BLOCK_SIZE=4;
}

-- disable_query_log
CALL populate_t1();
-- enable_query_log

connect (hang,localhost,root);
SET DEBUG_SYNC='alter_table_inplace_trans_commit SIGNAL hung WAIT_FOR ever';
send
CREATE INDEX idx_title ON t1(title);

connection default;
SET DEBUG_SYNC='now WAIT_FOR hung';
let $shutdown_timeout=0;
--source include/restart_mysqld.inc
disconnect hang;

SELECT COUNT(*) FROM t1;

CHECK TABLE t1;

EXPLAIN SELECT * FROM t1 WHERE title = 'a10';

SELECT * FROM t1 WHERE title = 'a10';

SELECT * FROM t1 WHERE title = 'a5000';

SELECT * FROM t1 WHERE title = 'a10000';

SELECT * FROM t1 WHERE title = 'a10010';

DROP TABLE t1;

-- echo # Test Blob

if ($row_format != 'COMPRESSED') {
  eval CREATE TABLE t1(
        a INT PRIMARY KEY,
        b TEXT,
        c TEXT) ENGINE=InnoDB ROW_FORMAT=$row_format;
}

if ($row_format == 'COMPRESSED') {
  SET GLOBAL innodb_file_per_table=1;

  eval CREATE TABLE t1(
        a INT PRIMARY KEY,
        b TEXT,
        c TEXT) ENGINE=InnoDB ROW_FORMAT=$row_format KEY_BLOCK_SIZE=4;
}

INSERT INTO t1 VALUES
	(1, REPEAT('a',10000), 'a'),
	(2, REPEAT('b',20000), 'b'),
	(3, REPEAT('c',40000), 'c'),
	(4, REPEAT('d',60000), 'd');

SELECT CHAR_LENGTH(b) FROM t1;

connect (hang,localhost,root);
SET DEBUG_SYNC='alter_table_inplace_trans_commit SIGNAL hung WAIT_FOR ever';
send
ALTER TABLE t1 DROP COLUMN c, FORCE;

connection default;
SET DEBUG_SYNC='now WAIT_FOR hung';
--source include/restart_mysqld.inc
disconnect hang;
let $shutdown_timeout=60;

CHECK TABLE t1;

SELECT CHAR_LENGTH(b) FROM t1;

DROP TABLE t1;

DROP PROCEDURE populate_t1;
