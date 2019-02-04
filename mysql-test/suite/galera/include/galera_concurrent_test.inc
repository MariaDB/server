#
# Perform a quick concurrent test on two nodes using a set of predefined statements.
#
# Such tests are not deterministic, so we are hoping to catch assertions, slave apply errors
# and cases where the two nodes diverge
#
# Parameters:
# - $wsrep_trx_fragment_size
# - $count
# - $query_node_1
# - $query_node_1a (optional)
# - $query_node_2
#

if (!$count) {
	--let $count = 50;
}

if (!$node_1a_connected) {
	--connect node_1a, 127.0.0.1, root, , test, $NODE_MYPORT_1
	--let $node_1a_connected = 1
}

--echo Running a concurrent test with the following queries:
--echo $query_node_1
--echo $query_node_1a
--echo $query_node_2

--connection node_1
--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

CREATE TABLE t1 (f1 INTEGER PRIMARY KEY AUTO_INCREMENT, f2 CHAR(255)) ENGINE=InnoDB;

SET SESSION wsrep_sync_wait = 0;
--eval SET SESSION wsrep_trx_fragment_size = $wsrep_trx_fragment_size;

--connection node_1a
SET SESSION wsrep_sync_wait = 0;
--eval SET SESSION wsrep_trx_fragment_size = $wsrep_trx_fragment_size;

--connection node_2
SET SESSION wsrep_sync_wait = 0;
--eval SET SESSION wsrep_trx_fragment_size = $wsrep_trx_fragment_size;

--disable_query_log
--let $i = `SELECT $count`
while ($i)
{
	--connection node_1
	--send_eval $query_node_1

        --connection node_1a
        if ($query_node_1a) {
		--send_eval $query_node_1a
	}

	--connection node_2
	--send_eval $query_node_2

        --connection node_1
        --error 0,ER_QUERY_INTERRUPTED,ER_LOCK_DEADLOCK,ER_DUP_ENTRY
	--reap

	--connection node_1a
        if ($query_node_1a) {
	        --error 0,ER_QUERY_INTERRUPTED,ER_LOCK_DEADLOCK,ER_DUP_ENTRY
	        --reap
	}

        --connection node_2
        --error 0,ER_QUERY_INTERRUPTED,ER_LOCK_DEADLOCK,ER_DUP_ENTRY
        --reap
        --dec $i
}

SET SESSION wsrep_sync_wait = 15;
--enable_query_log

--let $diff_servers = 1 2
--source include/diff_servers.inc

DROP TABLE t1;

--let $query_node_1 = ""
--let $query_node_1a = ""
--let $query_node_2 = ""

--echo Concurrent test end
