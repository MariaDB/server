--echo Loading wsrep provider ...

--disable_query_log
--eval SET GLOBAL wsrep_cluster_address = '$wsrep_cluster_address_orig';
--enable_query_log

--source include/galera_wait_ready.inc
