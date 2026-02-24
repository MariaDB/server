# ==== Purpose ====
#
# During shutdown of the master, check Slave_IO_Running status
# after reconnecting a specified number of times.
#
# ==== Usage ====
#
# --let $overflow= 0
# --let $retry_count= 3
# --let $expected_Slave_IO_Running_status= Connecting
# --source rpl_mdev36002.inc
#
# Parameters:
#  $overflow
#   If overflow equals 1, the DEBUG flag 'set_slave_err_count_near_overflow'
#   will be activated, and the number of tries made will be initialized to
#   ULONG_MAX - 2.
#
#  $retry_count
#   The number of reconnections the slave will do during shutdown of the master.
#
#  $expected_Slave_IO_Running_status
#   Whether Slave_IO_Running should be Connecting/No
#   after reconnecting $retry_count times.
#


--source include/have_debug.inc
--source include/have_debug_sync.inc

# Set MASTER_CONNECT_RETRY as 1 second
--let $rpl_skip_start_slave= 1
--source include/master-slave.inc

--connection slave
CHANGE MASTER TO MASTER_CONNECT_RETRY= 1;
--source include/start_slave.inc


# Set err_count as ULONG_MAX if necessary and starts up sync points
SET @saved_dbug= @@GLOBAL.debug_dbug;

if ($overflow)
{
  SET @@GLOBAL.debug_dbug= '+d,set_slave_err_count_near_overflow';
}
SET @@GLOBAL.debug_dbug= '+d,sync_master_retry';


# stop the master
--let $rpl_server_number= 1
--source include/rpl_stop_server.inc

# Everytime the reconnection is done, check the status of Slave_IO_Running
while ($retry_count)
{
  SET @@SESSION.debug_sync= 'now WAIT_FOR master_retry_sleep';
  --let $slave_param= Slave_IO_Running
  --let $slave_param_value= Connecting
  --source include/wait_for_slave_param.inc

  SET @@SESSION.debug_sync= 'now SIGNAL master_retry_continue';
  --dec $retry_count
}

# after exhausting exactly that many number of tries,
# the Slave_IO_Running status should be expected.

--echo Slave_IO_Running status should be $expected_Slave_IO_Running_status
--let $slave_param= Slave_IO_Running
--let $slave_param_value= $expected_Slave_IO_Running_status
--source include/wait_for_slave_param.inc

SET @@GLOBAL.debug_dbug= @saved_dbug;


# Restart the master to reestablish a successful slave connection for clean-up
--let $rpl_server_number= 1
--source include/rpl_start_server.inc

--connection slave
--disable_warnings
START SLAVE;
--enable_warnings
--let $rpl_allow_error= 1
--source include/wait_for_slave_io_to_start.inc

--source include/rpl_end.inc
