--echo Loading wsrep provider ...

--disable_query_log
--eval SET GLOBAL wsrep_provider = '$wsrep_provider_orig';
--eval SET GLOBAL wsrep_cluster_address = '$wsrep_cluster_address_orig';
--enable_query_log

--enable_reconnect
--source include/wait_until_connected_again.inc
--source include/wait_until_ready.inc
