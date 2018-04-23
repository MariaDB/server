# include/galera_wait_ready.inc
#
# Waits for galera node to transition to READY state.
#

--enable_reconnect
--disable_query_log
--disable_result_log
let $wait_counter = 600;
while ($wait_counter)
{
  --disable_abort_on_error
  let $success = `SELECT VARIABLE_VALUE = 'ON' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_ready'`;
  --enable_abort_on_error
  if ($success)
  {
    let $wait_counter = 0;
  }
  if (!$success)
  {
     real_sleep 0.1;
     dec $wait_counter;
  }
}

if (!$success)
{
  die "Server did not transition to READY state";
}
--disable_reconnect
--enable_query_log
--enable_result_log
