--echo #
--echo # PACKAGE BODY initialization section:
--echo #   OPEN,FETCH,CLOSE local_cursor0, INTO local_variable0
--echo #   OPEN,FETCH,CLOSE local_cursor0, INTO local_variable1
--echo # FUNCTION:
--echo #   OPEN,FETCH,CLOSE package_body_cursor0, INTO package_body_variable0
--echo #   OPEN,FETCH,CLOSE package_body_cursor0, INTO package_body_variable1
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
  DECLARE mv0 TEXT;
  DECLARE mv1 TEXT;
  DECLARE mv2 TEXT;
  DECLARE mc0 CURSOR FOR SELECT 'mc0' AS c FROM DUAL;
  DECLARE mc1 CURSOR FOR SELECT 'mc1' AS c FROM DUAL;
  FUNCTION f1() RETURNS TEXT
  BEGIN
    OPEN mc0;
    FETCH mc0 INTO mv0;
    CLOSE mc0;
    OPEN mc1;
    FETCH mc1 INTO mv1;
    CLOSE mc1;
    RETURN CONCAT(mv0,' ',mv1,' ',mv2);
  END;
  BEGIN -- package body initialization
    DECLARE lv0 TEXT;
    DECLARE lv1 TEXT;
    DECLARE lc0 CURSOR FOR SELECT 'lc0' AS c FROM DUAL;
    DECLARE lc1 CURSOR FOR SELECT 'lc1' AS c FROM DUAL;
    OPEN lc0;
    FETCH lc0 INTO lv0;
    CLOSE lc0;
    OPEN lc1;
    FETCH lc1 INTO lv1;
    CLOSE lc1;
    SET mv2= CONCAT(lv0,' ',lv1);
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
  mv0 TEXT;
  mv1 TEXT;
  mv2 TEXT;
  CURSOR mc0 IS SELECT 'mc0' AS c FROM DUAL;
  CURSOR mc1 IS SELECT 'mc1' AS c FROM DUAL;
  FUNCTION f1 RETURN TEXT AS
  BEGIN
    OPEN mc0;
    FETCH mc0 INTO mv0;
    CLOSE mc0;
    OPEN mc1;
    FETCH mc1 INTO mv1;
    CLOSE mc1;
    RETURN CONCAT(mv0,' ',mv1,' ',mv2);
  END;
BEGIN
  DECLARE
    lv0 TEXT;
    lv1 TEXT;
    CURSOR lc0 IS SELECT 'lc0' AS c FROM DUAL;
    CURSOR lc1 IS SELECT 'lc1' AS c FROM DUAL;
  BEGIN
    OPEN lc0;
    FETCH lc0 INTO lv0;
    CLOSE lc0;
    OPEN lc1;
    FETCH lc1 INTO lv1;
    CLOSE lc1;
    mv2:= CONCAT(lv0,' ',lv1);
  END;
END;
/
DELIMITER ;/


}

SELECT pkg.f1();
if ($code > 0)
{
SHOW PACKAGE BODY CODE pkg;
SHOW FUNCTION CODE pkg.f1;
}
DROP PACKAGE pkg;
