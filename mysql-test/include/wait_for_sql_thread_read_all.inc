# This script ensures that a slave has executed everything in the relay log
# files. This can be used to ensure slave is in a predicate place after
# a FLUSH RELAY LOGS command.
# Note that this script does not guarantee that the slave has read everything
# from the master(s). If this is needed on should use the sync_with_master
# command or the sync_with_master script.

--let $show_statement = show all slaves status
--let $field = Slave_SQL_State
--let $condition = = 'Slave has read all relay log; waiting for more updates'
--let $wait_for_all = 1
--source include/wait_show_condition.inc
