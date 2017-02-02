--let $query= CREATE PROCEDURE p1() AS var $type; rec ROW(var $type); BEGIN CREATE TABLE t1 AS SELECT var,rec.var FROM DUAL;END
--eval $query
CALL p1();
SHOW CREATE TABLE t1;
DROP TABLE t1;
DROP PROCEDURE p1;
