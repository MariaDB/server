--let $CHILD2_1_CREATE_TABLES= $CHILD2_1_CREATE_TABLES_BACKUP
--let $CHILD2_1_SELECT_TABLES= $CHILD2_1_SELECT_TABLES_BACKUP
--let $OUTPUT_CHILD_GROUP2= $OUTPUT_CHILD_GROUP2_BACKUP
--let $USE_GENERAL_LOG= $USE_GENERAL_LOG_BACKUP
--connection master_1
set session spider_quick_mode= @old_spider_quick_mode;
set session spider_quick_page_size= @old_spider_quick_page_size;
--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_deinit.inc
--enable_result_log
--enable_query_log
--enable_warnings
