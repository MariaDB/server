--echo #
--echo # PROCEDURE
--echo #   FOR rec IN package_body_cursor_with_package_body_variables_in_select_list
--echo #


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
