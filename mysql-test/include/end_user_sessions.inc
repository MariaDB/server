# include file: end the sessions of a user, and wait until they are gone.
#
# Usage:
#   --let $wait_user= user_name
#   --source include/end_user_sessions.inc
#
# A busy session of a client that is gone, like the dump thread of a slave,
# ends by itself only with the disconnect_watcher ON, so it is killed.

--disable_query_log
--eval KILL USER $wait_user
--enable_query_log
let $wait_condition= SELECT COUNT(*)=0 FROM information_schema.processlist
  WHERE user='$wait_user';
--source include/wait_condition.inc
