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

CREATE TABLE p1 (pk INTEGER PRIMARY KEY, f2 CHAR(30));
INSERT INTO p1 VALUES (1, 'INITIAL VALUE');


CREATE TABLE p2 (pk INTEGER PRIMARY KEY, f2 CHAR(30));
INSERT INTO p2 VALUES (1, 'INITIAL VALUE');
INSERT INTO p2 VALUES (2, 'INITIAL VALUE');

CREATE TABLE c1 (pk INTEGER PRIMARY KEY, fk INTEGER, FOREIGN KEY (fk) REFERENCES p1(pk));
INSERT INTO c1 VALUES (1,1);

CREATE TABLE c2 (pk INTEGER PRIMARY KEY, fk1 INTEGER, fk2 INTEGER, FOREIGN KEY (fk1) REFERENCES p1(pk), FOREIGN KEY (fk2) REFERENCES p2(pk));
INSERT INTO c2 VALUES (1,1,1), (2,1,2);

--connection node_1
SET AUTOCOMMIT=ON;
START TRANSACTION;

UPDATE p1 SET f2 = 'TO DEADLOCK' WHERE pk = 1;

--connection node_2
SET SESSION wsrep_sync_wait=0;
# wait for tables to be created in node 2 and all rows inserted as well
--let $wait_condition = SELECT COUNT(*) = 2 FROM INFORMATION_SCHEMA.INNODB_SYS_TABLES WHERE NAME LIKE 'test/c%'
--source include/wait_condition.inc
--let $wait_condition = SELECT COUNT(*) = 2  FROM c2
--source include/wait_condition.inc

# replicate the DDL to be tested
--eval $table_admin_command TABLE c1 $table_admin_command_end

--connection node_1
--error ER_LOCK_DEADLOCK
COMMIT;

SELECT COUNT(*) AS EXPECT_1 FROM p1 WHERE f2 = 'INITIAL VALUE';
SELECT COUNT(*) AS EXPECT_2 FROM p2 WHERE f2 = 'INITIAL VALUE';

--connection node_2
SELECT COUNT(*) AS EXPECT_1 FROM p1 WHERE f2 = 'INITIAL VALUE';
SELECT COUNT(*) AS EXPECT_2 FROM p2 WHERE f2 = 'INITIAL VALUE';

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
--eval $table_admin_command TABLE c1 $table_admin_command_end

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
SELECT COUNT(*) AS EXPECT_2 FROM p2 WHERE f2 = 'INITIAL VALUE';

--connection node_2
SELECT COUNT(*) AS EXPECT_1 FROM p1 WHERE f2 = 'INITIAL VALUE';
SELECT COUNT(*) AS EXPECT_2 FROM p2 WHERE f2 = 'INITIAL VALUE';


--echo ######################################################################
--echo #
--echo # Scenario #3: 2 DMLs working on two FK parent tables try to replicate, 
--echo #              but fails in certification for earlier DDL on child table
--echo #              which is child to both FK parents 
--echo #
--echo ######################################################################

--connection node_1
BEGIN;

--connection node_1b
BEGIN;

--connection node_1a
# Block the applier on node #1 and issue DDL on node 2
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

--connection node_2
--eval $table_admin_command TABLE c2 $table_admin_command_end

--connection node_1a
--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc
--let $expected_cert_failures = `SELECT VARIABLE_VALUE+2 FROM information_schema.global_status WHERE VARIABLE_NAME = 'wsrep_local_cert_failures'`

--connection node_1
UPDATE p1 SET f2 = 'TO DEADLOCK' WHERE pk = 1;
--send COMMIT

--connection node_1b
UPDATE p2 SET f2 = 'TO DEADLOCK' WHERE pk = 2;
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

--connection node_1b
--error ER_LOCK_DEADLOCK
--reap
SELECT 'I deadlocked';

SELECT COUNT(*) AS EXPECT_1 FROM p1 WHERE f2 = 'INITIAL VALUE';
SELECT COUNT(*) AS EXPECT_2 FROM p2 WHERE f2 = 'INITIAL VALUE';

--connection node_2
SELECT COUNT(*) AS EXPECT_1 FROM p1 WHERE f2 = 'INITIAL VALUE';
SELECT COUNT(*) AS EXPECT_2 FROM p2 WHERE f2 = 'INITIAL VALUE';

DROP TABLE c1, c2;
DROP TABLE p1, p2;
