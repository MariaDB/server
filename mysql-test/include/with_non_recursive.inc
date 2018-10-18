
flush STATUS;
CREATE TABLE t1(a int, b int, c int);
INSERT INTO t1 VALUES(NULL,NULL,NULL),(2,3,4);

WITH qn AS (SELECT a FROM t1) SELECT 1 FROM dual;
WITH qn AS (SELECT a FROM t1), qn2 AS (SELECT b FROM t1) SELECT 1 FROM dual;

--error ER_DUP_QUERY_NAME
WITH qn AS (SELECT a FROM t1), qn AS (SELECT b FROM t1) SELECT 1 FROM qn;

WITH qn AS (SELECT b AS a FROM t1) SELECT qn.a, qn2.a  FROM qn, qn AS qn2;

WITH qn AS (SELECT b AS a FROM t1), qn2 AS (SELECT c FROM t1 WHERE a IS NULL OR a>0)
SELECT qn.a, qn2.c  FROM qn, qn2;

WITH qn AS (SELECT 10*a AS a FROM t1), qn2 AS (SELECT 3*a FROM qn)
SELECT * FROM qn2;

WITH qn AS (SELECT a FROM t1), qn2 AS (SELECT a FROM qn)
SELECT * FROM qn2;

let $query=
WITH qn AS (SELECT b AS a FROM t1), qn2 AS (SELECT a FROM qn WHERE a IS NULL OR a>0)
SELECT qn.a, qn2.a  FROM qn, qn2;

eval $query;
eval EXPLAIN $query;

--error ER_NO_SUCH_TABLE
WITH qn2 AS (SELECT a FROM qn WHERE a IS NULL OR a>0), qn AS (SELECT b AS a FROM t1)
SELECT qn2.a  FROM qn2;

--error ER_NO_SUCH_TABLE
WITH qn1 AS (WITH qn3 AS (SELECT * FROM qn2) SELECT * FROM qn3), qn2 AS (SELECT 1)
SELECT * FROM qn1;

WITH qn0 AS (SELECT 1), qn1 AS (SELECT * FROM qn0), qn2 AS (SELECT 1), qn3 AS (SELECT 1 FROM qn1, qn2) 
SELECT 1 FROM qn3;

explain WITH qn AS (SELECT 1) SELECT 2;
WITH qn AS (SELECT 1) SELECT 2;

--error ER_NO_SUCH_TABLE
WITH qn2 AS (SELECT a FROM qn WHERE a IS NULL OR a>0), qn AS (SELECT b AS a FROM qn2)
SELECT qn.a  FROM qn;

--error ER_NO_SUCH_TABLE
WITH qn AS (SELECT a FROM qn) SELECT qn.a FROM qn;

--error ER_NO_SUCH_TABLE
WITH qn1 AS (SELECT a FROM qn3), qn2 AS (SELECT a FROM qn1),
  qn3 AS (SELECT a FROM t1), qn4 AS (SELECT a FROM qn2)
SELECT a FROM qn4;

WITH qn AS (SELECT * FROM t1) SELECT (SELECT max(a) FROM qn);

SELECT (WITH qn AS (SELECT 10*a AS a FROM t1), qn2 AS (SELECT 3*a AS b FROM qn) 
        SELECT * FROM qn2 LIMIT 1)
FROM t1;

SELECT *
FROM (WITH qn AS (SELECT 10*a AS a FROM t1), qn2 AS (SELECT 3*a AS b FROM qn)
      SELECT * FROM qn2)
AS dt;

WITH qn AS (WITH qn2 AS (SELECT "qn2" AS a FROM t1) SELECT "qn", a FROM qn2)
SELECT * FROM qn;

--echo # outer ref to a table, placed in a QN in a subq (later)
IF (0)
{ SELECT (WITH qn AS (SELECT t2.a*a AS a FROM t1), qn2 AS (SELECT 3*a AS b FROM qn)
          SELECT * FROM qn2 LIMIT 1)
FROM t2 AS t1;

--echo # outer ref to a QN, placed in a QN in a subq
WITH qn AS (SELECT b AS a FROM t1)
SELECT (WITH qn2 AS (SELECT a FROM qn WHERE a IS NULL OR a>0)
        SELECT qn2.a FROM qn2) FROM qn;
}

