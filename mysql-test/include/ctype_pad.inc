--echo #
--echo # Start of ctype_pad.inc
--echo #

--echo #
--echo # Unique indexes
--echo #

eval CREATE TABLE t1 (a VARCHAR(10) PRIMARY KEY) COLLATE $coll;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('abc'),('abc  '),(' a'),(' a '),('a   ');
SELECT HEX(a), a FROM t1 ORDER BY a;
SELECT HEX(a), a FROM t1 IGNORE INDEX(PRIMARY) ORDER BY a;
SELECT HEX(a), a FROM t1 IGNORE INDEX(PRIMARY) ORDER BY a DESC;

--echo #
--echo # UNION
--echo #

eval CREATE TABLE t2 (a VARCHAR(10)) COLLATE $coll;
INSERT INTO t2 VALUES ('abc '),('abc  '),(' a'),('a ');
SELECT HEX(a),a FROM (SELECT * FROM t1 UNION SELECT * FROM t2 ORDER BY a) td;
DROP TABLE t1;
DROP TABLE t2;

--echo #
--echo # DISTINCT, COUNT, MAX
--echo #

eval CREATE TABLE t1 (a VARCHAR(10)) COLLATE $coll;
INSERT INTO t1 VALUES ('a'),('a  '),(' a'),(' a '),('a   ');
SELECT HEX(a), a FROM (SELECT DISTINCT a FROM t1 ORDER BY a) td;
SELECT COUNT(DISTINCT a) FROM t1 ORDER BY a;
SELECT HEX(MAX(a)), MAX(a) FROM t1;

--echo #
--echo # GROUP BY
--echo #

eval CREATE TABLE t2 (a VARCHAR(10), b int, c varchar(10)) COLLATE $coll;
INSERT t2 values('ab', 12, 'cd'), ('ab', 2, 'ed'), ('aa', 20, 'er'), ('aa  ', 0, 'er ');
SELECT HEX(a), cnt FROM (SELECT a, COUNT(a) AS cnt FROM t2 GROUP BY a ORDER BY a) AS td;
DROP TABLE t2;

--echo #
--echo # Weights
--echo #

SELECT HEX(WEIGHT_STRING(a AS CHAR(10))) FROM t1;
DROP TABLE t1;

--echo #
--echo # IF, CASE, LEAST
--echo #

eval SELECT IF('abc' COLLATE $coll = 'abc ', 'pad', 'nopad');
eval SELECT CASE 'abc' COLLATE $coll WHEN 'abc ' THEN 'pad' ELSE 'nopad' END;
eval SELECT CASE WHEN 'abc' COLLATE $coll = 'abc ' THEN 'pad' ELSE 'nopad' END;
eval SELECT HEX(LEAST('abc ' COLLATE $coll, 'abc  '));
eval SELECT HEX(GREATEST('abc ' COLLATE $coll, 'abc  '));

--echo #
--echo # Collation mix
--echo #

eval CREATE TABLE t1 (a VARCHAR(10)) COLLATE $coll_pad;
INSERT INTO t1 VALUES ('a'),('a ');
SELECT COUNT(*) FROM t1 WHERE a='a';
eval SELECT COUNT(*) FROM t1 WHERE a='a' COLLATE $coll_pad;
eval SELECT COUNT(*) FROM t1 WHERE a='a' COLLATE $coll;
eval ALTER TABLE t1 MODIFY a VARCHAR(10) COLLATE $coll;
SELECT COUNT(*) FROM t1 WHERE a='a';
eval SELECT COUNT(*) FROM t1 WHERE a='a' COLLATE $coll_pad;
eval SELECT COUNT(*) FROM t1 WHERE a='a' COLLATE $coll;
DROP TABLE t1;

--echo #
--echo # End of ctype_pad.inc
--echo #
