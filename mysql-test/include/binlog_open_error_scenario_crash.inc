#
# Sub-helper for binlog_open_error_scenario.inc — crash kind.
#
# Sets the crash DBUG knob, fires the configured trigger which is expected
# to crash the server, then restarts and inspects post-restart state.
#

#
# The crash is asserted by wait_until_disconnected.inc below, which dies on
# timeout if the server stayed up; the --error list on each trigger only
# tolerates the race between a clean error reply and the connection simply
# dropping.
#

--let $_scn_expect_file= $MYSQLTEST_VARDIR/tmp/mysqld.1.expect

# Tell mtr to wait for us to bring the server back.
--exec echo "wait" > $_scn_expect_file

--disable_query_log
--eval SET @@global.debug_dbug = "d,$scn_dbug"
--enable_query_log

if ($scn_trigger == flush)
{
  --error 0,2006,2013,ER_CONNECTION_KILLED
  FLUSH BINARY LOGS;
}
if ($scn_trigger == reset_master)
{
  --error 0,2006,2013,ER_CONNECTION_KILLED
  RESET MASTER;
}
if ($scn_trigger == checksum_update)
{
  --error 0,2006,2013,ER_CONNECTION_KILLED
  --eval SET GLOBAL binlog_checksum= IF(@@global.binlog_checksum='NONE', 'CRC32', 'NONE')
}

--source include/wait_until_disconnected.inc

--exec echo "restart" > $_scn_expect_file
--enable_reconnect
--source include/wait_until_connected_again.inc
--disable_reconnect

--let $_scn_expect_file=
