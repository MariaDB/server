--echo #
--echo # REF CURSOR with RETURN, with FETCH INTO record_variable
--echo # VARCHAR to INT to VARCHAR conversion
--echo #


--echo # Using explicit ROW type

DELIMITER $$;
CREATE PROCEDURE p1 AS
  TYPE rec0_t IS RECORD (a INT, b INT);
  TYPE cur0_t IS REF CURSOR RETURN rec0_t;
  c0 cur0_t;
  v0a INT;
  v0b VARCHAR(10);
BEGIN
  OPEN c0 FOR SELECT '1', '2 ' FROM DUAL;
  FETCH c0 INTO v0a, v0b;
  SELECT LENGTH(v0a) || ' ' || LENGTH(v0b) || ' <'|| v0a || '><'|| v0b || '> ' AS line;
  CLOSE c0;
END;
$$
DELIMITER ;$$
CALL p1;

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

DROP PROCEDURE p1;


--echo # Using table%ROWTYPE

CREATE TABLE table_sample (a INT, b INT);
DELIMITER $$;
CREATE PROCEDURE p1 AS
  TYPE cur0_t IS REF CURSOR RETURN table_sample%ROWTYPE;
  c0 cur0_t;
  v0a INT;
  v0b VARCHAR(10);
BEGIN
  OPEN c0 FOR SELECT '1', '2 ' FROM DUAL;
  FETCH c0 INTO v0a, v0b;
  SELECT LENGTH(v0a) || ' ' || LENGTH(v0b) || ' <'|| v0a || '><'|| v0b || '> ' AS line;
  CLOSE c0;
END;
$$
DELIMITER ;$$
CALL p1;

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

DROP PROCEDURE p1;
DROP TABLE table_sample;


--echo # Using cursor%ROWTYPE

CREATE TABLE table_sample (a INT, b INT);
DELIMITER $$;
CREATE PROCEDURE p1 AS
  CURSOR cursor_sample IS SELECT a, b FROM table_sample;
  TYPE cur0_t IS REF CURSOR RETURN cursor_sample%ROWTYPE;
  c0 cur0_t;
  v0a INT;
  v0b VARCHAR(10);
BEGIN
  OPEN c0 FOR SELECT '1', '2 ' FROM DUAL;
  FETCH c0 INTO v0a, v0b;
  SELECT LENGTH(v0a) || ' ' || LENGTH(v0b) || ' <'|| v0a || '><'|| v0b || '> ' AS line;
  CLOSE c0;
END;
$$
DELIMITER ;$$
CALL p1;

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

DROP PROCEDURE p1;
DROP TABLE table_sample;
