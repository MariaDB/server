# One round of the workload of galera_binlog_in_engine_mixed.inc.

# galera_st_clean_slave.inc and galera_st_kill_slave.inc leave autocommit
# off in the node_2 session.
--connection node_1
SET SESSION autocommit = 1;
--connection node_2
SET SESSION autocommit = 1;

--connection node_1
CREATE TABLE t2 (id INT PRIMARY KEY, node INT, b LONGBLOB) ENGINE=InnoDB;
INSERT INTO t2 VALUES (1, 1, 'small');
INSERT INTO t2 SELECT seq, 1, REPEAT('x', 2000) FROM seq_100_to_199;
SET SESSION wsrep_trx_fragment_unit = 'bytes';
SET SESSION wsrep_trx_fragment_size = 20000;
UPDATE t2 SET b = REPEAT('s', 3000) WHERE node = 1;
SET SESSION wsrep_trx_fragment_size = 0;

--connection node_2
--let $wait_condition = SELECT COUNT(*) = 101 FROM t2 WHERE b LIKE 's%'
--source include/wait_condition.inc
INSERT INTO t2 VALUES (2, 2, 'small');
INSERT INTO t2 SELECT seq, 2, REPEAT('y', 2000) FROM seq_200_to_299;
SET SESSION wsrep_trx_fragment_unit = 'bytes';
SET SESSION wsrep_trx_fragment_size = 20000;
UPDATE t2 SET b = REPEAT('t', 3000) WHERE node = 2;
SET SESSION wsrep_trx_fragment_size = 0;

--connection node_1
--let $wait_condition = SELECT COUNT(*) = 101 FROM t2 WHERE b LIKE 't%'
--source include/wait_condition.inc
--let $data_1 = `SELECT CONCAT(COUNT(*), ' ', SUM(CRC32(b))) FROM t2`
--connection node_2
--let $data_2 = `SELECT CONCAT(COUNT(*), ' ', SUM(CRC32(b))) FROM t2`
if ($data_1 != $data_2)
{
  --die The nodes have different data in t2: node_1 $data_1, node_2 $data_2
}
--echo # t2 has the same data on both nodes: $data_1

--connection node_1
DROP TABLE t2;
--connection node_2
--let $wait_condition = SELECT COUNT(*) = 0 FROM information_schema.tables WHERE table_name = 't2'
--source include/wait_condition.inc
--source suite/galera/include/galera_binlog_in_engine_check_gtid.inc
