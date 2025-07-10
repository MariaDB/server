--source sp-cursor-package-body-init.inc

#
# This test has a package body variable in the package body select list
# and makes sure that after metadata change and re-parsing FETCH still
# returns the values of the same package body variable.
# (not the value of the procedure local variable).
#

if ($oracle == 0)
{
DELIMITER /;
CREATE PACKAGE pkg
  PROCEDURE p1();
END;
/
CREATE PACKAGE BODY pkg
  DECLARE vp0 VARCHAR(10) DEFAULT 'vp0';
  DECLARE c0 CURSOR FOR SELECT a, vp0 FROM t1;
  PROCEDURE p1()
  BEGIN
    DECLARE vc0 ROW TYPE OF c0;
    CREATE TABLE t2 AS SELECT vc0.a AS a, vp0 AS b;
    SHOW CREATE TABLE t2;
    SELECT * FROM t2;
    DROP TABLE t2;
  END;
END;
/
DELIMITER ;/
}


if ($oracle > 0)
{
SET sql_mode=ORACLE;
DELIMITER /;
CREATE PACKAGE pkg AS
  PROCEDURE p1;
END;
/
CREATE PACKAGE BODY pkg AS
  vp0 VARCHAR(10) := 'vp0';
  CURSOR c0 IS SELECT a, vp0 FROM t1;
  PROCEDURE p1 AS
    vc0 c0%ROWTYPE;
  BEGIN
    CREATE TABLE t2 AS SELECT vc0.a AS a, vp0 AS b;
    SHOW CREATE TABLE t2;
    SELECT * FROM t2;
    DROP TABLE t2;
  END;
END;
/
DELIMITER ;/
}

CREATE TABLE t1 (a INT);
INSERT INTO t1 VALUES (10);
CALL pkg.p1();
ALTER TABLE t1 MODIFY a TEXT;
UPDATE t1 SET a=CONCAT('a',a);
CALL pkg.p1();
DROP TABLE t1;
DROP PACKAGE pkg;
