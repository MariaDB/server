SET @@max_recursive_iterations = 10000;
flush STATUS;

--error ER_RECURSIVE_WITHOUT_ANCHORS
WITH RECURSIVE qn AS (SELECT * FROM qn2), qn2 AS (SELECT * FROM qn)
SELECT * FROM qn;

--error ER_RECURSIVE_WITHOUT_ANCHORS
WITH RECURSIVE qn AS (SELECT 1 FROM qn)
SELECT count(*) FROM qn;

WITH RECURSIVE qn AS (SELECT 1 UNION ALL SELECT 1 FROM qn UNION ALL SELECT 1)
SELECT count(*) FROM qn;

--error ER_RECURSIVE_WITHOUT_ANCHORS
WITH RECURSIVE qn AS (SELECT 1 FROM qn UNION ALL SELECT 1 FROM qn)
SELECT * FROM qn;

WITH RECURSIVE qn AS (SELECT 1 FROM dual UNION ALL SELECT 1 FROM dual)
SELECT * FROM qn;

WITH RECURSIVE qn AS (SELECT 1 FROM dual UNION SELECT 1 FROM qn)
SELECT * FROM qn;

CREATE TABLE t1(b int);
INSERT INTO t1 VALUES(10),(20),(10);

WITH RECURSIVE qn AS (SELECT max(b) AS a FROM t1 UNION SELECT a FROM qn)
SELECT * FROM qn;

--error ER_NOT_STANDARD_COMPLIANT_RECURSIVE
WITH RECURSIVE qn AS (SELECT b AS a FROM t1 UNION SELECT max(a) FROM qn)
SELECT * FROM qn;

WITH RECURSIVE qn AS (SELECT rank() over (ORDER BY b) AS a FROM t1 UNION SELECT a FROM qn)
SELECT * FROM qn;

--error ER_NOT_STANDARD_COMPLIANT_RECURSIVE
WITH RECURSIVE qn AS (SELECT b AS a FROM t1 UNION SELECT rank() over (ORDER BY a) FROM qn)
SELECT * FROM qn;

DROP TABLE t1;

--error ER_NOT_STANDARD_COMPLIANT_RECURSIVE
WITH RECURSIVE qn AS (SELECT 1 AS a FROM dual UNION ALL SELECT max(a) FROM qn)
SELECT * FROM qn;

WITH RECURSIVE qn AS (SELECT 1 AS a FROM dual GROUP BY a UNION ALL SELECT a+1 FROM qn WHERE a<3)
SELECT * FROM qn;

WITH RECURSIVE qn AS (SELECT 1 AS a FROM dual UNION ALL SELECT a FROM qn GROUP BY a)
SELECT count(*)FROM qn;

--error ER_NOT_STANDARD_COMPLIANT_RECURSIVE
WITH RECURSIVE qn AS (SELECT 1 FROM dual UNION ALL SELECT 1 FROM dual WHERE 1 NOT in(SELECT * FROM qn))
SELECT * FROM qn;

--error ER_NOT_SUPPORTED_YET
WITH RECURSIVE qn AS (SELECT 1 FROM dual UNION ALL SELECT 1 FROM qn ORDER BY (SELECT * FROM qn))
SELECT count(*) FROM qn;

WITH RECURSIVE qn AS (SELECT 1 FROM dual UNION ALL SELECT * FROM (SELECT * FROM qn) AS dt)
SELECT count(*) FROM qn;

--error ER_NOT_SUPPORTED_YET
WITH RECURSIVE qn AS (SELECT 1 AS a FROM dual UNION ALL SELECT 1 FROM qn ORDER BY a)
SELECT count(*) FROM qn;

WITH RECURSIVE qn AS (SELECT 1 AS a FROM dual UNION ALL (SELECT 1 FROM qn ORDER BY a))
SELECT count(*) FROM qn;

--echo # Allowed on non-recursive query block (though pointless)
WITH RECURSIVE qn AS ((SELECT 1 AS a FROM dual ORDER BY a) UNION ALL SELECT a+1 FROM qn WHERE a<3)
SELECT * FROM qn;

