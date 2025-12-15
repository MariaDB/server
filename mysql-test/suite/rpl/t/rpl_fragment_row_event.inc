#
#   Helper file to run a test case for rpl_fragment_row_event.test with various
# parameters to define what to verify for the test.
#
# Parameters:
#   $insert_size             (uint) : Size of the row to insert in the test
#                                     case
#   $test_mysqlbinlog_replay (bool) : Whether or not to replay the binary log
#                                     via mysqlbinlog and verify the server
#                                     executes the BINLOG base64 statements
#                                     in the same manner the SQL thread does.
#   $test_show_binlog_events (bool) : Whether or not to test SHOW BINLOG EVENTS
#   $test_update             (bool) : Whether or not to test UPDATE commands
#                                     can be fragmented
#   $test_delete             (bool) : Whether or not to test DELETE commands
#                                     can be fragmented
#   $test_multi_table_trx    (bool) : Whether or not to update multiple tables
#                                     in a single transaction
#   $server_1_max_packet     (uint) : slave_max_allowed_packet on server_1
#   $server_2_max_packet     (uint) : slave_max_allowed_packet on server_2
#   $server_3_max_packet     (uint) : slave_max_allowed_packet on server_3
#

if ($server_1_max_packet)
{
  --connection server_1
  --let $inc_server_1_old_slave_max_packet= `SELECT @@global.slave_max_allowed_packet`
  --let $inc_server_1_old_net_buf= `SELECT @@global.net_buffer_length`
  --eval set @@global.slave_max_allowed_packet= $server_1_max_packet
  --eval set @@global.net_buffer_length= $server_1_max_packet
  --disconnect server_1
  --source include/wait_until_disconnected.inc
  --connect (server_1,127.0.0.1,root,,test,$SERVER_MYPORT_1,)
}

--connection server_2
--source include/stop_slave.inc
if ($server_2_max_packet)
{
  --let $inc_server_2_old_slave_max_packet= `SELECT @@global.slave_max_allowed_packet`
  --let $inc_server_2_old_net_buf= `SELECT @@global.net_buffer_length`
  --eval set @@global.slave_max_allowed_packet= $server_2_max_packet
  --eval set @@global.net_buffer_length= $server_2_max_packet
  --disconnect server_2
  --source include/wait_until_disconnected.inc
  --connect (server_2,127.0.0.1,root,,test,$SERVER_MYPORT_2,)
}

--connection server_3
--source include/stop_slave.inc
if ($server_3_max_packet)
{
  --let $inc_server_3_old_slave_max_packet= `SELECT @@global.slave_max_allowed_packet`
  --let $inc_server_3_old_net_buf= `SELECT @@global.net_buffer_length`
  --eval set @@global.slave_max_allowed_packet= $server_3_max_packet
  --eval set @@global.net_buffer_length= $server_3_max_packet
  --disconnect server_3
  --source include/wait_until_disconnected.inc
  --connect (server_3,127.0.0.1,root,,test,$SERVER_MYPORT_3,)
}

--echo #
--echo # Initialize test data
if ($test_show_binlog_events)
{
  --let $n_servers= 3
  --let $i= 1
  while (`SELECT $i <= $n_servers`)
  {
    --connection server_$i
    --source include/wait_for_binlog_checkpoint.inc
    --inc $i
  }
}
--connection server_1
create table t1 (a int, b longblob) engine=innodb;
if ($test_multi_table_trx)
{
  create table t2 (a int, b longblob) engine=innodb;
}

--echo #
--echo # Test Write_rows_log_event fragmentation
--disable_query_log
if ($insert_size)
{
  # subtract 9 from the insert for the first column
  --let $insert_text_len= `SELECT $insert_size - 9`
  --eval insert into t1 values (1, repeat("a", $insert_text_len))
}
if (!$insert_size)
{
  insert into t1 select seq,repeat(seq,512) from seq_1_to_10;
  BEGIN;
  # Do the insert into t2 first; otherwise, this insert would also concat
  # the second statement into t1
  if ($test_multi_table_trx)
  {
    insert into t2 select 1 as a, group_concat(b) as b from t1;
  }
  insert into t1 select 11 as a, group_concat(b) as b from t1;
  COMMIT;
}
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

--echo # Ensuring Write_rows replication event replay is consistent..
--let $diff_tables= server_1:test.t1, server_2:test.t1, server_3:test.t1
--source include/diff_tables.inc
if ($test_multi_table_trx)
{
  --let $diff_tables= server_1:test.t2, server_2:test.t2, server_3:test.t2
  --source include/diff_tables.inc
}

