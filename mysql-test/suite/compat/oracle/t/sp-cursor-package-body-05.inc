--echo #
--echo # PACKAGE BODY initialization nested BEGIN..END block:
--echo #   FETCH package_body_cursor INTO local_variable
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo #

DELIMITER /;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN INT;
END;
/
CREATE PACKAGE BODY pkg AS
  CURSOR cur IS SELECT 1 AS c FROM DUAL;
  vc0 INT;
  vc1 INT;
  FUNCTION f1 RETURN INT AS
  BEGIN
    OPEN cur;
    FETCH cur INTO vc1;
    RETURN vc0 + vc1;
  END;
BEGIN
  DECLARE
    vc2 INT;
  BEGIN
    OPEN cur;
    FETCH cur INTO vc2;
    CLOSE cur;
    vc0:= vc2;
  END;
END;
/
DELIMITER ;/
