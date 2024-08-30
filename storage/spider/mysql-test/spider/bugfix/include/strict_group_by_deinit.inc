--let $MASTER_1_COMMENT_2_1= $MASTER_1_COMMENT_2_1_BACKUP
--let $CHILD2_1_DROP_TABLES= $CHILD2_1_DROP_TABLES_BACKUP
--let $CHILD2_1_CREATE_TABLES= $CHILD2_1_CREATE_TABLES_BACKUP
--let $CHILD2_1_SELECT_TABLES= $CHILD2_1_SELECT_TABLES_BACKUP
--let $CHILD2_2_DROP_TABLES= $CHILD2_2_DROP_TABLES_BACKUP
--let $CHILD2_2_CREATE_TABLES= $CHILD2_2_CREATE_TABLES_BACKUP
--let $CHILD2_2_SELECT_TABLES= $CHILD2_2_SELECT_TABLES_BACKUP
--connection child2_1
set global sql_mode= @old_sql_mode;
--connection master_1
set session spider_sync_sql_mode= @old_spider_sync_sql_mode;
--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_deinit.inc
--enable_result_log
--enable_query_log
--enable_warnings
