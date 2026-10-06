# Kill the statement matching the LIKE pattern $nrt_pattern (an SQL
# expression) on the remote server, and wait until it is gone. The remote
# server may have ended it already, e.g. when it noticed that the client
# closed the connection. If the statement is just being sent and may not be
# there yet, set $nrt_wait_start to wait for it first.

connection slave;
if ($nrt_wait_start)
{
  let $wait_condition=
    SELECT COUNT(*) >= 1 FROM information_schema.processlist
    WHERE info LIKE $nrt_pattern AND id != CONNECTION_ID();
  --source include/wait_condition.inc
}
let $nrt_id= `SELECT IFNULL(MAX(id), 0) FROM information_schema.processlist
              WHERE info LIKE $nrt_pattern AND id != CONNECTION_ID()`;
if ($nrt_id)
{
  --disable_query_log
  eval KILL $nrt_id;
  --enable_query_log
}
let $wait_condition=
  SELECT COUNT(*) = 0 FROM information_schema.processlist
  WHERE info LIKE $nrt_pattern AND id != CONNECTION_ID();
--source include/wait_condition.inc
let $nrt_wait_start= 0;
