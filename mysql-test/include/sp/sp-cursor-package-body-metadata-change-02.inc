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
  DECLARE pv0 INT DEFAULT 10;
  DECLARE cur CURSOR FOR SELECT a, pv0 FROM t1;
  PROCEDURE p1()
  BEGIN
    DECLARE vc0 TEXT DEFAULT '';
    DECLARE vc1 TEXT DEFAULT '';
    OPEN cur;
    FETCH cur INTO vc0, vc1;
    CLOSE cur;
    SELECT vc0, vc1;
  END;
END;
/
DELIMITER ;/
}


if ($oracle > 0)
{
DELIMITER /;
CREATE PACKAGE pkg AS
  PROCEDURE p1;
END;
/
CREATE PACKAGE BODY pkg AS
  pv0 INT := 10;
  CURSOR cur IS SELECT a, pv0 FROM t1;
  PROCEDURE p1 AS
    vc0 TEXT := '';
    vc1 TEXT := '';
  BEGIN
    OPEN cur;
    FETCH cur INTO vc0, vc1;
    CLOSE cur;
    SELECT vc0, vc1;
  END;
END;
/
DELIMITER ;/
}

CREATE TABLE t1 (a INT);
INSERT INTO t1 VALUES (10);
CALL pkg.p1();
CALL pkg.p1();
ALTER TABLE t1 MODIFY a TEXT;
UPDATE t1 SET a=CONCAT('a',a);
CALL pkg.p1();
CALL pkg.p1();
DROP TABLE t1;
DROP PACKAGE pkg;
