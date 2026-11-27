# include/galera_backup_lock_window.inc
#
# Hold one write at a sync point. While it is held, node 1 takes the
# backup locks up to BACKUP STAGE START, FLUSH, BLOCK_DDL, BLOCK_COMMIT,
# or takes FLUSH TABLES WITH READ LOCK. Then release the write. Print one
# line for each lock:
#
#   <lock> <local|applier> <write>: lock:<lock outcome> write:<outcome>
#
# Lock outcomes:
#   ok                          the lock statements ended
#   <stage> waits:backup_lock   <stage> waits for the MDL of the held write
#   <stage> waits:pause         the provider pause of <stage> waits for the
#                               held write
# After a wait, ",then:<ok|error:errno>" shows the outcome when the write
# is released.
# Write outcomes are as in galera_backup_lock_matrix.inc.
# A write that commits after the lock statements ended, while the lock
# must block it, prints "BUG: ... while <lock> is held". See
# include/galera_backup_lock_bug.inc.
# The test stops if the lock and the write do not both settle: a deadlock.
#
# FTWRL takes its lock before it pauses. With BF_ABORT_MARIABACKUP,
# can_grant_lock() does not grant the backup lock to TOI and appliers, so
# a write held before its MDL deadlocks with FTWRL. That round is not run,
# and prints "BUG: ... not run, deadlock". See galera_ftwrl_bf_deadlock.
#
# With BF_ABORT_MARIABACKUP, BLOCK_COMMIT also takes its lock before it
# pauses. A held write that then waits for a backup lock that BLOCK_COMMIT
# blocks deadlocks with it: local and applied DDL, applied non-transactional
# DML. These rounds are not run, and print "BUG: ... not run, deadlock".
# See galera_backup_block_commit_bf_deadlock.
#
# Parameters:
#   $op         name of the write
#   $op_stmt    the statement
#   $op_node    1: run on node 1 (local), 2: run on node 2 (applier)
#   $op_mode    wsrep_mode to add on the node that runs the write, or empty
#   $op_sync    local: DEBUG_SYNC point to hold the write at.
#               applier: wsrep_apply_cb or wsrep_apply_toi.
#   $op_toi     1: the held write is in the provider order (applier, or
#               local TOI), so a provider pause waits for it.
#   $op_before_mdl  1: the write is held in the provider order before it
#               takes its backup lock MDL.
#
# Connections: node_1 takes the locks and $lock_id is its connection ID,
# node_1a runs local writes and $op_id is its connection ID, node_1b
# observes, node_2 runs writes for the applier on node 1.
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

--connection node_1
--let $bf_mode= `SELECT FIND_IN_SET('BF_ABORT_MARIABACKUP', @@GLOBAL.wsrep_mode) > 0`

