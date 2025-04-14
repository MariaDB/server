--echo #
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO local_variable
--echo #

DELIMITER /;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS INT;
END;
/
CREATE PACKAGE BODY pkg
  DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
  FUNCTION f1() RETURNS INT
  BEGIN
    DECLARE vc INT DEFAULT 0;
    OPEN cur;
    FETCH cur INTO vc;
    CLOSE cur;
    RETURN vc;
  END;
END;
/
DELIMITER ;/
