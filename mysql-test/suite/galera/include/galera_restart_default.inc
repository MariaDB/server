# include/galera_restart_default.inc
#
# Restart both nodes with the options of the test configuration, after
# the test started node 1 with other options. Otherwise mtr restarts only
# node 1 for the next test, and node 1 bootstraps a new cluster.
#
# Node 2 shuts down first, so node 1 is safe to bootstrap.
#
# The included files enable the query log, so disable the connect log
# as well.
#

--disable_query_log
--disable_connect_log
--connection node_2
--source include/shutdown_mysqld.inc
--let $restart_default_expect_2= $_expect_file_name

--connection node_1
--let $wait_condition= SELECT VARIABLE_VALUE = 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc
--source include/shutdown_mysqld.inc
--let $start_mysqld_params=
--source include/start_mysqld.inc

--connection node_2
--let $_expect_file_name= $restart_default_expect_2
--source include/start_mysqld.inc

--connection node_1
--let $wait_condition= SELECT VARIABLE_VALUE = 2 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc
--enable_connect_log
--enable_query_log
