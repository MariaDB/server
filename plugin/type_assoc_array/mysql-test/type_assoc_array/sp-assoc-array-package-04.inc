SET sql_mode=ORACLE;

--echo #
--echo # MDEV-39115 `FUNCTION does not exist` error on package body assoc array variable
--echo #

DELIMITER $$;
CREATE PACKAGE pkg1 AS
  PROCEDURE p1;
END;
$$
CREATE OR REPLACE PACKAGE BODY pkg1 AS
  TYPE assoc_t IS TABLE OF INTEGER INDEX BY INTEGER;
  assoc0 assoc_t;
  PROCEDURE p1 AS
  BEGIN
    assoc0(0):= 10;
    SELECT assoc0.exists(0);
  END;
END;
$$
DELIMITER ;$$
if (`SELECT @code`)
{
  EXECUTE IMMEDIATE 'SHOW PROCEDURE CODE pkg.p1';
}
CALL pkg1.p1;
DROP PACKAGE pkg1;