CREATE VIEW v AS
WITH qn AS (SELECT 10*a AS a FROM t1), qn2 AS (SELECT 3*a AS b FROM qn)
  SELECT * FROM qn2;
SELECT * FROM v;
DROP VIEW v;

CREATE TABLE t2
WITH qn AS (SELECT 10*a AS a FROM t1), qn2 AS (SELECT 3*a AS b FROM qn)
  SELECT * FROM qn2;
SELECT * FROM t2;

INSERT INTO t2
WITH qn AS (SELECT 10*a AS a FROM t1), qn2 AS (SELECT 3*a AS b FROM qn)
  SELECT * FROM qn2;
SELECT * FROM t2;
DROP TABLE t2;

let $query=
WITH qn AS (SELECT * FROM t1 LIMIT 10)
SELECT (SELECT max(a) FROM qn WHERE a=0),
       (SELECT min(b) FROM qn WHERE b=3);
eval explain $query;
eval $query;

CREATE TABLE qn SELECT "base";
SELECT * FROM qn;
WITH qn AS (SELECT "with") SELECT * FROM qn;

--echo # In a non-recursive WITH, the scope of the QN doesn't extend to its
--echo # subquery, so "qn" inside AS() is the base table.
WITH qn AS (SELECT * FROM qn) SELECT * FROM qn;

--echo # View doesn't look out to external QNs
CREATE VIEW v AS SELECT * FROM qn;
SELECT * FROM v;

WITH qn AS (SELECT "with") SELECT * FROM v;
WITH qn AS (SELECT * FROM v) SELECT * FROM qn;

--echo # Even if the base table is temporarily dropped
DROP TABLE qn;
--error ER_VIEW_INVALID
WITH qn AS (SELECT "with") SELECT * FROM v;
--error ER_VIEW_INVALID
WITH qn AS (SELECT * FROM v) SELECT * FROM qn;
CREATE TABLE qn SELECT "base" AS a;

--echo # Neither does SP
CREATE FUNCTION f() RETURNS varchar(10) RETURN (SELECT * FROM qn);
SELECT f();

WITH qn AS (SELECT "with") SELECT f();
WITH qn AS (SELECT f()) SELECT * FROM qn;

--echo # QN shadows tmp table
CREATE TEMPORARY TABLE qn SELECT "tmp" AS a;
SELECT * FROM qn;

WITH qn AS (SELECT "with") SELECT * FROM qn;

DROP FUNCTION f;
DROP VIEW v;

WITH qn AS (SELECT "with") SELECT * FROM (SELECT "dt") AS qn;

WITH qn AS (SELECT "outer" AS a) 
  SELECT (WITH qn AS (SELECT "inner" AS a) SELECT a FROM qn), qn.a FROM qn;

--error ER_PARSE_ERROR
WITH test.qn AS (SELECT "with") SELECT * FROM test.qn;

WITH qn AS (SELECT "with") SELECT * FROM qn;
WITH qn AS (SELECT "with") SELECT * FROM test.qn;
WITH qn AS (SELECT "with" AS a) SELECT a FROM qn;
WITH qn AS (SELECT "with" AS a) SELECT qn.a FROM qn;
WITH qn AS (SELECT "with" AS a) SELECT test.qn.a FROM qn;
WITH qn AS (SELECT "with" AS a) SELECT a FROM test.qn;
WITH qn AS (SELECT "with" AS a) SELECT qn.a FROM test.qn;
WITH qn AS (SELECT "with" AS a) SELECT test.qn.a FROM test.qn;

DROP TEMPORARY TABLE qn;

WITH qn AS (SELECT "with" AS a) SELECT a FROM test.qn;
WITH qn AS (SELECT "with" AS a) SELECT qn.a FROM test.qn;
WITH qn AS (SELECT "with" AS a) SELECT test.qn.a FROM test.qn;

DROP TABLE qn;

