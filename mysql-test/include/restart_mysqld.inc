# ==== Usage ====
#
# [--let $shutdown_timeout= 60]
# [--let $allow_rpl_inited= 1]
# --source include/restart_mysqld.inc

--source include/not_embedded.inc

--source include/shutdown_mysqld.inc
--source include/start_mysqld.inc
