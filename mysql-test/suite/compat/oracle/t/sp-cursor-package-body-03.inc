--echo #
--echo # PACKAGE BODY initialization section:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo #

DELIMITER /;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN INT;
END;
/
CREATE PACKAGE BODY pkg AS
  CURSOR cur IS SELECT 1 AS c FROM DUAL;
  vc INT;
  FUNCTION f1 RETURN INT AS
  BEGIN
    RETURN vc;
  END;
BEGIN
  OPEN cur;
  FETCH cur INTO vc;
  CLOSE cur;
END;
/
DELIMITER ;/
