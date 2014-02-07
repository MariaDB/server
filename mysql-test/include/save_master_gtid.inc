# ==== Purpose ====
#
# Save the current binlog GTID position on the master, to be used
# with include/sync_with_master_gtid.inc.
#
#
# ==== Usage ====
#
# [--let $rpl_debug= 1]
# --source include/save_master_gtid.inc
#
# Parameters:
#   $rpl_debug
#     See include/rpl_init.inc


--let $include_filename= save_master_gtid.inc
--source include/begin_include_file.inc

--let $master_pos= `SELECT @@gtid_binlog_pos`

if ($rpl_debug)
{
  --echo save_master_gtid saved master_pos='$master_pos'
}

--let $include_filename= save_master_gtid.inc
--source include/end_include_file.inc
