DELIMITER $$;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS INT;
END;
$$
CREATE PACKAGE BODY pkg
  DECLARE vc INT DEFAULT 0;
  FUNCTION f1() RETURNS INT
  BEGIN
    DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
    OPEN cur;
    FETCH cur INTO vc; -- SHOW CODE should display vc with a "PACKAGE_BODY" prefix
    CLOSE cur;
    RETURN vc;
  END;
  BEGIN
    DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
    OPEN cur;
    FETCH cur INTO vc; -- SHOW CODE should display vc without a prefix
    CLOSE cur;
  END;
END;
$$
DELIMITER ;$$
