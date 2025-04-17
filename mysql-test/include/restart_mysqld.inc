# ==== Usage ====
#
# [--let $shutdown_timeout= 60]
# [--let $allow_rpl_inited= 1]
# Create a copy of the var directory after restart
# [--let $$backup_on_restart=1]
# --source include/restart_mysqld.inc

--source include/not_embedded.inc

--source include/shutdown_mysqld.inc
if ($backup_on_restart)
{
  --exec /bin/tar cfzP /tmp/mtr-backup.tgz --exclude=*.sock $MYSQLTEST_VARDIR/*
}
--source include/start_mysqld.inc

# The following sleep is required to give sleep_until_file_created() time
# to notice that the server was restarted. sleep_until_file_created() would
# fail if the server was restarted or killed at once after restart.
--sleep 0.1
