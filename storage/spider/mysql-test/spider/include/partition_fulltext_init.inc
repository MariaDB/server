--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
--let $MASTER_1_COMMENT_2_1_BACKUP= $MASTER_1_COMMENT_2_1
let $MASTER_1_COMMENT_2_1=
  COMMENT='table "tbl_a", bka_mode "1"'
  PARTITION BY KEY(pkey) (
    PARTITION pt1 COMMENT='srv "s_2_1"',
    PARTITION pt2 COMMENT='srv "s_2_2"',
    PARTITION pt3 COMMENT='srv "s_2_3"'
  );
--let $CHILD2_1_DROP_TABLES_BACKUP= $CHILD2_1_DROP_TABLES
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    words text NOT NULL,
    PRIMARY KEY (pkey),
    FULLTEXT (words)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey FROM tbl_a ORDER BY pkey;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%select %';
--let $CHILD2_2_DROP_TABLES_BACKUP= $CHILD2_2_DROP_TABLES
let $CHILD2_2_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_2_CREATE_TABLES_BACKUP= $CHILD2_2_CREATE_TABLES
let $CHILD2_2_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    words text NOT NULL,
    PRIMARY KEY (pkey),
    FULLTEXT (words)
  ) $CHILD2_2_ENGINE $CHILD2_2_CHARSET;
--let $CHILD2_2_SELECT_TABLES_BACKUP= $CHILD2_2_SELECT_TABLES
let $CHILD2_2_SELECT_TABLES=
  SELECT pkey FROM tbl_a ORDER BY pkey;
let $CHILD2_2_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%select %';
--let $CHILD2_3_DROP_TABLES_BACKUP= $CHILD2_3_DROP_TABLES
let $CHILD2_3_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_3_CREATE_TABLES_BACKUP= $CHILD2_3_CREATE_TABLES
let $CHILD2_3_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    words text NOT NULL,
    PRIMARY KEY (pkey),
    FULLTEXT (words)
  ) $CHILD2_3_ENGINE $CHILD2_3_CHARSET;
--let $CHILD2_3_SELECT_TABLES_BACKUP= $CHILD2_3_SELECT_TABLES
let $CHILD2_3_SELECT_TABLES=
  SELECT pkey FROM tbl_a ORDER BY pkey;
let $CHILD2_3_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%select %';
--let $OUTPUT_CHILD_GROUP2_BACKUP= $OUTPUT_CHILD_GROUP2
--let $OUTPUT_CHILD_GROUP2= 1
--let $USE_GENERAL_LOG_BACKUP= $USE_GENERAL_LOG
--let $USE_GENERAL_LOG= 1
--connection master_1
set @old_join_cache_level= @@join_cache_level;
set session join_cache_level= 5;
set @old_optimizer_switch= @@optimizer_switch;
set session optimizer_switch= 'mrr=on';
