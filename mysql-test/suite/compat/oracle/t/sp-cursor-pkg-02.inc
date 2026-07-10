# Mixing a package body variable and a local variable in the same FETCH.
DELIMITER $$;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN TEXT;
END;
$$
CREATE PACKAGE BODY pkg AS
  vc1 INT := 0;
  FUNCTION f1 RETURN TEXT AS
    CURSOR cur IS SELECT 1 AS c1, 2 AS c2 FROM DUAL;
    vc2 INT := 0;
  BEGIN
    OPEN cur;
    FETCH cur INTO vc1, vc2;
    CLOSE cur;
    RETURN vc1 || ' ' || vc2;
  END;
END;
$$
DELIMITER ;$$
