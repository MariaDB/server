# ==== Usage ====
#
# [--let $shutdown_timeout= 60]
# [--let $allow_rpl_inited= 1]
# --source include/restart_mysqld.inc

--source include/not_embedded.inc

--source include/shutdown_mysqld.inc
--source include/start_mysqld.inc

# The following sleep is required to give sleep_until_file_created() time
# to notice that the server was restarted. sleep_until_file_created() would
# fail if the server was restarted or killed at once after restart.
--sleep 0.1
