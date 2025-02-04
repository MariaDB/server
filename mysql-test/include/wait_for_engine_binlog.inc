# include/wait_for_engine_binlog.inc
#
# SUMMARY
#
#    Waits until a specific (engine-implemented) binlog file is seen with
#    the specified size in SHOW BINARY LOGS.
#    Used to avoid sporadic failures due to races with the binlog
#    pre-allocation thread.
#
# USAGE
#
#    --let $binlog_name= binlog-000002.ibb
#    --let $binlog_size= 262144
#    --source include/wait_for_engine_binlog.inc
#
#   OPTIONALLY:
#
#    let $wait_timeout= 60; # Override default 30 seconds with 60.
#    let $wait_notfound= 1; # Wait until specified binlog _not_ found.
#
# EXAMPLE
#    binlog_in_engine.binlog_flush_purge.test
#

--disable_query_log

let $_wait_counter= 300;
if ($wait_timeout)
{
  let $_wait_counter= `SELECT $wait_timeout * 10`;
}
# Reset $wait_timeout so that its value won't be used on subsequent
# calls, and default will be used instead.
let $wait_timeout= 0;

--let $_expect= 1
--let $_message= exist
if ($wait_notfound) {
  --let $_expect= 0
  --let $_message= no longer exist
}
let $wait_notfound= 0;

--let $_done= 0
--let $_i= 0
while (!$_done) {
  --let $_j= 1
  --let $_end= 0
  --let $_found= 0
  while (!$_end) {
    --let $_x= query_get_value(SHOW BINARY LOGS, Log_name, $_j)
    if ($_x == No such row) {
      --let $_end= 1
    }
    if ($_x == $binlog_name) {
      --let $_y= query_get_value(SHOW BINARY LOGS, File_size, $_j)
      if ($_y == $binlog_size) {
        --let $_found= 1
	--let $_end= 1
      }
    }
    inc $_j;
  }
  if ($_found == $_expect) {
    --let $_done= 1
  }
  if (!$_done) {
    sleep 0.1;
    inc $_i;
    if ($_i >= $_wait_counter) {
      SHOW BINARY LOGS;
      --die Timeout waiting for binlog '$binlog_name' to $_message
    }
  }
}

--enable_query_log
