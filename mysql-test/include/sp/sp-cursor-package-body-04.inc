--echo #
--echo # PACKAGE BODY initialization:
--echo #   FETCH package_body_cursor INTO package_body_variable
--echo # FUNCTION:
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
  DECLARE vc0 INT;
  DECLARE vc1 INT;
  FUNCTION f1() RETURNS INT
  BEGIN
    OPEN cur;
    FETCH cur INTO vc1;
    RETURN vc0 + vc1;
  END;
  -- initialization
  OPEN cur;
  FETCH cur INTO vc0;
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
  vc0 INT;
  vc1 INT;
  FUNCTION f1 RETURN INT AS
  BEGIN
    OPEN cur;
    FETCH cur INTO vc1;
    RETURN vc0 + vc1;
  END;
BEGIN
  OPEN cur;
  FETCH cur INTO vc0;
  CLOSE cur;
END;
/
DELIMITER ;/

}


--disable_view_protocol
--disable_ps2_protocol
SELECT pkg.f1() FROM DUAL;
--error ER_SP_CURSOR_ALREADY_OPEN
SELECT pkg.f1() FROM DUAL;
--enable_ps2_protocol
--enable_view_protocol

if ($code > 0)
{
SHOW PACKAGE BODY CODE pkg;
SHOW FUNCTION CODE pkg.f1;
}
DROP PACKAGE pkg;
