--echo #
--echo # PACKAGE BODY initialization section:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo #

DELIMITER /;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS INT;
END;
/
CREATE PACKAGE BODY pkg
  DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
  DECLARE vc INT;
  FUNCTION f1() RETURNS INT
  BEGIN
    RETURN vc;
  END;
  -- initialization
  OPEN cur;
  FETCH cur INTO vc;
  CLOSE cur;
END;
/
DELIMITER ;/
