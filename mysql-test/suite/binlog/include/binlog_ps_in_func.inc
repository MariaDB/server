--echo #
--echo # MDEV-39518 Allow prepared statements in stored functions in assignment 
--echo #

--echo #
--echo # p3(set) - f2(set) - f1(immediate)
--echo #

RESET MASTER;

CREATE TABLE t1 (a INT);
SHOW CREATE TABLE t1;
DELIMITER $$;
CREATE FUNCTION f1_immediate() RETURNS INT
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (10)';
  RETURN 10;
END;
$$
CREATE FUNCTION f2_set() RETURNS INT
BEGIN
  DECLARE nrows INT;
  SET nrows= f1_immediate();
  RETURN nrows;
END;
$$
CREATE PROCEDURE p3_set()
BEGIN
  DECLARE nrows INT;
  SET nrows= f2_set();
END;
$$
DELIMITER ;$$
START TRANSACTION;
CALL p3_set;
COMMIT;
SELECT * FROM t1;
DROP PROCEDURE p3_set;
DROP FUNCTION f2_set;
DROP FUNCTION f1_immediate;
DROP TABLE t1;

--let $binlog_file = LAST
--source include/show_binlog_events.inc

--echo #
--echo # p3(set) - f2(set,immediate) - f1(immediate)
--echo #

RESET MASTER;

CREATE TABLE t1 (a INT);
SHOW CREATE TABLE t1;
DELIMITER $$;
CREATE FUNCTION f1_immediate() RETURNS INT
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (10)';
  RETURN 10;
END;
$$
CREATE FUNCTION f2_set_immediate() RETURNS INT
BEGIN
  DECLARE nrows INT;
  SET nrows= f1_immediate();
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (11)';
  RETURN nrows;
END;
$$
CREATE PROCEDURE p3_set()
BEGIN
  DECLARE nrows INT;
  SET nrows= f2_set_immediate();
END;
$$
DELIMITER ;$$
START TRANSACTION;
CALL p3_set;
COMMIT;
SELECT * FROM t1;
DROP PROCEDURE p3_set;
DROP FUNCTION f2_set_immediate;
DROP FUNCTION f1_immediate;
DROP TABLE t1;

--let $binlog_file = LAST
--source include/show_binlog_events.inc


--echo #
--echo # p3(set) - f2(call,immediate) - p1(immediate)
--echo #

RESET MASTER;

CREATE TABLE t1 (a INT);
SHOW CREATE TABLE t1;
DELIMITER $$;
CREATE PROCEDURE p1_immediate()
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (10)';
END;
$$
CREATE FUNCTION f2_call() RETURNS INT
BEGIN
  CALL p1_immediate();
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (11)';
  RETURN 0;
END;
$$
CREATE PROCEDURE p3_set()
BEGIN
  DECLARE nrows INT;
  SET nrows= f2_call();
END;
$$
DELIMITER ;$$
START TRANSACTION;
CALL p3_set;
COMMIT;
SELECT * FROM t1;
DROP PROCEDURE p3_set;
DROP FUNCTION f2_call;
DROP PROCEDURE p1_immediate;
DROP TABLE t1;

--let $binlog_file = LAST
--source include/show_binlog_events.inc


--echo #
--echo # Package
--echo #

RESET MASTER;

CREATE TABLE t1 (a INT);
SHOW CREATE TABLE t1;

DELIMITER $$;
CREATE PACKAGE pkg1
  PROCEDURE parse(query TEXT);
  FUNCTION exec() RETURNS INT;
  PROCEDURE close();
END;
$$
CREATE PACKAGE BODY pkg1
  PROCEDURE parse(query TEXT)
  BEGIN
    PREPARE stmt FROM query;
  END;
  FUNCTION exec() RETURNS INT
  BEGIN
    EXECUTE stmt;
    RETURN ROW_COUNT();
  END;
  PROCEDURE close()
  BEGIN
    DEALLOCATE PREPARE stmt;
  END;
END;
$$
DELIMITER ;$$

START TRANSACTION;
CALL pkg1.parse('INSERT INTO t1 VALUES (10)');
DELIMITER $$;
BEGIN NOT ATOMIC
  DECLARE nrows INT;
  SET nrows= pkg1.exec();
  SELECT nrows;
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (11)';
END;
$$
DELIMITER ;$$
COMMIT;

