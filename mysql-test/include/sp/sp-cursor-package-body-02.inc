--echo #
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO local_variable
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
  FUNCTION f1 RETURN INT AS
    vc INT := 0;
  BEGIN
    OPEN cur;
    FETCH cur INTO vc;
    CLOSE cur;
    RETURN vc;
  END;
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