--let $stage_i= 1
while ($stage_i <= 5)
{
  --let $skip= 0
  if ($stage_i == 5)
  {
    if ($bf_mode)
    {
      if ($op_before_mdl)
      {
        --let $skip= 1
      }
    }
  }
  if ($stage_i == 4)
  {
    if ($bf_mode)
    {
      --let $skip= `SELECT IF(FIND_IN_SET('$op_side:$op', 'local:toi_ddl,applier:toi_aria_nontrans,applier:toi_myisam,applier:toi_ddl'), 2, 0)`
    }
  }
  if ($skip == 1)
  {
    --echo BUG: FTWRL $op_side $op: not run, deadlock: FTWRL waits for the provider pause, the write waits for the backup lock
  }
  if ($skip == 2)
  {
    --echo BUG: BLOCK_COMMIT $op_side $op: not run, deadlock: BLOCK_COMMIT waits for the provider pause, the write waits for the backup lock
  }
  if (!$skip)
  {
    # The node resyncs after the previous lock asynchronously.
    --connection node_1b
    --let $wait_condition= SELECT VARIABLE_VALUE = 'Synced' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment'
    --source include/wait_condition.inc
    --disable_query_log

    # Hold the write.
    if ($op_node == 1)
    {
      --connection node_1a
      --eval SET DEBUG_SYNC= '$op_sync SIGNAL op_held WAIT_FOR op_go'
      --send_eval $op_stmt
      --connection node_1b
      SET DEBUG_SYNC= 'now WAIT_FOR op_held';
      --let $replicated= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_replicated'`
    }
    if ($op_node == 2)
    {
      --connection node_1b
      --eval SET GLOBAL debug_dbug= '+d,sync.$op_sync'
      --connection node_2
      --eval $op_stmt
      --connection node_1b
      --eval SET DEBUG_SYNC= CONCAT('now WAIT_FOR sync.', '$op_sync', '_reached')
      SET GLOBAL debug_dbug= @debug_dbug_saved;
      --let $committed= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_last_committed'`
    }

    # Take the locks one statement at a time. Stop at a statement that
    # waits for the held write.
    --let $paused= 0
    --let $lock_waits= 0
    --let $lock_outcome= ok
    --let $lock_i= 1
    --let $lock_end= $stage_i
    if ($stage_i == 5)
    {
      --let $lock_i= 5
    }
    while ($lock_i <= $lock_end)
    {
      --let $lock_stage= START
      --let $lock_stmt= BACKUP STAGE START
      if ($lock_i == 2)
      {
        --let $lock_stage= FLUSH
        --let $lock_stmt= BACKUP STAGE FLUSH
      }
      if ($lock_i == 3)
      {
        --let $lock_stage= BLOCK_DDL
        --let $lock_stmt= BACKUP STAGE BLOCK_DDL
      }
      if ($lock_i == 4)
      {
        --let $lock_stage= BLOCK_COMMIT
        --let $lock_stmt= BACKUP STAGE BLOCK_COMMIT
      }
      if ($lock_i == 5)
      {
        --let $lock_stage= FTWRL
        --let $lock_stmt= FLUSH TABLES WITH READ LOCK
      }
      --connection node_1
      --send_eval $lock_stmt

      # A pause desyncs the node before it drains the provider order.
      --connection node_1b
      --let $lock_settled= COMMAND <> 'Query' OR STATE = 'Waiting for backup lock'
      if ($op_toi)
      {
        if (!$paused)
        {
          --let $lock_settled= $lock_settled OR (SELECT VARIABLE_VALUE <> 'Synced' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment')
        }
      }
      --let $wait_condition= SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $lock_id AND ($lock_settled)
      --source include/wait_condition.inc
      --disable_query_log
      --let $lock_outcome= `SELECT CASE WHEN COMMAND <> 'Query' THEN 'ok' WHEN STATE = 'Waiting for backup lock' THEN '$lock_stage waits:backup_lock' ELSE '$lock_stage waits:pause' END FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $lock_id`
      if ($lock_outcome == ok)
      {
        --connection node_1
        --reap
        --connection node_1b
        --let $paused= `SELECT VARIABLE_VALUE <> 'Synced' FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_local_state_comment'`
        --inc $lock_i
      }
      if ($lock_outcome != ok)
      {
        --let $lock_waits= 1
        --let $lock_end= 0
      }
    }

    # Release the write. Wait until the lock statement has ended and the
    # write has ended or waits.
    --connection node_1b
    if ($op_node == 1)
    {
      SET DEBUG_SYNC= 'now SIGNAL op_go';
      # A paused provider holds a replicated write in certification.
      --let $op_waits= STATE = 'Waiting for backup lock'
      if ($paused)
      {
        --let $op_waits= $op_waits OR (SELECT VARIABLE_VALUE > $replicated FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_replicated')
      }
      --let $op_settled= SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $op_id AND (COMMAND <> 'Query' OR $op_waits)
    }
    if ($op_node == 2)
    {
      --eval SET DEBUG_SYNC= CONCAT('now SIGNAL signal.', '$op_sync')
      --let $applier_waits= SELECT COUNT(*) > 0 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE USER = 'system user' AND STATE = 'Waiting for backup lock'
      --let $applier_committed= SELECT VARIABLE_VALUE > $committed FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_last_committed'
      --let $op_settled= SELECT ($applier_waits) OR ($applier_committed)
    }
    --let $wait_condition= SELECT ($op_settled) AND (SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $lock_id AND COMMAND <> 'Query')
    if (!$lock_waits)
    {
      --let $wait_condition= $op_settled
    }
    --let $wait_timeout= 10
    --source include/wait_condition.inc
    --disable_query_log
    if (!$success)
    {
      --die Deadlock: $stage_i $op_side $op: lock:$lock_outcome
    }

    if ($lock_waits)
    {
      --connection node_1
      --error 0, ER_LOCK_DEADLOCK, ER_QUERY_INTERRUPTED, ER_UNKNOWN_COM_ERROR
      --reap
      --let $then= ok
      if ($mysql_errno)
      {
        --let $then= error:$mysql_errno
      }
      --let $lock_outcome= $lock_outcome,then:$then
      # Take the rest of the locks.
      --inc $lock_i
      while ($lock_i <= $stage_i)
      {
        if ($lock_i == 2)
        {
          BACKUP STAGE FLUSH;
        }
        if ($lock_i == 3)
        {
          BACKUP STAGE BLOCK_DDL;
        }
        if ($lock_i == 4)
        {
          BACKUP STAGE BLOCK_COMMIT;
        }
        --inc $lock_i
      }
    }

    --let $stage= START
    if ($stage_i == 2)
    {
      --let $stage= FLUSH
    }
    if ($stage_i == 3)
    {
      --let $stage= BLOCK_DDL
    }
    if ($stage_i == 4)
    {
      --let $stage= BLOCK_COMMIT
    }
    if ($stage_i == 5)
    {
      --let $stage= FTWRL
    }

    --connection node_1b
    --let $op_ended= 0
    if ($op_node == 1)
    {
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
      --let $outcome= waits:backup_lock
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

    # A write held before its MDL must wait for FLUSH and BLOCK_DDL.
    --let $bug= 0
    if ($lock_outcome == ok)
    {
      if ($outcome == committed)
      {
        --let $bug_stage= $stage
        if (!$op_before_mdl)
        {
          if ($stage == FLUSH)
          {
            --let $bug_stage= START
          }
          if ($stage == BLOCK_DDL)
          {
            --let $bug_stage= START
          }
        }
        --let $bug_op= $op
        --source include/galera_backup_lock_bug.inc
      }
    }
    if ($bug)
    {
      --echo BUG: $stage $op_side $op: lock:$lock_outcome write:$outcome while $bug_lock is held
    }
    if (!$bug)
    {
      --echo $stage $op_side $op: lock:$lock_outcome write:$outcome
    }

    # Node 1 applies all that it received before the next lock.
    --connection node_1b
    SET DEBUG_SYNC= 'RESET';
    SET SESSION wsrep_sync_wait= 1;
    --disable_result_log
    SELECT COUNT(*) FROM t_i;
    --enable_result_log
    SET SESSION wsrep_sync_wait= 0;
  }

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
