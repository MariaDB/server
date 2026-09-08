--echo #
--echo # A function without PS called differently:
--echo # - from a procedure through a function with PS
--echo # - from a procedure directly
--echo #

CREATE TABLE t1 (val VARCHAR(30), caller VARCHAR(128));

DELIMITER $$;
CREATE FUNCTION f_without_ps(ofs INT, caller VARCHAR(128)) RETURNS INT
BEGIN
  DECLARE caller2 VARCHAR(128) DEFAULT CONCAT(caller,'/f_without_ps');
  INSERT INTO t1 (val, caller) VALUES (ofs+1, caller2);
  INSERT INTO t1 (val, caller) VALUES (ofs+2, caller2);
  RETURN 1;
END
$$
CREATE FUNCTION f_with_ps(ofs INT, caller VARCHAR(128)) RETURNS INT
BEGIN
  DECLARE query TEXT DEFAULT 'INSERT INTO t1 (val,caller) VALUES (?,?)';
  DECLARE caller2 VARCHAR(128) DEFAULT CONCAT(caller,'/f_with_ps');
  DECLARE a INT DEFAULT f_without_ps(ofs, caller2);
  EXECUTE IMMEDIATE query USING ofs+3, caller2;
  EXECUTE IMMEDIATE query USING ofs+4, caller2;
  RETURN 1;
END
$$
CREATE PROCEDURE p1()
BEGIN
  DECLARE query TEXT DEFAULT 'INSERT INTO t1 (val,caller) VALUES (?,?)';
  DECLARE v1 INT DEFAULT f_with_ps(100, 'p1');
  DECLARE v2 INT DEFAULT f_without_ps(200, 'p1');
  EXECUTE IMMEDIATE query USING 301, 'p1';
  EXECUTE IMMEDIATE query USING 302, 'p1';
END $$
DELIMITER ;$$
