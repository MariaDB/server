--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
--let $MASTER_1_COMMENT_2_1_BACKUP= $MASTER_1_COMMENT_2_1
let $MASTER_1_COMMENT_2_1=
  COMMENT='table "tbl_a", srv "s_2_1"';
--let $CHILD2_1_DROP_TABLES_BACKUP= $CHILD2_1_DROP_TABLES
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    txt_utf8 char(8) NOT NULL,
    txt_cp932 char(8) NOT NULL COLLATE cp932_japanese_ci,
    PRIMARY KEY (pkey)
  ) $CHILD2_1_ENGINE DEFAULT CHARACTER SET utf8;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey, txt_utf8, txt_cp932 FROM tbl_a ORDER BY pkey;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND (argument LIKE '%insert %' OR argument LIKE '%update %');
--connection master_1
set @old_spider_direct_dup_insert= @@spider_direct_dup_insert;
set session spider_direct_dup_insert= 1;
