--echo #
--echo # Using a package body variable as a key: a wrong data type: POINT
--echo #

DELIMITER $$;
CREATE PACKAGE pkg AS
  PROCEDURE p1;
END;
$$
CREATE PACKAGE BODY pkg AS
  v POINT := POINT(1,1);
  PROCEDURE p1 AS
    TYPE marks_t IS TABLE OF VARCHAR(20) INDEX BY INT;
    marks marks_t:= marks_t(1 => 'x43', 2 => 'x99');
  BEGIN
    SELECT marks(v);
    marks(v):= 'x43a';
  END;
END;
$$
DELIMITER ;$$
if (`SELECT @code`)
{
  EXECUTE IMMEDIATE 'SHOW PROCEDURE CODE pkg.p1';
}
--error ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION
CALL pkg.p1;
DROP PACKAGE pkg;
