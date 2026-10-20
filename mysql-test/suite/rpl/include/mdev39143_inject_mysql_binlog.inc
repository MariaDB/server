# MDEV-39143 test helper: substitute a MySQL-recorded binlog fixture for the
# MariaDB master's own master-bin.000001, so the slave can be tested against
# real MySQL-generated row events without a MySQL server being involved in
# the test itself.  The server must be down while the file is swapped, and
# is restarted so it reopens the new master-bin.000001 and starts a fresh
# master-bin.000002 of its own.
#
# Caller must --let $mdev39143_binlog_fixture= <path to the .000001 file>
# before sourcing this file; it is unset again afterwards.

--connection master
--let $datadir= `SELECT @@datadir`
--let $rpl_server_number= 1
--source include/rpl_stop_server.inc
--remove_file $datadir/master-bin.000001
--copy_file $mdev39143_binlog_fixture $datadir/master-bin.000001
--source include/rpl_start_server.inc
--let $mdev39143_binlog_fixture=