--error ER_NOT_SUPPORTED_YET
WITH RECURSIVE qn AS (SELECT 1 AS a FROM dual UNION ALL SELECT 1 FROM qn LIMIT 10)
SELECT count(*) FROM qn;

WITH RECURSIVE qn AS (SELECT 1 AS a FROM dual UNION ALL (SELECT 1 FROM qn LIMIT 10)) 
SELECT count(*) FROM qn;

WITH RECURSIVE qn AS(SELECT 1 UNION ALL SELECT DISTINCT 3 FROM qn)
SELECT count(*) FROM qn;

--error ER_NOT_STANDARD_COMPLIANT_RECURSIVE
WITH RECURSIVE qn AS (SELECT 1 FROM dual UNION ALL SELECT 1 FROM dual WHERE 1 NOT in(SELECT * FROM qn))
SELECT * FROM qn;

--echo # Numbers from 123 to 130:
WITH RECURSIVE qn AS (SELECT 123 AS a UNION ALL SELECT 1+a FROM qn WHERE a<130) SELECT * FROM qn;

--echo # One-level recursive sequence of numbers
WITH RECURSIVE qn AS (SELECT 1 AS n, 2 AS un UNION ALL SELECT 1+n, un*5-6 FROM qn WHERE n<10) SELECT * FROM qn;

--echo # Fibonacci
WITH RECURSIVE qn AS (SELECT 1 AS n, 1 AS un, 1 AS unp1 UNION ALL SELECT 1+n, unp1, un+unp1 FROM qn WHERE n<10) SELECT * FROM qn;

--echo # Validate that cast(a_varchar as char) produces a varchar, not a char.
CREATE TABLE t(c char(3), vc varchar(3), b binary(3), vb varbinary(3));
CREATE TABLE u
SELECT cast(c AS char(4)), cast(vc AS char(4)),
       cast(b AS binary(4)), cast(vb AS binary(4)),
       "abc" AS literal_c, cast("abc" AS char(4)),
       _binary "abc" AS literal_b, cast(_binary "abc" AS binary(4))
FROM t;
SHOW CREATE TABLE u;
DROP TABLE t,u;

--echo # if it used char the 'x' would fall off due to spaces.
WITH RECURSIVE qn AS (SELECT 1 AS n, cast('x' AS char(100)) AS un UNION ALL SELECT 1+n, concat(un,'x') FROM qn WHERE n<10) 
SELECT * FROM qn;

--echo # String now growing at the left
WITH RECURSIVE qn AS (SELECT cast("x" AS char(10)) AS a FROM dual UNION ALL SELECT concat("x",a) FROM qn WHERE length(a)<10) 
SELECT * FROM qn;

--echo # Forgot cast-as-char(10) in anchor => qn.a column has length 1
--echo # => concat() is cast as char(1) => truncation
--echo # => length is always 1 => infinite loop;
let $query=
WITH RECURSIVE qn AS (SELECT "x" AS a FROM dual UNION ALL SELECT concat("x",a) FROM qn WHERE length(a)<10) 
SELECT count(*) FROM qn;
eval $query;

--echo # Overflow integer type INT (max 4G)
WITH RECURSIVE qn AS (SELECT 1 AS a FROM dual UNION ALL SELECT a*2000 FROM qn WHERE a<10000000000000000000) 
SELECT count(*) FROM qn;

--echo # Use Decimal
WITH RECURSIVE qn AS (SELECT cast(1 AS decimal(30,0)) AS a FROM dual UNION ALL SELECT a*2000 FROM qn WHERE a<10000000000000000000) 
SELECT * FROM qn;

WITH RECURSIVE qn AS (SELECT 123 AS a UNION ALL SELECT NULL FROM qn WHERE a IS NOT NULL) 
SELECT * FROM qn;

--echo # The Point becomes a string which is an invalid integer, cast string to int -> result 0:

WITH RECURSIVE qn AS (
SELECT 1 AS a,1
UNION ALL
SELECT a+1,ST_PointFromText('POINT(10 10)') FROM qn WHERE a<2)
SELECT * FROM qn;

--error ER_CANT_CREATE_GEOMETRY_OBJECT
WITH RECURSIVE qn AS (
SELECT 1 AS a,ST_PointFromText('POINT(10 10)')
UNION ALL
SELECT a+1,1 FROM qn WHERE a<2)
SELECT * FROM qn;

--error ER_WRONG_NUMBER_OF_COLUMNS_IN_SELECT
WITH RECURSIVE qn AS (SELECT 1 UNION ALL SELECT 3, 0 FROM qn)
SELECT * FROM qn;

--echo # Mismatch in column name and column count;
--error ER_WITH_COL_WRONG_LIST
WITH RECURSIVE q (b) AS (SELECT 1, 1 UNION ALL SELECT 1, 1 FROM q)
SELECT b FROM q;

--error ER_NOT_STANDARD_COMPLIANT_RECURSIVE
WITH RECURSIVE qn AS ( SELECT 123 AS a UNION ALL SELECT 1+qn.a FROM qn, qn AS qn1 WHERE qn1.a<130)
SELECT * FROM qn;

flush STATUS;
WITH RECURSIVE qn AS (SELECT 123 AS a UNION ALL SELECT 1+a FROM qn WHERE a<125)
SELECT * FROM qn;
SHOW STATUS LIKE "handler_write";
flush STATUS;

WITH RECURSIVE qn AS (SELECT 123 AS a UNION ALL SELECT 1+a FROM qn WHERE a<125)
SELECT * FROM qn, qn AS qn1;
SHOW STATUS LIKE "handler_write";
SHOW STATUS LIKE 'Created_tmp%table%';

--echo # Also works if references are nested inside other query names:
flush STATUS;
WITH RECURSIVE 
  inner_ AS (SELECT 123 AS a UNION ALL SELECT 1+a FROM inner_ WHERE a<125), 
  outer_ AS (SELECT * FROM inner_ LIMIT 10)
SELECT * FROM outer_, outer_ AS outer1;
SHOW STATUS LIKE "handler_write";

flush STATUS;
WITH RECURSIVE 
  inner_ AS ( SELECT 123 AS a UNION ALL SELECT 1+a FROM inner_ WHERE a<125),
  outer_ AS(SELECT inner_.a, inner1.a AS a1 FROM inner_, inner_ AS inner1 LIMIT 10)
SELECT * FROM outer_;
SHOW STATUS LIKE "handler_write";

--echo # Even if the two query names are recursive:
flush STATUS;
WITH RECURSIVE 
  inner_ AS (SELECT 123 AS a UNION ALL SELECT 1+a FROM inner_ WHERE a<125),
  outer_ AS (SELECT a FROM inner_ UNION ALL SELECT a*2 FROM outer_ WHERE a<1000)
SELECT a FROM outer_;
SHOW STATUS LIKE "handler_write";

CREATE TABLE t1(a int);
INSERT INTO t1 VALUES(1),(2);

--error ER_NOT_STANDARD_COMPLIANT_RECURSIVE
WITH RECURSIVE qn AS (SELECT 1 FROM t1 UNION ALL SELECT 1 FROM t1 LEFT JOIN qn ON 1)
SELECT * FROM qn;

--echo # Empty anchor
WITH RECURSIVE qn AS (SELECT a FROM t1 WHERE 0 UNION ALL SELECT a+1 FROM qn)
SELECT * FROM qn;

WITH RECURSIVE qn AS (SELECT a FROM t1 WHERE a>10 UNION ALL SELECT a+1 FROM qn)
SELECT * FROM qn;

--echo # UNION DISTINCT in anchor parts 
INSERT INTO t1 VALUES(1),(2);

SET @c=0, @d=0;
--error ER_NOT_SUPPORTED_YET
WITH RECURSIVE qn AS
(SELECT 1,0 AS col FROM t1 UNION DISTINCT SELECT 1,0 FROM t1
UNION ALL
SELECT 3, 0*(@c:=@c+1) FROM qn WHERE @c<1
UNION ALL
SELECT 3, 0*(@d:=@d+1) FROM qn WHERE @d<1
)
SELECT * FROM qn;