--echo # Ensuring Write_rows data was fragmented into Partial_rows_log_events
--let $n_servers= 3
--let $i= 1
while (`SELECT $i <= $n_servers`)
{
  --echo # Server_$i binlogs for Write_rows event
  --connection server_$i
  --let $mysqld_datadir=`select @@datadir`
  --let $binlog_insert_filename= query_get_value(SHOW MASTER STATUS, File, 1)
  --let $binlog_file_param= $mysqld_datadir/$binlog_insert_filename
  --let $binlog_analysis_file= $MYSQLTEST_VARDIR/binlog_insert_frags.out
  flush logs;
  --echo # MYSQL_BINLOG binlog_file_param --result-file=binlog_analysis_file --base64-output=never
  --exec $MYSQL_BINLOG $binlog_file_param --result-file=$binlog_analysis_file --base64-output=never

  --let SEARCH_PATTERN= Partial_rows \([0-9]+ / [0-9]+\)
  --let SEARCH_FILE= $binlog_analysis_file
  --let SEARCH_OUTPUT= count
  --source include/search_pattern_in_file.inc
  --let SEARCH_OUTPUT= matches
  --source include/search_pattern_in_file.inc

  if ($test_show_binlog_events)
  {
    --echo # Ensure SHOW BINLOG EVENTS shows Partial_rows_log_event data
    --let $binlog_file= $binlog_insert_filename
    --source include/show_binlog_events.inc
  }
  --inc $i
}

if ($test_update)
{
  --echo #
  --echo # Test Update_rows_log_event fragmentation
  if ($test_show_binlog_events)
  {
    --let $n_servers= 3
    --let $i= 1
    while (`SELECT $i <= $n_servers`)
    {
      --connection server_$i
      --source include/wait_for_binlog_checkpoint.inc
      --inc $i
    }
  }
  --connection server_1
  update t1 set a=a+1 where a=11;
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
  --let $n_servers= 3
  --let $i= 1
  while (`SELECT $i <= $n_servers`)
  {
    --echo # Server_$i binlogs for Update_rows event
    --connection server_$i
    --let $mysqld_datadir=`select @@datadir`
    --let $binlog_update_filename= query_get_value(SHOW MASTER STATUS, File, 1)
    --let $binlog_file_param= $mysqld_datadir/$binlog_update_filename
    --let $binlog_analysis_file= $MYSQLTEST_VARDIR/binlog_update_frags.out
    flush logs;
    --echo # MYSQL_BINLOG binlog_file_param --result-file=binlog_analysis_file --base64-output=never
    --exec $MYSQL_BINLOG $binlog_file_param --result-file=$binlog_analysis_file --base64-output=never

    --let SEARCH_PATTERN= Partial_rows \([0-9]+ / [0-9]+\)
    --let SEARCH_FILE= $binlog_analysis_file
    --let SEARCH_OUTPUT= count
    --source include/search_pattern_in_file.inc
    --let SEARCH_OUTPUT= matches
    --source include/search_pattern_in_file.inc

    if ($test_show_binlog_events)
    {
      --echo # Ensure SHOW BINLOG EVENTS shows Partial_rows_log_event data
      --let $binlog_file= $binlog_update_filename
      --source include/show_binlog_events.inc
    }

    --inc $i
  }
}

if ($test_delete)
{
  --echo #
  --echo # Test Delete_rows_log_event fragmentation
  if ($test_show_binlog_events)
  {
    --let $n_servers= 3
    --let $i= 1
    while (`SELECT $i <= $n_servers`)
    {
      --connection server_$i
      --source include/wait_for_binlog_checkpoint.inc
      --inc $i
    }
  }
  --connection server_1
  delete from t1;
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
  --let $n_servers= 3
  --let $i= 1
  while (`SELECT $i <= $n_servers`)
  {
    --echo # Server_$i binlogs for Delete_rows event
    --connection server_$i
    --let $mysqld_datadir=`select @@datadir`
    --let $binlog_delete_filename= query_get_value(SHOW MASTER STATUS, File, 1)
    --let $binlog_file_param= $mysqld_datadir/$binlog_delete_filename
    --let $binlog_analysis_file= $MYSQLTEST_VARDIR/binlog_delete_frags.out
    flush logs;
    --echo # MYSQL_BINLOG binlog_file_param --result-file=binlog_analysis_file --base64-output=never
    --exec $MYSQL_BINLOG $binlog_file_param --result-file=$binlog_analysis_file --base64-output=never

    --let SEARCH_PATTERN= Partial_rows \([0-9]+ / [0-9]+\)
    --let SEARCH_FILE= $binlog_analysis_file
    --let SEARCH_OUTPUT= count
    --source include/search_pattern_in_file.inc
    --let SEARCH_OUTPUT= matches
    --source include/search_pattern_in_file.inc

    if ($test_show_binlog_events)
    {
      --echo # Ensure SHOW BINLOG EVENTS shows Partial_rows_log_event data
      --let $binlog_file= $binlog_delete_filename
      --source include/show_binlog_events.inc
    }

    --inc $i
  }
}

