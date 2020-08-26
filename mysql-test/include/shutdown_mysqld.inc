# ==== Usage ====
#
# [--let $shutdown_timeout= 60]
# [--let $allow_rpl_inited= 1]
# --source include/shutdown_mysqld.inc

# The default value is empty
--let $server_shutdown_timeout=

if ($shutdown_timeout)
{
  --let $server_shutdown_timeout= $shutdown_timeout
}

# This is the first half of include/restart_mysqld.inc.
if ($rpl_inited)
{
  if (!$allow_rpl_inited)
  {
    --die ERROR IN TEST: When using the replication test framework (master-slave.inc, rpl_init.inc etc), use rpl_restart_server.inc instead of restart_mysqld.inc. If you know what you are doing and you really have to use restart_mysqld.inc, set allow_rpl_inited=1 before you source restart_mysqld.inc
  }
}

# Write file to make mysql-test-run.pl expect the "crash", but don't start it
--let $_server_id= `SELECT @@server_id`
--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/mysqld.$_server_id.expect
--exec echo "wait" > $_expect_file_name

# Avoid warnings from connection threads that does not have time to exit
--disable_query_log
set @@global.log_warnings=0;
--enable_query_log

--let $server_shutdown_timeout= 60
if ($VALGRIND_TEST)
{
  --let $server_shutdown_timeout= 300
}

if ($shutdown_timeout)
{
  --let $server_shutdown_timeout= $shutdown_timeout
}
if ($shutdown_timeout == 0)
{
  --let $server_shutdown_timeout= 0
}

# Send shutdown to the connected server
--shutdown_server $server_shutdown_timeout
--source include/wait_until_disconnected.inc

