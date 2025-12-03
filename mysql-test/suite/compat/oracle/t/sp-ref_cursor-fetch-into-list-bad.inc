# 2 > 3
CREATE TABLE t1 (a INT, b VARCHAR(10));
INSERT INTO t1 VALUES (1,'b1');
DELIMITER $$;
eval
CREATE PROCEDURE p1 IS
  $decl_cursor_sample1
  $decl_var_sample1
  TYPE cur_t IS REF CURSOR RETURN $return;
  c0 cur_t;
  v0a INT;
  v0b VARCHAR(10);
  v0c VARCHAR(10);
BEGIN
  OPEN c0 FOR SELECT * FROM t1;
  FETCH c0 INTO v0a, v0b, v0c;
END;
$$
DELIMITER ;$$

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
CALL p1;

DROP PROCEDURE p1;
DROP TABLE t1;


# 2 > 1
CREATE TABLE t1 (a INT, b VARCHAR(10));
INSERT INTO t1 VALUES (1,'b1');
DELIMITER $$;
eval
CREATE PROCEDURE p1 IS
  $decl_cursor_sample1
  $decl_var_sample1
  TYPE cur_t IS REF CURSOR RETURN $return;
  c0 cur_t;
  v0a INT;
BEGIN
  OPEN c0 FOR SELECT * FROM t1;
  FETCH c0 INTO v0a;
END;
$$
DELIMITER ;$$

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
CALL p1;

DROP PROCEDURE p1;
DROP TABLE t1;


# 1 > 2
CREATE TABLE t1 (a INT);
INSERT INTO t1 VALUES (1);
DELIMITER $$;
eval
CREATE PROCEDURE p1 IS
  $decl_cursor_sample1
  $decl_var_sample1
  TYPE cur_t IS REF CURSOR RETURN $return;
  c0 cur_t;
  v0a INT;
  v0b INT;
BEGIN
  OPEN c0 FOR SELECT * FROM t1;
  FETCH c0 INTO v0a, v0b;
END;
$$
DELIMITER ;$$

if (`SELECT @debug_ref_cursor`)
{
  SHOW PROCEDURE CODE p1;
}

--error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION
CALL p1;

DROP PROCEDURE p1;
DROP TABLE t1;


let $decl_cursor_sample1=/*not set*/;
let $decl_var_sample1=/*not set*/;
let $return=/*not set*/;
