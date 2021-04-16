--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
--let $MASTER_1_COMMENT_2_1_BACKUP= $MASTER_1_COMMENT_2_1
let $MASTER_1_COMMENT_2_1=
  COMMENT='table "tbl_a", host "127.0.0.1", port "$MASTER_1_MYPORT", user "root"';
--connection master_1
set @old_spider_same_server_link= @@spider_same_server_link;
set spider_same_server_link= ON;
