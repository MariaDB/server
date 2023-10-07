--let $query= CREATE PROCEDURE p1() BEGIN DECLARE var $type; DECLARE rec $type ARRAY[1]; CREATE TABLE t1 AS SELECT var,rec[1] FROM DUAL;END
--eval $query
CALL p1();
SELECT column_name, column_type, collation_name FROM INFORMATION_SCHEMA.COLUMNS WHERE table_name='t1';
DROP TABLE t1;
DROP PROCEDURE p1;
