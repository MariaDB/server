let $wait_condition = SELECT 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_ready' AND VARIABLE_VALUE = 'OFF';
--source include/wsrep_wait_condition.inc
