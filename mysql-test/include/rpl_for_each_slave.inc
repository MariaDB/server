# ==== Purpose ====
#
# Execute a .inc file once for each server that was configured as a
# slave by rpl_init.inc, for example start_slave.inc or stop_slave.inc file.
#
#
# ==== Usage ====
#
# --let $rpl_source_file
# [--let $rpl_debug= 1]
# --source include/rpl_for_each_slave.inc
#
# Parameters:
#   $rpl_source_file
#     The file that will be sourced.
#
#   $rpl_server_count
#     The number of servers to configure. If this is not set, the largest
#     number in $rpl_topology will be used.
#     This parameter is obtained from rpl_init.inc.
#
#   $rpl_master_list
#     This parameter is calculated from within rpl_init.inc.
#
#   $rpl_server_count_length
#     Set to LENGTH($rpl_server_count). So if $rpl_server_count < 10,
#     then $rpl_server_count_length = 1; if 10 <= $rpl_server_count <
#     100, then $rpl_server_count_length = 2, etc.
#     This parameter is calculated from within rpl_change_topology.inc.
#
#   $rpl_debug
#     See include/rpl_init.inc


--let $include_filename= rpl_for_each_file.inc [$rpl_source_file]
--source include/begin_include_file.inc

--let $_rpl_server= $rpl_server_count
while ($_rpl_server)
{
  --let $_rpl_has_master= `SELECT SUBSTRING('$rpl_master_list', 1 + ($_rpl_server - 1) * $rpl_server_count_length, $rpl_server_count_length) != ''`
  if ($_rpl_has_master)
  {
    --let $rpl_connection_name= server_$_rpl_server
    --source include/rpl_connection.inc
    --source $rpl_source_file
  }
  --dec $_rpl_server
}


--let $include_filename= rpl_for_each_file.inc [$rpl_source_file]
--source include/end_include_file.inc
