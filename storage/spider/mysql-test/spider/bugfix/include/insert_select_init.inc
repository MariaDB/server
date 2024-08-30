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
--let $MASTER_1_COMMENT_2_1_2_BACKUP= $MASTER_1_COMMENT_2_1_2
let $MASTER_1_COMMENT_2_1_2=
  COMMENT='table "tbl_b", srv "s_2_1"';
--let $CHILD2_1_DROP_TABLES_BACKUP= $CHILD2_1_DROP_TABLES
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_1_DROP_TABLES2_BACKUP= $CHILD2_1_DROP_TABLES2
let $CHILD2_1_DROP_TABLES2=
  DROP TABLE IF EXISTS tbl_b;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE tbl_a (
    skey int NOT NULL,
    dt date NOT NULL,
    tm time NOT NULL,
    KEY idx1 (skey,dt,tm)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_CREATE_TABLES2_BACKUP= $CHILD2_1_CREATE_TABLES2
let $CHILD2_1_CREATE_TABLES2=
  CREATE TABLE tbl_b (
    pkey int NOT NULL,
    dt datetime NOT NULL,
    PRIMARY KEY (pkey),
    KEY idx1 (dt)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey, dt FROM tbl_b ORDER BY pkey;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%select %';
--connection master_1
set @old_spider_direct_dup_insert= @@spider_direct_dup_insert;
set session spider_direct_dup_insert= 1;
