--echo #
--echo # PROCEDURE:
--echo #   FOR rec IN package_body_cursor
--echo #

--source sp-cursor-package-body-init.inc

if ($oracle == 0)
{

DELIMITER /;
CREATE PACKAGE pkg
  PROCEDURE p1();
END;
/
CREATE PACKAGE BODY pkg
  DECLARE c0 CURSOR FOR SELECT 10 AS c0, 'c10' AS c1 FROM DUAL UNION
                        SELECT 11 AS c0, 'c11' AS c1 FROM DUAL;
  PROCEDURE p1()
  BEGIN
    FOR r0 IN c0
    DO
      SELECT r0.c0, r0.c1;
    END FOR;
  END;
END;
/
DELIMITER ;/

}


if ($oracle > 0)
{

DELIMITER /;
CREATE PACKAGE pkg IS
  PROCEDURE p1;
END;
/
CREATE PACKAGE BODY pkg AS
  CURSOR c0 IS SELECT 10 AS c0, 'c10' AS c1 FROM DUAL UNION
               SELECT 11 AS c0, 'c11' AS c1 FROM DUAL;
  PROCEDURE p1 AS
  BEGIN
    FOR r0 IN c0
    LOOP
      SELECT r0.c0, r0.c1;
    END LOOP;
  END;
END;
/
DELIMITER ;/

}


CALL pkg.p1();
if ($code > 0)
{
SHOW PACKAGE BODY CODE pkg;
SHOW PROCEDURE CODE pkg.p1;
}
DROP PACKAGE pkg;
