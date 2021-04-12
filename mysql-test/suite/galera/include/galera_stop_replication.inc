--echo Unloading wsrep provider ...

--let $wsrep_cluster_address_orig = `SELECT @@wsrep_cluster_address`
--let $wsrep_provider_options_orig = `SELECT @@wsrep_provider_options`

SET GLOBAL wsrep_cluster_address = '';
