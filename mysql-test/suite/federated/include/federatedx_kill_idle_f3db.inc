# Kill the idle FederatedX connection(s) to f3db on the remote server and
# wait until they are gone. Run it on the remote ("slave") connection.

let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.processlist
  WHERE command='Sleep' AND db='f3db';
--source include/wait_condition.inc

let $id= `SELECT id FROM information_schema.processlist
          WHERE command='Sleep' AND db='f3db'`;
--disable_query_log
eval KILL $id;
--enable_query_log

let $wait_condition= SELECT COUNT(*) = 0 FROM information_schema.processlist
  WHERE db='f3db';
--source include/wait_condition.inc
