--echo #
--echo # PROCEDURE:
--echo #   FOR rec IN package_body_cursor_with_parameters
--echo #

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