INSERT INTO t1 VALUES(1),(2);

SET @c=0, @d=0;
--error ER_NOT_SUPPORTED_YET
WITH RECURSIVE qn AS
(
SELECT 1,0 AS col FROM t1
UNION DISTINCT
SELECT 3, 0*(@c:=@c+1) FROM qn WHERE @c<1
UNION ALL
SELECT 3, 0*(@d:=@d+1) FROM qn WHERE @d<1
)
SELECT * FROM qn;

--echo # create select
CREATE TABLE t2
WITH RECURSIVE qn AS (SELECT 123 AS a UNION ALL SELECT 1+a FROM qn WHERE a<130)
SELECT * FROM qn;

SELECT * FROM t2;
DROP TABLE t2;

--echo # insert select
DELETE FROM t1;
INSERT INTO t1
WITH RECURSIVE qn AS (SELECT 123 AS a UNION ALL SELECT 1+a FROM qn WHERE a<130)
SELECT * FROM qn;
SELECT * FROM t1;

--echo # Using insertion target inside recursive query
DELETE FROM t1;
INSERT INTO t1 VALUES(1),(2);
INSERT INTO t1
WITH RECURSIVE qn AS (SELECT 123 AS a UNION ALL SELECT 1+qn.a FROM qn, t1 WHERE qn.a<125)
SELECT * FROM qn;
SELECT * FROM t1;

DROP TABLE t1;

--echo # insert into tmp table (a likely use case)

CREATE TEMPORARY TABLE t1(a int);
INSERT INTO t1
WITH RECURSIVE qn AS (SELECT 123 AS a UNION ALL SELECT 1+a FROM qn WHERE a<130)
SELECT * FROM qn;
SELECT * FROM t1;
DROP TABLE t1;

--echo # create view
CREATE VIEW v1 AS
WITH RECURSIVE qn AS (
SELECT 123 AS a UNION ALL SELECT 1+a FROM qn WHERE a<130)
SELECT * FROM qn;
SELECT * FROM v1;
DROP VIEW v1;

let $query=
WITH RECURSIVE q(b) AS
 (SELECT 1 UNION ALL SELECT 1+b FROM q WHERE b<10)
 SELECT (SELECT q1.b FROM q AS q2 WHERE q2.b=3) FROM q AS q1 WHERE q1.b=3;
eval EXPLAIN $query;
eval $query;

--echo # Two recursive members.

CREATE TABLE t1 (id int, name char(10), leftpar int, rightpar int);
INSERT INTO t1 VALUES (1, "A", 2, 3), (2, "LA", 4, 5), (4, "LLA", 6, 7), (6, "LLLA", NULL, NULL), (7, "RLLA", NULL, NULL), 
  (5, "RLA", 8, 9), (8, "LRLA", NULL, NULL), (9, "RRLA", NULL, NULL), (3, "RA", 10, 11), (10, "LRA", 12, 13), (11, "RRA", 14, 15), 
  (15, "RRRA", NULL, NULL), (16, "B", 17, 18), (17, "LB", NULL, NULL), (18, "RB", NULL, NULL);

--echo # Shuffle rows to make sure the algorithm works with any read order of rows above

CREATE TABLE t2 SELECT * FROM t1 ORDER BY rand();

--echo # Tree-walking query. We turn off the Query Cache: indeed
--echo # sometimes pb2 enables Query Cache and as we run twice the
--echo # same query the 2nd may not actually be executed so the value
--echo # of Created_tmp_tables displayed at end becomes "one less").

let $query=
WITH RECURSIVE tree_of_a AS
(SELECT *, cast(id AS char(200)) AS path FROM t2 WHERE name="A"
UNION ALL
SELECT t2.*, concat(tree_of_a.path,",",t2.id) FROM t2 JOIN tree_of_a ON t2.id=tree_of_a.leftpar
UNION ALL
SELECT t2.*, concat(tree_of_a.path,",",t2.id) FROM t2 JOIN tree_of_a ON t2.id=tree_of_a.rightpar)
  SELECT * FROM tree_of_a;

