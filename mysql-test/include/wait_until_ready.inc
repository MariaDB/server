# If wsrep patch is enabled, wait for a minute until node is ready.
# Note: include/wait_for_status_var.inc cannot be used here, as server rejects
# all commands except SHOW & SET until its ready. (see wsrep_ready status
# variable)

--disable_result_log
--disable_query_log
--enable_reconnect

let $counter= 600;

# Check if wsrep_ready status variable exists.
if (`SHOW STATUS LIKE 'wsrep_ready'`)
{
  let $wsrep_ready= query_get_value("SHOW STATUS LIKE 'wsrep_ready'", Value, 1);

  while ($wsrep_ready == 'OFF')
  {
    if (!$counter)
    {
      echo ===============================================;
      echo Node still not ready after a minute, giving up!;
      echo ===============================================;
      die;
    }
    dec $counter;
    sleep 0.1;
    let $wsrep_ready= query_get_value("SHOW STATUS LIKE 'wsrep_ready'", Value, 1);
  }
}

--disable_reconnect
--enable_query_log
--enable_result_log
