DELIMITER $$;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN INT;
END;
$$
CREATE PACKAGE BODY pkg AS
  vc INT := 0;
  FUNCTION f1 RETURN INT AS
    CURSOR cur IS SELECT 1 AS c FROM DUAL;
  BEGIN
    OPEN cur;
    FETCH cur INTO vc; -- SHOW CODE should display vc with a "PACKAGE_BODY" prefix
    CLOSE cur;
    RETURN vc;
  END;
BEGIN
  DECLARE
    CURSOR cur IS SELECT 1 AS c FROM DUAL;
  BEGIN
    OPEN cur;
    FETCH cur INTO vc; -- SHOW CODE should display vc without a prefix
    CLOSE cur;
  END;
END;
$$
DELIMITER ;$$