--error ER_NO_SUCH_TABLE
WITH qn AS (SELECT "with" AS a) SELECT a FROM test.qn;
--error ER_NO_SUCH_TABLE
WITH qn AS (SELECT "with" AS a) SELECT qn.a FROM test.qn;
--error ER_NO_SUCH_TABLE
WITH qn AS (SELECT "with" AS a) SELECT test.qn.a FROM test.qn;

WITH qn AS (SELECT b AS a FROM t1 UNION SELECT b+5 FROM t1), qn2 AS (SELECT a FROM qn WHERE a IS NULL OR a>0)
  SELECT qn.a FROM qn
UNION SELECT qn2.a FROM qn2 WHERE qn2.a>3;

--error ER_PARSE_ERROR
WITH qn AS (SELECT "with" AS a)
WITH qn2 AS (SELECT "with" AS a)
SELECT a FROM test.qn;

--echo # with comma
--error ER_PARSE_ERROR
WITH qn AS (SELECT "with" AS a),
WITH qn2 AS (SELECT "with" AS a)
SELECT a FROM test.qn;

--echo # ORDER BY removed unless there is LIMIT or single table (check "Using filesort")
explain extended WITH qn AS (SELECT a FROM t1 ORDER BY 1) SELECT a FROM qn;
explain extended WITH qn AS (SELECT a FROM t1 ORDER BY 1) SELECT qn.a FROM qn, t1 AS t2;
explain extended WITH qn AS (SELECT a FROM t1 ORDER BY 1 LIMIT 10) SELECT qn.a FROM qn, t1 AS t2;

--echo # FD detection
WITH qn AS (SELECT a, b FROM t1) SELECT b FROM qn GROUP BY a;
WITH qn AS (SELECT a, b FROM t1 WHERE a=b) SELECT b FROM qn GROUP BY a;
WITH qn AS (SELECT a, sum(b) AS s FROM t1 GROUP BY a) SELECT s FROM qn GROUP BY a;

--echo # CTEs work if used in SET

SET @myvar= (WITH qn AS (SELECT a, sum(b) AS s FROM t1 GROUP BY a)
  SELECT s FROM qn GROUP BY a HAVING s IS NOT NULL);
SELECT @myvar;

--echo # CTE works with semijoin
let $query= WITH cte AS (SELECT * FROM t1 AS t2 LIMIT 1)
  SELECT * FROM t1 WHERE t1.a IN (SELECT a+0 FROM cte);
eval explain $query;
eval $query;

let $query=
WITH cte AS (SELECT * FROM t1 AS t2)
SELECT * FROM t1 WHERE t1.a IN (SELECT a+0 FROM cte);
eval explain $query;
eval $query;

--echo # Column names

--echo # empty list
--error ER_PARSE_ERROR
WITH qn () AS (SELECT 1) SELECT * FROM qn, qn qn1;

--error ER_WITH_COL_WRONG_LIST
WITH qn (foo, bar) AS (SELECT 1) SELECT * FROM qn, qn qn1;
explain WITH qn (foo, bar) AS (SELECT 1, 2 FROM t1 LIMIT 2) SELECT * FROM qn, qn qn1;
WITH qn (foo, bar) AS (SELECT 1, 2 FROM t1 LIMIT 2) SELECT * FROM qn, qn qn1;
WITH qn (foo, bar) AS (SELECT 1 AS col, 2 AS coll FROM t1 LIMIT 2) SELECT * FROM qn, qn qn1;

WITH qn (foo, bar) AS (SELECT 1 AS col, 2 AS coll UNION SELECT a,b FROM t1) SELECT qn1.bar FROM qn qn1;
WITH qn (foo, bar) AS (SELECT a, b FROM t1 LIMIT 2) SELECT qn.bar,foo FROM qn;

CREATE TABLE t3 WITH qn (foo, bar) AS (SELECT a, b FROM t1 LIMIT 2) SELECT bar,foo FROM qn;
DESC t3;
DROP TABLE t3;

