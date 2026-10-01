#
# Body of galera.MDEV-41012-collision, parameterized by the data type.
#
#   $fbt_type   - data type of the primary key column
#   $fbt_value1 - a literal of that type
#   $fbt_value2 - a *different* literal whose binary representation folds to
#                 the same latin1_swedish_ci weight string as $fbt_value1
#

--connection node_1
--eval CREATE TABLE t (id $fbt_type PRIMARY KEY, v INT) ENGINE=InnoDB
--eval INSERT INTO t VALUES ($fbt_value1, 0), ($fbt_value2, 0)

--connection node_2
SET SESSION wsrep_sync_wait = 0;
--let $wait_condition = SELECT COUNT(*) = 2 FROM t
--source include/wait_condition.inc

SET GLOBAL wsrep_slave_threads = 2;

# Park the applier of the first UPDATE on entry to the apply monitor.
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

--connection node_1
--eval UPDATE t SET v = 1 WHERE id = $fbt_value1

--connection node_2
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_wait_sync_point.inc

# Disarm, so the second applier runs into the real dependency check.
--source include/galera_clear_sync_point.inc

--let $apply_waits = `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_apply_waits'`

--connection node_1
--eval UPDATE t SET v = 2 WHERE id = $fbt_value2

--connection node_2
# The two statements modify two *different* rows, so their
# certification keys must differ, there must be no dependency between the two
# writesets, and the second applier must be free to apply its row while the
# first one is still parked.
--let $wait_condition = SELECT (SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_apply_waits') > $apply_waits OR EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.INNODB_TRX WHERE trx_rows_modified > 0)
--source include/wait_condition.inc

# While the key was collated with latin1_swedish_ci the two keys were equal,
# the second writeset was made to depend on the first, and both of these
# reported 0.
--disable_query_log
--eval SELECT VARIABLE_VALUE = $apply_waits AS no_false_dependency FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_apply_waits'
--enable_query_log
SELECT COUNT(*) = 1 AS second_row_applied_in_parallel FROM INFORMATION_SCHEMA.INNODB_TRX WHERE trx_rows_modified > 0;

--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_signal_sync_point.inc

--let $wait_condition = SELECT SUM(v) = 3 FROM t
--source include/wait_condition.inc
SELECT id, v FROM t ORDER BY v;

SET GLOBAL wsrep_slave_threads = DEFAULT;
SET SESSION wsrep_sync_wait = DEFAULT;

--connection node_1
DROP TABLE t;
