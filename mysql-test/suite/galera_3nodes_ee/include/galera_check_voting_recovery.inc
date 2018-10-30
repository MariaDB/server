#
# Test that after an inconsistency vote that kicked out node #2, the 
# cluster is able to recover
#

# Nodes #1 and #3 remain in the cluster

--connection node_1
--let $wait_condition = SELECT VARIABLE_VALUE = 2 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';
--source include/wait_condition.inc

SELECT VARIABLE_VALUE = 'Primary' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_status';

--connect node_3X, 127.0.0.1, root, , test, $NODE_MYPORT_3
--connection node_3X
SELECT VARIABLE_VALUE = 2 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';
SELECT VARIABLE_VALUE = 'Primary' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_status';

# Node #2 is kicked out

--connection node_2
SET SESSION wsrep_on=OFF;
SELECT VARIABLE_VALUE = 'non-Primary' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_status';
SELECT VARIABLE_VALUE = 'Inconsistent' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment';
SET SESSION wsrep_on=ON;

# Restore cluster

--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/mysqld.2.expect
--exec echo "wait" > $_expect_file_name
# Exclude SHOW commands run during server shutdown from sync waiting
--disable_query_log
SET SESSION wsrep_sync_wait = 7;
--enable_query_log
--shutdown_server
--source include/wait_until_disconnected.inc

--sleep 5
--source include/start_mysqld.inc
--source include/galera_wait_ready.inc

--connection node_1
--let $wait_condition = SELECT VARIABLE_VALUE = 3 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size';
--source include/wait_condition.inc

--connection node_2
SELECT VARIABLE_VALUE = 'Primary' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_status';

# Confirm that the t1 table is now identical throughout

--connection node_1
SHOW CREATE TABLE t1;

--connection node_2
SHOW CREATE TABLE t1;

--connection node_3X
SHOW CREATE TABLE t1;
