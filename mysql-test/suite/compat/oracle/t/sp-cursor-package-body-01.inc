--echo #
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo #

DELIMITER /;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN INT;
END;
/
CREATE PACKAGE BODY pkg AS
  vc INT := 0;
  CURSOR cur IS SELECT 1 AS c FROM DUAL;
  FUNCTION f1 RETURN INT AS
  BEGIN
    OPEN cur;
    FETCH cur INTO vc;
    CLOSE cur;
    RETURN vc;
  END;
END;
/
DELIMITER ;/
