--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
--let $MASTER_1_COMMENT_P_2_1_BACKUP= $MASTER_1_COMMENT_P_2_1
let $MASTER_1_COMMENT_P_2_1=
  PARTITION BY RANGE(i) (
    PARTITION pt1 VALUES LESS THAN (5) COMMENT='srv "s_2_1", table "ta_r2"',
    PARTITION pt2 VALUES LESS THAN (10) COMMENT='srv "s_2_1", table "ta_r3"',
    PARTITION pt3 VALUES LESS THAN MAXVALUE COMMENT='srv "s_2_1", table "ta_r4"'
  );
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE ta_r2 (
    i INT,
    j JSON,
    PRIMARY KEY(i)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET $STR_SEMICOLON
  CREATE TABLE ta_r3 (
    i INT,
    j JSON,
    PRIMARY KEY(i)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET $STR_SEMICOLON
  CREATE TABLE ta_r4 (
    i INT,
    j JSON,
    PRIMARY KEY(i)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
