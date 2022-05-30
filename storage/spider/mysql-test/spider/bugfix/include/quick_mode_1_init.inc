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
--let $MASTER_1_COMMENT_2_2_BACKUP= $MASTER_1_COMMENT_2_2
let $MASTER_1_COMMENT_2_2=
  COMMENT='table "tbl_b", srv "s_2_2"';
--let $CHILD2_1_DROP_TABLES_BACKUP= $CHILD2_1_DROP_TABLES
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey FROM tbl_a ORDER BY pkey;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%select %';
--let $CHILD2_2_DROP_TABLES_BACKUP= $CHILD2_2_DROP_TABLES
let $CHILD2_2_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_b;
--let $CHILD2_2_CREATE_TABLES_BACKUP= $CHILD2_2_CREATE_TABLES
let $CHILD2_2_CREATE_TABLES=
  CREATE TABLE tbl_b (
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
  ) $CHILD2_2_ENGINE $CHILD2_2_CHARSET;
--let $CHILD2_2_SELECT_TABLES_BACKUP= $CHILD2_2_SELECT_TABLES
let $CHILD2_2_SELECT_TABLES=
  SELECT pkey FROM tbl_b ORDER BY pkey;
let $CHILD2_2_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%select %';
--connection master_1
set @old_spider_quick_mode= @@spider_quick_mode;
set session spider_quick_mode= 1;
set @old_spider_quick_page_size= @@spider_quick_page_size;
set session spider_quick_page_size= 3;
set @old_spider_quick_page_byte= @@spider_quick_page_byte;
let $MASTER_1_SET_QUICK_PAGE_BYTE_6=
  set session spider_quick_page_byte= 6;
let $MASTER_1_SET_QUICK_PAGE_BYTE_0=
  set session spider_quick_page_byte= 0;
