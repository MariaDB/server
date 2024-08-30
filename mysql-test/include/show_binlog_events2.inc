# ==== Purpose ====
#
# A lighter version of show_binlog_event.inc, with the same purpose
# to execute SHOW BINLOG EVENTS and mask non-deterministic output.
#
#
# Parameters:
#
#   $binlog_file
#     Filename for the 'IN' clause of SHOW BINLOG EVENTS. If none
#     given, no argument is given to SHOW BINLOG EVENTS, meaning that
#     it uses the first binlog. If you set this to "LAST", it prints
#     the last binlog (according to SHOW MASTER STATUS).
#
#   $binlog_start
#     Position for the 'FROM' clause of SHOW BINLOG EVENTS. If none
#     given, starts right after the  Binlog_checkpoint_log_even.
#
#   $regexp_replace
#     A user's custom addon to standard preexisting list.
#

if ($binlog_start)
{
  --let $_binlog_start=$binlog_start
}
if (!$binlog_start)
{
  --let $_binlog_start=256
}
if ($binlog_file)
{
  --let $_in_binlog_file=in '$binlog_file'
}
--let $_from_binlog_start=from $_binlog_start
--replace_result "$_from_binlog_start" "from <binlog_start>" $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
--replace_column 2 # 5 #
--replace_regex /\/\* xid=.* \*\//\/* XID *\// /table_id: [0-9]+/table_id: #/ /file_id=[0-9]+/file_id=#/ /GTID [0-9]+-[0-9]+-[0-9]+/GTID #-#-#/ $replace_regexp
--eval show binlog events $_in_binlog_file from $_binlog_start
