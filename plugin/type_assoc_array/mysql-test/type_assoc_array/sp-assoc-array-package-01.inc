--echo #
--echo # Using a package body variable as a key
--echo #

DELIMITER $$;
CREATE PACKAGE pkg AS
  PROCEDURE p1;
END;
$$
CREATE PACKAGE BODY pkg AS
  v1 INT := 1;
  v2 INT := 2;
  PROCEDURE p1 AS
    TYPE marks_t IS TABLE OF VARCHAR(20) INDEX BY INT;
    marks marks_t:= marks_t(1 => 'x01', 2 => 'x02');
  BEGIN
    SELECT marks(v1);
    marks(v1):= 'x01a';
    SELECT marks(v1);
    SELECT 'x01b' INTO marks(v1);
    SELECT marks(v1);

    SELECT marks(v2);
    marks(v2):= 'x02a';
    SELECT marks(v2);
    SELECT 'x02b' INTO marks(v2);
    SELECT marks(v2);
  END;
END;
$$
DELIMITER ;$$
if (`SELECT @code`)
{
  EXECUTE IMMEDIATE 'SHOW PROCEDURE CODE pkg.p1';
}
CALL pkg.p1;
DROP PACKAGE pkg;
