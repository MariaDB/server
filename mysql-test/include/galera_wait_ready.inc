# include/galera_wait_ready.inc
#
# Waits for galera node to transition to READY state.
#
# Parameters
# ----------
# $galera_wait_ready_timeout (optional)
#   Seconds to wait before giving up, 60 by default. Raise it around a
#   restart which has to wait for a full SST, as that takes considerably
#   longer than a plain restart and on a loaded machine can exceed the
#   default. The value is reset here, so it only applies to the next
#   sourcing of this file.
#

--enable_reconnect
--disable_query_log
--disable_result_log

let $wait_counter = 600;
if ($galera_wait_ready_timeout)
{
  let $wait_counter = $($galera_wait_ready_timeout * 10);
}

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
  die "Server did not transition to READY state in $_galera_wait_ready_timeout seconds";
}
--disable_reconnect
--enable_query_log
--enable_result_log
