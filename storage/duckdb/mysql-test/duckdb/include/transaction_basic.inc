#
# Basic transaction tests for DuckDB engine
# Tests: Atomicity, Durability, Autocommit, Isolation
# Also tests mixed InnoDB+DuckDB transactions
#

--source include/have_innodb.inc

--disable_query_log
CREATE DATABASE IF NOT EXISTS db_transaction CHARACTER SET utf8mb4;
USE db_transaction;
--enable_query_log

# -----------------------------------------------------------------
# Test 1: Atomicity - ROLLBACK discards changes
# -----------------------------------------------------------------
--echo # Test 1: Atomicity

DROP TABLE IF EXISTS t1;
CREATE TABLE t1 (
  id INT DEFAULT 1 PRIMARY KEY,
  name VARCHAR(10) DEFAULT 'NAME'
) ENGINE = DuckDB;

INSERT INTO t1 (id, name) VALUES (1, '1');

BEGIN;
INSERT INTO t1 (id, name) VALUES (2, '2');
INSERT INTO t1 (id, name) VALUES (3, '3');
ROLLBACK;

SELECT * FROM t1;

# -----------------------------------------------------------------
# Test 2: Durability - data survives restart
# -----------------------------------------------------------------
--echo # Test 2: Durability after restart

--source include/restart_mysqld.inc
--disable_query_log
USE db_transaction;
--enable_query_log
SELECT * FROM t1;

# -----------------------------------------------------------------
# Test 3: Uncommitted data lost on restart
# -----------------------------------------------------------------
--echo # Test 3: Restart rolls back uncommitted transaction

BEGIN;
INSERT INTO t1 (id, name) VALUES (4, '4');
INSERT INTO t1 (id, name) VALUES (5, '5');
--source include/restart_mysqld.inc
--disable_query_log
USE db_transaction;
--enable_query_log
SELECT * FROM t1;

# -----------------------------------------------------------------
# Test 4: Autocommit
# -----------------------------------------------------------------
--echo # Test 4: Autocommit persists through restart

SET autocommit = 1;
INSERT INTO t1 (id, name) VALUES (3, '3');
--source include/restart_mysqld.inc
--disable_query_log
USE db_transaction;
--enable_query_log
SELECT * FROM t1;

# -----------------------------------------------------------------
# Test 5: Isolation - uncommitted data not visible to other sessions
# -----------------------------------------------------------------
--echo # Test 5: Isolation

--disable_query_log
DROP TABLE t1;
CREATE TABLE t1 (
  id INT DEFAULT 1 PRIMARY KEY,
  name VARCHAR(10) DEFAULT 'NAME'
) ENGINE = DuckDB;
--enable_query_log

connect (con1,localhost,root,,db_transaction);
connect (con2,localhost,root,,db_transaction);

connection con1;
BEGIN;
INSERT INTO t1 (id, name) VALUES (1, '1');

connection con2;
--echo # Should be empty
SELECT * FROM t1;

connection con1;
COMMIT;

connection con2;
--echo # Should show 1 row
SELECT * FROM t1;

# -----------------------------------------------------------------
# Test 6: Mixed InnoDB+DuckDB atomicity
# -----------------------------------------------------------------
--echo # Test 6: Mixed InnoDB+DuckDB atomicity

--disable_query_log
connection default;
DROP TABLE t1;
--enable_query_log

DROP TABLE IF EXISTS t_duck;
DROP TABLE IF EXISTS t_inno;
CREATE TABLE t_duck (
  id INT DEFAULT 1 PRIMARY KEY,
  name VARCHAR(10) DEFAULT 'NAME'
) ENGINE = DuckDB;
CREATE TABLE t_inno (
  id INT DEFAULT 1 PRIMARY KEY,
  name VARCHAR(10) DEFAULT 'NAME'
) ENGINE = InnoDB;

INSERT INTO t_duck (id, name) VALUES (1, '1');
INSERT INTO t_inno (id, name) VALUES (1, '1');

BEGIN;
INSERT INTO t_duck (id, name) VALUES (2, '2');
INSERT INTO t_inno (id, name) VALUES (2, '2');
ROLLBACK;

SELECT * FROM t_duck;
SELECT * FROM t_inno;

# -----------------------------------------------------------------
# Test 7: Mixed InnoDB+DuckDB isolation
# -----------------------------------------------------------------
--echo # Test 7: Mixed InnoDB+DuckDB isolation

--disable_query_log
DROP TABLE t_duck;
DROP TABLE t_inno;
CREATE TABLE t_duck (
  id INT DEFAULT 1 PRIMARY KEY,
  name VARCHAR(10) DEFAULT 'NAME'
) ENGINE = DuckDB;
CREATE TABLE t_inno (
  id INT DEFAULT 1 PRIMARY KEY,
  name VARCHAR(10) DEFAULT 'NAME'
) ENGINE = InnoDB;
--enable_query_log

connection con1;
BEGIN;
INSERT INTO t_duck (id, name) VALUES (3, '3');
INSERT INTO t_inno (id, name) VALUES (3, '3');

connection con2;
SELECT * FROM t_duck;
SELECT * FROM t_inno;

connection con1;
COMMIT;

connection con2;
SELECT * FROM t_duck;
SELECT * FROM t_inno;

# -----------------------------------------------------------------
# Test 8: Mixed InnoDB+DuckDB durability
# -----------------------------------------------------------------
--echo # Test 8: Mixed InnoDB+DuckDB durability

connection default;
INSERT INTO t_duck (id, name) VALUES (4, '4');
INSERT INTO t_inno (id, name) VALUES (4, '4');

disconnect con1;
disconnect con2;

--source include/restart_mysqld.inc
--disable_query_log
USE db_transaction;
--enable_query_log
SELECT * FROM t_duck;
SELECT * FROM t_inno;

# -----------------------------------------------------------------
# Cleanup
# -----------------------------------------------------------------
--disable_query_log
DROP TABLE t_duck;
DROP TABLE t_inno;
DROP DATABASE db_transaction;
USE test;
--enable_query_log

