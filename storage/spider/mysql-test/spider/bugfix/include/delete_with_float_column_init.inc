--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--source ../t/slave_test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey, f FROM tbl_a ORDER BY pkey;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%delete %';
--connection slave1_1
--disable_warnings
--disable_query_log
--disable_result_log
--source ../include/init_spider.inc
--enable_result_log
--enable_query_log
--enable_warnings
--connection master_1
set @old_binlog_format= @@binlog_format;
set session binlog_format= 'ROW';
