DELIMITER $$;
CREATE PROCEDURE p0 AS
BEGIN
  IF @debug_ref_cursor IS TRUE
  THEN
    EXECUTE IMMEDIATE 'SHOW PROCEDURE CODE p1';
  END IF;
  CALL p1;
END;
$$
DELIMITER ;$$

# 1 > 1
CREATE TABLE t1 (a INT);
INSERT INTO t1 VALUES (1);
CREATE TABLE t2 (a INT);
DELIMITER $$;
eval
CREATE PROCEDURE p1 IS
  $decl_cursor_sample1
  $decl_cursor_sample2
  $decl_var_sample1
  TYPE cur_t IS REF CURSOR RETURN $return;
  c0 cur_t;
  v0 $type_v0;
BEGIN
  OPEN c0 FOR SELECT * FROM t1;
  FETCH c0 INTO v0;
  CLOSE c0;
  SELECT v0.a;
END;
$$
DELIMITER ;$$

CALL p0;

DROP PROCEDURE p1;
DROP TABLE t1, t2;


# 2 > 2
CREATE TABLE t1 (a INT, b VARCHAR(10));
INSERT INTO t1 VALUES (1, 'b1');
CREATE TABLE t2 (a INT, b VARCHAR(10));
DELIMITER $$;
eval
CREATE PROCEDURE p1 IS
  $decl_cursor_sample1
  $decl_cursor_sample2
  $decl_var_sample1
  TYPE cur_t IS REF CURSOR RETURN $return;
  c0 cur_t;
  v0 $type_v0;
BEGIN
  OPEN c0 FOR SELECT * FROM t1;
  FETCH c0 INTO v0;
  CLOSE c0;
  SELECT v0.a, v0.b;
END;
$$
DELIMITER ;$$

CALL p0;

DROP PROCEDURE p1;
DROP TABLE t1, t2;

DROP PROCEDURE p0;
