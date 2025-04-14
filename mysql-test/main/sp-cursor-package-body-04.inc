--echo #
--echo # PACKAGE BODY initialization:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo #

DELIMITER /;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS INT;
END;
/
CREATE PACKAGE BODY pkg
  DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
  DECLARE vc0 INT;
  DECLARE vc1 INT;
  FUNCTION f1() RETURNS INT
  BEGIN
    OPEN cur;
    FETCH cur INTO vc1;
    RETURN vc0 + vc1;
  END;
  -- initialization
  OPEN cur;
  FETCH cur INTO vc0;
  CLOSE cur;
END;
/
DELIMITER ;/
