#
# Check how semi-sync accounts for transactions and events which are marked
# with @@skip_replication, at the wait point configured by the caller.
#
# Whether such an event is really filtered away is decided by the slave,
# through --replicate-events-marked-for-skip: only FILTER_ON_MASTER makes the
# master's dump thread drop the event before sending it. A slave using the
# default (REPLICATE) receives such events and ACKs them, and each connected
# slave may use a different value.
#
#   Case 1 (mixed filtering) server_2 filters, server_3 replicates. server_3
#           receives and ACKs the transaction, so semi-sync must behave exactly
#           as it does for any other transaction.
#   Case 2 (all slaves filter) both slaves filter. No connected slave can ever
#           ACK the transaction, so the master must neither await an ACK nor
#           account for one. Before MDEV-38486 it blocked until
#           rpl_semi_sync_master_timeout expired and degraded to asynchronous.
#
# Both cases exercise the two paths which register a transaction for an ACK:
# CREATE DATABASE takes the direct binlog write path, the InnoDB transaction
# takes the group commit leader path.
#
# Parameters:
#   $wait_point (string)  The rpl_semi_sync_master_wait_point in use. Only used
#                         to label the test output.
#
# Requirements:
#   Topology 1->2,1->3 with semi-sync enabled on all three servers, server_2
#   running with replicate_events_marked_for_skip=FILTER_ON_MASTER and server_3
#   with the default REPLICATE. server_3 is restored to REPLICATE on the way
#   out, so this file can be sourced repeatedly.
#

--echo #
--echo # Wait point: $wait_point
--echo #

--connection server_3
SET @skip_before= @@global.replicate_events_marked_for_skip;

--connection server_1
--let $status_var= Rpl_semi_sync_master_clients
--let $status_var_value= 2
--source include/wait_for_status_var.inc

--let $ss= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_status', Value, 1)
if (`SELECT strcmp("$ss","ON") != 0`)
{
  SHOW STATUS LIKE 'Rpl_semi_sync_master_status';
  --die Rpl_semi_sync_master_status must be ON before the test
}

--echo # Baseline: a normal transaction is ACKed and reaches both slaves.
CREATE TABLE t1 (a INT) ENGINE=InnoDB;
INSERT INTO t1 VALUES (1);
--source include/save_master_gtid.inc
--connection server_2
--source include/sync_with_master_gtid.inc
--connection server_3
--source include/sync_with_master_gtid.inc

--echo #
--echo # Case 1 (mixed filtering): server_3 replicates skip_replication events,
--echo # so it ACKs them and semi-sync must stay in effect.
--echo #
--connection server_1
--let $yes_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
SET SESSION skip_replication= 1;
CREATE DATABASE db_skip;
CREATE TABLE db_skip.t1 (a INT) ENGINE=InnoDB;
INSERT INTO db_skip.t1 VALUES (1);
SET SESSION skip_replication= 0;

--let $ss= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_status', Value, 1)
if (`SELECT strcmp("$ss","ON") != 0`)
{
  SHOW STATUS LIKE 'Rpl_semi_sync_master_status';
  --die semi-sync degraded although a slave which replicates skip events was connected
}
--let $yes_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
if (`SELECT $yes_after <= $yes_before`)
{
  --die semi-sync did not await an ACK for a skip_replication txn a slave replicates
}
--echo # Semi-sync stayed ON and Rpl_semi_sync_master_yes_tx advanced -- PASS

--echo # Marker transaction, so both slaves can be synced past the skip events.
INSERT INTO t1 VALUES (2);
--source include/save_master_gtid.inc
--connection server_2
--source include/sync_with_master_gtid.inc
--connection server_3
--source include/sync_with_master_gtid.inc

--echo # server_3 (REPLICATE) applied the skip_replication changes.
--connection server_3
if (`SELECT COUNT(*) <> 1 FROM db_skip.t1`)
{
  --die server_3 (REPLICATE) should have received the skip_replication changes
}
--echo # server_2 (FILTER_ON_MASTER) never received them.
--connection server_2
if (`SELECT COUNT(*) <> 0 FROM information_schema.schemata WHERE schema_name='db_skip'`)
{
  --die server_2 (FILTER_ON_MASTER) must not have received the filtered database
}

--echo #
--echo # Case 2 (all slaves filter): reconfigure server_3 to FILTER_ON_MASTER,
--echo # so that no connected slave receives skip_replication events. Such a
--echo # transaction can never be ACKed: the master must not await an ACK, and
--echo # must not count one as either granted or missed.
--echo #
--connection server_3
--source include/stop_slave.inc
SET GLOBAL replicate_events_marked_for_skip= FILTER_ON_MASTER;
--source include/start_slave.inc

--connection server_1
--let $status_var= Rpl_semi_sync_master_clients
--let $status_var_value= 2
--source include/wait_for_status_var.inc

--let $yes_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
--let $no_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_no_tx', Value, 1)
--let $ack_before= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_request_ack', Value, 1)

SET SESSION skip_replication= 1;
CREATE DATABASE db_skip2;
CREATE TABLE db_skip2.t1 (a INT) ENGINE=InnoDB;
INSERT INTO db_skip2.t1 VALUES (1);
SET SESSION skip_replication= 0;

--let $ss= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_status', Value, 1)
if (`SELECT strcmp("$ss","ON") != 0`)
{
  SHOW STATUS LIKE 'Rpl_semi_sync_master_status';
  --die semi-sync degraded: the master awaited an ACK which no slave can send
}

--let $ack_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_request_ack', Value, 1)
if (`SELECT $ack_after <> $ack_before`)
{
  --die an ACK was requested for a transaction which reaches no slave
}
--let $yes_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_yes_tx', Value, 1)
--let $no_after= query_get_value(SHOW STATUS LIKE 'Rpl_semi_sync_master_no_tx', Value, 1)
if (`SELECT $yes_after <> $yes_before OR $no_after <> $no_before`)
{
  --die the master waited for, and accounted for, an ACK it never requested
}
--echo # Semi-sync stayed ON and no ACK was requested, granted or missed -- PASS

--echo #
--echo # Cleanup
--echo #
--connection server_1
# IF EXISTS because the slaves which filtered the CREATE never got the
# database, while the DROP itself is replicated to them.
DROP DATABASE IF EXISTS db_skip;
DROP DATABASE IF EXISTS db_skip2;
DROP TABLE t1;
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc

--echo # Restore server_3 to the filtering it was called with.
--connection server_3
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc
SET GLOBAL replicate_events_marked_for_skip= @skip_before;
--source include/start_slave.inc
