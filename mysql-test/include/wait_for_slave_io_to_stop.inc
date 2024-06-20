# ==== Purpose ====
#
# Waits until the IO thread of the current connection has stopped
# gracefully.
#
# Note: this script will fail if the IO thread stops with an error.
# If you expect an error in the IO thread, use
# include/wait_for_slave_io_error.inc instead.
#
# This script also fails if a timeout is reached (default 300
# seconds).
#
#
# ==== Usage ====
#
# [--let $slave_timeout= NUMBER]
# [--let $rpl_debug= 1]
# --source include/wait_for_slave_io_to_stop.inc
#
# Parameters:
#   $slave_timeout
#     See include/wait_for_slave_param.inc.
#
#   $rpl_allow_error
#     By default, this file fails if there is an error in the IO
#     thread. However, if an error in the IO thread is possible and allowed,
#     setting $rpl_allow_error=1 will prevent this file from failing if
#     there is an error in the IO thread.
#     (If an error is _always_ expected, a better alternative might be to
#     use wait_for_slave_io_error.inc instead of this file).
#     Note: This is currently always enabled, since a simple STOP SLAVE
#     IO_THREAD can cause an error if it interrupts the slave's initial
#     communication with the master (MDEV-32892).
#
#   $rpl_debug
#     See include/rpl_init.inc


--let $include_filename= wait_for_slave_io_to_stop.inc
--source include/begin_include_file.inc


--let $slave_param= Slave_IO_Running
--let $slave_param_value= No
--let $_io_stop_save_allow_error= $slave_error_param
# Disabled, as IO errors are left behind when a normal STOP SLAVE interrupts
# the initial communication between the IO thread and the master (MDEV-32892).
#if (!$rpl_allow_error)
#{
#  --let $slave_error_param= Last_IO_Errno
#}
--source include/wait_for_slave_param.inc
--let $slave_error_param= $_io_stop_save_allow_error


--let $include_filename= wait_for_slave_io_to_stop.inc
--source include/end_include_file.inc
