# include/galera_backup_lock_matrix.inc
#
# Run one write while node 1 holds each backup lock: BACKUP STAGE START,
# FLUSH, BLOCK_DDL, BLOCK_COMMIT and FLUSH TABLES WITH READ LOCK. Print
# one line for each lock:
#
#   <lock> <paused|synced> <local|applier> <write>: <outcome>
#
# paused: node 1 is desynced and the provider is paused.
# Outcomes on node 1:
#   committed             the write committed while the lock was held
#   error:<errno>         the write failed
#   waits:backup_lock     the write waits for the backup lock
#   waits:certification   the write waits for certification (paused)
#   held:pause            the write set is received but not applied (paused)
# After a wait, ",then:<committed|error:errno>" shows the outcome when the
# lock is released.
# A write that commits, or fails in commit, while the lock must block it
# prints "BUG: ... while <lock> is held". See
# include/galera_backup_lock_bug.inc.
#
# Parameters:
#   $op         name of the write
#   $op_stmt    the statement
#   $op_com     Com_xxx status variable of the statement (local writes)
#   $op_node    1: run on node 1 (local), 2: run on node 2 (applier)
#   $op_mode    wsrep_mode to add on the node that runs the write, or empty
#
# wait_condition.inc enables the query log; it is disabled again after it.
#
# Connections: node_1 holds the lock, node_1a runs local writes and
# $op_id is its connection ID, node_1b observes, node_2 runs writes for
# the applier on node 1.
#

--disable_query_log
--disable_connect_log

--let $op_side= local
if ($op_node == 2)
{
  --let $op_side= applier
}

if ($op_mode)
{
  if ($op_node == 1)
  {
    --connection node_1
  }
  if ($op_node == 2)
  {
    --connection node_2
  }
  --let $op_mode_saved= `SELECT @@GLOBAL.wsrep_mode`
  --eval SET GLOBAL wsrep_mode= CONCAT_WS(',', NULLIF(@@GLOBAL.wsrep_mode, ''), '$op_mode')
}

