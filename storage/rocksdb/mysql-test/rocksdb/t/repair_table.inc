# 
# REPAIR TABLE statements
#
# Note: the output is likely to be different for the engine under test,
# in which case rdiff will be needed. Or, the output might say that
# the storage engine does not support REPAIR.
#

--disable_warnings
DROP TABLE IF EXISTS t1,t2;
--enable_warnings

CREATE TABLE t1 (a INT PRIMARY KEY, b CHAR(8)) ENGINE=rocksdb;
INSERT INTO t1 (a,b) VALUES (1,'a'),(2,'b');
CREATE TABLE t2 (a INT, b CHAR(8) PRIMARY KEY) ENGINE=rocksdb;

REPAIR TABLE t1;

INSERT INTO t1 (a,b) VALUES (3,'c');
INSERT INTO t2 (a,b) VALUES (4,'d');
REPAIR NO_WRITE_TO_BINLOG TABLE t1, t2;
INSERT INTO t2 (a,b) VALUES (5,'e'),(6,'f');
REPAIR LOCAL TABLE t2;
INSERT INTO t1 (a,b) VALUES (7,'g'),(8,'h');
INSERT INTO t2 (a,b) VALUES (9,'i');
REPAIR LOCAL TABLE t2, t1 EXTENDED;
INSERT INTO t1 (a,b) VALUES (10,'j');
INSERT INTO t2 (a,b) VALUES (11,'k');
REPAIR TABLE t1, t2 QUICK USE_FRM;
INSERT INTO t1 (a,b) VALUES (12,'l');
INSERT INTO t2 (a,b) VALUES (13,'m');
REPAIR NO_WRITE_TO_BINLOG TABLE t1, t2 QUICK EXTENDED USE_FRM;
FLUSH TABLE t1;

DROP TABLE t1, t2;