--echo # Note that without ORDER BY, order of rows would be random as BNL
--echo # implies that the randomized t2 is the driving table in the
--echo # joining of rows.

--replace_column 10 #
eval EXPLAIN extended $query ORDER BY path;

eval $query ORDER BY path;

WITH RECURSIVE tree_of_a AS
(SELECT *, cast(id AS char(200)) AS path FROM t2 WHERE name="A"
UNION ALL
SELECT t2.*, concat(tree_of_a.path,",",t2.id) FROM t2 JOIN tree_of_a ON
(t2.id=tree_of_a.leftpar OR t2.id=tree_of_a.rightpar))
  SELECT * FROM tree_of_a ORDER BY path;

--echo case where an index is automatically created on the derived table and used to read this table in the outer query 
let $query=
WITH RECURSIVE tree_of_a AS
(
SELECT *, cast(id AS char(200)) AS path FROM t2 WHERE name="A"
UNION ALL
SELECT t2.*, concat(tree_of_a.path,",",t2.id) FROM t2 JOIN tree_of_a ON t2.id=tree_of_a.leftpar
UNION ALL
SELECT t2.*, concat(tree_of_a.path,",",t2.id) FROM t2 JOIN tree_of_a ON t2.id=tree_of_a.rightpar
)
SELECT * FROM tree_of_a WHERE id=2;

--replace_column 10 #
eval EXPLAIN $query;
eval $query;

DROP TABLE t1,t2;

let $query=
WITH RECURSIVE cte AS (SELECT 1 AS n UNION ALL SELECT n+1 FROM cte WHERE n<10000)
  SELECT sum(cte1.n*cte2.n*cte3.n)=2490508525950000
  FROM cte cte1, cte cte2, cte cte3
WHERE cte1.n=cte2.n+10 AND cte2.n+20=cte3.n;

--replace_column 10 #
eval EXPLAIN $query;
eval $query;

--echo # Transitive closure

CREATE TABLE nodes(id int);
CREATE TABLE arcs(from_id int, to_id int);
INSERT INTO nodes VALUES(1),(2),(3),(4),(5),(6),(7),(8);
INSERT INTO arcs VALUES(1,3), (3,6), (1,4), (4,6), (6,2), (2,1);

--echo # UNION ALL leads to infinite loop as 1 is reachable from 1, so we stop it with a maximum depth 8 

WITH RECURSIVE cte AS
(SELECT id, 0 AS depth FROM nodes WHERE id=1
  UNION ALL
  SELECT to_id, depth+1 FROM arcs, cte WHERE from_id=cte.id AND depth<8)
SELECT count(*), max(depth) FROM cte;

WITH RECURSIVE cte AS
(
  SELECT id, cast(id AS char(200)) AS path, 0 AS is_cycle
  FROM nodes WHERE id=1
  UNION ALL
  SELECT to_id, concat(cte.path, ",", to_id), find_in_set(to_id, path)
  FROM arcs, cte
  WHERE from_id=cte.id AND is_cycle=0
)
SELECT * FROM cte;

WITH RECURSIVE cte AS
(
  SELECT id FROM nodes WHERE id=1
  UNION
  SELECT to_id FROM arcs, cte WHERE from_id=cte.id
)
SELECT * FROM cte;

DROP TABLE nodes, arcs;

--echo # Hash field and MEMORY don't work together. Make long distinct
--echo # key to force hash field, to see if it switches to InnoDB.

--echo # Not too long key (500 bytes in latin1)
WITH RECURSIVE cte AS
 (SELECT 1 AS n, repeat('a',500) AS f, '' AS g, '' AS h, '' AS i
  UNION
  SELECT n+1,'','','','' FROM cte WHERE n<100)
SELECT sum(n) FROM cte;

SHOW STATUS LIKE 'Created_tmp_disk_tables';

