#
# Commit @@skip_replication=1 changes on the master and check that semi-sync
# neither requested, granted nor missed an ACK for them, while an ordinary
# transaction committed right after is still ACKed.
#
# Both paths which register a transaction for an ACK are exercised: CREATE
# DATABASE takes the direct binlog write path, the InnoDB transactions take the
# group commit path, including a group which mixes a skip_replication and an
# ordinary transaction.
#
# Parameters:
#   $wait_point (string)  rpl_semi_sync_master_wait_point to use
#   $skip_mode (string)   replicate_events_marked_for_skip to use on the slave
#
# Requirements:
#   master-slave.inc with semi-sync enabled on the master, table t1 on the
#   master, connections con_skip (with @@skip_replication=1) and con_normal to
#   the master, and the slave stopped with semi-sync enabled. The slave is left
#   stopped on the way out, so this file can be sourced repeatedly.
#

--echo #
--echo # Wait point $wait_point, slave $skip_mode
--echo #

--connection master
--eval SET GLOBAL rpl_semi_sync_master_wait_point= $wait_point

--connection slave
--eval SET GLOBAL replicate_events_marked_for_skip= $skip_mode
--source include/start_slave.inc

--connection master
--let $status_var= Rpl_semi_sync_master_clients
--let $status_var_value= 1
--source include/wait_for_status_var.inc
--let $status_var= Rpl_semi_sync_master_status
--let $status_var_value= ON
--source include/wait_for_status_var.inc

--let $ack_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_request_ack', Value, 1)
--let $yes_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
--let $no_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_no_tx', Value, 1)

SET SESSION skip_replication= 1;
CREATE DATABASE db_skip;
CREATE TABLE db_skip.t1 (a INT) ENGINE=InnoDB;
INSERT INTO db_skip.t1 VALUES (1);
DROP DATABASE db_skip;
SET SESSION skip_replication= 0;

--let $ss= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_status', Value, 1)
if (`SELECT strcmp("$ss","ON") != 0`)
{
  --die semi-sync switched off: the master awaited an ACK for a skip_replication transaction
}
--let $ack_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_request_ack', Value, 1)
--let $yes_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
--let $no_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_no_tx', Value, 1)
if (`SELECT $ack_after <> $ack_before OR $yes_after <> $yes_before OR $no_after <> $no_before`)
{
  --echo request_ack $ack_before -> $ack_after, yes_tx $yes_before -> $yes_after, no_tx $no_before -> $no_after
  --die an ACK was requested or accounted for a skip_replication transaction
}
--echo # No ACK was requested, granted or missed for the skip_replication changes

#
# A skip_replication and an ordinary transaction in one binlog group commit:
# the leader registers and, for AFTER_SYNC, awaits the ACKs of the whole group,
# so exactly one ACK must be requested and granted whichever of the two leads.
#
--let $round= 2
while ($round)
{
  if ($round == 2)
  {
    --let $leader= con_skip
    --let $follower= con_normal
  }
  if ($round == 1)
  {
    --let $leader= con_normal
    --let $follower= con_skip
  }
  --echo # Group commit led by $leader, joined by $follower

  --connection master
  --let $groups_before= query_get_value(SHOW STATUS LIKE 'Binlog_group_commits', Value, 1)
  --let $ack_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_request_ack', Value, 1)
  --let $yes_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)

  --connection $leader
  SET debug_sync= 'commit_after_get_LOCK_log SIGNAL leader_ready WAIT_FOR leader_go';
  --send INSERT INTO t1 VALUES (2)

  --connection master
  SET debug_sync= 'now WAIT_FOR leader_ready';

  --connection $follower
  SET debug_sync= 'commit_after_release_LOCK_prepare_ordered SIGNAL follower_queued';
  --send INSERT INTO t1 VALUES (2)

  --connection master
  SET debug_sync= 'now WAIT_FOR follower_queued';
  SET debug_sync= 'now SIGNAL leader_go';

  --connection $leader
  --reap
  --connection $follower
  --reap

  --connection master
  SET debug_sync= 'RESET';
  --let $groups_after= query_get_value(SHOW STATUS LIKE 'Binlog_group_commits', Value, 1)
  if (`SELECT $groups_after <> $groups_before + 1`)
  {
    --die the two transactions were not committed in one binlog group commit
  }
  --let $ss= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_status', Value, 1)
  if (`SELECT strcmp("$ss","ON") != 0`)
  {
    --die semi-sync switched off: the master awaited an ACK for a skip_replication transaction
  }
  --let $ack_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_request_ack', Value, 1)
  --let $yes_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
  --let $no_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_no_tx', Value, 1)
  if (`SELECT $ack_after <> $ack_before + 1 OR $yes_after <> $yes_before + 1 OR $no_after <> $no_before`)
  {
    --echo request_ack $ack_before -> $ack_after, yes_tx $yes_before -> $yes_after, no_tx $no_before -> $no_after
    --die the group did not request and get exactly one ACK, for the ordinary transaction
  }
  --echo # Exactly one ACK was requested and granted, for the ordinary transaction
  --dec $round
}

--connection master
--let $yes_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
INSERT INTO t1 VALUES (1);
--let $yes_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
if (`SELECT $yes_after <> $yes_before + 1`)
{
  --echo yes_tx $yes_before -> $yes_after
  --die the ordinary transaction after the skip_replication changes was not ACKed
}
--echo # The ordinary transaction which followed was ACKed

# It is also the last event, so that the slave can sync to it even when the
# master filters away the skip_replication events before it.
--source include/save_master_gtid.inc

--connection slave
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