--error ER_WITH_COL_WRONG_LIST
WITH qn (foo, bar) AS (SELECT 1 FROM t1) SELECT * FROM qn, qn qn1;
WITH qn (foo, bar) AS (SELECT 1, 2 FROM t1) SELECT * FROM qn, qn qn1;
explain WITH qn (foo, bar) AS (SELECT 1, 2 FROM t1) SELECT * FROM qn, qn qn1;
WITH qn (foo, bar) AS (SELECT 1 AS col, 2 AS coll FROM t1) SELECT * FROM qn, qn qn1;
WITH qn (foo, bar) AS (SELECT a, b FROM t1) SELECT qn1.bar,foo FROM qn qn1;

CREATE TABLE t3 WITH qn (foo, bar) AS (SELECT a, b FROM t1) SELECT bar,foo FROM qn;
DESC t3;
DROP TABLE t3;

--echo # Disambiguates same-name expressions
--error ER_DUP_FIELDNAME
WITH qn AS (SELECT 1,1) SELECT * FROM qn;
WITH qn (foo, bar) AS (SELECT 1,1) SELECT * FROM qn;
--error ER_DUP_FIELDNAME
WITH qn AS (SELECT 1,1 FROM t1) SELECT * FROM qn;
WITH qn (foo, bar) AS (SELECT 1,1 FROM t1) SELECT * FROM qn;
--error ER_DUP_FIELDNAME
WITH qn (foo, foo) AS (SELECT 1,2) SELECT * FROM qn;

CREATE VIEW v1 AS WITH qn (foo, bar) AS (SELECT 1,1) SELECT * FROM qn;
SHOW CREATE VIEW v1;
SHOW fields FROM v1;
SELECT * FROM v1;
DROP VIEW v1;

CREATE VIEW v1 AS WITH qn (foo, bar) AS (SELECT 1,1 FROM t1) SELECT * FROM qn;
SHOW CREATE VIEW v1;
SELECT * FROM v1;
DROP VIEW v1;

--error ER_VIEW_WRONG_LIST
CREATE VIEW v1 (bar) AS SELECT 1, 2 FROM t1;
DROP TABLE t1;

--echo # Prove that a materialized QN is shared among all references:
CREATE TABLE t1(a int);
INSERT INTO t1 VALUES(1),(2),(3),(4);
flush STATUS;
WITH qn AS (SELECT 123 AS col) SELECT * FROM qn;
SHOW STATUS LIKE "handler_write";
flush STATUS;
WITH qn AS (SELECT 123 AS col) SELECT * FROM qn, qn AS qn1;
SHOW STATUS LIKE "handler_write";

CREATE VIEW qn AS SELECT 123 AS col;
flush STATUS;
SELECT * FROM qn, qn AS qn1;
SHOW STATUS LIKE "handler_write";
DROP VIEW qn;
DROP TABLE t1;

CREATE VIEW v AS SELECT (WITH qn AS (SELECT "with") SELECT * FROM qn) AS scal_subq FROM dual;
SHOW CREATE VIEW v;
SELECT * FROM v;
DROP VIEW v;

CREATE VIEW v AS SELECT * FROM (WITH qn AS (SELECT "with") SELECT * FROM qn) AS dt;
SHOW CREATE VIEW v;
SELECT * FROM v;
DROP VIEW v;

CREATE TABLE t1 (a int);

let $query=
WITH qne AS (SELECT a FROM t1),
     qnm AS (SELECT a FROM t1),
     qnea AS (SELECT a FROM t1),
     qnma AS (SELECT a FROM t1)
SELECT qne.a,qnm.a,alias1.a,alias2.a
FROM qne, qnm, qnea AS alias1, qnma AS alias2;

eval explain $query;
DROP TABLE t1;

--echo # Automatic index creation if materialized
CREATE TABLE t1 (a int);
INSERT INTO t1(a) VALUES (1),(2),(3),(4),(5),(6),(7),(8),(9),(0);
ANALYZE TABLE t1;

let $query=
WITH tt AS (SELECT * FROM t1) SELECT /*+ no_merge(tt) */ tt.a
  FROM t1 STRAIGHT_JOIN tt WHERE t1.a=tt.a LIMIT 1;
flush STATUS;

--echo # Should use auto_key0 and ref access.
--replace_column 10 #
eval explain $query;
SHOW STATUS LIKE "handler_write";

flush STATUS;
eval $query;
SHOW STATUS LIKE "handler_write";

