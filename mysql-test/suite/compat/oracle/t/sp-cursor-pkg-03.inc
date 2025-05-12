# Fetching into a PACKAGE BODY variable of the ROW type
DELIMITER $$;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN TEXT;
END;
$$
CREATE PACKAGE BODY pkg AS
  vc ROW(p1 INT, p2 INT);
  FUNCTION f1 RETURN TEXT AS
    CURSOR cur IS SELECT 1 AS c1, 2 AS c2 FROM DUAL;
  BEGIN
    OPEN cur;
    FETCH cur INTO vc;
    CLOSE cur;
    RETURN vc.p1 || ' ' || vc.p2;
  END;
END;
$$
DELIMITER ;$$
