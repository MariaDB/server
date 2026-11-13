# Starts a transaction on node_2 that inserts row 1, then sends $bf_stmt,
# which must sleep ("User sleep"), and has node_1 insert a conflicting
# row 1: node_2's transaction is aborted while $bf_stmt runs. Shows what
# node_2 gets and what is left. Needs t1 (f1 INT PRIMARY KEY, f2 CHAR(10))
# and the connection node_2a; leaves t1 empty, on node_1.
--connection node_2
--let $bf_aborts_before= `SELECT VARIABLE_VALUE FROM information_schema.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_bf_aborts'`
BEGIN;
INSERT INTO t1 VALUES (1, 'node_2');
--send_eval $bf_stmt
--connection node_2a
--let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist WHERE state = 'User sleep'
--source include/wait_condition.inc
--connection node_1
INSERT INTO t1 VALUES (1, 'node_1');
--connection node_2
--error ER_LOCK_DEADLOCK
--reap
--let $wait_condition= SELECT COUNT(*) = 1 FROM t1 WHERE f2 = 'node_1'
--source include/wait_condition.inc
--echo # The transaction is gone: node_1's row is there
SELECT @@in_transaction;
SELECT * FROM t1 ORDER BY f1;
--disable_query_log
--eval SELECT VARIABLE_VALUE - $bf_aborts_before AS bf_aborts FROM information_schema.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_bf_aborts'
--enable_query_log
--connection node_1
DELETE FROM t1;
--connection node_2a
--let $wait_condition= SELECT COUNT(*) = 0 FROM t1
--source include/wait_condition.inc
--connection node_1
