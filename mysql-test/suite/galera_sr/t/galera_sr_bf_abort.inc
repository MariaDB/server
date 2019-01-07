#
# Test SR BF abort for all sync points in master side code path
#
# The procedure in all test cases is the following:
# 1) Start SR transaction on node 1, do INSERT + SELECT .. FOR UPDATE
# 2) Set up sync point on node 1 to block slave thread processing
#    in apply monitor
# 3) Do write on node 2 which will conflict with SELECT .. FOR UPDATE
# 4) Set up desired sync point on master side and commit
# 5) Wait until commit reaches master side sync point, clear sync points
#    and release all sync point waiters
# 6) COMMIT on node 1 should return deadlock error
#

--connection node_1
CREATE TABLE t1 (f1 INTEGER PRIMARY KEY) ENGINE=InnoDB;

--eval SET SESSION wsrep_trx_fragment_size = $wsrep_trx_fragment_size
SET AUTOCOMMIT=OFF;

INSERT INTO t1 VALUES (1);
SELECT * FROM t1 FOR UPDATE;

# Set up sync point
--connection node_1a
--let galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_set_sync_point.inc

# Conflicting insert
--connection node_2

SET AUTOCOMMIT=ON;
INSERT INTO t1 VALUES (2);

--connection node_1a
--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc
--let $galera_sync_point = $galera_sr_bf_abort_sync_point
--source include/galera_set_sync_point.inc

--connection node_1
if ($galera_sr_bf_abort_at_commit)
{
  --send COMMIT
}
if (!$galera_sr_bf_abort_at_commit)
{
  --send INSERT INTO t1 VALUES (3)
}

--connection node_1a

--let $cmp = `SELECT STRCMP('apply_monitor_slave_enter_sync', '$galera_sr_bf_abort_sync_point') = -1`

if ($cmp)
{
  --let $galera_sync_point = apply_monitor_slave_enter_sync $galera_sr_bf_abort_sync_point
}
if (!$cmp)
{
  --let $galera_sync_point = $galera_sr_bf_abort_sync_point apply_monitor_slave_enter_sync
}
--source include/galera_wait_sync_point.inc

# Let conflicting insert proceed, make sure it hits abort_trx_end and
# let both threads continue.

--source include/galera_clear_sync_point.inc
--let $galera_sync_point = abort_trx_end
--source include/galera_set_sync_point.inc
--let $galera_sync_point = apply_monitor_slave_enter_sync
--source include/galera_signal_sync_point.inc
--let $galera_sync_point = abort_trx_end $galera_sr_bf_abort_sync_point

--source include/galera_wait_sync_point.inc
--source include/galera_clear_sync_point.inc
--let $galera_sync_point = abort_trx_end
--source include/galera_signal_sync_point.inc
--let $galera_sync_point = $galera_sr_bf_abort_sync_point
--source include/galera_signal_sync_point.inc

# Deadlock should now be retured by node_1
--connection node_1
if (!$galera_sr_bf_abort_at_commit)
{
  --error ER_LOCK_DEADLOCK
  --reap
}
if ($galera_sr_bf_abort_at_commit)
{
  --reap
}

ROLLBACK;

# Release slave insert
--connection node_1a
--source include/galera_clear_sync_point.inc
--let $galera_sync_point = abort_trx_end
--source include/galera_signal_sync_point.inc

# Verify that nodes are consistent

# End result:
# If the statement which was BF aborted was commit,
# node_1 must replay the transaction so that the table
# will have rows 1, 2. If it in turn was INSERT,
# node_1 must abort the transaction so that only
# INSERT ... VALUES (2) survives.

--connection node_1
SELECT * FROM t1;
if ($galera_sr_bf_abort_at_commit)
{
  SELECT COUNT(*) = 1 FROM t1 WHERE f1 = 1;
  SELECT COUNT(*) = 1 FROM t1 WHERE f1 = 2;
}
if (!$galera_sr_bf_abort_at_commit)
{
  SELECT COUNT(*) = 1 FROM t1 WHERE f1 = 2;
}
--connection node_2
SELECT * FROM t1;
if ($galera_sr_bf_abort_at_commit)
{
  SELECT COUNT(*) = 1 FROM t1 WHERE f1 = 1;
  SELECT COUNT(*) = 1 FROM t1 WHERE f1 = 2;
}
if (!$galera_sr_bf_abort_at_commit)
{
  SELECT COUNT(*) = 1 FROM t1 WHERE f1 = 2;
}

--connection node_1
SELECT COUNT(*) = 0 FROM mysql.wsrep_streaming_log;
--connection node_2
SELECT COUNT(*) = 0 FROM mysql.wsrep_streaming_log;

# Delete entery to verify that node is unblocked
--connection node_1
SET AUTOCOMMIT=ON;
SET SESSION wsrep_trx_fragment_size = 0;
DELETE FROM t1;

DROP TABLE t1;
