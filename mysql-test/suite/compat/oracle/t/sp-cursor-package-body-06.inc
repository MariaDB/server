--echo #
--echo # PACKAGE BODY initialization:
--echo #   OPEN package_body_cursor
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo # PROCEDURE:
--echo #   CLOSE package_body_cursor
--echo #

DELIMITER /;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN INT;
  PROCEDURE p1close;
END;
/
CREATE PACKAGE BODY pkg AS
  CURSOR cur IS SELECT 1 AS c FROM DUAL;
  vc0 INT;
  FUNCTION f1 RETURN INT AS
  BEGIN
    FETCH cur INTO vc0;
    RETURN vc0;
  END;
  PROCEDURE p1close AS
  BEGIN
    CLOSE cur;
  END;
BEGIN
  OPEN cur;
END;
/
DELIMITER ;/
