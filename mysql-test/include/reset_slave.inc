# ==== Purpose ====
#
# Reset the slave on the active connection
#
#
# ==== Usage ====
#
# [--let $master_use_gtid_option= NO]
# --source include/reset_slave.inc
#
# Parameters:
#   $master_use_gtid_option
#     Sets the context for the reset slave. When No, execute
#     CHANGE MASTER TO MASTER_USE_GTID=No after the RESET SLAVE. When
#     Slave_Pos, execute set gtid_slave_pos= "" after RESET SLAVE.
#
#   $reset_slave_keep_gtid_state
#     When master_use_gtid_option is Slave_Pos, this defines whether or not
#     gtid_slave_pos will be reset as well. Accepted values are of boolean
#     type. Default value is false.
#

--let $include_filename= reset_slave.inc
--source include/begin_include_file.inc


if (!$rpl_debug)
{
  --disable_query_log
}

if (!$master_use_gtid_option)
{
  --let $master_use_gtid_option= Slave_Pos
}

if (!$reset_slave_keep_gtid_state)
{
  --let $reset_slave_keep_gtid_state=0
}

if (`SELECT strcmp("$master_use_gtid_option","Slave_Pos") != 0 AND strcmp("$master_use_gtid_option","No") != 0`)
{
  die Invalid option provided as master_use_gtid_option, Slave_Pos or No are the only allowed options;
}

RESET SLAVE;
if (`SELECT strcmp("$master_use_gtid_option","Slave_Pos") = 0 AND NOT $reset_slave_keep_gtid_state`)
{
  SET @@GLOBAL.gtid_slave_pos= "";
}
if (`SELECT strcmp("$master_use_gtid_option","No") = 0`)
{
  CHANGE MASTER TO MASTER_USE_GTID=No;
}

--let $include_filename= reset_slave.inc
--source include/end_include_file.inc
