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
#   server_2_sync_slave_reply (bool) True if server_2_dbug is configured to use
#                                    debug_sync to synchronize the slave reply
#                                    (i.e. to pause before the reply)
#   server_3_sync_slave_reply (bool) True if server_3_dbug is configured to use
#                                    debug_sync to synchronize the slave reply
#                                    (i.e. to pause before the reply)
#

--connection server_1
let $log_error_file= `SELECT @@GLOBAL.log_error`;
if (!$n_logged_delayed_shutdown_notes)
{
  let $n_logged_delayed_shutdown_notes= 0;
}

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

if ($server_2_sync_slave_reply)
{
  --connection server_2
  set debug_sync= "now wait_for io_thd_at_slave_reply";
}
if ($server_3_sync_slave_reply)
{
  --connection server_3
  set debug_sync= "now wait_for io_thd_at_slave_reply";
}

--connection server_1_con2
--echo #-- Begin master shutdown
SHUTDOWN WAIT FOR ALL SLAVES;

# Use server_2 to search error log because 1 is down
--connection server_2

--echo # Waitng for shutdown to be delayed..
# Increment the number of notes to find each test case to ensure the pattern is
# ours
--inc $n_logged_delayed_shutdown_notes
let SEARCH_FILE= $log_error_file;
let SEARCH_PATTERN=Delaying shutdown to await semi-sync ACK;
let SEARCH_WAIT=FOUND $n_logged_delayed_shutdown_notes;
source include/search_pattern_in_file.inc;

if (`SELECT $server_2_sync_slave_reply AND $server_2_expect_row_count`)
{
  --connection server_2
  set debug_sync= "now signal io_thd_do_reply";
}
if (`SELECT $server_3_sync_slave_reply AND $server_3_expect_row_count`)
{
  --connection server_3
  set debug_sync= "now signal io_thd_do_reply";
}

--echo # Reaping transaction..
--connection server_1
--error 2013
--reap
--source include/wait_until_disconnected.inc

# Timeout should be hit from prior reap
--connection server_2
if (`SELECT $server_2_sync_slave_reply AND NOT $server_2_expect_row_count`)
{
  --connection server_2
  set debug_sync= "now signal io_thd_do_reply";
}
if (`SELECT $server_3_sync_slave_reply AND NOT $server_3_expect_row_count`)
{
  --connection server_3
  set debug_sync= "now signal io_thd_do_reply";
}

--echo # Reaping shutdown..
--connection server_1_con2
--source include/wait_until_disconnected.inc

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
# If server_2_expect_row_count is 0, we are simulating an error on the replica
# and the IO thread will end with errno 1595.
# Otherwise, we still expect error, because the master has shutdown at this
# point, and the IO thread may or may not have realized the shutdown, and
# started to try to automatically reconnect. This may result in the IO thread
# giving a 2003 error if the slave tries to reconnect to a shutdown master.
# Additionally disable warnings because the slave may have stopped in err
# automatically, and we don't want a sporadic "Slave is already stopped"
# warning.
--disable_warnings
--let $rpl_allow_error= 1
--source include/stop_slave_io.inc
--enable_warnings
--let $rpl_allow_error=
--source include/stop_slave_sql.inc
SET @@GLOBAL.debug_dbug= @sav_server_2_dbug;
SET @@GLOBAL.rpl_semi_sync_slave_enabled= @sav_enabled_server_2;

--connection server_3
# Expect error for IO thread, see above comment for stopping server_2
--disable_warnings
--let $rpl_allow_error= 1
--source include/stop_slave_io.inc
--enable_warnings
--let $rpl_allow_error=
--source include/stop_slave_sql.inc
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
