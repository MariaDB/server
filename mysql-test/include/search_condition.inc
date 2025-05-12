--echo #
--echo # WHERE <search condition>
--echo #

let datatype=`SELECT DATA_TYPE FROM INFORMATION_SCHEMA.COLUMNS
               WHERE TABLE_NAME='t0' AND COLUMN_NAME='c'`;
let use_string_func=0;
let use_field=1;
let update_source=c+11;

if ($datatype == 'varchar')
{
  let use_string_func=1;
}

if ($datatype == 'datetime')
{
  let update_source=c+0.1;
}

if ($datatype == 'inet6')
{
  let use_field=0;
  let update_source=concat(c,'77');
}


CREATE TABLE t1 AS SELECT * FROM t0;
if ($use_field)
{
 SELECT * FROM t1 WHERE c;
}
SELECT * FROM t1 WHERE c IS FALSE;
SELECT * FROM t1 WHERE c IS TRUE;
SELECT * FROM t1 WHERE COALESCE(c);
if ($use_string_func)
{
  SELECT * FROM t1 WHERE CONCAT(c);
  SELECT * FROM t1 WHERE LEFT(c,100);
}
DROP TABLE t1;


--echo #
--echo # HAVING <search condition>
--echo #

CREATE TABLE t1 AS SELECT * FROM t0;
if ($use_field)
{
  SELECT COALESCE(c,c) AS c2 FROM t1 GROUP BY c2 HAVING c2;
}
SELECT COALESCE(c,c) AS c2 FROM t1 GROUP BY c2 HAVING c2 IS FALSE;
SELECT COALESCE(c,c) AS c2 FROM t1 GROUP BY c2 HAVING c2 IS TRUE;
SELECT COALESCE(c,c) AS c2 FROM t1 GROUP BY c2 HAVING COALESCE(c2);
if ($use_string_func)
{
  SELECT CONCAT(c,'0') AS c2 FROM t1 GROUP BY c2 HAVING LEFT(c2,100);
}
DROP TABLE t1;


--echo #
--echo # <join condition> :=  ON <search condition>
--echo #

CREATE TABLE t1 AS SELECT * FROM t0;
if ($use_field)
{
  SELECT t1.c FROM t1 JOIN t1 AS t2 ON (t1.c);
}
SELECT t1.c FROM t1 JOIN t1 AS t2 ON (t1.c IS FALSE);
SELECT t1.c FROM t1 JOIN t1 AS t2 ON (t1.c IS TRUE);
SELECT t1.c FROM t1 JOIN t1 AS t2 ON (COALESCE(t1.c));
if ($use_string_func)
{
  SELECT t1.c FROM t1 JOIN t1 AS t2 ON (CONCAT(t1.c));
}
DROP TABLE t1;


--echo #
--echo # <delete statement: searched>
--echo #   DELETE FROM <target table> [ WHERE <search condition> ]
--echo #

if ($use_field)
{
  CREATE TABLE t1 AS SELECT * FROM t0;
  DELETE FROM t1 WHERE c;
  SELECT * FROM t1;
  DROP TABLE t1;
}

CREATE TABLE t1 AS SELECT * FROM t0;
DELETE FROM t1 WHERE c IS FALSE;
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT * FROM t0;
DELETE FROM t1 WHERE c IS TRUE;
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT * FROM t0;
DELETE FROM t1 WHERE COALESCE(c);
SELECT * FROM t1;
DROP TABLE t1;

if ($use_string_func)
{
  CREATE TABLE t1 AS SELECT * FROM t0;
  DELETE FROM t1 WHERE CONCAT(c);
  SELECT * FROM t1;
  DROP TABLE t1;
}


--echo #
--echo # <update statement: searched>
--echo #   UPDATE <target table> SET <set clause list> [ WHERE <search condition> ]

if ($use_field)
{
  CREATE TABLE t1 AS SELECT * FROM t0;
  eval UPDATE t1 SET c=$update_source WHERE c;
  SELECT * FROM t1;
  DROP TABLE t1;
}

CREATE TABLE t1 AS SELECT * FROM t0;
eval UPDATE t1 SET c=$update_source WHERE c IS FALSE;
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT * FROM t0;
eval UPDATE t1 SET c=$update_source WHERE c IS TRUE;
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT * FROM t0;
eval UPDATE t1 SET c=$update_source WHERE COALESCE(c);
SELECT * FROM t1;
DROP TABLE t1;

if ($use_string_func)
{
  CREATE TABLE t1 AS SELECT * FROM t0;
  eval UPDATE t1 SET c=$update_source WHERE COALESCE(c);
  SELECT * FROM t1;
  DROP TABLE t1;
}

--echo #
--echo # <check constraint definition>
--echo #   CHECK <left paren> <search condition> <right paren>

if ($use_field)
{
  CREATE TABLE t1 LIKE t0;
  ALTER TABLE t1 ADD CONSTRAINT check0 CHECK(c);
--error ER_CONSTRAINT_FAILED
  INSERT INTO t1 SELECT * FROM t0 WHERE NOT c;
  INSERT INTO t1 SELECT * FROM t0 WHERE c;
  SELECT * FROM t1;
  DROP TABLE t1;
}

CREATE TABLE t1 LIKE t0;
ALTER TABLE t1 ADD CONSTRAINT check0 CHECK(c IS FALSE);
INSERT INTO t1 SELECT * FROM t0 WHERE c IS FALSE;
--error ER_CONSTRAINT_FAILED
INSERT INTO t1 SELECT * FROM t0 WHERE c IS TRUE;
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1 LIKE t0;
ALTER TABLE t1 ADD CONSTRAINT check0 CHECK(c IS TRUE);
--error ER_CONSTRAINT_FAILED
INSERT INTO t1 SELECT * FROM t0 WHERE c IS FALSE;
INSERT INTO t1 SELECT * FROM t0 WHERE c IS TRUE;
SELECT * FROM t1;
DROP TABLE t1;

CREATE TABLE t1 LIKE t0;
ALTER TABLE t1 ADD CONSTRAINT check0 CHECK(COALESCE(c));
--error ER_CONSTRAINT_FAILED
INSERT INTO t1 SELECT * FROM t0 WHERE c IS FALSE;
INSERT INTO t1 SELECT * FROM t0 WHERE c IS TRUE;
SELECT * FROM t1;
DROP TABLE t1;


--echo #
--echo # <case expression>
--echo #   WHEN <search condition> THEN <result>

CREATE TABLE t1 AS SELECT * FROM t0;
SELECT c, CASE WHEN c THEN 'true' ELSE 'false' END AS c2 FROM t1;
SELECT c, CASE WHEN COALESCE(c) THEN 'true' ELSE 'false' END AS c2 FROM t1;
DROP TABLE t1;
