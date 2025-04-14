--echo #
--echo # PACKAGE BODY initialization section:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo #

--source sp-cursor-package-body-init.inc

if ($oracle == 0)
{

DELIMITER /;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS INT;
END;
/
CREATE PACKAGE BODY pkg
  DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
  DECLARE vc INT;
  FUNCTION f1() RETURNS INT
  BEGIN
    RETURN vc;
  END;
  -- initialization
  OPEN cur;
  FETCH cur INTO vc;
  CLOSE cur;
END;
/
DELIMITER ;/

}


if ($oracle > 0)
{

DELIMITER /;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN INT;
END;
/
CREATE PACKAGE BODY pkg AS
  CURSOR cur IS SELECT 1 AS c FROM DUAL;
  vc INT;
  FUNCTION f1 RETURN INT AS
  BEGIN
    RETURN vc;
  END;
BEGIN
  OPEN cur;
  FETCH cur INTO vc;
  CLOSE cur;
END;
/
DELIMITER ;/

}

SELECT pkg.f1() FROM DUAL;
if ($code > 0)
{
SHOW PACKAGE BODY CODE pkg;
SHOW FUNCTION CODE pkg.f1;
}
DROP PACKAGE pkg;
