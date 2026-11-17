# Kills, with KILL $sqlext_kill (QUERY or CONNECTION), a connection
# running TEST SQLEXT $sqlext_verb SELECT SLEEP(100), while that SQL runs.
# A killed query leaves the connection usable; a killed connection ends.
# Runs on the default connection and leaves the caller there.
connect (con1, localhost, root,,);
let $id= `SELECT CONNECTION_ID()`;
send_eval TEST SQLEXT $sqlext_verb SELECT SLEEP(100);
connection default;
let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist
  WHERE id = $id AND state = 'User sleep';
--source include/wait_condition.inc
--disable_query_log
eval KILL $sqlext_kill $id;
--enable_query_log
connection con1;
if ($sqlext_kill == QUERY)
{
  --error ER_QUERY_INTERRUPTED
  reap;
  --echo # The connection is usable afterwards
  eval TEST SQLEXT $sqlext_verb SELECT 1 AS a;
  disconnect con1;
  connection default;
}
if ($sqlext_kill == CONNECTION)
{
  --error 2013,ER_CONNECTION_KILLED
  reap;
  disconnect con1;
  connection default;
  let $wait_condition= SELECT COUNT(*) = 0 FROM information_schema.processlist
    WHERE id = $id;
  --source include/wait_condition.inc
}
