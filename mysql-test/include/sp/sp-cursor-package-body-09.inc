--echo #
--echo # PROCEDURE:
--echo #   FOR rec IN package_body_cursor_with_parameters
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
  DECLARE c0 CURSOR(c0 INT, c1 VARCHAR(10)) FOR SELECT c0, c1 FROM DUAL;
  PROCEDURE p1()
  BEGIN
    FOR i IN 1..2
    DO
      FOR r0 IN c0(1,'c1')
      DO
        SELECT r0.c0, r0.c1;
      END FOR;
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
  CURSOR c0(c0 INT, c1 VARCHAR(10)) IS SELECT c0, c1 FROM DUAL;
  PROCEDURE p1 AS
  BEGIN
    FOR i IN 1..2
    LOOP
      FOR r0 IN c0(1,'c1')
      LOOP
        SELECT r0.c0, r0.c1;
      END LOOP;
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
