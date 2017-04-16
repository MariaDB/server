--let $show_statement = show all slaves status
--let $field = Slave_SQL_State
--let $condition = = 'Slave has read all relay log; waiting for the slave I/O thread to update it'
--let $wait_for_all = 1
--source include/wait_show_condition.inc

