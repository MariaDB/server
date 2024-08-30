--disable_warnings
--disable_query_log
--disable_result_log
--source ../../t/test_init.inc
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
  DROP TABLE IF EXISTS tbl_a $STR_SEMICOLON
  DROP TABLE IF EXISTS tbl_b;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE tbl_a (
    akey int NOT NULL,
    val int NOT NULL,
    PRIMARY KEY (akey)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET $STR_SEMICOLON
  CREATE TABLE tbl_b (
    bkey int NOT NULL,
    akey int NOT NULL,
    PRIMARY KEY (bkey)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT akey, val FROM tbl_a ORDER BY akey $STR_SEMICOLON
  SELECT bkey, akey FROM tbl_b ORDER BY bkey;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%`tbl_a`%' $STR_SEMICOLON
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%`tbl_b`%';
