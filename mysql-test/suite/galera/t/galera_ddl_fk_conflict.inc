#
# Test for MDL BF-BF lock conflict
# There are some DDL statements, which take extensive MDL lock for
# a table referenced by foreign key constraint from the actual affetec table.
# This extensive MDL lock may cause MDL BF-BF confclict situations, if the
# FK parent table is not listed as certification key in the replication write set.
# i.e. if replication allows such DDL to apply in parallel with regular DML operating
# on the FK parent table.
#
# This test has two scenarios, where DML modifies FK parent table in node 1,
# and offending DDL for FK child table is sent from node 2.
#
# param: $table_admin_command
#        DDL table command to test, script will build full SQL statement:
#        $table_admin_command TABLE c;
#
# param: $table_admin_command_end
#        Optional additional SQL syntax to end the SQL statement, if any
#        $table_admin_command TABLE c $table_admin_command_end;
#
# scenario 1, can be used to test if a DDL statement causes such MDL locking vulnerability.
# call this test script with some table DDL command in $table_admin_command
# if scenario 1 passes (especially COMMIT does fail for ER_LOCK_DEADLOCK),
# then this particular DDL is vulnerable. scenraio 2 should fail for this DDL
# unless code has not been fixed to append parent table certification keys for it.
#

--echo ######################################################################
--echo # Test for $table_admin_command $table_admin_command_end
--echo ######################################################################


--echo ######################################################################
--echo #
--echo # Scenario #1: DML working on FK parent table BF aborted by DDL
--echo #              over child table
--echo #
--echo ######################################################################

--connection node_1
SET SESSION wsrep_sync_wait=0;

CREATE TABLE p (pk INTEGER PRIMARY KEY, f2 CHAR(30));
INSERT INTO p VALUES (1, 'INITIAL VALUE');
INSERT INTO p VALUES (2, 'INITIAL VALUE');

CREATE TABLE c (pk INTEGER PRIMARY KEY, fk INTEGER, FOREIGN KEY (fk) REFERENCES p(pk));
INSERT INTO c VALUES (1,1), (2,2);

--connection node_1
SET AUTOCOMMIT=ON;
START TRANSACTION;

UPDATE p SET f2 = 'TO DEADLOCK' WHERE pk = 1;

--connection node_2
SET SESSION wsrep_sync_wait=0;
--eval $table_admin_command TABLE c $table_admin_command_end

--connection node_1
--error ER_LOCK_DEADLOCK
COMMIT;

SELECT COUNT(*) AS EXPECT_2 FROM p WHERE f2 = 'INITIAL VALUE';

--connection node_2
SELECT COUNT(*) AS EXPECT_2 FROM p WHERE f2 = 'INITIAL VALUE';

--echo ######################################################################
--echo #
--echo # Scenario #2: DML working on FK parent table tries to replicate, but 
--echo #              fails in certification for earlier DDL on child table
--echo #
--echo ######################################################################

--connection node_1
BEGIN;

# Block the applier on node #1 and issue DDL on node 2
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

--connection node_2
--eval $table_admin_command TABLE c $table_admin_command_end

--connection node_1a
--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc
--let $expected_cert_failures = `SELECT VARIABLE_VALUE+1 FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_local_cert_failures'`

--connection node_1
UPDATE p SET f2 = 'TO DEADLOCK' WHERE pk = 1;
--send COMMIT

--connection node_1a
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_signal_sync_point.inc

--let $wait_condition = SELECT VARIABLE_VALUE = $expected_cert_failures FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_local_cert_failures'
--source include/wait_condition.inc

--connection node_1
--error ER_LOCK_DEADLOCK
--reap

SELECT 'I deadlocked';

SELECT COUNT(*) AS EXPECT_2 FROM p WHERE f2 = 'INITIAL VALUE';

--connection node_2
SELECT COUNT(*) AS EXPECT_2 FROM p WHERE f2 = 'INITIAL VALUE';

DROP TABLE c;
DROP TABLE p;
