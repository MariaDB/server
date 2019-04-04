--connection master_1
DROP DATABASE IF EXISTS auto_test_local;
--connection child2_1
DROP DATABASE IF EXISTS auto_test_remote1;
DROP DATABASE IF EXISTS auto_test_remote10;
--let $MASTER_1_COMMENT_2_1= $MASTER_1_COMMENT_2_1_BACKUP
--let $CHILD2_1_DROP_TABLES= $CHILD2_1_DROP_TABLES_BACKUP
--let $CHILD2_1_CREATE_TABLES= $CHILD2_1_CREATE_TABLES_BACKUP
--let $CHILD2_1_SELECT_TABLES= $CHILD2_1_SELECT_TABLES_BACKUP
--let $OUTPUT_CHILD_GROUP2= $OUTPUT_CHILD_GROUP2_BACKUP
--let $USE_GENERAL_LOG= $USE_GENERAL_LOG_BACKUP
--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_deinit.inc
--enable_result_log
--enable_query_log
--enable_warnings
