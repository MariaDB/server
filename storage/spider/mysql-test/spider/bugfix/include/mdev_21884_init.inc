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
--let $MASTER_1_COMMENT_P_2_1_BACKUP= $MASTER_1_COMMENT_P_2_1
let $MASTER_1_COMMENT_P_2_1=
  COMMENT='table "ta_r3"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (10) COMMENT='srv "s_2_1", table "ta_r2",
    priority "1000"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE COMMENT='srv "s_2_1", priority "1000001"'
  );
--let $CHILD2_1_DROP_TABLES_BACKUP= $CHILD2_1_DROP_TABLES
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS ta_r2 $STR_SEMICOLON
  DROP TABLE IF EXISTS ta_r3;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    txt char(8) NOT NULL,
    PRIMARY KEY (pkey, txt)
  ) $CHILD2_1_ENGINE DEFAULT CHARACTER SET utf8;
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE ta_r2 (
    a INT,
    b CHAR(1),
    c DATETIME,
    PRIMARY KEY(a,b)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET $STR_SEMICOLON
  CREATE TABLE ta_r3 (
    a INT,
    b CHAR(1),
    c DATETIME,
    PRIMARY KEY(a,b)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey, txt FROM tbl_a ORDER BY pkey;
let $CHILD2_1_SELECT_TABLES=
  SELECT a, b, c FROM ta_r2 ORDER BY a $STR_SEMICOLON
  SELECT a, b, c FROM ta_r3 ORDER BY a;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%select %';
--connection master_1
set @old_spider_bgs_mode= @@spider_bgs_mode;
set session spider_bgs_mode= 2;
set @old_spider_quick_mode= @@spider_quick_mode;
set session spider_quick_mode= 3;
