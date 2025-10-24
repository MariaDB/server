--echo #
--echo # REF CURSOR with RETURN, with FETCH INTO record_variable
--echo # VARCHAR to TIMESTAMP to VARCHAR conversion
--echo #

CREATE TABLE t1 (a INT, b VARCHAR(30));
INSERT INTO t1 VALUES (1,'2001-01-01 ');
INSERT INTO t1 VALUES (1,'2001-01-01 10:20:30.123 ');
INSERT INTO t1 VALUES (1,'2001-01-01 10:20:30.123456 ');

DELIMITER $$;
CREATE PROCEDURE p1 IS
  TYPE rec0_t IS RECORD (a INT, b TIMESTAMP(6));
  TYPE cur0_t IS REF CURSOR RETURN rec0_t;
  c0 cur0_t;
  v0a t1.a%TYPE;
  v0b t1.b%TYPE;
BEGIN
  OPEN c0 FOR SELECT * FROM t1;
  LOOP
    FETCH c0 INTO v0a, v0b;
    EXIT WHEN c0%NOTFOUND;
    SELECT LENGTH(v0a) || ' ' || LENGTH(v0b) || ' <'|| v0a || '><'|| v0b || '>' AS line;
  END LOOP;
  CLOSE c0;
END;
$$
DELIMITER  ;$$

CALL p1;

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

DROP PROCEDURE p1;
DROP TABLE t1;
