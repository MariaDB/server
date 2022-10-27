--connection master_1
alter table mysql.spider_table_sts add column checksum bigint unsigned default null after update_time;
DROP DATABASE IF EXISTS auto_test_local;

--let $MASTER_1_COMMENT_2_1= $MASTER_1_COMMENT_2_1_BACKUP
--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_deinit.inc
--enable_result_log
--enable_query_log
--enable_warnings