CALL pkg1.close();
SELECT * FROM t1;

DROP PACKAGE pkg1;
SELECT * FROM t1;
DROP TABLE t1;

--let $binlog_file = LAST
--source include/show_binlog_events.inc


--echo #
--echo # Package, with SYS_REFCURSOR
--echo #

SET sql_mode=ORACLE;

RESET MASTER;

CREATE TABLE t1 (a INT);
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES (10), (20);

DELIMITER $$;
CREATE PACKAGE pkg1 AS
  PROCEDURE parse(query TEXT);
  FUNCTION exec() RETURN INT;
  PROCEDURE close();
END;
$$
CREATE PACKAGE BODY pkg1 AS
  PROCEDURE parse(query TEXT) AS
  BEGIN
    PREPARE stmt FROM query;
  END;
  FUNCTION exec() RETURN INT AS
    c0 SYS_REFCURSOR;
    nrows INT:= 0;
    v0 INT;
  BEGIN
    OPEN c0 FOR PREPARE stmt;
    LOOP
      FETCH c0 INTO v0;
      EXIT WHEN c0%NOTFOUND;
      EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (?)' USING v0+100;
      nrows := nrows + 1;
    END LOOP;
    CLOSE c0;
    RETURN nrows;
  END;
  PROCEDURE close() AS
  BEGIN
    DEALLOCATE PREPARE stmt;
  END;
END;
$$
DELIMITER ;$$

CALL pkg1.parse('SELECT a FROM t1');
START TRANSACTION;
DELIMITER $$;
DECLARE
  nrows INT;
BEGIN 
  SET nrows= pkg1.exec();
  SELECT nrows;
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (200)'; -- Additional INSERT
END;
$$
DELIMITER ;$$
COMMIT;
CALL pkg1.close();
SELECT * FROM t1;

DROP PACKAGE pkg1;
SELECT * FROM t1;
DROP TABLE t1;

SET sql_mode=DEFAULT;
--let $binlog_file = LAST
--source include/show_binlog_events.inc


--echo #
--echo # MDEV-40227 Replication breaks when a function performs multiple INSERT statements with binlog_format=MIXED or STATEMENT
--echo #

--source include/reset_master.inc

CREATE TABLE t1 (id INT PRIMARY KEY AUTO_INCREMENT, v INT);
DELIMITER $$;
CREATE FUNCTION f() RETURNS INT
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1(v) VALUES (100)';
  EXECUTE IMMEDIATE 'INSERT INTO t1(v) VALUES (200)';
  INSERT INTO t1(v) VALUES (100);
  INSERT INTO t1(v) VALUES (200);
  RETURN 0;
END;
$$
CREATE PROCEDURE p()
BEGIN
  DECLARE n INT DEFAULT f();
END;
$$
DELIMITER ;$$
CALL p();
DROP PROCEDURE p;
DROP FUNCTION f;

DROP TABLE t1;

--let $binlog_file = LAST
--source include/show_binlog_events.inc


--echo #
--echo # MDEV-40318 MIXED binlog format does not switch an unsafe-for-SBR dynamic SQL (via PS) to ROW logging, causing replica divergence
--echo #

--source include/reset_master.inc

CREATE TABLE td (id INT PRIMARY KEY AUTO_INCREMENT,
                 ts_now DATETIME(6), rnd DOUBLE, uu CHAR(36), usr_var INT);

DELIMITER $$;
CREATE FUNCTION f_nd(seed INT) RETURNS INT
BEGIN
  SET @uv = seed * 7;
  EXECUTE IMMEDIATE 'INSERT INTO td (ts_now, rnd, uu, usr_var) '
                     'VALUES (NOW(6), RAND(?), UUID(), @uv)'
                    USING seed;
  RETURN seed;
END $$
CREATE PROCEDURE p_nd(seed INT)
BEGIN
  DECLARE r INT DEFAULT f_nd(seed);
END $$
DELIMITER ;$$

CALL p_nd(1);
CALL p_nd(2);
CALL p_nd(3);
CALL p_nd(4);
CALL p_nd(5);

DROP FUNCTION f_nd;
DROP PROCEDURE p_nd;
DROP TABLE td;

--let $binlog_file = LAST
--source include/show_binlog_events.inc
