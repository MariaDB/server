--echo #
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo #

DELIMITER /;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS INT;
END;
/
CREATE PACKAGE BODY pkg
  DECLARE vc INT DEFAULT 0;
  DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
  FUNCTION f1() RETURNS INT
  BEGIN
    OPEN cur;
    FETCH cur INTO vc;
    CLOSE cur;
    RETURN vc;
  END;
END;
/
DELIMITER ;/
