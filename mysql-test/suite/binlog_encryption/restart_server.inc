#
# We can not use the common include/restart_mysqld.inc or include/rpl_restart_server.inc,
# because they have hardcoded connection names (master, master1) 
# which are not initiated by rpl_init.inc.
# This is the relevant and simplified part of the same set of scripts.
#
# ==== Usage ====
#
# --let $rpl_server_number= N
#     Number to identify the server that needs to reconnect.
#     1 is the master server, 2 the slave server
# [--let $rpl_server_parameters= --flag1 --flag2 ...]
# --source restart_server.inc
#

--let $_cur_con= $CURRENT_CONNECTION

--connection default
--enable_reconnect

--connection $_cur_con
--enable_reconnect
--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.$rpl_server_number.expect

shutdown_server;

--source include/wait_until_disconnected.inc

--let $_rpl_start_server_command= restart
if ($rpl_server_parameters)
{
  --let $_rpl_start_server_command= restart:$rpl_server_parameters
}
--exec echo "$_rpl_start_server_command" > $MYSQLTEST_VARDIR/tmp/mysqld.$rpl_server_number.expect
--source include/wait_until_connected_again.inc
