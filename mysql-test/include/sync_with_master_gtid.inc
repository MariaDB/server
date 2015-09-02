# ==== Purpose ====
#
# Wait until the slave has reached a certain GTID position.
# Similar to --sync_with_master, but using GTID instead of old-style
# binlog file/offset coordinates.
#
#
# ==== Usage ====
#
# --let $master_pos= `SELECT @@GLOBAL.gtid_binlog_pos`
# [--let $slave_timeout= NUMBER]
# [--let $rpl_debug= 1]
# --source include/sync_with_master_gtid.inc
#
# Syncs slave to the specified GTID position.
#
# Must be called on the slave.
#
# Parameters:
#   $master_pos
#     The GTID position to sync to. Typically obtained from
#      @@GLOBAL.gtid_binlog_pos on the master.
#
#   $slave_timeout
#      Timeout in seconds. The default is 2 minutes.
#
#   $rpl_debug
#      See include/rpl_init.inc

--let $include_filename= sync_with_master_gtid.inc
--source include/begin_include_file.inc

let $_slave_timeout= $slave_timeout;
if (!$_slave_timeout)
{
  let $_slave_timeout= 120;
  if ($VALGRIND_TEST)
  {
    let $_slave_timeout= 1200;
  }
}

--let $_result= `SELECT master_gtid_wait('$master_pos', $_slave_timeout)`
if ($_result == -1)
{
  --let $_current_gtid_pos= `SELECT @@GLOBAL.gtid_slave_pos`
  --echo Timeout in master_gtid_wait('$master_pos', $_slave_timeout), current slave GTID position is: $_current_gtid_pos.
  --die Failed to sync with master
}

--let $include_filename= sync_with_master_gtid.inc
--source include/end_include_file.inc
