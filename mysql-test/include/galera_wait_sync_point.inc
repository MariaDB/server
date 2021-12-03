--let $wait_timeout = 10
--let $wsrep_on_orig = `SELECT @@wsrep_on`
SET SESSION wsrep_on = 0;

#
# following is only for debugging purposes
# should be commented out when test wporks as planned
#
#--sleep 1
# SHOW PROCESSLIST;
#SHOW STATUS LIKE 'wsrep_%';
#--echo $galera_sync_point

--let $wait_condition = SELECT 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_debug_sync_waiters' AND VARIABLE_VALUE = '$galera_sync_point'
--source include/wait_condition.inc
--eval SET SESSION wsrep_on = $wsrep_on_orig

