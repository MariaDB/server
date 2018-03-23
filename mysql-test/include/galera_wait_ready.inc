
--disable_result_log
--disable_query_log
let $counter= 100;
let $not_synced= 1;
while ($not_synced)
{
  --error 0,1205,ER_SERVER_SHUTDOWN,ER_CONNECTION_KILLED,2002,2006,2013
  if (`SELECT variable_value=4 from information_schema.global_status where variable_name='wsrep_local_state'`)
  {
        let $not_synced = 0;
  }

  dec $counter;
  if (!$counter)
  {
    SHOW STATUS LIKE 'wsrep%';
    --die Server failed to sync in time
  }
  --sleep 5
}

--enable_query_log
--enable_result_log

let $wait_condition = SELECT 1 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_ready' AND VARIABLE_VALUE = 'ON';
--source include/wait_condition.inc
