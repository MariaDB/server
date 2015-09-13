let $wait_timeout = 10;
SET SESSION wsrep_on = 0;
--let $wait_condition = SELECT 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_debug_sync_waiters' AND VARIABLE_VALUE = '$galera_sync_point'
--source include/wait_condition.inc
SET SESSION wsrep_on = 1;
