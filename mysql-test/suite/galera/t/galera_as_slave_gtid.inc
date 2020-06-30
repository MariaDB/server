#
# Test Galera as a slave to a MariaDB master using GTIDs
#
# suite/galera/galera_2nodes_as_slave.cnf describes the setup of the nodes
# suite/galera/t/galera_as_slave_gtid.cnf has the GTID options
#
# In addition to performing DDL and DML, we check that the gtid of the master is preserved inside the cluster
#

--source include/have_innodb.inc
--source include/have_log_bin.inc
--source include/galera_cluster.inc

# As node #3 is not a Galera node, and galera_cluster.inc does not open connetion to it
# we open the node_3 connection here
--connect node_3, 127.0.0.1, root, , test, $NODE_MYPORT_3

--connection node_2
--disable_query_log
--eval CHANGE MASTER TO MASTER_HOST='127.0.0.1', MASTER_USER='root', MASTER_PORT=$NODE_MYPORT_3;
--enable_query_log
START SLAVE;

--connection node_3
CREATE TABLE t1 (f1 INTEGER PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t1 VALUES(1);

SELECT LENGTH(@@global.gtid_binlog_state) > 1;
--let $gtid_binlog_state_node1 = `SELECT @@global.gtid_binlog_state;`

--connection node_1
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't1';
--source include/wait_condition.inc

--let $wait_condition = SELECT COUNT(*) = 1 FROM t1;
--source include/wait_condition.inc

--connection node_2
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't1';
--source include/wait_condition.inc

--let $wait_condition = SELECT COUNT(*) = 1 FROM t1;
--source include/wait_condition.inc

--disable_query_log

--eval SELECT '$gtid_binlog_state_node1' = @@global.gtid_binlog_state AS gtid_binlog_state_equal;
#--eval SELECT GTID_SUBSET('$gtid_executed_node1', @@global.gtid_executed) AS gtid_executed_equal;

--enable_query_log

--connection node_1
SELECT COUNT(*) = 1 FROM t1;

--disable_query_log
--eval SELECT '$gtid_binlog_state_node1' = @@global.gtid_binlog_state AS gtid_binlog_state_equal;
#--eval SELECT GTID_SUBSET('$gtid_executed_node1', @@global.gtid_executed) AS gtid_executed_equal;
--enable_query_log

--connection node_3
DROP TABLE t1;

#
# Unfortunately without the sleep below the following statement fails with "query returned no rows", which
# is difficult to understand given that it is an aggregate query. A "query execution was interrupted"
# warning is also reported by MTR, which is also weird.
#

--sleep 1

--connection node_1
--let $wait_condition = SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't1';
--source include/wait_condition.inc

--connection node_2
--let $wait_condition = SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME = 't1';
--source include/wait_condition.inc

STOP SLAVE;
RESET SLAVE ALL;

--echo #cleanup
--connection node_1
set global wsrep_on=OFF;
reset master;
set global wsrep_on=ON;

--connection node_2
set global wsrep_on=OFF;
reset master;
set global wsrep_on=ON;

--connection node_3
reset master;
