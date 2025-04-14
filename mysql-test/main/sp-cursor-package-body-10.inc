--echo #
--echo # PROCEDURE
--echo #   FOR rec IN package_body_cursor_with_package_body_variables_in_select_list
--echo #


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
