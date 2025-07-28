#
# Helper file that ensures mysqlbinlog --stop-position behavior for local
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
--echo # --stop-position tests
--echo #

--echo #  Case 1.a) With one binlog file, a --stop-position before the end of
--echo # the file should not result in a warning
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-position=binlog_f1_pre_rotate binlog_f1_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-position=$binlog_f1_pre_rotate $binlog_f1_full --result-file=$binlog_out 2>&1

--echo #
--echo #  Case 1.b) With one binlog file, a --stop-position at the exact end of
--echo # the file should not result in a warning
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-position=binlog_f1_end binlog_f1_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-position=$binlog_f1_end $binlog_f1_full --result-file=$binlog_out 2>&1

--echo #
--echo #  Case 1.c) With one binlog file, a --stop-position past the end of the
--echo # file should(!) result in a warning
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --short-form --stop-position=binlog_f1_over_eof binlog_f1_full --result-file=$binlog_out_relpath 2>&1
--replace_result $binlog_f1_over_eof <BINLOG_F1_OVER_EOF>
--sorted_result
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --short-form --stop-position=$binlog_f1_over_eof $binlog_f1_full --result-file=$binlog_out 2>&1

--echo #
--echo #  Case 2.a) With two binlog files, a --stop-position targeting b2 which
--echo # exists in the size of b1 should:
--echo #    1) not provide any warnings
--echo #    2) not prevent b2 from outputting its desired events before the
--echo #       stop position
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-position=binlog_f2_mid binlog_f1_full binlog_f2_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-position=$binlog_f2_mid $binlog_f1_full $binlog_f2_full --result-file=$binlog_out 2>&1

--let $server_id= `SELECT @@GLOBAL.server_id`
--let $assert_file= $binlog_out
--let $assert_text= Ensure all intended GTIDs are present
--let $assert_select= GTID $domain_id-$server_id-
--let $assert_count= 4
--source include/assert_grep.inc

--let $assert_text= Ensure the next GTID binlogged is _not_ present
--let $assert_select= GTID $binlog_f2_gtid_after_midpoint
--let $assert_count= 0
--source include/assert_grep.inc

--echo #
--echo #  Case 2.b) With two binlog files, a --stop-position targeting the end
--echo # of binlog 2 should:
--echo #    1) not provide any warnings
--echo #    2) not prevent b2 from outputting its entire binary log
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-position=binlog_f2_end binlog_f1_full binlog_f2_full --result-file=$binlog_out_relpath 2>&1
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-position=$binlog_f2_end $binlog_f1_full $binlog_f2_full --result-file=$binlog_out 2>&1

--let $server_id= `SELECT @@GLOBAL.server_id`
--let $assert_text= Ensure a GTID exists for each transaction
--let $assert_select= GTID $domain_id-$server_id-
--let $assert_count= 8
--source include/assert_grep.inc

--let $assert_text= Ensure the last GTID binlogged is present
--let $assert_select= GTID $binlog_f2_last_gtid
--let $assert_count= 1
--source include/assert_grep.inc

--echo #
--echo #  Case 2.c) With two binlog files, a --stop-position targeting beyond
--echo # the eof of binlog 2 should:
--echo #    1) provide a warning that the stop position was not reached
--echo #    2) not prevent b2 from outputting its entire binary log
--echo # MYSQL_BINLOG $PARAM_READ_FROM_REMOTE_OUT --stop-position=binlog_f2_over_eof binlog_f1_full binlog_f2_full --result-file=$binlog_out_relpath 2>&1
--replace_result $binlog_f2_over_eof <BINLOG_F2_OVER_EOF>
--sorted_result
--exec $MYSQL_BINLOG $PARAM_READ_FROM_REMOTE --stop-position=$binlog_f2_over_eof $binlog_f1_full $binlog_f2_full --result-file=$binlog_out 2>&1

--let $server_id= `SELECT @@GLOBAL.server_id`
--let $assert_text= Ensure a GTID exists for each transaction
--let $assert_select= GTID $domain_id-$server_id-
--let $assert_count= 8
--source include/assert_grep.inc