--echo # With two references
let $query=
WITH tt AS (SELECT * FROM t1) SELECT /*+ no_merge(tt) no_merge(tt_)*/ tt.a
FROM t1 STRAIGHT_JOIN tt STRAIGHT_JOIN tt AS tt_
WHERE t1.a=tt.a AND tt.a=tt_.a LIMIT 1;
eval $query;

WITH q AS (SELECT * FROM t1)
SELECT /*+ merge(q) no_merge(q1) */ * FROM q, q q1 WHERE q.a=1 AND q1.a=2;
DROP TABLE t1;

--echo # Must not create more than 64 indexes.

--disable_query_log
CREATE TABLE t ( c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int, c8 int, c9 int, c10 int,
 c11 int, c12 int, c13 int, c14 int, c15 int, c16 int, c17 int, c18 int, c19 int, c20 int, 
 c21 int, c22 int, c23 int, c24 int, c25 int, c26 int, c27 int, c28 int, c29 int, c30 int, 
 c31 int, c32 int, c33 int, c34 int, c35 int, c36 int, c37 int, c38 int, c39 int, c40 int, 
 c41 int, c42 int, c43 int, c44 int, c45 int, c46 int, c47 int, c48 int, c49 int, c50 int, 
 c51 int, c52 int, c53 int, c54 int, c55 int, c56 int, c57 int, c58 int, c59 int, c60 int, 
 c61 int, c62 int, c63 int, c64 int, c65 int, c66 int, c67 int, c68 int, c69 int, c70 int, 
 c71 int, c72 int, c73 int, c74 int, c75 int, c76 int, c77 int, c78 int, c79 int, c80 int, 
 c81 int, c82 int, c83 int, c84 int, c85 int, c86 int, c87 int, c88 int, c89 int, c90 int, 
 c91 int, c92 int, c93 int, c94 int, c95 int, c96 int, c97 int, c98 int, c99 int, c100 int);

