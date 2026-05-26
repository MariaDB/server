--echo #
--echo # MDEV-39518 Allow prepared statements in stored functions in assignment right hand
--echo #

--echo #
--echo # p3(set) - f2(set) - f1(immediate)
--echo #

--source include/reset_master.inc

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

--source include/reset_master.inc

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

--source include/reset_master.inc

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

--source include/reset_master.inc

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

--source include/reset_master.inc

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

--echo #
--echo # End of MDEV-40318
--echo #


--echo #
--echo # Test functions without PS
--echo #

--source include/reset_master.inc

CREATE TABLE t1 (id INT PRIMARY KEY AUTO_INCREMENT, num INT);

DELIMITER $$;
CREATE FUNCTION f_without_ps() RETURNS INT
BEGIN
  INSERT INTO t1 (num) VALUES (1);
  INSERT INTO t1 (num) VALUES (2);
  RETURN 1;
END $$
CREATE PROCEDURE p1()
BEGIN
  DECLARE r INT DEFAULT f_without_ps();
END $$
DELIMITER ;$$

CALL p1();
CALL p1();
CALL p1();
CALL p1();
CALL p1();

INSERT INTO t1 (num) VALUES (11);
INSERT INTO t1 (num) VALUES (12);

DROP FUNCTION f_without_ps;
DROP PROCEDURE p1;
DROP TABLE t1;

--let $MYSQLD_DATADIR= `SELECT @@datadir`
--let $binlog_name= query_get_value(SHOW MASTER STATUS, File, 1)
--replace_regex /^\# at .*// /.*SET (@@|TIMESTAMP).*// /.* end_log_pos .*// /xid=\d*/xid=<xid>/
--exec $MYSQL_BINLOG --force-if-open --base64-output=DECODE-ROWS --verbose --verbose $MYSQLD_DATADIR/$binlog_name


--echo #
--echo # Test the same functions but with PS
--echo #

--source include/reset_master.inc

CREATE TABLE t1 (id INT PRIMARY KEY AUTO_INCREMENT, num INT);

DELIMITER $$;
CREATE FUNCTION f_with_ps() RETURNS INT
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1 (num) VALUES (1)';
  EXECUTE IMMEDIATE 'INSERT INTO t1 (num) VALUES (2)';
  RETURN 1;
END $$
CREATE PROCEDURE p1()
BEGIN
  DECLARE r INT DEFAULT f_with_ps();
END $$
DELIMITER ;$$

CALL p1();
CALL p1();
CALL p1();
CALL p1();
CALL p1();

EXECUTE IMMEDIATE 'INSERT INTO t1 (num) VALUES (11)';
EXECUTE IMMEDIATE 'INSERT INTO t1 (num) VALUES (12)';

DROP FUNCTION f_with_ps;
DROP PROCEDURE p1;
DROP TABLE t1;

--let $MYSQLD_DATADIR= `SELECT @@datadir`
--let $binlog_name= query_get_value(SHOW MASTER STATUS, File, 1)
--replace_regex /^\# at .*// /.*SET (@@|TIMESTAMP).*// /.* end_log_pos .*// /xid=\d*/xid=<xid>/
--exec $MYSQL_BINLOG --force-if-open --base64-output=DECODE-ROWS --verbose --verbose $MYSQLD_DATADIR/$binlog_name


--echo #
--echo # A mixture of functions with and without PS
--echo #

--source include/reset_master.inc
--source include/ps_in_func-mixture-01-create.inc
CALL p1();
SELECT * FROM t1 ORDER BY val, caller;
--source include/ps_in_func-mixture-01-drop.inc

--let $MYSQLD_DATADIR= `SELECT @@datadir`
--let $binlog_name= query_get_value(SHOW MASTER STATUS, File, 1)
--replace_regex /^\# at .*// /.*SET (@@|TIMESTAMP).*// /.* end_log_pos .*// /xid=\d*/xid=<xid>/
--exec $MYSQL_BINLOG --force-if-open --base64-output=DECODE-ROWS --verbose --verbose $MYSQLD_DATADIR/$binlog_name


