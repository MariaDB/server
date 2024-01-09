#
# Helper file to ensure that a primary waits for all ACKS (or timeout) from its
# replicas before shutting down.
#
# Parameters:
#   server_2_dbug (string)          Debug setting to simulate delay or error on
#                                   the first replica (server 2)
#   server_3_dbug (string)          Debug setting to simulate delay or error on
#                                   the second replica (server 3)
#   semisync_timeout (int)          Rpl_semi_sync_master_timeout to use
#   server_2_expect_row_count (int) The number of rows expected on the first
#                                   replica after the shutdown
#   server_3_expect_row_count (int) The number of rows expected on the second
#                                   replica after the shutdown
#

--connection server_1
let $log_error_file= `SELECT @@GLOBAL.log_error`;

--echo #--
--echo #-- Semi-sync Setup

--connection server_1
--save_master_pos

echo #-- Enable semi-sync on slaves
let slave_last= 3;
--let i= 2
while (`SELECT $i <= $slave_last`)
{
  --connection server_$i
  --sync_with_master

  source include/stop_slave.inc;
  set global rpl_semi_sync_slave_enabled = 1;
  source include/start_slave.inc;
  show status like 'Rpl_semi_sync_slave_status';

  --inc $i
}

--echo #-- Enable semi-sync on master
--connection server_1
SET @@GLOBAL.rpl_semi_sync_master_enabled = 1;
--eval set @@global.rpl_semi_sync_master_timeout= $semisync_timeout

--echo #-- Wait for master to recognize semi-sync slaves
--connection server_1
let $status_var= Rpl_semi_sync_master_clients;
let $status_var_value= 2;
source include/wait_for_status_var.inc;

--echo #-- Master should have semi-sync enabled with 2 connections
show status like 'Rpl_semi_sync_master_status';
show status like 'Rpl_semi_sync_master_clients';

--echo #-- Prepare servers to simulate delay or error
--connection server_2
--eval SET @@GLOBAL.debug_dbug= $server_2_dbug
--connection server_3
--eval SET @@GLOBAL.debug_dbug= $server_3_dbug

--echo #--
--echo #-- Test begins

--connection server_1_con2
--echo #-- Give enough time after timeout/ack received to query yes_tx/no_tx
SET @@GLOBAL.debug_dbug= "+d,delay_shutdown_phase_2_after_semisync_wait";

--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait
EOF

--connection server_1
--echo #-- Begin semi-sync transaction
--send INSERT INTO t1 VALUES (1)

--connection server_1_con2
--echo #-- Wait until master recognizes a connection is awaiting semi-sync ACK
let $status_var= Rpl_semi_sync_master_wait_sessions;
let $status_var_value= 1;
source include/wait_for_status_var.inc;

--echo #-- Begin master shutdown
--send SHUTDOWN WAIT FOR ALL SLAVES

--connection server_1
--reap
--echo #-- Ensure either ACK was received (yes_tx=1) or timeout (no_tx=1)
show status like 'Rpl_semi_sync_master_yes_tx';
show status like 'Rpl_semi_sync_master_no_tx';

--connection server_1_con2
--reap
--source include/wait_until_disconnected.inc

--echo # Check logs to ensure shutdown was delayed
--let SEARCH_FILE=$log_error_file
--let SEARCH_PATTERN=Delaying shutdown to await semi-sync ACK
--source include/search_pattern_in_file.inc

--echo # Validate slave data is in correct state
--connection server_2
--eval select count(*)=$server_2_expect_row_count from t1
--connection server_3
--eval select count(*)=$server_3_expect_row_count from t1

--echo #
--echo #-- Re-synchronize slaves with master and disable semi-sync

