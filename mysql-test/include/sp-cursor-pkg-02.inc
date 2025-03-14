# Mixing a package body variable and a local variable in the same FETCH.
DELIMITER $$;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS TEXT;
END;
$$
CREATE PACKAGE BODY pkg
  DECLARE vc1 INT DEFAULT 0;
  FUNCTION f1() RETURNS TEXT
  BEGIN
    DECLARE vc2 INT DEFAULT 0;
    DECLARE cur CURSOR FOR SELECT 1 AS c1, 2 AS c2 FROM DUAL;
    OPEN cur;
    FETCH cur INTO vc1, vc2;
    CLOSE cur;
    RETURN CONCAT(vc1, ' ', vc2);
  END;
END;
$$
DELIMITER ;$$