WITH qn AS (SELECT * FROM t LIMIT 2)
SELECT
(SELECT max(c1) FROM qn WHERE qn.c1=1),
(SELECT max(c2) FROM qn WHERE qn.c2=1),
(SELECT max(c3) FROM qn WHERE qn.c3=1),
(SELECT max(c4) FROM qn WHERE qn.c4=1),
(SELECT max(c5) FROM qn WHERE qn.c5=1),
(SELECT max(c6) FROM qn WHERE qn.c6=1),
(SELECT max(c7) FROM qn WHERE qn.c7=1),
(SELECT max(c8) FROM qn WHERE qn.c8=1),
(SELECT max(c9) FROM qn WHERE qn.c9=1),
(SELECT max(c10) FROM qn WHERE qn.c10=1),
(SELECT max(c11) FROM qn WHERE qn.c11=1),
(SELECT max(c12) FROM qn WHERE qn.c12=1),
(SELECT max(c13) FROM qn WHERE qn.c13=1),
(SELECT max(c14) FROM qn WHERE qn.c14=1),
(SELECT max(c15) FROM qn WHERE qn.c15=1),
(SELECT max(c16) FROM qn WHERE qn.c16=1),
(SELECT max(c17) FROM qn WHERE qn.c17=1),
(SELECT max(c18) FROM qn WHERE qn.c18=1),
(SELECT max(c19) FROM qn WHERE qn.c19=1),
(SELECT max(c20) FROM qn WHERE qn.c20=1),
(SELECT max(c21) FROM qn WHERE qn.c21=1),
(SELECT max(c22) FROM qn WHERE qn.c22=1),
(SELECT max(c23) FROM qn WHERE qn.c23=1),
(SELECT max(c24) FROM qn WHERE qn.c24=1),
(SELECT max(c25) FROM qn WHERE qn.c25=1),
(SELECT max(c26) FROM qn WHERE qn.c26=1),
(SELECT max(c27) FROM qn WHERE qn.c27=1),
(SELECT max(c28) FROM qn WHERE qn.c28=1),
(SELECT max(c29) FROM qn WHERE qn.c29=1),
(SELECT max(c30) FROM qn WHERE qn.c30=1),
(SELECT max(c31) FROM qn WHERE qn.c31=1),
(SELECT max(c32) FROM qn WHERE qn.c32=1),
(SELECT max(c33) FROM qn WHERE qn.c33=1),
(SELECT max(c34) FROM qn WHERE qn.c34=1),
(SELECT max(c35) FROM qn WHERE qn.c35=1),
(SELECT max(c36) FROM qn WHERE qn.c36=1),
(SELECT max(c37) FROM qn WHERE qn.c37=1),
(SELECT max(c38) FROM qn WHERE qn.c38=1),
(SELECT max(c39) FROM qn WHERE qn.c39=1),
(SELECT max(c40) FROM qn WHERE qn.c40=1),
(SELECT max(c41) FROM qn WHERE qn.c41=1),
(SELECT max(c42) FROM qn WHERE qn.c42=1),
(SELECT max(c43) FROM qn WHERE qn.c43=1),
(SELECT max(c44) FROM qn WHERE qn.c44=1),
(SELECT max(c45) FROM qn WHERE qn.c45=1),
(SELECT max(c46) FROM qn WHERE qn.c46=1),
(SELECT max(c47) FROM qn WHERE qn.c47=1),
(SELECT max(c48) FROM qn WHERE qn.c48=1),
(SELECT max(c49) FROM qn WHERE qn.c49=1),
(SELECT max(c50) FROM qn WHERE qn.c50=1),
(SELECT max(c51) FROM qn WHERE qn.c51=1),
(SELECT max(c52) FROM qn WHERE qn.c52=1),
(SELECT max(c53) FROM qn WHERE qn.c53=1),
(SELECT max(c54) FROM qn WHERE qn.c54=1),
(SELECT max(c55) FROM qn WHERE qn.c55=1),
(SELECT max(c56) FROM qn WHERE qn.c56=1),
(SELECT max(c57) FROM qn WHERE qn.c57=1),
(SELECT max(c58) FROM qn WHERE qn.c58=1),
(SELECT max(c59) FROM qn WHERE qn.c59=1),
(SELECT max(c60) FROM qn WHERE qn.c60=1),
(SELECT max(c61) FROM qn WHERE qn.c61=1),
(SELECT max(c62) FROM qn WHERE qn.c62=1),
(SELECT max(c63) FROM qn WHERE qn.c63=1),
(SELECT max(c64) FROM qn WHERE qn.c64=1),
(SELECT max(c65) FROM qn WHERE qn.c65=1),
(SELECT max(c66) FROM qn WHERE qn.c66=1),
(SELECT max(c67) FROM qn WHERE qn.c67=1),
(SELECT max(c68) FROM qn WHERE qn.c68=1),
(SELECT max(c69) FROM qn WHERE qn.c69=1),
(SELECT max(c70) FROM qn WHERE qn.c70=1),
(SELECT max(c71) FROM qn WHERE qn.c71=1),
(SELECT max(c72) FROM qn WHERE qn.c72=1),
(SELECT max(c73) FROM qn WHERE qn.c73=1),
(SELECT max(c74) FROM qn WHERE qn.c74=1),
(SELECT max(c75) FROM qn WHERE qn.c75=1),
(SELECT max(c76) FROM qn WHERE qn.c76=1),
(SELECT max(c77) FROM qn WHERE qn.c77=1),
(SELECT max(c78) FROM qn WHERE qn.c78=1),
(SELECT max(c79) FROM qn WHERE qn.c79=1),
(SELECT max(c80) FROM qn WHERE qn.c80=1),
(SELECT max(c81) FROM qn WHERE qn.c81=1),
(SELECT max(c82) FROM qn WHERE qn.c82=1),
(SELECT max(c83) FROM qn WHERE qn.c83=1),
(SELECT max(c84) FROM qn WHERE qn.c84=1),
(SELECT max(c85) FROM qn WHERE qn.c85=1),
(SELECT max(c86) FROM qn WHERE qn.c86=1),
(SELECT max(c87) FROM qn WHERE qn.c87=1),
(SELECT max(c88) FROM qn WHERE qn.c88=1),
(SELECT max(c89) FROM qn WHERE qn.c89=1),
(SELECT max(c90) FROM qn WHERE qn.c90=1),
(SELECT max(c91) FROM qn WHERE qn.c91=1),
(SELECT max(c92) FROM qn WHERE qn.c92=1),
(SELECT max(c93) FROM qn WHERE qn.c93=1),
(SELECT max(c94) FROM qn WHERE qn.c94=1),
(SELECT max(c95) FROM qn WHERE qn.c95=1),
(SELECT max(c96) FROM qn WHERE qn.c96=1),
(SELECT max(c97) FROM qn WHERE qn.c97=1),
(SELECT max(c98) FROM qn WHERE qn.c98=1),
(SELECT max(c99) FROM qn WHERE qn.c99=1),
(SELECT max(c100) FROM qn WHERE qn.c100=1)
FROM dual;
--enable_query_log

