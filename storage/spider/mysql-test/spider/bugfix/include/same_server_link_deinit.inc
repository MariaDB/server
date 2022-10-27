--connection master_1
set global spider_same_server_link= @old_global_spider_same_server_link;
set session spider_same_server_link= @old_session_spider_same_server_link;
--let $MASTER_1_COMMENT_2_1= $MASTER_1_COMMENT_2_1_BACKUP
--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_deinit.inc
--enable_result_log
--enable_query_log
--enable_warnings
