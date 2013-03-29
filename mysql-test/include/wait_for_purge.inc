# include/wait_for_purge.inc
#
# SUMMARY
#
#    Repeatedly attempt to purge binlogs up to a specific point, until
#    SHOW BINARY LOGS shows it has succeeded.
#    This can be useful to avoid races where slaves are stopped but the
#    corresponding binlog dump thread on the master has not yet terminated.
#    Async binlog checkpoints can also delay purge.
#
#    Note that it may be necessary to inject at least one event in the binlog
#    after stopping slaves, or master may not detect stop of slave for a long
#    time (or forever), depending on details of test case, if replication
#    heartbeat is enabled, etc.
#
# USAGE:
#
#    --let $purge_binlogs_to=master-bin.000003
#    --source include/wait_for_purge.inc
#
# SIDE EFFECTS:
#
#    Disables --query_log while running, enables it afterwards.

--echo include/wait_for_purge.inc "$purge_binlogs_to"

let $_wait_count= 300;
let $_done= 0;

--disable_query_log
while ($_wait_count)
{
  dec $_wait_count;
  eval PURGE BINARY LOGS TO "$purge_binlogs_to";
  let $_cur_binlog= query_get_value(SHOW BINARY LOGS, Log_name, 1);
  if (`SELECT "$_cur_binlog" = "$purge_binlogs_to"`)
  {
    let $_done= 1;
    let $_wait_count= 0;
  }
  if ($_wait_count)
  {
    real_sleep 0.1;
  }
}

if (!$_done)
{
  SHOW FULL PROCESSLIST;
  eval SHOW BINARY LOGS;
  --die ERROR: failed while waiting for binlog purge to $purge_binlogs_to
}
--enable_query_log
