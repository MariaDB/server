#
# Helper file for rpl_master_slave_mismatched_columns_full.test to test various
# combinations of binlog_row_image
#

--echo # binlog_row_image=FULL
--connection server_1
set @save_binlog_row_image= @@global.binlog_row_image;
set @@global.binlog_row_image=FULL;
--connection server_2
set @save_binlog_row_image= @@global.binlog_row_image;
set @@global.binlog_row_image=FULL;
--source rpl_master_slave_mismatched_columns_full_null.inc

--echo # binlog_row_image=MINIMAL
--connection server_1
set @@global.binlog_row_image=MINIMAL;
--connection server_2
set @@global.binlog_row_image=MINIMAL;
--source rpl_master_slave_mismatched_columns_full_null.inc


--echo # binlog_row_image=FULL
--connection server_1
set @@global.binlog_row_image= @save_binlog_row_image;
--connection server_2
set @@global.binlog_row_image= @save_binlog_row_image;
--source rpl_master_slave_mismatched_columns_full_null.inc

