# Include this script after a shutdown to wait until the connection
# to the server has been lost or timeout occurs.
# When you change this file you may have to chance its cousin
# wait_until_connected_again.inc

--disable_result_log
--disable_query_log
let $counter= 600;
let $mysql_errno= 0;
while (!$mysql_errno)
{
  --error 0,ER_SERVER_SHUTDOWN,ER_CONNECTION_KILLED,2002,2006,2013
  show status;

  dec $counter;
  if (!$counter)
  {
    --die Server failed to disappear
  }
  --real_sleep 0.1
}
--enable_query_log
--enable_result_log
