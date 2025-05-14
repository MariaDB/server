#
#
#
# Parameters:
#   $insert_size             : (uint)
#   $test_mysqlbinlog_replay : (bool)
#   $test_update             : (bool)
#   $test_delete             : (bool)
#

--connection server_2
--source include/stop_slave.inc
--connection server_3
--source include/stop_slave.inc

--echo #
--echo # Initialize test data
--connection server_1
create table t1 (a int, b longblob) engine=innodb;

--echo #
--echo # Test Write_rows_log_event fragmentation
--disable_query_log
if ($insert_size)
{
  # subtract 8 from the insert for the first column
  --let $insert_text_len= `SELECT $insert_size - 9`
  --eval insert into t1 values (1, repeat("a", $insert_text_len))
}
if (!$insert_size)
{
  insert into t1 select seq,repeat(seq,512) from seq_1_to_10;
  insert into t1 select 11 as a, group_concat(b) as b from t1;
}
--enable_query_log

--connection server_2
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc

--connection server_3
--source include/start_slave.inc
--source include/sync_with_master_gtid.inc
--source include/stop_slave.inc

--echo # Ensuring Write_rows replication event replay is consistent..
--let $diff_tables= server_1:test.t1, server_2:test.t1, server_3:test.t1
--source include/diff_tables.inc

--echo # Ensuring Write_rows data was fragmented into Partial_rows_log_events
--connection server_1
--let $mysqld_datadir=`select @@datadir`
--let $binlog_insert_filename= query_get_value(SHOW MASTER STATUS, File, 1)
--let $binlog_file_param= $mysqld_datadir/$binlog_insert_filename
--let $binlog_analysis_file= $MYSQLTEST_VARDIR/binlog_insert_frags.out
FLUSH LOGS;
--echo # MYSQL_BINLOG binlog_file_param --result-file=binlog_analysis_file --base64-output=never
--exec $MYSQL_BINLOG $binlog_file_param --result-file=$binlog_analysis_file --base64-output=never

--let SEARCH_PATTERN= Partial_rows \([0-9]+ / [0-9]+\)
--let SEARCH_FILE= $binlog_analysis_file
--let SEARCH_OUTPUT= count
--source include/search_pattern_in_file.inc
--let SEARCH_OUTPUT= matches
--source include/search_pattern_in_file.inc


if ($test_update)
{
  --echo #
  --echo # Test Update_rows_log_event fragmentation
  --connection server_1
  --disable_query_log
  update t1 set a=a+1 where a=11;
  --enable_query_log
  --source include/save_master_gtid.inc

  --connection server_2
  --source include/start_slave.inc
  --source include/sync_with_master_gtid.inc
  --source include/stop_slave.inc

  --connection server_3
  --source include/start_slave.inc
  --source include/sync_with_master_gtid.inc
  --source include/stop_slave.inc

  --echo # Ensuring Update_rows replication event replay is consistent..
  --let $diff_tables= server_1:test.t1, server_2:test.t1, server_3:test.t1
  --source include/diff_tables.inc

  --echo # Ensuring Update_rows data was fragmented into Partial_rows_log_events
  --connection server_1
  --let $binlog_update_filename= query_get_value(SHOW MASTER STATUS, File, 1)
  --let $binlog_file_param= $mysqld_datadir/$binlog_update_filename
  --let $binlog_analysis_file= $MYSQLTEST_VARDIR/binlog_update_frags.out
  FLUSH LOGS;
  --echo # MYSQL_BINLOG binlog_file_param --result-file=binlog_analysis_file --base64-output=never
  --exec $MYSQL_BINLOG $binlog_file_param --result-file=$binlog_analysis_file --base64-output=never

  --let SEARCH_PATTERN= Partial_rows \([0-9]+ / [0-9]+\)
  --let SEARCH_FILE= $binlog_analysis_file
  --let SEARCH_OUTPUT= count
  --source include/search_pattern_in_file.inc
  --let SEARCH_OUTPUT= matches
  --source include/search_pattern_in_file.inc
}

if ($test_delete)
{
  --echo #
  --echo # Test Delete_rows_log_event fragmentation
  --connection server_1
  --disable_query_log
  delete from t1;
  --enable_query_log
  --source include/save_master_gtid.inc

  --connection server_2
  --source include/start_slave.inc
  --source include/sync_with_master_gtid.inc
  --source include/stop_slave.inc

  --connection server_3
  --source include/start_slave.inc
  --source include/sync_with_master_gtid.inc
  --source include/stop_slave.inc

  --echo # Ensuring Delete_rows replication event replay is consistent..
  --let $diff_tables= server_1:test.t1, server_2:test.t1, server_3:test.t1
  --source include/diff_tables.inc

  --echo # Ensuring Delete_rows data was fragmented into Partial_rows_log_events
  --connection server_1
  --let $binlog_delete_filename= query_get_value(SHOW MASTER STATUS, File, 1)
  --let $binlog_file_param= $mysqld_datadir/$binlog_delete_filename
  --let $binlog_analysis_file= $MYSQLTEST_VARDIR/binlog_delete_frags.out
  FLUSH LOGS;
  --echo # MYSQL_BINLOG binlog_file_param --result-file=binlog_analysis_file --base64-output=never
  --exec $MYSQL_BINLOG $binlog_file_param --result-file=$binlog_analysis_file --base64-output=never

  --let SEARCH_PATTERN= Partial_rows \([0-9]+ / [0-9]+\)
  --let SEARCH_FILE= $binlog_analysis_file
  --let SEARCH_OUTPUT= count
  --source include/search_pattern_in_file.inc
  --let SEARCH_OUTPUT= matches
  --source include/search_pattern_in_file.inc
}

if ($test_mysqlbinlog_replay)
{
  --echo # Reset data state on server_2 for mysqlbinlog replay
  --connection server_2
  set statement sql_log_bin=0 for drop table t1;

  --echo # MYSQL_BINLOG mysqld_datadir/binlog_insert_filename | MYSQL_SLAVE
  --exec $MYSQL_BINLOG $mysqld_datadir/$binlog_insert_filename | $MYSQL_SLAVE

  if ($test_update)
  {
    --echo # MYSQL_BINLOG mysqld_datadir/binlog_update_filename | MYSQL_SLAVE
    --exec $MYSQL_BINLOG $mysqld_datadir/$binlog_update_filename | $MYSQL_SLAVE
  }

  if ($test_delete)
  {
    --echo # MYSQL_BINLOG mysqld_datadir/binlog_delete_filename | MYSQL_SLAVE
    --exec $MYSQL_BINLOG $mysqld_datadir/$binlog_delete_filename | $MYSQL_SLAVE
  }

  --echo # Ensuring mysqlbinlog event replay is consistent..
  --let $diff_tables= server_1:test.t1, server_2:test.t1
  --source include/diff_tables.inc
}


--echo #
--echo # Cleanup test case
--connection server_2
--source include/start_slave.inc
--connection server_3
--source include/start_slave.inc
--connection server_1
drop table t1;
FLUSH LOGS;
--source include/save_master_gtid.inc

--connection server_3
--source include/sync_with_master_gtid.inc
