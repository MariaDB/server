#
# This include file checks some very basic capabilities for restart insert
# update and delete for tables with no pk
# NOTE: requires table with structure similar to
# CREATE TABLE t1 (a INT, b CHAR(8)) ENGINE=rocksdb;
#

SHOW CREATE TABLE t1;
SHOW COLUMNS IN t1;

### test INSERT
INSERT INTO t1 (a,b) VALUES (76,'bar');
INSERT INTO t1 (a,b) VALUES (35,'foo');
INSERT INTO t1 (a,b) VALUES (77,'baz');

## test SELECT w/ index scans
--sorted_result
SELECT * FROM t1 WHERE a = 35;
--sorted_result
SELECT * FROM t1 WHERE a = 35 AND b = 'foo';
--sorted_result
SELECT * FROM t1 WHERE a = 77 OR b = 'bar';
--sorted_result
SELECT * FROM t1 WHERE a > 35;
--sorted_result
SELECT * FROM t1;

# test UPDATE
UPDATE t1 SET a=a+100;
--sorted_result
SELECT * FROM t1;

UPDATE t1 SET a=a-100, b='bbb' WHERE a>100;
--sorted_result
SELECT * FROM t1;
UPDATE t1 SET a=300, b='ccc' WHERE a>70;
--sorted_result
SELECT * FROM t1;
UPDATE t1 SET a=123 WHERE a=35;
--sorted_result
SELECT * FROM t1;
UPDATE t1 SET a=321 WHERE b='ccc';
--sorted_result
SELECT * FROM t1;


## test RESTART/OPEN
--source include/restart_mysqld.inc
## test insert after restart
INSERT INTO t1 (a,b) VALUES (45,'bob');
--sorted_result
SELECT * FROM t1;

# test DELETE
DELETE FROM t1 WHERE a=123;
--sorted_result
SELECT * FROM t1;

DELETE FROM t1 WHERE b > 'bbb' AND a > 100;
--sorted_result
SELECT * FROM t1;

# test TRUNCATE
TRUNCATE TABLE t1;

