--source include/galera_cluster.inc
--source include/force_restart.inc

--connection node_1
reset master;
--connection node_2
reset master;

#
# Test Galera with --log-bin --log-slave-updates .
# This way the actual MySQL binary log is used,
# rather than Galera's own implementation
#

CREATE TABLE t1 (id INT PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t1 VALUES (1);

CREATE TABLE t2 (id INT) ENGINE=InnoDB;
INSERT INTO t2 VALUES (1);
INSERT INTO t2 VALUES (1);

--connection node_2
SELECT COUNT(*) = 1 FROM t1;
SELECT COUNT(*) = 2 FROM t2;

--connection node_1
ALTER TABLE t1 ADD COLUMN f2 INTEGER;
--let $MASTER_MYPORT=$NODE_MYPORT_1
--source include/show_binlog_events.inc

--connection node_2
SELECT COUNT(*) = 2 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_NAME = 't1';
--let $MASTER_MYPORT=$NODE_MYPORT_2
--source include/show_binlog_events.inc

DROP TABLE t1;
DROP TABLE t2;

--echo #cleanup
--connection node_1
RESET MASTER;
