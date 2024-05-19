--connection node_1
CREATE TABLE t1 (pk INT AUTO_INCREMENT PRIMARY KEY);

--connection node_1
--error 1051
DROP TABLE nonexistent;

# Verify cluster is intact
INSERT INTO t1 VALUES (DEFAULT);
--connection node_2
--let $wait_condition = SELECT COUNT(*) = 1 FROM t1;
--source include/wait_condition.inc

--connection node_1
--error 1146
TRUNCATE TABLE nonexistent;

# Verify cluster is intact
INSERT INTO t1 VALUES (DEFAULT);
--connection node_2
--let $wait_condition = SELECT COUNT(*) = 2 FROM t1;
--source include/wait_condition.inc

--connection node_1
--error 1049
CREATE TABLE nonexistent.t1 (s INT);

# Verify cluster is intact
INSERT INTO t1 VALUES (DEFAULT);
--connection node_2
--let $wait_condition = SELECT COUNT(*) = 3 FROM t1;
--source include/wait_condition.inc

--connection node_1
--error 1050
CREATE TABLE t1 (s INT);

# Verify cluster is intact
INSERT INTO t1 VALUES (DEFAULT);
--connection node_2
--let $wait_condition = SELECT COUNT(*) = 4 FROM t1;
--source include/wait_condition.inc

--connection node_1
--error 1146
ALTER TABLE nonexistent ADD COLUMN (c INT);

# Verify cluster is intact
INSERT INTO t1 VALUES (DEFAULT);
--connection node_2
--let $wait_condition = SELECT COUNT(*) = 5 FROM t1;
--source include/wait_condition.inc

DROP TABLE t1;
