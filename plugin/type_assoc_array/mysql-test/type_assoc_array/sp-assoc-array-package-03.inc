--echo #
--echo # MDEV-37430 sql_mode=ORACLE: TYPE definitions in PACKAGE BODY
--echo # TYPEs in PACKAGE BODY, variables in PACKAGE BODY
--echo #

DELIMITER $$;
CREATE PACKAGE pkg1 AS
  PROCEDURE p1;
END;
$$
CREATE PACKAGE BODY pkg1 AS
  TYPE record_t IS RECORD (a INT, b VARCHAR(32));
  TYPE assoc0_t IS TABLE OF record_t INDEX BY INT;
  TYPE assoc1_t IS TABLE OF VARCHAR(32) INDEX BY INT;
  a0 assoc0_t;
  a1 assoc1_t;
  PROCEDURE p1 AS
  BEGIN

    a0(10):= record_t(10,'test');
    SELECT a0(10).a AS a, a0(10).b AS b;
    CREATE TABLE t1 AS SELECT a0(10).a AS a, a0(10).b AS b;
    SELECT * FROM t1;
    SHOW CREATE TABLE t1;
    SELECT * FROM t1;
    DROP TABLE t1;

    a1(10):= 'test';
    SELECT a1(10) AS a1;
    CREATE TABLE t1 AS SELECT a1(10) AS a;
    SELECT * FROM t1;
    SHOW CREATE TABLE t1;
    SELECT * FROM t1;
    DROP TABLE t1;
  END;
END;
$$
DELIMITER ;$$

CALL pkg1.p1;

DROP PACKAGE pkg1;
