# MDEV-38999 fixture: common teardown shared by the galera_MDEV-38999* tests.
#
# Expects: $wsrep_provider_options_orig set by galera_mdev38999_init.inc.

--connection node_2
--disable_query_log
--eval SET GLOBAL wsrep_provider_options = "$wsrep_provider_options_orig"
SET SESSION wsrep_sync_wait = 0;
--enable_query_log

--connection node_1
DROP TABLE t1;

--disconnect node_2a