DROP TABLE t;

--echo # Choice between two auto_key:

CREATE TABLE t1(a int, b int);
INSERT INTO t1 VALUES (NULL, 6), (NULL, 10);
let $query= WITH t2 AS (SELECT * FROM t1)
  SELECT /*+ no_merge(t2) */ * FROM t2 WHERE (a = a OR b <= 6) AND (a IS NULL);

--echo # Test the covering key; note that MEMORY doesn't use a covering key (always reads the "data file"). But InnoDB does.
eval EXPLAIN $query;
eval $query;
DROP TABLE t1;

CREATE VIEW v1 AS SELECT "with";
WITH v1 AS (SELECT * FROM v1) SELECT * FROM v1;
DROP VIEW v1;

CREATE VIEW v1 AS
WITH qn AS (SELECT 1 AS col) SELECT * FROM qn;
SELECT * FROM v1;
DROP VIEW v1;
CREATE TABLE t1(a int, b int);

CREATE VIEW v1 AS
WITH qn AS (SELECT a FROM t1), qn2 AS (SELECT b FROM t1)
SELECT /*+ merge(qn) no_merge(qn2) */ qn.a,qn2.b FROM qn, qn2;
explain SELECT * FROM v1;
DROP VIEW v1;

--echo # Materializing view doesn't impose materializing query name
CREATE algorithm=temptable VIEW v1 AS
WITH qn AS (SELECT a FROM t1) SELECT qn.a  FROM qn;
explain SELECT * FROM v1;
DROP VIEW v1;
DROP TABLE t1;

--echo # CTE referenced four times, including in subqueries in other CTEs

CREATE TABLE sales_days(day_of_sale DATE, amount INT);
INSERT INTO sales_days VALUES
  ('2015-01-02', 100), ('2015-01-05', 200), ('2015-02-02', 10),  ('2015-02-10', 100), ('2015-03-02', 10),  ('2015-03-18', 1);

WITH
 # FIRST CTE: one ROW per MONTH, WITH amount sold ON ALL days OF MONTH
 sales_by_month(MONTH,total) AS 
  (SELECT MONTH(day_of_sale), sum(amount) FROM sales_days WHERE year(day_of_sale)=2015 GROUP BY MONTH(day_of_sale)),
 # SECOND CTE: best MONTH
 best_month(MONTH, total, award) AS
  (SELECT MONTH, total, "best" FROM sales_by_month WHERE total=(SELECT max(total) FROM sales_by_month)),
 # 3rd CTE: worst MONTH
 worst_month(MONTH, total, award) AS
 (SELECT MONTH, total, "worst" FROM sales_by_month WHERE total=(SELECT min(total) FROM sales_by_month))
 # Now SHOW results:
 SELECT * FROM best_month UNION ALL SELECT * FROM worst_month;

DROP TABLE sales_days;

WITH qn AS (SELECT 1) SELECT * FROM qn, qn qn1;

CREATE TABLE t1(a int);
INSERT INTO t1 VALUES(1),(2);
WITH qn(a) AS (SELECT 1 FROM t1 LIMIT 2) SELECT * FROM qn WHERE qn.a=(SELECT * FROM qn qn1 LIMIT 1) UNION SELECT 2;
DROP TABLE t1;

SHOW STATUS LIKE 'Created_tmp_disk_tables';
