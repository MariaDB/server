# ==== Purpose ====
#
# If using DEBUG_SYNC to coordinate a slave's SQL DELAY via the DEBUG_DBUG
# identifier "sql_delay_by_debug_sync", this helper file will help synchronize
# a slave with the master for statements which don't need to be delayed. This
# can be helpful, for example, for setup/cleanup statements, if they must be
# run in the same lifetime as the statements used for the test.
#
# The actual synchronization will take place based on the input parameter
# slave_sync_method, which can be "gtid", "file_coord", or "none"; and will use
# the helper files sync_with_master_gtid.inc or sync_with_master.inc (or none
# at all), respectively.
#
#
# ==== Requirements ====
#
# --source include/have_debug.inc
# --source include/have_debug_sync.inc
# set @@GLOBAL.debug_dbug= "+d,sql_delay_by_debug_sync";
#
#
# ==== Usage ====
#
# --let $slave_sync_method= gtid|file_coord|none
# [--let $num_event_groups= NUMBER]
# --source include/sync_with_master_sql_delay_debug_sync.inc
#
#
# Parameters:
#   $slave_sync_method
#       Value can be gtid, file_coord, or none; and will synchronize the slave
#       with the master via this method (i.e. using sync_with_master_gtid.inc
#       or sync_with_master.inc, respectively), after synchronizing the SQL
#       delay
#
#   $num_event_groups
#       Number of event groups to synchronize the SQL delay for. If unset, will
#       be default to 1.
#

--let $include_filename= sync_with_master_sql_delay_debug_sync.inc
--source include/begin_include_file.inc

if (!$slave_sync_method)
{
  --die Parameter slave_sync_method must be set
}

if (`select "$slave_sync_method" not like "gtid" and "$slave_sync_method" not like "file_coord" and "$slave_sync_method" not like "none"`)
{
  --die Parameter slave_sync_method must have value "gtid", "file_coord" or "none"
}

if (`select "$slave_sync_method" not like "none" and strcmp("$master_pos", "") = 0`)
{
  --die sync_with_master.inc or sync_with_master_gtid.inc was not called to populate variable master_pos
}

if (!$num_event_groups)
{
  --let $num_event_groups= 1
}

while ($num_event_groups)
{
  set debug_sync= "now WAIT_FOR at_sql_delay";
  set debug_sync= "now SIGNAL continue_sql_thread";
  --dec $num_event_groups
}

if (`select "$slave_sync_method" LIKE "gtid"`)
{
  --source include/sync_with_master_gtid.inc
}

if (`select "$slave_sync_method" LIKE "file_coord"`)
{
  --source include/sync_with_master.inc
}

--let $include_filename= sync_with_master_sql_delay_debug_sync.inc
--source include/end_include_file.inc
