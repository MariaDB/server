# MDEV-39143 test helper: having fully applied (or halted on) the injected
# MySQL binlog, drop the test database and repoint the slave at
# master-bin.000002 - the empty binlog the master opened when
# mdev39143_inject_mysql_binlog.inc restarted it - so that master and slave
# agree on a position and rpl_end.inc can shut down cleanly.  The injected
# binlog's end has no counterpart on the master, so save_master_pos /
# sync_with_master cannot line the two up against it directly.
#
# Caller must already have stopped whichever of the slave's threads are
# still running (that part genuinely differs per test - e.g. a clean
# STOP SLAVE vs. a STOP SLAVE IO_THREAD after the SQL thread halted on an
# expected error - so it is left to the caller rather than folded in here),
# must already be on the slave connection, and must
# --let $mdev39143_cleanup_db= <database to drop> before sourcing this file;
# it is unset again afterwards.

eval DROP DATABASE $mdev39143_cleanup_db;
CHANGE MASTER TO Master_log_file='master-bin.000002', Master_log_pos=4, Master_use_gtid=No;
START SLAVE;
--connection master
--save_master_pos
--connection slave
--sync_with_master
--let $mdev39143_cleanup_db=
