--connection node_1
CREATE TABLE t1(a int not null primary key auto_increment, b int) ENGINE=InnoDB;
CREATE TABLE t2(a int not null primary key, b int) ENGINE=InnoDB;
INSERT INTO t1 VALUES (NULL,1),(NULL,2);
INSERT INTO t1(b) SELECT b+1 from t1;
INSERT INTO t1(b) SELECT b+1 from t1;
INSERT INTO t1(b) SELECT b+1 from t1;
INSERT INTO t1(b) SELECT b+1 from t1;
CREATE TABLE t3 AS SELECT * FROM t1;
CREATE TABLE t4 AS SELECT * FROM t2;
CREATE TABLE t5 (a INT UNIQUE) AS SELECT 1 AS a;
CREATE TABLE t6 (a INT UNIQUE) REPLACE SELECT 1 AS a;
CREATE TABLE t7 (a INT UNIQUE) REPLACE SELECT 1 AS a,2 AS b UNION SELECT 1 AS a,
3 AS c;

SELECT COUNT(*) AS EXPECT_32 FROM t1;
SELECT COUNT(*) AS EXPECT_0 FROM t2;
SELECT COUNT(*) AS EXPECT_32 FROM t3;
SELECT * FROM t4;
SELECT * FROM t5;
SELECT * FROM t6;
SELECT * FROM t7;

--connection node_2
--echo # Veryfy CTAS replication
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't1'
--source include/wait_condition.inc
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't2'
--source include/wait_condition.inc
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't3'
--source include/wait_condition.inc
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't4'
--source include/wait_condition.inc
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't5'
--source include/wait_condition.inc
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't6'
--source include/wait_condition.inc
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't7'
--source include/wait_condition.inc

SELECT COUNT(*) AS EXPECT_32 FROM t1;
SELECT COUNT(*) AS EXPECT_0 FROM t2;
SELECT COUNT(*) AS EXPECT_32 FROM t3;
SELECT * FROM t4;
SELECT * FROM t5;
SELECT * FROM t6;
SELECT * FROM t7;

--connection node_1
DROP TABLE t1,t2,t3,t4,t5,t6,t7;