if (`SELECT ($server_2_expect_row_count + $server_3_expect_row_count) < 2`)
{
--echo #-- FIXME: workaround for MDEV-28141, preventing errored replicas from
--echo # killing their semi-sync connections
# I.e. we can't create a new kill connection to the primary if we know that the
# primary is shutting down for risk of Packets out of order error. So we wait
# to hit a debug_sync point before the creation of the new kill_connection, and
# don't progress until the primary has been shutdown, so no new connection can
# be formed.
# Note this is only needed in the error case (using corrupt_queue_event), as
# the running io_thread will otherwise automatically detect that the primary
# has shutdown before progressing to the cleanup of the io thread.
}

if (!$server_2_expect_row_count)
{
  --connection server_2
  set debug_sync= "now wait_for at_semisync_kill_connection";
  set debug_sync= "now signal continue_semisync_kill_connection";
  --echo # Wait for debug_sync signal to have been received before issuing RESET
  let $wait_condition= select count(*)=0 from information_schema.processlist where state like "debug sync point%";
  source include/wait_condition.inc;
  set debug_sync= "reset";
}
if (!$server_3_expect_row_count)
{
  --connection server_3
  set debug_sync= "now wait_for at_semisync_kill_connection";
  set debug_sync= "now signal continue_semisync_kill_connection";
  --echo # Wait for debug_sync signal to have been received before issuing RESET
  let $wait_condition= select count(*)=0 from information_schema.processlist where state like "debug sync point%";
  source include/wait_condition.inc;
  set debug_sync= "reset";
}

--echo #-- Stop slaves

--connection server_2
if (!$server_2_expect_row_count)
{
  # ER_SLAVE_RELAY_LOG_WRITE_FAILURE
  --let $slave_io_errno=1595
  --source include/wait_for_slave_io_error.inc
  --source include/stop_slave_sql.inc
}
if ($server_2_expect_row_count)
{
  --echo #-- Waiting for IO thread to realize master shutdown and stop
  --let $slave_io_errno=2003
  --source include/wait_for_slave_io_error.inc
  --source include/stop_slave_sql.inc
  #--source include/stop_slave.inc
}
SET @@GLOBAL.debug_dbug= @sav_server_2_dbug;
SET @@GLOBAL.rpl_semi_sync_slave_enabled= @sav_enabled_server_2;

--connection server_3
if (!$server_3_expect_row_count)
{
  --echo #-- Waiting for IO thread to stop from corrupt_queue_event injection
  # ER_SLAVE_RELAY_LOG_WRITE_FAILURE (1595) if corrupt_queue failure
  --let $slave_io_errno=1595
  --source include/wait_for_slave_io_error.inc
  --source include/stop_slave_sql.inc
}
if ($server_3_expect_row_count)
{
  --echo #-- Waiting for IO thread to realize master shutdown and stop
  --let $slave_io_errno=2003
  --source include/wait_for_slave_io_error.inc
  --source include/stop_slave_sql.inc
}
SET @@GLOBAL.debug_dbug= @sav_server_3_dbug;
SET @@GLOBAL.rpl_semi_sync_slave_enabled= @sav_enabled_server_3;

--echo #-- Bring the master back up
--connection server_1_con2
--append_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
restart
EOF

--enable_reconnect
--source include/wait_until_connected_again.inc

--connection default
--enable_reconnect
--source include/wait_until_connected_again.inc

--connection server_1
--enable_reconnect
--source include/wait_until_connected_again.inc

# Because we restarted, we have to store the default values again for later
# restoration at cleanup
set @sav_master_timeout= @@global.rpl_semi_sync_master_timeout;
set @sav_enabled_master= @@GLOBAL.rpl_semi_sync_master_enabled;
set @sav_master_dbug= @@GLOBAL.debug_dbug;

TRUNCATE TABLE t1;
--save_master_pos

--echo #-- Bring slaves back up
--let i= 2
while (`SELECT $i <= $slave_last`)
{
  --connection server_$i
  source include/start_slave.inc;
  show status like 'Rpl_semi_sync_slave_status';
  --sync_with_master
  SELECT COUNT(*)=0 from t1;
  --inc $i
}
