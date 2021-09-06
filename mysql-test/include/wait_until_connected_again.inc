#
# Include this script to wait until the connection to the
# server has been restored or timeout occurs.
# You should have done --enable_reconnect first
# When you change this file you may have to chance its cousin
# wait_until_disconnected.inc

--disable_result_log
--disable_query_log
let $counter= 5000;
let $mysql_errno= 9999;
while ($mysql_errno)
{
  --error 0,ER_ACCESS_DENIED_ERROR,ER_SERVER_SHUTDOWN,ER_CONNECTION_KILLED,ER_LOCK_WAIT_TIMEOUT,2002,2006,2013
  select 1;

  dec $counter;
  if (!$counter)
  {
    --die Server failed to restart
  }
  if (!$mysql_errno)
  {
    if (!$NO_WSREP)
    {
      # WSREP: SHOW STATUS queries are allowed even if wsrep
      #        is not ready. Make sure wsrep is ready before
      #        returning from this script
      source include/wait_wsrep_ready.inc;
    }
  }
  if ($mysql_errno == 1045)
  {
    let $mysql_errno=0;
  }
  if ($mysql_errno > 0)
  {
    --sleep 0.1
  }
}
--enable_query_log
--enable_result_log