--echo #
--echo # A dynamic statement inside a function with PS fires a trigger,
--echo # and the trigger body calls another function.
--echo #
--echo # The trigger is not logged per statement: the INSERT which fired it
--echo # is logged instead, and the replica fires the trigger itself. So the
--echo # trigger body must not inherit the per-statement logging mode of the
--echo # enclosing function, and the function called from the trigger body
--echo # must not start a second binlog event union on top of the trigger's.
--echo #

--source include/reset_master.inc

CREATE TABLE t1 (a INT);
CREATE TABLE t2 (a INT);
DELIMITER $$;
CREATE FUNCTION f_double(a INT) RETURNS INT
BEGIN
  RETURN a * 2;
END;
$$
DELIMITER ;$$
CREATE TRIGGER tr1 AFTER INSERT ON t1 FOR EACH ROW
  INSERT INTO t2 VALUES (f_double(NEW.a));
DELIMITER $$;
CREATE FUNCTION f_with_ps() RETURNS INT
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (10)';
  RETURN 10;
END;
$$
CREATE PROCEDURE p1()
BEGIN
  DECLARE nrows INT;
  SET nrows= f_with_ps();
END;
$$
DELIMITER ;$$

CALL p1();
SELECT * FROM t1;
SELECT * FROM t2;

DROP PROCEDURE p1;
DROP FUNCTION f_with_ps;
DROP TRIGGER tr1;
DROP FUNCTION f_double;
DROP TABLE t1, t2;

--let $MYSQLD_DATADIR= `SELECT @@datadir`
--let $binlog_name= query_get_value(SHOW MASTER STATUS, File, 1)
--replace_regex /^\# at .*// /.*SET (@@|TIMESTAMP).*// /.* end_log_pos .*// /xid=\d*/xid=<xid>/
--exec $MYSQL_BINLOG --force-if-open --base64-output=DECODE-ROWS --verbose --verbose $MYSQLD_DATADIR/$binlog_name


--echo #
--echo # A dynamic statement inside a function with PS fires a trigger
--echo # whose body calls RAND().
--echo #
--echo # The trigger body is not logged: the INSERT which fired it is logged
--echo # and the replica fires the trigger itself. So the rand() call inside
--echo # the trigger belongs to the INSERT, and the INSERT must be preceded
--echo # by a Rand event carrying the seeds. The same INSERT executed at the
--echo # top level and from a plain stored procedure is shown for comparison:
--echo # all three must log the seeds the same way.
--echo #

CREATE TABLE t1 (a INT);
CREATE TABLE t2 (a INT, r DOUBLE);
CREATE TRIGGER tr1 AFTER INSERT ON t1 FOR EACH ROW
  INSERT INTO t2 VALUES (NEW.a, RAND());
DELIMITER $$;
CREATE PROCEDURE p_plain()
BEGIN
  INSERT INTO t1 VALUES (2);
END;
$$
CREATE FUNCTION f_with_ps() RETURNS INT
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (3)';
  RETURN 3;
END;
$$
CREATE PROCEDURE p_ps()
BEGIN
  DECLARE nrows INT;
  SET nrows= f_with_ps();
END;
$$
DELIMITER ;$$

--echo # Top level statement
--source include/reset_master.inc
INSERT INTO t1 VALUES (1);
--source include/show_binlog_events.inc

--echo # Inside a plain stored procedure
--source include/reset_master.inc
CALL p_plain();
--source include/show_binlog_events.inc

--echo # A dynamic statement inside a function with PS
--source include/reset_master.inc
CALL p_ps();
--source include/show_binlog_events.inc

SELECT a FROM t2 ORDER BY a;
DROP PROCEDURE p_ps;
DROP FUNCTION f_with_ps;
DROP PROCEDURE p_plain;
DROP TRIGGER tr1;
DROP TABLE t1, t2;
