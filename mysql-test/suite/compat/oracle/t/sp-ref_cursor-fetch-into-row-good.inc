# 1 > 1
CREATE TABLE t1 (a INT);
INSERT INTO t1 VALUES (1);
DELIMITER $$;
eval
CREATE PROCEDURE p1 IS
  $decl_cursor_sample1
  $decl_var_sample1
  TYPE cur_t IS REF CURSOR RETURN $return;
  c0 cur_t;
  v0 ROW(a INT);
BEGIN
  OPEN c0 FOR SELECT * FROM t1;
  FETCH c0 INTO v0;
  CLOSE c0;
  SELECT v0.a;
END;
$$
DELIMITER ;$$

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

CALL p1;

DROP PROCEDURE p1;
DROP TABLE t1;


# 2 > 2
CREATE TABLE t1 (a INT, b VARCHAR(10));
INSERT INTO t1 VALUES (1, 'b1');
DELIMITER $$;
eval
CREATE PROCEDURE p1 IS
  $decl_cursor_sample1
  $decl_var_sample1
  TYPE cur_t IS REF CURSOR RETURN $return;
  c0 cur_t;
  v0 ROW(a INT, b VARCHAR(10));
BEGIN
  OPEN c0 FOR SELECT * FROM t1;
  FETCH c0 INTO v0;
  CLOSE c0;
  SELECT v0.a, v0.b;
END;
$$
DELIMITER ;$$

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

CALL p1;

DROP PROCEDURE p1;
DROP TABLE t1;
