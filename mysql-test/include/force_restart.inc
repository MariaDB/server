# ==== Purpose ====
#
# Tell mtr that all servers must be restarted after the test has
# finished.
#
# ==== Usage ====
#
# --source include/force_restart.inc
#
--source include/get_datadir.inc

--let $_force_restart_datadir= $MARIADB_DATADIR
--append_file $_force_restart_datadir/mtr/force_restart
1
EOF
