#
# Helper file that ensures mysqlbinlog --stop-datetime behavior for local
# files or a remote server
#
# Parameters:
#   read_from_remote_server (bool): A boolean that changes which source to use
#   for mysqlbinlog. When true, reads remotely; when false, uses local files.
#

--connection default
--let $MYSQLD_DATADIR= `select @@datadir`

# PARAM_READ_FROM_REMOTE is used as a parameter to mysqlbinlog (_OUT suffix is
# output in echo commands). If using local files, they are blank; if reading
# from remote server, it is overridden to the correct values.
--let $PARAM_READ_FROM_REMOTE=
# Used in echo statements to remove potentially changing values
--let $PARAM_READ_FROM_REMOTE_OUT=

if ($read_from_remote_server)
{
  --let $PARAM_READ_FROM_REMOTE= --read-from-remote-server --user=root --host=127.0.0.1 --port=$MASTER_MYPORT
  --let $PARAM_READ_FROM_REMOTE_OUT= --read-from-remote-server
  # binlog files in --read-from-remote-server don't use file system path
  --let $binlog_f1_full= $binlog_f1
  --let $binlog_f2_full= $binlog_f2
}
if (!$read_from_remote_server)
{
  # If using local files, file system path to the binlog files is needed
  --let $binlog_f1_full= $MYSQLD_DATADIR/$binlog_f1
  --let $binlog_f2_full= $MYSQLD_DATADIR/$binlog_f2
}

--echo #
--echo # --stop-datetime tests
--echo # Note: MDEV-35528 reported that mysqlbinlog would fail on tests cases
--echo # 2.a, 2.b, and 2.c.
--echo #

--echo #  Case 1.a) With one binlog file, a --stop-datetime before the end of
--echo # the file should not result in a warning
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-datetime='$b1_timestamp2' binlog_f1_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-datetime='$b1_timestamp2' $binlog_f1_full --result-file=$binlog_out 2>&1

--echo #
--echo #  Case 1.b) With one binlog file, a --stop-datetime at the end of the
--echo # file should not result in a warning
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-datetime='$b1_timestamp3' binlog_f1_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-datetime='$b1_timestamp3' $binlog_f1_full --result-file=$binlog_out 2>&1

--echo #
--echo #  Case 1.c) With one binlog file, a --stop-datetime beyond the end of
--echo # the file should(!) result in a warning
--let $future_timestamp= 2035-12-06 10:20:30.123456
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-datetime='$b2_timestamp1' binlog_f1_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-datetime='$b2_timestamp1' $binlog_f1_full --result-file=$binlog_out 2>&1

--echo #
--echo #  Case 2.a) With two binlog files, a --stop-datetime within the
--echo # timespan of binlog 2 should:
--echo #    1) not provide any warnings
--echo #    2) not prevent binlog 1 or 2 from outputting the desired events

--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-datetime='$b2_timestamp1' binlog_f1_full binlog_f2_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-datetime='$b2_timestamp1' $binlog_f1_full $binlog_f2_full --result-file=$binlog_out 2>&1

--let $server_id= `SELECT @@GLOBAL.server_id`
--let $domain_id= `SELECT @@GLOBAL.gtid_domain_id`
--let $assert_file= $binlog_out
--let $assert_text= Ensure all intended GTIDs are present
--let $assert_select= GTID $domain_id-$server_id-
--let $assert_count= 3
--source include/assert_grep.inc

--let $assert_text= Ensure the next GTID binlogged is _not_ present
--let $assert_select= GTID $binlog_f2_gtid_after_midpoint
--let $assert_count= 0
--source include/assert_grep.inc

--echo #
--echo #  Case 2.b) With two binlog files, a --stop-datetime at the end of
--echo # binlog 2 should:
--echo #    1) not provide any warnings
--echo #    2) not prevent binlog 1 or 2 from outputting all events
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-datetime='$b2_timestamp3' binlog_f1_full binlog_f2_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-datetime='$b2_timestamp3' $binlog_f1_full $binlog_f2_full --result-file=$binlog_out 2>&1

--let $assert_text= Ensure a GTID exists for each transaction
--let $assert_select= GTID $domain_id-$server_id-
--let $assert_count= 8
--source include/assert_grep.inc

--echo #
--echo #  Case 2.c) With two binlog files, a --stop-datetime beyond the end of
--echo # binlog 2 should:
--echo #    1) provide a warning that the stop datetime was not reached
--echo #    2) not prevent binlog 1 or 2 from outputting all events
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-datetime='$b2_timestamp_not_reached' binlog_f1_full binlog_f2_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-datetime='$b2_timestamp_not_reached' $binlog_f1_full $binlog_f2_full --result-file=$binlog_out 2>&1

--let $assert_text= Ensure a GTID exists for each transaction
--let $assert_select= GTID $domain_id-$server_id-
--let $assert_count= 8
--source include/assert_grep.inc

