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
alter table mysql.spider_table_sts drop column checksum;
insert into mysql.spider_table_sts values ('auto_test_local', 'tbl_a', 0, 0, 0, 0, 0, '2019-01-01 00:00:00', '2019-01-01 00:00:00', '2019-01-01 00:00:00');
