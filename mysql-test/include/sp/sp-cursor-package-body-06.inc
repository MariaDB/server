--echo #
--echo # PACKAGE BODY initialization:
--echo #   OPEN package_body_cursor
--echo # FUNCTION:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo # PROCEDURE:
--echo #   CLOSE package_body_cursor
--echo #

--source sp-cursor-package-body-init.inc

if ($oracle == 0)
{

DELIMITER /;
CREATE PACKAGE pkg
  FUNCTION f1() RETURNS INT;
  PROCEDURE p1close();
END;
/
CREATE PACKAGE BODY pkg
  DECLARE cur CURSOR FOR SELECT 1 AS c FROM DUAL;
  DECLARE vc0 INT;
  FUNCTION f1() RETURNS INT
  BEGIN
    FETCH cur INTO vc0;
    RETURN vc0;
  END;
  PROCEDURE p1close()
  BEGIN
    CLOSE cur;
  END;
  -- initialization
  OPEN cur;
END;
/
DELIMITER ;/

}


if ($oracle > 0)
{

DELIMITER /;
CREATE PACKAGE pkg AS
  FUNCTION f1 RETURN INT;
  PROCEDURE p1close;
END;
/
CREATE PACKAGE BODY pkg AS
  CURSOR cur IS SELECT 1 AS c FROM DUAL;
  vc0 INT;
  FUNCTION f1 RETURN INT AS
  BEGIN
    FETCH cur INTO vc0;
    RETURN vc0;
  END;
  PROCEDURE p1close AS
  BEGIN
    CLOSE cur;
  END;
BEGIN
  OPEN cur;
END;
/
DELIMITER ;/

}


--disable_view_protocol
--disable_ps2_protocol
SELECT pkg.f1() FROM DUAL;
--enable_ps2_protocol
CALL pkg.p1close();
--error ER_SP_CURSOR_NOT_OPEN
SELECT pkg.f1() FROM DUAL;
--enable_view_protocol

if ($code > 0)
{
SHOW PACKAGE BODY CODE pkg;
SHOW FUNCTION CODE pkg.f1;
SHOW PROCEDURE CODE pkg.p1close;
}
DROP PACKAGE pkg;
