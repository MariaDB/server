#
# Body of galera.MDEV-41012, parameterized by the data type of the foreign key.
#
#   $fbt_type  - data type of the parent PK / child FK column
#   $fbt_value - a literal of that type whose binary representation contains
#                bytes that latin1_swedish_ci folds (0xC0, 0x6A, 0x6B, 0xD2 ...)
#

--connection node_1
--eval CREATE TABLE p (id $fbt_type PRIMARY KEY, v INT) ENGINE=InnoDB
--eval CREATE TABLE c (id INT AUTO_INCREMENT PRIMARY KEY, p_id $fbt_type NOT NULL, KEY k (p_id), FOREIGN KEY (p_id) REFERENCES p (id)) ENGINE=InnoDB
--eval INSERT INTO p VALUES ($fbt_value, 0)

--connection node_2
SET SESSION wsrep_sync_wait = 0;
--let $wait_condition = SELECT COUNT(*) = 1 FROM p
--source include/wait_condition.inc

# Two appliers, so that the two writesets below *can* be applied in parallel
# if certification does not make them depend on each other.
SET GLOBAL wsrep_slave_threads = 2;

# Park the first applier on entry to the apply monitor, i.e. before the child
# INSERT is applied and before its FK check locks the parent row.
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

--connection node_1
--eval INSERT INTO c (p_id) VALUES ($fbt_value)

--connection node_2
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_wait_sync_point.inc

# clear the sync point. The second applier must run into the real dependency
--source include/galera_clear_sync_point.inc

--let $apply_waits = `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_apply_waits'`

--connection node_1
--eval UPDATE p SET v = v + 1 WHERE id = $fbt_value

--connection node_2
# Wait until the second applier has blocked in the apply monitor because
--let $wait_condition = SELECT (SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_apply_waits') > $apply_waits
--source include/wait_condition.inc

# the second applier waited instead of applying it in parallel.
SELECT COUNT(*) = 0 AS no_row_modified_in_parallel FROM INFORMATION_SCHEMA.INNODB_TRX WHERE trx_rows_modified > 0;

# Release the child INSERT
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_signal_sync_point.inc

--let $wait_condition = SELECT COUNT(*) = 1 FROM c
--source include/wait_condition.inc
--let $wait_condition = SELECT v = 1 FROM p
--source include/wait_condition.inc

SELECT v FROM p;
SELECT COUNT(*) FROM c;

SET GLOBAL wsrep_slave_threads = DEFAULT;
SET SESSION wsrep_sync_wait = DEFAULT;

--connection node_1
DROP TABLE c, p;
