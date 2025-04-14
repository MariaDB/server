--echo #
--echo # PACKAGE BODY initialization:
--echo #   OPEN package_body_cursor
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo # PROCEDURE:
--echo #   CLOSE package_body_cursor
--echo #

DELIMITER /;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS INT;
  PROCEDURE p1close();
END;
/
CREATE PACKAGE BODY pkg
  DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
  DECLARE vc0 INT;
  FUNCTION f1() RETURNS INT
  BEGIN
    FETCH cur INTO vc0;
    RETURN vc0;
  END;
  PROCEDURE p1close()
  BEGIN
    CLOSE cur;
  END;
  -- initialization
  OPEN cur;
END;
/
DELIMITER ;/
