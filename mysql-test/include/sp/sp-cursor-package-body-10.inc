--echo #
--echo # PROCEDURE
--echo #   FOR rec IN package_body_cursor_with_package_body_variables_in_select_list
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
  DECLARE v0 INT DEFAULT 1;
  DECLARE v1 VARCHAR(10) DEFAULT 'v1';
  DECLARE c0 CURSOR FOR SELECT v0, v1 FROM DUAL;
  PROCEDURE p1()
  BEGIN
    FOR i IN 1..2
    DO
      FOR r0 IN c0
      DO
        SELECT r0.v0, r0.v1;
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
CREATE PACKAGE pkg AS
  PROCEDURE p1;
END;
/
CREATE PACKAGE BODY pkg AS
  v0 INT := 1;
  v1 VARCHAR(10) := 'v1';
  CURSOR c0 IS SELECT v0, v1 FROM DUAL;
  PROCEDURE p1 AS
  BEGIN
    FOR i IN 1..2
    LOOP
      FOR r0 IN c0
      LOOP
        SELECT r0.v0, r0.v1;
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
