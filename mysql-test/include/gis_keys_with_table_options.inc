--source include/have_geometry.inc

#
# Spatial objects with keys
#

#
# Bug #30825: Problems when putting a non-spatial index on a GIS column
#

eval CREATE TABLE t1 (a INT, p POINT, PRIMARY KEY(a)) $TABLE_OPTION13;
eval CREATE TABLE t2 (a INT, p POINT, PRIMARY KEY(a), INDEX(p)) $TABLE_OPTION14;
INSERT INTO t1 VALUES (1, POINTFROMTEXT('POINT(1 2)'));
INSERT INTO t2 VALUES (1, POINTFROMTEXT('POINT(1 2)'));

# no index, returns 1 as expected
SELECT COUNT(*) FROM t1 WHERE p=POINTFROMTEXT('POINT(1 2)');

# with index, returns 1 as expected
# EXPLAIN shows that the index is not used though
# due to the "most rows covered anyway, so a scan is more effective" rule
EXPLAIN 
SELECT COUNT(*) FROM t2 WHERE p=POINTFROMTEXT('POINT(1 2)');
SELECT COUNT(*) FROM t2 WHERE p=POINTFROMTEXT('POINT(1 2)');

# adding another row to the table so that
# the "most rows covered" rule doesn't kick in anymore
# now EXPLAIN shows the index used on the table
# and we're getting the wrong result again
INSERT INTO t1 VALUES (2, POINTFROMTEXT('POINT(3 4)'));
INSERT INTO t2 VALUES (2, POINTFROMTEXT('POINT(3 4)'));
EXPLAIN 
SELECT COUNT(*) FROM t1 WHERE p=POINTFROMTEXT('POINT(1 2)');
SELECT COUNT(*) FROM t1 WHERE p=POINTFROMTEXT('POINT(1 2)');

--replace_column 9 #
EXPLAIN 
SELECT COUNT(*) FROM t2 WHERE p=POINTFROMTEXT('POINT(1 2)');
SELECT COUNT(*) FROM t2 WHERE p=POINTFROMTEXT('POINT(1 2)');

EXPLAIN 
SELECT COUNT(*) FROM t2 IGNORE INDEX(p) WHERE p=POINTFROMTEXT('POINT(1 2)');
SELECT COUNT(*) FROM t2 IGNORE INDEX(p) WHERE p=POINTFROMTEXT('POINT(1 2)');

DROP TABLE t1, t2;

--echo End of 5.0 tests