if ($test_mysqlbinlog_replay)
{
  --echo # Reset data state on server_2 for mysqlbinlog replay
  --connection server_2
  set statement sql_log_bin=0 for drop table t1;
  if ($test_multi_table_trx)
  {
    set statement sql_log_bin=0 for drop table t2;
  }

  --let $binlog_analysis_file= $MYSQLTEST_VARDIR/binlog_base64_output.sql
  --echo # MYSQL_BINLOG mysqld_datadir/binlog_insert_filename --result-file=binlog_analysis_file
  --exec $MYSQL_BINLOG $mysqld_datadir/$binlog_insert_filename --result-file=$binlog_analysis_file

  --echo # Show how many Partial_rows_log_events use mysqlbinlog @0, @1 fragmentation (ev_len > 1024)
  --let SEARCH_FILE= $binlog_analysis_file
  --let SEARCH_PATTERN= BINLOG @binlog_fragment_0, @binlog_fragment_1
  --let SEARCH_OUTPUT= count
  --source include/search_pattern_in_file.inc

  --echo # Show how many Partial_rows_log_events use regular BINLOG base64 output (ev_len <= 1024)
  --echo #  Note: 1 always comes from the Format_description_log_event
  --let SEARCH_PATTERN= BINLOG '
  --let SEARCH_OUTPUT= count
  --source include/search_pattern_in_file.inc

  --echo # MYSQL_SLAVE --init-command="set sql_log_bin=0" < binlog_analysis_file
  --exec $MYSQL_SLAVE --init-command="set sql_log_bin=0" < $binlog_analysis_file

  if ($test_update)
  {
    --echo # MYSQL_BINLOG mysqld_datadir/binlog_update_filename | MYSQL_SLAVE --init-command="set sql_log_bin=0"
    --exec $MYSQL_BINLOG $mysqld_datadir/$binlog_update_filename | $MYSQL_SLAVE --init-command="set sql_log_bin=0"
  }

  if ($test_delete)
  {
    --echo # MYSQL_BINLOG mysqld_datadir/binlog_delete_filename | MYSQL_SLAVE --init-command="set sql_log_bin=0"
    --exec $MYSQL_BINLOG $mysqld_datadir/$binlog_delete_filename | $MYSQL_SLAVE --init-command="set sql_log_bin=0"
  }

  --echo # Ensuring mysqlbinlog event replay is consistent..
  --let $diff_tables= server_1:test.t1, server_2:test.t1
  --source include/diff_tables.inc
}


--echo #
--echo # Cleanup test case
if ($server_1_max_packet)
{
  --connection server_1
  --eval set @@global.slave_max_allowed_packet= $inc_server_1_old_slave_max_packet
  --eval set @@global.net_buffer_length= $inc_server_1_old_net_buf
}

--connection server_2
if ($server_1_max_packet)
{
  --eval set @@global.slave_max_allowed_packet= $inc_server_2_old_slave_max_packet
  --eval set @@global.net_buffer_length= $inc_server_2_old_net_buf
}
--source include/start_slave.inc

--connection server_3
if ($server_1_max_packet)
{
  --eval set @@global.slave_max_allowed_packet= $inc_server_3_old_slave_max_packet
  --eval set @@global.net_buffer_length= $inc_server_3_old_net_buf
}
--source include/start_slave.inc

--connection server_1
drop table t1;
if ($test_multi_table_trx)
{
  drop table t2;
}
flush logs;
--source include/save_master_gtid.inc

--connection server_2
--source include/sync_with_master_gtid.inc
flush logs;

--connection server_3
--source include/sync_with_master_gtid.inc
flush logs;
