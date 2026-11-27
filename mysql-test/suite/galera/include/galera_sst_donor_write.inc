# include/galera_sst_donor_write.inc
#
# Send a write on the SST donor while its mariabackup is held after
# $op_stage, and wait up to 5 seconds for it to end. Print one line:
#
#   <stage> local <write>: waits
#   BUG: <stage> local <write>: committed while ...
#   BUG: <stage> local <write>: the SST donor crashed
#
# After a crash, also print the assertion found in the error log of the
# donor. A write that waits is reaped after the SST: $op_pending is 1.
#
# Parameters:
#   $op_conn   connection that runs the write
#   $op_stage  the backup stage that mariabackup holds
#   $op        name of the write
#   $op_stmt   the statement
#   $op_com    Com_xxx status variable of the statement
#
# Sets $outcome to waits, committed, error:<errno> or crashed.
# Connections: node_1b observes.
#

--connection $op_conn
SET SESSION wsrep_sync_wait= 0;
--let $op_id= `SELECT CONNECTION_ID()`
--let $op_count= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = '$op_com'`
--send_eval $op_stmt

# The write runs until its Com_xxx counter has increased and its
# connection is idle. Check with one statement, so that a crash between
# two statements does not stop the test: the scalar subquery fails with
# ER_SUBQUERY_NO_1_ROW when the write has ended.
--connection node_1b
--disable_query_log
--let $op_running= (SELECT COUNT(*) FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $op_id AND COMMAND = 'Query') + (SELECT VARIABLE_VALUE = $op_count FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = '$op_com') > 0
--let $outcome= waits
--let $op_i= 50
while ($op_i)
{
  --disable_result_log
  --error 0, ER_SUBQUERY_NO_1_ROW, 2006, 2013, 2026
  --eval SELECT (SELECT 1 FROM DUAL WHERE $op_running UNION ALL SELECT 2 FROM DUAL WHERE NOT ($op_running) UNION ALL SELECT 3 FROM DUAL WHERE NOT ($op_running))
  --enable_result_log
  if ($mysql_errno)
  {
    --let $outcome= crashed
    if ($mysql_errno == 1242)
    {
      --let $outcome= ended
    }
    --let $op_i= 0
  }
  if ($op_i)
  {
    --real_sleep 0.1
    --dec $op_i
  }
}
--enable_query_log

--let $op_pending= 0
if ($outcome == waits)
{
  --let $op_pending= 1
  --echo $op_stage local $op: waits
}
if ($outcome == ended)
{
  --connection $op_conn
  --disable_result_log
  --error 0, ER_UNKNOWN_COM_ERROR
  --reap
  --enable_result_log
  --let $outcome= committed
  if ($mysql_errno)
  {
    --let $outcome= error:$mysql_errno
    --echo $op_stage local $op: $outcome
  }
  if (!$mysql_errno)
  {
    --echo BUG: $op_stage local $op: committed while BACKUP STAGE $op_stage is held on the SST donor
  }
  --connection node_1b
}
if ($outcome == crashed)
{
  --echo BUG: $op_stage local $op: the SST donor crashed
  --let SEARCH_FILE= $MYSQLTEST_VARDIR/log/mysqld.1.err
  --let SEARCH_PATTERN= Assertion .backup_stage <= BACKUP_WAIT_FOR_FLUSH
  --source include/search_pattern_in_file.inc
}
