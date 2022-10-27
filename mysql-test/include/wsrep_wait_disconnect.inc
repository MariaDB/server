let $wait_condition = SELECT 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_ready' AND VARIABLE_VALUE = 'OFF';
# since this is called until AFTER provider disconnects,we need to allow
# queries in non-prim
#
# We are also forced to use a hard-coded value for wsrep_sync_wait here because
# we can not issue a SELECT query to obtain the original value and then restore
# it
disable_query_log;
SET SESSION wsrep_sync_wait = 7;
--let $restore_wsrep_on = `SHOW VARIABLES WHERE Variable_name = 'wsrep_on' AND Value = 'ON'`
SET SESSION wsrep_on = OFF;

--source include/wait_condition.inc

if ($restore_wsrep_on != "")
{
  --eval SET SESSION wsrep_on = ON
}
SET SESSION wsrep_sync_wait = 15;
enable_query_log;
