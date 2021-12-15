--echo #
--echo # Start of sp-package-concurrent-dml.inc
--echo #

--source include/count_sessions.inc

let $object_type= `SELECT @object_type`;

SET sql_mode=ORACLE;
DELIMITER $$;
CREATE PACKAGE pkg1 AS
  PROCEDURE p1;
END;
$$
CREATE PACKAGE BODY pkg1 AS
  PROCEDURE p2 AS
  BEGIN
    SELECT 'This is p2' AS msg;
  END;
  PROCEDURE p1 AS
  BEGIN
    SELECT 'This is p1' AS msg;
    DO GET_LOCK('mdev15070',120);
    CALL p2();
    DO RELEASE_LOCK('mdev15070');
  END;
END;
$$
DELIMITER ;$$

connect (con2,localhost,root);
connection con2;
DO GET_LOCK('mdev15070', 120);

connection default;
send CALL pkg1.p1;

connection con2;
let $wait_condition=
  SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST
  WHERE state = "User lock" AND info LIKE "%GET_LOCK%mdev15070%";
--source include/wait_condition.inc


if ($object_type==view)
{
  CREATE VIEW v1 AS SELECT 1 AS c;
  DROP VIEW v1;
}


if ($object_type==package_replace_pkg1)
{
  SET sql_mode=ORACLE;
  DELIMITER $$;
  CREATE OR REPLACE PACKAGE pkg1 AS
    PROCEDURE p1;
  END;
  $$
  DELIMITER ;$$
  DROP PACKAGE pkg1;
}


if ($object_type==package_body_replace_pkg1)
{
  SET sql_mode=ORACLE;
  DELIMITER $$;
  CREATE OR REPLACE PACKAGE BODY pkg1 AS
    PROCEDURE p1 AS
    BEGIN
      SELECT 'This is p1 version 2' AS msg;
    END;
  END;
  $$
  DELIMITER ;$$
  DROP PACKAGE pkg1;
}


if ($object_type==trigger)
{
  CREATE TABLE t1 (a INT);
  CREATE TRIGGER tr1 BEFORE INSERT ON t1 FOR EACH ROW SET NEW.a=1;
  DROP TRIGGER tr1;
  DROP TABLE t1;
}


if ($object_type=='db')
{
  CREATE DATABASE test1;
  CREATE FUNCTION test1.f1() RETURNS INT RETURN 10;
  DROP DATABASE test1;
}


DO RELEASE_LOCK('mdev15070');

disconnect con2;

connection default;
reap;

DROP PACKAGE IF EXISTS pkg1;

--source include/wait_until_count_sessions.inc
