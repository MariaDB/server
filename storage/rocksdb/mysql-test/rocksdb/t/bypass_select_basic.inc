--source include/have_rocksdb.inc

--source ../include/bypass_create_table.inc

SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /*+ no_bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;

SELECT /*+bypassabc*/ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /*+bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /* +bypassabc*/ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /*aaaaaaaaabbbbbbbbb*/ id1,id2,id1_type,id2_type,data,version
from link_table WHERE id1=1 and id2=2 and link_type=3;
SELECT /*+*/ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /*+b*/ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /*+byp*/ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /*+bypw*/ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /*-b*/ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /**/ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;

--echo # Point query
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2 IN (2, 3, 4) and link_type=3;
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2 IN (2) and link_type=3;
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1 IN (1) and id2 IN (2) and link_type=3;
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1 IN (1, 2) and id2 IN (2, 3, 4) and link_type=3;

--echo # Prefix range query

--echo # Prefix range query with SK
SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 3 AND time = 10
ORDER BY TIME DESC LIMIT 10;
SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 3 AND time = 10
ORDER BY TIME ASC LIMIT 10;

--echo # Prefix range query with SK with limits
SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 10;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 5;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 1;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 0;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 0,10;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 0,5;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 0,1;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 1,0;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 1,10;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 1,5;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 1,1;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 1,0;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 5,10;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 5,5;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 5,1;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 5,0;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 10,10;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 10,5;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 10,1;

SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (id1_type)
WHERE link_type = 3 AND id1 = 1 AND visibility = 4 AND time >= 10
ORDER BY TIME DESC LIMIT 10,0;

--echo # Prefix range query with PK
SELECT /*+ bypass */ id1, id2, link_type FROM link_table FORCE INDEX (PRIMARY)
WHERE link_type=3 and id1=1 ORDER BY id2 DESC;
SELECT /*+ bypass */ id1, id2, link_type FROM link_table FORCE INDEX (PRIMARY)
WHERE link_type=3 and id1=1 ORDER BY id2 ASC;

--echo # Prefix range query with PK + value
SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (PRIMARY)
WHERE link_type=3 and id1=1 ORDER BY id2 DESC;
SELECT /*+ bypass */ id1, id2, link_type, visibility, data, time, version
FROM link_table FORCE INDEX (PRIMARY)
WHERE link_type=3 and id1=1 ORDER BY id2 ASC;

--echo # Transaction
BEGIN;
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
UPDATE link_table set data="bcd" WHERE id1=1 and id2=2 and link_type = 3;
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
COMMIT;

BEGIN;
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
UPDATE link_table set data="cde" WHERE id1=1 and id2=2 and link_type = 3;
SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;
ROLLBACK;

SELECT /*+ bypass */ id1,id2,id1_type,id2_type,data,version from link_table
WHERE id1=1 and id2=2 and link_type=3;

--echo # Data types
SELECT /*+ bypass */ id1 FROM link_table where link_type="3";
SELECT /*+ bypass */ id1 FROM link_table where link_type="3" AND id1="1";
SELECT /*+ bypass */ id1 FROM link_table where link_type="3" AND id1=True;
SELECT /*+ bypass */ id1 FROM link_table where link_type="3" AND id1=b'1';
SELECT /*+ bypass */ id1 FROM link_table where link_type="3" AND id1=x'01';
SELECT /*+ bypass */ id1 FROM link_table where link_type="3" AND id1=NULL;

DROP TABLE count_table;
DROP TABLE link_table;
DROP TABLE link_table3;
DROP TABLE link_table2;
DROP TABLE id_table;
DROP TABLE node_table;
DROP TABLE link_table5;
DROP TABLE link_table6;
DROP TABLE link_table4;
