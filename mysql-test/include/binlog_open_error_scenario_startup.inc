#
# Sub-helper for binlog_open_error_scenario.inc — startup kind.
#
# sql/mysqld.cc's startup binlog-open call is unconditionally fatal:
#   error= mysql_bin_log.open(...);
#   if (unlikely(error)) unireg_abort(1);
# so any MYSQL_BIN_LOG::open() failure at startup aborts the whole server,
# regardless of which internal step failed -- there is no "binlog turned
# off, server stays up" outcome here (contrast with FLUSH BINARY LOGS /
# RESET MASTER, which do stay up). Confirmed via mysqld.cc:5657 and by
# running fail_init_log_file_name, which aborts with exit code 1.
#
# Shut the good server down, exec mysqld directly with the fault knob and
# assert its exit code, then bring a clean server back up -- the standard
# MTR idiom for "server expected to fail to start" (see e.g.
# main/plugin_loaderr.test).
#

# The abort is asserted by the --error 1 on the direct mysqld exec below.
--write_line wait $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--shutdown_server
--source include/wait_until_disconnected.inc

--error 1
--exec $MYSQLD_CMD --log-bin=master-bin --debug-dbug=+d,$scn_dbug --disable-log-error

--write_line restart $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--enable_reconnect
--source include/wait_until_connected_again.inc
--disable_reconnect
