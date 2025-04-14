--echo #
--echo # PROCEDURE:
--echo #   FOR rec IN package_body_cursor
--echo #

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
