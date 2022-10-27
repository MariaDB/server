--let $MASTER_1_COMMENT_2_1= $MASTER_1_COMMENT_2_1_BACKUP
--let $MASTER_1_COMMENT_2_2= $MASTER_1_COMMENT_2_2_BACKUP
--let $CHILD2_1_DROP_TABLES= $CHILD2_1_DROP_TABLES_BACKUP
--let $CHILD2_1_CREATE_TABLES= $CHILD2_1_CREATE_TABLES_BACKUP
--let $CHILD2_1_SELECT_TABLES= $CHILD2_1_SELECT_TABLES_BACKUP
--let $CHILD2_2_DROP_TABLES= $CHILD2_2_DROP_TABLES_BACKUP
--let $CHILD2_2_CREATE_TABLES= $CHILD2_2_CREATE_TABLES_BACKUP
--let $CHILD2_2_SELECT_TABLES= $CHILD2_2_SELECT_TABLES_BACKUP
--let $OUTPUT_CHILD_GROUP2= $OUTPUT_CHILD_GROUP2_BACKUP
--let $USE_GENERAL_LOG= $USE_GENERAL_LOG_BACKUP
--connection master_1
set session spider_quick_mode= @old_spider_quick_mode;
set session spider_quick_page_size= @old_spider_quick_page_size;
set session spider_quick_page_byte= @old_spider_quick_page_byte;
--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_deinit.inc
--enable_result_log
--enable_query_log
--enable_warnings
