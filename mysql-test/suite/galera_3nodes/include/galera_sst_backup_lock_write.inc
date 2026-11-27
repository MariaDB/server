# include/galera_sst_backup_lock_write.inc
#
# Run write number $item on the SST donor (node 1) while its mariabackup
# holds a backup stage. Print:
#
#   <stage> <local|applier> <write>: <outcome>
#
# Outcomes as in galera/include/galera_backup_lock_matrix.inc, and:
#   waits:table_lock  the write waits for a table lock that a local DML
#                     holds while it waits for the backup lock
# A write that commits while the stage must block it prints
# "BUG: ... committed while <lock> is held". See
# galera/include/galera_backup_lock_bug.inc.
#
# Write 1 is local TOI. The paused provider rejects it, so it sets
# $paused for the writes after it.
#
# Without BF_ABORT_MARIABACKUP, the donor does not pause and grants the
# backup lock to TOI. Local and applied ALTER TABLE at BLOCK_COMMIT then
# fail an assertion in a debug build. These writes are not run, and print
# "BUG: ... not run, ...".
# See galera.galera_sst_mariabackup_donor_ddl.
#
# Sets $blocked to 1 if the write waits for the backup lock. A local write
# that waits is reaped after the SST continues: $op_pending is 1.
#
# Connections: node_1a runs local writes and $op_id is its connection ID,
# node_1b observes, node_2 runs writes for the applier on node 1.
#

--disable_query_log
--disable_connect_log

--let $op_mode=
if ($item == 1)
{
  --let $op_node= 1
  --let $op= toi_ddl
  --let $op_com= Com_alter_table
  --let $op_stmt= ALTER TABLE t_i COMMENT 'local'
}
if ($item == 2)
{
  --let $op_node= 1
  --let $op= toi_grant
  --let $op_com= Com_grant
  --let $op_stmt= GRANT SELECT (a) ON test.t_i TO u1
}
if ($item == 3)
{
  --let $op_node= 1
  --let $op= toi_aria_trans
  --let $op_com= Com_insert
  --let $op_stmt= INSERT INTO t_at VALUES (NULL)
  --let $op_mode= REPLICATE_ARIA
}
if ($item == 4)
{
  --let $op_node= 1
  --let $op= toi_myisam
  --let $op_com= Com_insert
  --let $op_stmt= INSERT INTO t_m VALUES (NULL)
  --let $op_mode= REPLICATE_MYISAM
}
if ($item == 5)
{
  --let $op_node= 2
  --let $op= innodb
  --let $op_stmt= INSERT INTO t_i VALUES (NULL)
}
if ($item == 6)
{
  --let $op_node= 2
  --let $op= toi_ddl
  --let $op_stmt= ALTER TABLE t_i COMMENT 'applier'
}
if ($item == 7)
{
  --let $op_node= 2
  --let $op= toi_grant
  --let $op_stmt= GRANT SELECT (a) ON test.t_i TO u2
}
if ($item == 8)
{
  --let $op_node= 2
  --let $op= toi_aria_trans
  --let $op_stmt= INSERT INTO t_at VALUES (NULL)
  --let $op_mode= REPLICATE_ARIA
}
if ($item == 9)
{
  --let $op_node= 2
  --let $op= toi_myisam
  --let $op_stmt= INSERT INTO t_m VALUES (NULL)
  --let $op_mode= REPLICATE_MYISAM
}

--let $op_skip= 0
if ($op == toi_ddl)
{
  if ($sst_stage == BLOCK_COMMIT)
  {
    --connection node_1
    --let $op_skip= `SELECT FIND_IN_SET('BF_ABORT_MARIABACKUP', @@GLOBAL.wsrep_mode) = 0`
  }
}
if ($op_skip)
{
  --let $op_side= local
  if ($op_node == 2)
  {
    --let $op_side= applier
  }
  --echo BUG: $sst_stage $op_side $op: not run, ALTER TABLE fails an assertion in backup_log_ddl()
}
if (!$op_skip)
{
  --let $op_side= local
  --connection node_1
  if ($op_node == 2)
  {
    --let $op_side= applier
    --connection node_2
  }
  if ($op_mode)
  {
    --let $op_mode_saved= `SELECT @@GLOBAL.wsrep_mode`
    --eval SET GLOBAL wsrep_mode= CONCAT_WS(',', NULLIF(@@GLOBAL.wsrep_mode, ''), '$op_mode')
  }

  if ($op_node == 1)
  {
    --connection node_1b
    --let $count= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = '$op_com'`
    --connection node_1a
    --send_eval $op_stmt

    # The write has started when its Com_xxx counter has increased.
    --connection node_1b
    --let $wait_condition= SELECT VARIABLE_VALUE > $count FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = '$op_com'
    --source include/wait_condition.inc
    --disable_query_log
    --let $wait_condition= SELECT COUNT(*) = 1 FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $op_id AND (COMMAND <> 'Query' OR STATE IN ('Waiting for backup lock', 'Waiting for table level lock'))
    --source include/wait_condition.inc
    --disable_query_log
    --let $outcome= `SELECT CASE WHEN COMMAND <> 'Query' THEN 'ended' WHEN STATE = 'Waiting for backup lock' THEN 'waits:backup_lock' ELSE 'waits:table_lock' END FROM INFORMATION_SCHEMA.PROCESSLIST WHERE ID = $op_id`
    --let $op_ended= 0
    if ($outcome == ended)
    {
      --let $op_ended= 1
      --connection node_1a
      --disable_result_log
      --error 0, ER_UNKNOWN_COM_ERROR
      --reap
      --enable_result_log
      --let $outcome= committed
      if ($mysql_errno)
      {
        --let $outcome= error:$mysql_errno
      }
      if ($item == 1)
      {
        --let $paused= `SELECT $mysql_errno = 1047`
      }
    }
    if (!$op_ended)
    {
      --let $blocked= 1
      --let $op_pending= 1
      --let $op_pending_line= $sst_stage $op_side $op: $outcome
    }
  }

  if ($op_node == 2)
  {
    --connection node_1b
    --let $received= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_received'`
    --let $committed= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_last_committed'`
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
      --let $blocked= 1
    }
    if (`$applier_committed`)
    {
      --let $outcome= committed
    }
  }

  if ($op_mode)
  {
    --connection node_1
    if ($op_node == 2)
    {
      --connection node_2
    }
    --eval SET GLOBAL wsrep_mode= '$op_mode_saved'
  }

  if (!$op_pending)
  {
    --let $bug= 0
    if ($outcome == committed)
    {
      --let $bug_stage= $sst_stage
      --let $bug_op= $op
      --source suite/galera/include/galera_backup_lock_bug.inc
    }
    if ($bug)
    {
      --echo BUG: $sst_stage $op_side $op: committed while $bug_lock is held
    }
    if (!$bug)
    {
      --echo $sst_stage $op_side $op: $outcome
    }
  }
}

--enable_connect_log
--enable_query_log