--let $stage_i= 1
while ($stage_i <= 5)
{
  # The node resyncs after the previous lock asynchronously.
  --connection node_1b
  --let $wait_condition= SELECT VARIABLE_VALUE = 'Synced' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment'
  --source include/wait_condition.inc
  --disable_query_log

  --connection node_1
  if ($stage_i < 5)
  {
    BACKUP STAGE START;
    --let $stage= START
    if ($stage_i >= 2)
    {
      BACKUP STAGE FLUSH;
      --let $stage= FLUSH
    }
    if ($stage_i >= 3)
    {
      BACKUP STAGE BLOCK_DDL;
      --let $stage= BLOCK_DDL
    }
    if ($stage_i >= 4)
    {
      BACKUP STAGE BLOCK_COMMIT;
      --let $stage= BLOCK_COMMIT
    }
  }
  if ($stage_i == 5)
  {
    FLUSH TABLES WITH READ LOCK;
    --let $stage= FTWRL
  }

  # Desync is done before the lock statement returns.
  --connection node_1b
  --let $paused= `SELECT VARIABLE_VALUE <> 'Synced' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment'`
  --let $state= synced
  if ($paused)
  {
    --let $state= paused
  }
  --let $received= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_received'`
  --let $committed= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_last_committed'`
  --let $outcome=
  --let $then=
  --let $op_ended= 0

  if ($op_node == 1)
  {
    --let $count= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = '$op_com'`
    --let $replicated= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_replicated'`
    --connection node_1a
    --send_eval $op_stmt

    # The write has started when its Com_xxx counter has increased.
    --connection node_1b
    --let $wait_condition= SELECT VARIABLE_VALUE > $count FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = '$op_com'
    --source include/wait_condition.inc
    --disable_query_log
    # A paused provider holds a replicated write in certification. The
    # write set is replicated before certification.
    --let $settled= COMMAND <> 'Query' OR STATE = 'Waiting for backup lock'
    if ($paused)
    {
      --let $settled= $settled OR (SELECT VARIABLE_VALUE > $replicated FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_replicated')
    }
    --let $wait_condition= SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $op_id AND ($settled)
    --source include/wait_condition.inc
    --disable_query_log
    --let $outcome= `SELECT CASE WHEN COMMAND <> 'Query' THEN 'ended' WHEN STATE = 'Waiting for backup lock' THEN 'waits:backup_lock' ELSE 'waits:certification' END FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $op_id`

    if ($outcome == ended)
    {
      --let $op_ended= 1
      --connection node_1a
      --disable_result_log
      --error 0, ER_UNKNOWN_COM_ERROR, ER_ERROR_DURING_COMMIT, ER_LOCK_DEADLOCK, ER_QUERY_INTERRUPTED, ER_LOCK_WAIT_TIMEOUT
      --reap
      --enable_result_log
      --let $outcome= committed
      if ($mysql_errno)
      {
        --let $outcome= error:$mysql_errno
      }
    }
  }

  if ($op_node == 2)
  {
    --connection node_2
    --eval $op_stmt

    --connection node_1b
    --let $applier_waits= SELECT COUNT(*) > 0 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE USER = 'system user' AND STATE = 'Waiting for backup lock'
    --let $applier_committed= SELECT VARIABLE_VALUE > $committed FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_last_committed'
    # A paused provider does not apply the write set after it is received.
    --let $wait_condition= SELECT ($applier_waits) OR ($applier_committed)
    if ($paused)
    {
      --let $wait_condition= SELECT VARIABLE_VALUE > $received FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_received'
    }
    --source include/wait_condition.inc
    --disable_query_log
    --let $outcome= held:pause
    if (`$applier_waits`)
    {
      --let $outcome= waits:backup_lock
    }
    if (`$applier_committed`)
    {
      --let $outcome= committed
    }
  }

  --connection node_1
  if ($stage_i < 5)
  {
    BACKUP STAGE END;
  }
  if ($stage_i == 5)
  {
    UNLOCK TABLES;
  }

  if ($op_node == 1)
  {
    if (!$op_ended)
    {
      --connection node_1a
      --disable_result_log
      --error 0, ER_UNKNOWN_COM_ERROR, ER_ERROR_DURING_COMMIT, ER_LOCK_DEADLOCK, ER_QUERY_INTERRUPTED, ER_LOCK_WAIT_TIMEOUT
      --reap
      --enable_result_log
      --let $then= committed
      if ($mysql_errno)
      {
        --let $then= error:$mysql_errno
      }
      --let $outcome= $outcome,then:$then
    }
  }

  # ER_ERROR_DURING_COMMIT: the commit ran and failed.
  --let $bug= 0
  --let $bug_check= 0
  if ($outcome == committed)
  {
    --let $bug_check= 1
  }
  if ($outcome == error:1180)
  {
    --let $bug_check= 1
  }
  if ($bug_check)
  {
    --let $bug_stage= $stage
    --let $bug_op= $op
    --source include/galera_backup_lock_bug.inc
  }
  if ($bug)
  {
    --echo BUG: $stage $state $op_side $op: $outcome while $bug_lock is held
  }
  # ER_UNKNOWN_COM_ERROR after the wait: the unlocking statement released
  # the backup lock before it resumed the provider.
  if ($then == error:1047)
  {
    --let $bug= 1
    --echo BUG: $stage $state $op_side $op: $outcome: the unlock released the backup lock before it resumed the provider
  }
  if (!$bug)
  {
    --echo $stage $state $op_side $op: $outcome
  }

  # Node 1 applies all that it received before the next lock.
  --connection node_1b
  SET SESSION wsrep_sync_wait= 1;
  --disable_result_log
  SELECT COUNT(*) FROM t_i;
  --enable_result_log
  SET SESSION wsrep_sync_wait= 0;

  --inc $stage_i
}

if ($op_mode)
{
  if ($op_node == 1)
  {
    --connection node_1
  }
  if ($op_node == 2)
  {
    --connection node_2
  }
  --eval SET GLOBAL wsrep_mode= '$op_mode_saved'
}

--let $op_mode=
--enable_connect_log
--enable_query_log
