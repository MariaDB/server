--connection master_1
set global spider_internal_lock_wait_timeout= @old_spider_internal_lock_wait_timeout;
--let $MASTER_1_COMMENT_2_1= $MASTER_1_COMMENT_2_1_BACKUP
--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_deinit.inc
--enable_result_log
--enable_query_log
--enable_warnings
