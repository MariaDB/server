# include/galera_backup_unlock_resume.inc
#
# The unlocking statement on node_1 is held at its sync point, and the
# INSERT on node_1a waits for the backup lock or has ended. Give the
# INSERT up to 5 seconds to end, let the unlocking statement continue,
# then print the outcome of the INSERT.
#
# Parameters:
#   $unlock_stmt  the unlocking statement, for the output
#   $op_id        connection ID of node_1a
#
# Connections: node_1b runs this include.
#

--disable_query_log
--let $ur_i= 50
while ($ur_i)
{
  --let $ur_i= `SELECT IF(COMMAND <> 'Query', 0, $ur_i - 1) FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $op_id`
  if ($ur_i)
  {
    --real_sleep 0.1
  }
}
SET DEBUG_SYNC= 'now SIGNAL resume';
--connection node_1
--reap
--connection node_1a
--disable_result_log
--error 0, ER_UNKNOWN_COM_ERROR
--reap
--enable_result_log
if ($mysql_errno)
{
  --echo BUG: the INSERT failed with ER_UNKNOWN_COM_ERROR: $unlock_stmt released the backup lock before it resumed the provider
}
if (!$mysql_errno)
{
  --echo INSERT committed
}
--enable_query_log
