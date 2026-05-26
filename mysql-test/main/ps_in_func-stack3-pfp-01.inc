--let $part1= $(regexp_substr($param,'^[_a-z]*(?=[|])'))
--let $part2= $(regexp_substr($param,'(?<=[|])[_a-z]*(?=[|])'))
--let $part3= $(regexp_substr($param,'(?<=[|])[_a-z]*\$'))

--echo
--echo #
--echo # [$part1][$part2][$part3]
--echo #
--echo


CREATE TABLE t1 (a VARCHAR(30));

############################ f3

DELIMITER $$;
if ($part3 == 'none')
{
CREATE PROCEDURE p3()
BEGIN
  BEGIN END;
END;
$$
}
if ($part3 == 'immediate')
{
CREATE PROCEDURE p3()
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (''p3-immediate'')';
END;
$$
}
if ($part3 == 'direct')
{
CREATE PROCEDURE p3()
BEGIN
  INSERT INTO t1 VALUES ('f3-direct');
END;
$$
}
if ($part3 == 'direct_immediate')
{
CREATE PROCEDURE p3()
BEGIN
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (''p3-immediate'')';
  INSERT INTO t1 VALUES ('p3-direct');
END;
$$
}
DELIMITER ;$$

############################ f2

DELIMITER $$;

if ($part2 == 'none')
{
CREATE FUNCTION f2() RETURNS INT
BEGIN
  CALL p3;
  RETURN 0;
END;
$$
}
if ($part2 == 'immediate')
{
CREATE FUNCTION f2() RETURNS INT
BEGIN
  CALL p3();
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (''f2-immediate'')';
  RETURN 0;
END;
$$
}
if ($part2 == 'direct')
{
CREATE FUNCTION f2() RETURNS INT
BEGIN
  CALL p3();
  INSERT INTO t1 VALUES ('f2-direct');
  RETURN 0;
END;
$$
}
if ($part2 == 'direct_immediate')
{
CREATE FUNCTION f2() RETURNS INT
BEGIN
  CALL p3();
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (''f2-immediate'')';
  INSERT INTO t1 VALUES ('f2-direct');
  RETURN 0;
END;
$$
}
DELIMITER ;$$

############################ f1

DELIMITER $$;

if ($part1 == 'none')
{
CREATE PROCEDURE p1()
BEGIN
  DECLARE v2 VARCHAR(10) DEFAULT f2();
END;
$$
}
if ($part1 == 'immediate')
{
CREATE PROCEDURE p1()
BEGIN
  DECLARE v2 VARCHAR(10) DEFAULT f2();
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (''p1-immediate'')';
END;
$$
}
if ($part1 == 'direct')
{
CREATE PROCEDURE p1()
BEGIN
  DECLARE v2 VARCHAR(10) DEFAULT f2();
  INSERT INTO t1 VALUES ('p1-direct');
END;
$$
}
if ($part1 == 'direct_immediate')
{
CREATE PROCEDURE p1()
BEGIN
  DECLARE v2 VARCHAR(10) DEFAULT f2();
  EXECUTE IMMEDIATE 'INSERT INTO t1 VALUES (''p1-immediate'')';
  INSERT INTO t1 VALUES ('p1-direct');
END;
$$
}
DELIMITER ;$$
##########################
CALL p1;
SELECT * FROM t1 ORDER BY a DESC;
DROP PROCEDURE p1;
DROP FUNCTION f2;
DROP PROCEDURE p3;
DROP TABLE t1;

--let $param= ''
