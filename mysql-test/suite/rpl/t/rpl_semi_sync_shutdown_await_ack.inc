#
# Helper file to ensure that a primary waits for all ACKS (or timeout) from its
# replicas before shutting down.
#
# Parameters:
#   server_1_dbug (string)          Debug setting for primary (server 1)
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

  set global rpl_semi_sync_slave_enabled = 1;
  source include/stop_slave.inc;
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
--connection server_1
--eval SET @@GLOBAL.debug_dbug= $server_1_dbug
--connection server_2
--eval SET @@GLOBAL.debug_dbug= $server_2_dbug
--connection server_3
--eval SET @@GLOBAL.debug_dbug= $server_3_dbug

--echo #--
--echo #-- Test begins

--connection server_1
--echo #-- Begin semi-sync transaction
--send INSERT INTO t1 VALUES (1)

--connection server_1_con2
--echo #-- Wait until master recognizes a connection is awaiting semi-sync ACK
let $status_var= Rpl_semi_sync_master_wait_sessions;
let $status_var_value= 1;
source include/wait_for_status_var.inc;
show status like 'Rpl_semi_sync_master_wait_sessions';

--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait
EOF

--echo #-- Give enough time after timeout/ack received to query yes_tx/no_tx
SET @@GLOBAL.debug_dbug= "+d,delay_shutdown_phase_2_after_semisync_wait";

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

--echo #-- Stop slaves

--connection server_2
--eval SET @@GLOBAL.debug_dbug= "$sav_server_2_dbug"
--eval SET @@GLOBAL.rpl_semi_sync_slave_enabled= 0
source include/stop_slave.inc;

--connection server_3
--eval SET @@GLOBAL.debug_dbug= "$sav_server_3_dbug"
--eval SET @@GLOBAL.rpl_semi_sync_slave_enabled= 0
source include/stop_slave.inc;

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

--eval SET @@GLOBAL.debug_dbug= "$sav_master_dbug"
let $status_var= Rpl_semi_sync_master_clients;
let $status_var_value= 0;
source include/wait_for_status_var.inc;
--eval SET @@GLOBAL.rpl_semi_sync_master_enabled = 0
show status like 'Rpl_semi_sync_master_status';

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
