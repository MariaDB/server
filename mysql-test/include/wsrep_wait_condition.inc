# Helper script to allow to wait for condition on a node that may become
# non-primary. It attempts to preserve wsrep_sync_wait and wsrep_on session
# variables.
#
# We are forced to restore a global value for the session wsrep_sync_wait
# here because we can not always issue a SELECT query to obtain the original
# value and then restore it

disable_query_log;
SET SESSION wsrep_sync_wait = 8;
let $restore_wsrep_sync_wait = `SELECT @@GLOBAL.wsrep_sync_wait`;
let $restore_wsrep_on = `SELECT @@wsrep_on`;
SET SESSION wsrep_on = OFF;

--source include/wait_condition.inc

if ($restore_wsrep_on == 1)
{
  --eval SET SESSION wsrep_on = ON
}
--eval SET SESSION wsrep_sync_wait = $restore_wsrep_sync_wait

enable_query_log;
