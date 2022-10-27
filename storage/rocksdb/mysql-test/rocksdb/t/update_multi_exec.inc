CREATE TABLE t1 (a INT, b CHAR(8), pk INT AUTO_INCREMENT
PRIMARY KEY) ENGINE=rocksdb;
INSERT INTO t1 (a,b) VALUES
(1,'a'),(2,'b'),(3,'c'),(4,'d'),(5,'e'),(10000,'foobar');
INSERT INTO t1 (a,b) SELECT a, b FROM t1;

CREATE TABLE t2 (c CHAR(8), d INT, pk INT AUTO_INCREMENT
PRIMARY KEY) ENGINE=rocksdb;
INSERT INTO t2 (c,d) SELECT b, a FROM t1;

UPDATE t1 STRAIGHT_JOIN t2 SET t1.a = t2.d+100, t2.c = 'multi'
WHERE c < b AND a + d != 1;
--sorted_result
SELECT a,b FROM t1;
--sorted_result
SELECT c,d FROM t2;

UPDATE t2 STRAIGHT_JOIN t1 SET t2.d = DEFAULT
WHERE c = 'foobar' and b = c;
--sorted_result
SELECT a,b FROM t1;
--sorted_result
SELECT c,d FROM t2;

# Cleanup
DROP TABLE t1, t2;

