--echo #
--echo # UPDATE..RETURNING..INTO $TYPE
--echo #

CREATE TABLE t1 (a INT, b INT, KEY(a));

DELIMITER $$;
eval CREATE PROCEDURE p1()
BEGIN
  $PRE
  DECLARE r $TYPE;
  SELECT 'WHERE matches no rows, no index used' AS scenario;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1 WHERE b=999 RETURNING a,b INTO r;
  SELECT r.a, r.b;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1 WHERE b=999 RETURNING a INTO r.a;
  SELECT r.a, r.b;
  SELECT 'WHERE matches no rows, index used' AS scenario;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1 WHERE a=999 RETURNING a,b INTO r;
  SELECT r.a, r.b;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1 WHERE a=999 RETURNING a INTO r.a;
  SELECT r.a, r.b;
  SELECT 'Impossible WHERE' AS scenario;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1 WHERE 1=0 RETURNING a,b INTO r;
  SELECT r.a, r.b;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1 WHERE 1=0 RETURNING a INTO r.a;
  SELECT r.a, r.b;
  SELECT 'LIMIT 0' AS scenario;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1 LIMIT 0 RETURNING a,b INTO r;
  SELECT r.a, r.b;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1 LIMIT 0 RETURNING a INTO r.a;
  SELECT r.a, r.b;
  SELECT 'A row is found but it needs no update (b=b)' AS scenario;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b WHERE a=10 RETURNING a,b INTO r;
  SELECT r.a, r.b;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b WHERE a=10 RETURNING a INTO r.a;
  SELECT r.a, r.b;
  SELECT 'A row is found but it needs no update (b=100)' AS scenario;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=100 WHERE a=10 RETURNING a,b INTO r;
  SELECT r.a, r.b;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=100 WHERE a=10 RETURNING a INTO r.a;
  SELECT r.a, r.b;
  $POST
END;
$$
DELIMITER ;$$

DELIMITER $$;
eval CREATE PROCEDURE p2()
BEGIN
  $PRE
  DECLARE r $TYPE;
  SELECT 'Empty table, no WHERE' AS scenario;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1  RETURNING a,b INTO r;
  SELECT r.a, r.b;
  SET r.a=123, r.b=456;
  UPDATE t1 SET b=b+1  RETURNING a INTO r.a;
  SELECT r.a, r.b;
  $POST
END;
$$
DELIMITER ;$$

--echo # Empty table
CALL p2;
CALL p1;

INSERT INTO t1 VALUES (10,100),(20,200),(30,300);
--echo # Non-empty table
CALL p1;

--echo # Nothing was changed
SELECT * FROM t1;

DROP PROCEDURE p1;
DROP PROCEDURE p2;
DROP TABLE t1;
