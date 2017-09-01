# ==== Usage ====
#
# [--let $restart_parameters= --innodb-force-recovery=0 --innodb-read-only=1]
# [--let $mysqld_stub_cmd= $MYSQLD_LAST_CMD]
# [--let $error_log= $MYSQLTEST_VARDIR/log/mysqld.1.err]
# --source include/fail_restart_mysqld.inc

# Evaluate the default of $error_log
if (!$error_log)
{
  --let $error_log= $MYSQLTEST_VARDIR/log/mysqld.1.err
}

--error 1
--exec $mysqld_stub_cmd $restart_parameters >> $error_log  2>&1

# As the server is stopped
--disable_reconnect
