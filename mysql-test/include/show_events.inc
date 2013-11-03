##############################################################################
# It's an auxiliary file used to show binary log events or relay log events.
# It is only called by show_binlog_events.inc and show_relaylog_events.inc.
##############################################################################

# Do not modify $binlog_start - if we did, it could wrongly persist until a
# later call of show_events.inc.
if ($binlog_start)
{
  --let $_binlog_start= $binlog_start
}

--let $_statement=show binlog events
if ($is_relay_log)
{
  --let $_statement=show relaylog events
}

if ($binlog_file)
{
  --let $_binlog_file= $binlog_file
  if ($_binlog_file == 'LAST')
  {
    if ($is_relay_log)
    {
      --let $_binlog_file= query_get_value(SHOW SLAVE STATUS, Relay_Log_File, 1)
    }
    if (!$is_relay_log)
    {
      --let $_binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
    }
  }
  --let $_statement= $_statement in '$_binlog_file'
}

if (!$binlog_start)
{
  # If $binlog_start is not set, we will set it as the fourth event's
  # position (second in relay log which has not Binlog Checkpoing nor
  # Gtid_list events).
  # The first three events (Description Event, Gtid list, and Binlog Checkpoint
  # event) are always ignored. For description event's length might be changed
  # because of adding new events, 'SHOW BINLOG EVENTS LIMIT 3' is used to get
  # the right value.
  if ($is_relay_log)
  {
    --let $_binlog_start= query_get_value($_statement LIMIT 1, End_log_pos, 1)
  }
  if (!$is_relay_log)
  {
    --let $_binlog_start= query_get_value($_statement LIMIT 3, End_log_pos, 3)
  }
}

--let $_statement= $_statement from $_binlog_start

# Cannot use if($binlog_limit) since the variable may begin with a 0

if (`SELECT '$binlog_limit' <> ''`)
{
  --let $_statement= $_statement limit $binlog_limit
}

--replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR $_binlog_start <binlog_start>
--replace_column 2 # 4 # 5 #
--replace_regex /\/\* xid=.* \*\//\/* XID *\// /table_id: [0-9]+/table_id: #/ /file_id=[0-9]+/file_id=#/ /block_len=[0-9]+/block_len=#/ /Server ver:.*$/SERVER_VERSION, BINLOG_VERSION/ /GTID [0-9]+-[0-9]+-[0-9]+/GTID #-#-#/ /\[([0-9]-[0-9]-[0-9]+)\]/[#-#-#]/ /cid=[0-9]+/cid=#/
--eval $_statement