--echo # Too long key (>3000 bytes in latin1)
WITH RECURSIVE cte AS
 (
  SELECT 1 AS n,
  repeat('a',500) AS f, repeat('a',500) AS g,
  repeat('a',500) AS h, repeat('a',500) AS i,
  repeat('a',500) AS j, repeat('a',500) AS k,
  repeat('a',500) AS l, repeat('a',500) AS m
  UNION
  SELECT n+1, '','','','','','','','' FROM cte WHERE n<100)
SELECT sum(n) FROM cte;

--echo # In query planning, the recursive reference's row count is
--echo # said to be the estimated row count of all non-recursive query blocks

CREATE TABLE t1(a int);
INSERT INTO t1 VALUES (), (), (), (), (), (), (), (), (), (), (), (),(), (), ();
ANALYZE TABLE t1;
--echo # EXPLAIN says: in non-recursive QB we'll read 15 rows of t1,
--echo # in recursive QB we'll read 15 rows of qn, keep only 0.33
--echo # due to WHERE, that makes 4 (due to rounding), and in the
--echo # derived table we'll thus have 15+4=19. That ignores
--echo # next repetitions of the recursive QB which are unpredictable.
EXPLAIN WITH RECURSIVE qn AS
(SELECT 1 AS a FROM t1 UNION ALL SELECT a+1 FROM qn WHERE qn.a<100)
SELECT * FROM qn;
EXPLAIN WITH RECURSIVE qn AS
(SELECT 1 AS a FROM t1 UNION DISTINCT SELECT a+1 FROM qn WHERE qn.a<100)
SELECT * FROM qn;
DROP TABLE t1;

SHOW STATUS LIKE 'Created_tmp_disk_tables';

flush STATUS;

WITH RECURSIVE q (b) AS (SELECT 1 UNION ALL SELECT 1+b FROM q WHERE b<2000)
SELECT min(b),max(b),avg(b) FROM q;
SHOW STATUS LIKE 'Created_tmp_disk_tables';

--echo # Test when conversion to InnoDB affects recursive references which are not open yet (those of q1):
flush STATUS;
WITH RECURSIVE q (b) AS (SELECT 1 UNION ALL SELECT 1+b FROM q WHERE b<2000)
SELECT min(q.b),max(q.b),avg(q.b) FROM q, q AS q1;
SHOW STATUS LIKE 'Created_tmp_disk_tables';

--echo # Same, but make q1 the writer; this is to test overflow when the writer isn't first in the 'tmp_tables' list

flush STATUS;
WITH RECURSIVE q (b) AS
(SELECT 1 UNION ALL SELECT 1+b FROM q WHERE b<2000)
SELECT min(q.b),max(q.b),avg(q.b) FROM q RIGHT JOIN q AS q1 ON 1;
SHOW STATUS LIKE 'Created_tmp_disk_tables';

--echo # Test when outer query reads CTE with an index.
--echo # Overflow doesn't happen at same row as queries above, as this table has an index which makes it grow faster.

let $query=
WITH RECURSIVE q (b) AS (SELECT 1 UNION ALL SELECT 1+b FROM q WHERE b<2000)
SELECT min(b),max(b),avg(b) FROM q WHERE b=300;

eval EXPLAIN $query;
SHOW STATUS LIKE 'Created_tmp_disk_tables';
eval $query;
SHOW STATUS LIKE 'Created_tmp_disk_tables';

--echo # Verify that rows come out in insertion order.
--echo # If they didn't, the sequences of @c and of 'b'
--echo # would not be identical and the sum wouldn't be
--echo # 1^2 + ... + 2000^2 = n(n+1)(2n+1)/6 = 2668667000

SET @c:=1;
flush STATUS;
WITH RECURSIVE q (b, c) AS (SELECT 1, 1 UNION ALL SELECT (1+b), (@c:=(@c+1)) FROM q WHERE b<2000)
SELECT sum(b*c) FROM q;
SHOW STATUS LIKE 'Created_tmp_disk_tables';

