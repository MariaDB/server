--echo Resetting wsrep_cluster_address

--let $wsrep_cluster_size_orig = `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'`

SET GLOBAL wsrep_cluster_address = @@wsrep_cluster_address;

--source include/wait_until_connected_again.inc

# Wait for wsrep_cluster_size to go back to its original value

--let $wait_condition = SELECT VARIABLE_VALUE = $wsrep_cluster_size_orig FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc
