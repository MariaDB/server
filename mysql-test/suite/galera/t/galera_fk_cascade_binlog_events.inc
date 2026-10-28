#
# MDEV-38243: FK cascade changes written as row events into the write set
# (rpl_use_binlog_events_for_fk_cascade=ON) must be applied on the other node
# without re-running the cascade, and the nodes must stay consistent.
#
# If node_2 both applied the cascaded row events and re-executed the cascade,
# applying would fail on already deleted/updated rows and the node would drop
# out of the cluster, so a consistent node_2 is the observable here.
#

--connection node_1
SET SESSION rpl_use_binlog_events_for_fk_cascade=1;

--echo #
--echo # Scenario 1: ON DELETE CASCADE through 3 tables
--echo #
CREATE TABLE gp (id INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE p (id INT PRIMARY KEY, gp_id INT,
  FOREIGN KEY (gp_id) REFERENCES gp(id) ON DELETE CASCADE) ENGINE=InnoDB;
CREATE TABLE c (id INT PRIMARY KEY, p_id INT,
  FOREIGN KEY (p_id) REFERENCES p(id) ON DELETE CASCADE) ENGINE=InnoDB;
INSERT INTO gp VALUES (1),(2);
INSERT INTO p VALUES (1,1),(2,2),(3,1);
INSERT INTO c VALUES (1,1),(2,2),(3,3),(4,1);

DELETE FROM gp WHERE id=1;

--connection node_2
SELECT * FROM gp ORDER BY id;
SELECT * FROM p ORDER BY id;
SELECT * FROM c ORDER BY id;

--connection node_1
DROP TABLE c, p, gp;

--echo #
--echo # Scenario 2: ON UPDATE CASCADE and ON UPDATE / DELETE SET NULL
--echo #
CREATE TABLE p (id INT PRIMARY KEY, v INT) ENGINE=InnoDB;
CREATE TABLE cc (id INT PRIMARY KEY, pid INT, KEY(pid),
  FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE ON UPDATE CASCADE) ENGINE=InnoDB;
CREATE TABLE cn (id INT PRIMARY KEY, pid INT, KEY(pid),
  FOREIGN KEY (pid) REFERENCES p(id) ON DELETE SET NULL ON UPDATE SET NULL) ENGINE=InnoDB;
INSERT INTO p VALUES (1,10),(2,20),(3,30);
INSERT INTO cc VALUES (100,1),(200,2),(300,3);
INSERT INTO cn VALUES (100,1),(200,2),(300,3);

UPDATE p SET id=11 WHERE id=1;
DELETE FROM p WHERE id=2;

--connection node_2
SELECT * FROM p ORDER BY id;
SELECT * FROM cc ORDER BY id;
SELECT * FROM cn ORDER BY id;

--echo #
--echo # Scenario 3: rolled back cascades are not replicated
--echo #
--connection node_1
BEGIN;
DELETE FROM p WHERE id=3;
ROLLBACK;
BEGIN;
UPDATE p SET id=33 WHERE id=3;
SAVEPOINT s1;
DELETE FROM p WHERE id=11;
ROLLBACK TO SAVEPOINT s1;
COMMIT;

--connection node_2
SELECT * FROM p ORDER BY id;
SELECT * FROM cc ORDER BY id;
SELECT * FROM cn ORDER BY id;

--connection node_1
DROP TABLE cc, cn, p;

--echo #
--echo # Scenario 4: parallel applying on node_2
--echo #
CREATE TABLE p1 (f1 INT PRIMARY KEY, f2 INT) ENGINE=InnoDB;
CREATE TABLE p2 (f1 INT PRIMARY KEY, f2 INT) ENGINE=InnoDB;
CREATE TABLE c (f1 INT PRIMARY KEY, p1_id INT, p2_id INT, f2 INT,
  CONSTRAINT fk_1 FOREIGN KEY (p1_id) REFERENCES p1 (f1) ON DELETE CASCADE,
  CONSTRAINT fk_2 FOREIGN KEY (p2_id) REFERENCES p2 (f1) ON DELETE CASCADE) ENGINE=InnoDB;

--let $count = 200
--disable_query_log
while ($count)
{
  --eval INSERT INTO p1 VALUES ($count, 0)
  --eval INSERT INTO p2 VALUES ($count, 0)
  --eval INSERT INTO c VALUES ($count, $count, $count, 0)
  --dec $count
}
--enable_query_log

--connection node_2
SET GLOBAL wsrep_slave_threads=4;

--connection node_1
--let $count = 200
--disable_query_log
while ($count)
{
  --eval DELETE FROM p2 WHERE f1=$count
  --eval DELETE FROM p1 WHERE f1=$count
  --dec $count
}
--enable_query_log

--connection node_2
SELECT COUNT(*) FROM p1;
SELECT COUNT(*) FROM p2;
SELECT COUNT(*) FROM c;
SET GLOBAL wsrep_slave_threads=DEFAULT;

--connection node_1
DROP TABLE c, p1, p2;

--echo #
--echo # Scenario 5: child without PK and with a virtual column in a key,
--echo # next to an eligible sibling
--echo #
CREATE TABLE p (id INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE ce (id INT PRIMARY KEY, pid INT, KEY(pid),
  FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE ON UPDATE CASCADE) ENGINE=InnoDB;
CREATE TABLE ci (id INT, pid INT, v INT, vc INT AS (v + 1) VIRTUAL, KEY(pid), KEY(vc),
  FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE ON UPDATE CASCADE) ENGINE=InnoDB;
INSERT INTO p VALUES (1),(2),(3);
INSERT INTO ce VALUES (100,1),(200,2),(300,3);
INSERT INTO ci (id,pid,v) VALUES (100,1,1),(200,2,2),(300,3,3);

UPDATE p SET id=11 WHERE id=1;
DELETE FROM p WHERE id=2;

--connection node_2
SELECT * FROM p ORDER BY id;
SELECT * FROM ce ORDER BY id;
SELECT id, pid, v, vc FROM ci ORDER BY id;

--connection node_1
DROP TABLE ce, ci, p;

--echo #
--echo # Both nodes are still in the cluster and applied without errors
--echo #
--connection node_2
SELECT VARIABLE_VALUE AS cluster_size FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';
SELECT VARIABLE_VALUE AS local_state FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment';

--connection node_1
SET SESSION rpl_use_binlog_events_for_fk_cascade=DEFAULT;
