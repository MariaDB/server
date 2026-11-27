# MDEV-38999 fixture: common setup shared by the galera_MDEV-38999* tests.
#
# Expects: --connection node_2 already active, t1 already created on node_1.
# Leaves: node_2a connected, wsrep_provider_options saved in
# $wsrep_provider_options_orig, repl.causal_read_timeout shortened to 1s so
# a stuck causal wait fails fast instead of hanging for its default 90s.

--let $wsrep_provider_options_orig = `SELECT @@wsrep_provider_options`
SET GLOBAL wsrep_provider_options = "repl.causal_read_timeout=PT1S";
--connect node_2a, 127.0.0.1, root, , test, $NODE_MYPORT_2
--connection node_2
