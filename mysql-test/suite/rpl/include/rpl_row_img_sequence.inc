#
#   This include file validates that sequence events are properly binlogged
# and replicated.
#
# Parameters:
#   expected_columns (list<uint>, in) : A list of positive integers which
#       correspond to the column numbers that should be output in a binary
#       log's write_rows event
#

--echo # Create sequences with specific engines per server
--connection server_1
--eval SET STATEMENT sql_log_bin=0 FOR create sequence s1 cache=0 engine=$server_1_engine
--source include/save_master_gtid.inc

--connection server_2
--eval SET STATEMENT sql_log_bin=0 FOR create sequence s1 cache=0 engine=$server_2_engine
--source include/sync_with_master_gtid.inc

--connection server_3
--eval SET STATEMENT sql_log_bin=0 FOR create sequence s1 cache=0 engine=$server_3_engine
--source include/sync_with_master_gtid.inc


--echo # Pt.1 Ensure SETVAL replicates and binlogs correctly
--connection server_1
SELECT SETVAL(s1, 10);
--source include/save_master_gtid.inc

--echo # Validate SETVAL replicated correctly to other servers
--connection server_3
--source include/sync_with_master_gtid.inc
--let $diff_tables= server_1:test.s1,server_2:test.s1,server_3:test.s1
--source include/diff_tables.inc

--echo # Validate server_1 binlogged SETVAL with the correct columns
--connection server_1
--let binlog_filenamE= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;
--source include/ensure_binlog_row_event_columns.inc

--echo # Validate server_2 binlogged SETVAL with the correct columns
--connection server_2
--let binlog_filename= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;
--source include/ensure_binlog_row_event_columns.inc

--echo # Validate server_3 binlogged SETVAL with the correct columns
--connection server_3
--let binlog_filename= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;
--source include/ensure_binlog_row_event_columns.inc


--echo # Pt.2 Ensure NEXTVAL replicates and binlogs correctly
--connection server_1
--disable_ps2_protocol
SELECT NEXTVAL(s1);
--enable_ps2_protocol
--source include/save_master_gtid.inc

--echo # Validate NEXTVAL replicated correctly to other servers
--connection server_3
--source include/sync_with_master_gtid.inc
--let $diff_tables= server_1:test.s1,server_2:test.s1,server_3:test.s1
--source include/diff_tables.inc

--echo # Validate server_1 binlogged NEXTVAL with the correct columns
--connection server_1
--let binlog_filename= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;
--source include/ensure_binlog_row_event_columns.inc

--echo # Validate server_2 binlogged NEXTVAL with the correct columns
--connection server_2
--let binlog_filename= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;
--source include/ensure_binlog_row_event_columns.inc

--echo # Validate server_3 binlogged NEXTVAL with the correct columns
--connection server_3
--let binlog_filename= query_get_value(SHOW MASTER STATUS, File, 1)
FLUSH LOGS;
--source include/ensure_binlog_row_event_columns.inc


--echo # Cleanup
--connection server_1
DROP TABLE s1;
--source include/save_master_gtid.inc
--connection server_3
--source include/sync_with_master_gtid.inc
