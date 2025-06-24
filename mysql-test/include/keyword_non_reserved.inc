eval CREATE TABLE $keyword (a INT);
eval DROP TABLE $keyword;

eval CREATE TABLE t1 ($keyword int);
eval SELECT $keyword AS $keyword FROM t1 AS $keyword;
eval DROP TABLE t1;


eval CREATE TABLE $keyword ($keyword INT);
eval CREATE TRIGGER $keyword AFTER INSERT ON $keyword FOR EACH ROW BEGIN END;
eval DROP TRIGGER $keyword;
eval DROP TABLE $keyword;

eval PREPARE $keyword FROM 'select 1';
eval EXECUTE $keyword;
eval DEALLOCATE PREPARE $keyword;
