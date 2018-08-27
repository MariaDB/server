CREATE TABLE t1 (f1 INTEGER PRIMARY KEY, f2 CHAR(255)) ENGINE=InnoDB;

--connection node_2
SET SESSION wsrep_trx_fragment_size = 1;
START TRANSACTION;
INSERT INTO t1 VALUES (1, 'x');

--connection node_2a
--source include/galera_set_sync_point.inc

--connection node_2
--send COMMIT

--connection node_2b
--sleep 1
SET SESSION wsrep_sync_wait = 0;
SELECT COUNT(*) = 0 FROM t1;

--connection node_1
SET SESSION wsrep_sync_wait = 0;
SET SESSION wsrep_trx_fragment_size = 1;
SELECT COUNT(*) = 1 FROM t1;
REPLACE INTO t1 VALUES (1,'y');

--connection node_2b
SELECT COUNT(*) = 0 FROM t1;

--connection node_2a
--source include/galera_signal_sync_point.inc
--source include/galera_clear_sync_point.inc

--connection node_2
--reap
SELECT COUNT(*) = 1 FROM t1;

DROP TABLE t1;
