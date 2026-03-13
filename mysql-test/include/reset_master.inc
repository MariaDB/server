# ==== Purpose ====
#
# Execute a RESET MASTER on the current connection, first terminating any
# lingering binlog dump threads that might still be sitting idle and would
# block the RESTE MASTER.
#
# Note that any configured IO threads on other servers must be stopped before
# calling this, as RESET MASTER cannot be run while there is a slave connected.
#
#
# ==== Usage ====
#
# [--let $kill_timeout= NUMBER]
# [--let $reset_master_retries= NUMBER]
# --source include/reset_master.inc
#
# Parameters:
#   $kill_timeout
#     Maximum number of seconds to wait for dump threads to disappear.
#   $reset_master_retries
#     Maximum number of times RESET MASTER can get ER_BINLOG_IN_USE before
#     giving up.

--let $include_filename= reset_master.inc
--source include/begin_include_file.inc

--disable_query_log

let $_retries= 10;
if ($reset_master_retries)
{
  let $_retries= $reset_master_retries;
}

let $_success= 0;
let $_i= 0;
while ($_i < $_retries)
{
  inc $_i;
  --source include/kill_binlog_dump_threads.inc
  --let $errno= 0
  --error 0,ER_BINLOG_IN_USE
  RESET MASTER;
  if (!$errno) {
    let $_success= 1;
    let $_i = $_retries;
  }
}
if (!$_success)
{
    SHOW FULL PROCESSLIST;
    --die Timeout while trying to remove dump threads and run RESET MASTER.
}

--enable_query_log

--let $include_filename= reset_master.inc
--source include/end_include_file.inc
