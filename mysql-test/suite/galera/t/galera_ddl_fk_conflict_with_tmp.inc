--echo ######################################################################
--echo # Test for $table_admin_command $table_admin_command_end
--echo ######################################################################


--connection node_1
SET SESSION wsrep_sync_wait=0;

CREATE TABLE p1 (pk INTEGER PRIMARY KEY, f2 CHAR(30));
INSERT INTO p1 VALUES (1, 'INITIAL VALUE');


CREATE TABLE c1 (pk INTEGER PRIMARY KEY, fk INTEGER, FOREIGN KEY (fk) REFERENCES p1(pk));
INSERT INTO c1 VALUES (1,1);

--echo ######################################################################
--echo #
--echo # Scenario #4: DML working on FK parent table tries to replicate, but
--echo #              fails in certification for earlier DDL on child table
--echo #              and another temporary table. TMP table should be skipped
--echo #              but FK child table should be replicated with proper keys
--echo #
--echo ######################################################################

--connection node_1
BEGIN;

# Block the applier on node #1 and issue DDL on node 2
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

--connection node_2
# wait for tables to be created in node 2 and all rows inserted as well
--let $wait_condition = SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.INNODB_SYS_TABLES WHERE NAME LIKE 'test/c1'
--source include/wait_condition.inc
--let $wait_condition = SELECT COUNT(*) = 1  FROM c1
--source include/wait_condition.inc
CREATE TEMPORARY TABLE tmp (i int);
--eval $table_admin_command TABLE c1, tmp $table_admin_command_end
DROP TABLE tmp;

--connection node_1a
--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc
--let $expected_cert_failures = `SELECT VARIABLE_VALUE+1 FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_local_cert_failures'`

--connection node_1
UPDATE p1 SET f2 = 'TO DEADLOCK' WHERE pk = 1;
--send COMMIT

--connection node_1a
--let $wait_condition = SELECT VARIABLE_VALUE = $expected_cert_failures FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_local_cert_failures'
--source include/wait_condition.inc

--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_signal_sync_point.inc

--connection node_1
--error ER_LOCK_DEADLOCK
--reap

SELECT 'I deadlocked';
SELECT COUNT(*) AS EXPECT_1 FROM p1 WHERE f2 = 'INITIAL VALUE';

--connection node_2
SELECT COUNT(*) AS EXPECT_1 FROM p1 WHERE f2 = 'INITIAL VALUE';

DROP TABLE c1;
DROP TABLE p1;
