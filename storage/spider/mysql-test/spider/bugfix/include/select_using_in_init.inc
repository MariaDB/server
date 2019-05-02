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
    c1 int NOT NULL,
    c2 int NOT NULL,
    PRIMARY KEY (c1, c2)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT COUNT(*) FROM tbl_a;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE argument LIKE '%select %';
let $CHILD2_1_DATA_PREPARE=
  INSERT INTO tbl_a (c1,c2) VALUES (1, 1) $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 1, c2 + 1 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 2, c2 + 2 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 4, c2 + 4 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 8, c2 + 8 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 16, c2 + 16 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 32, c2 + 32 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 64, c2 + 64 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 128, c2 + 128 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 256, c2 + 256 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 512, c2 + 512 FROM tbl_a $STR_SEMICOLON
  INSERT INTO tbl_a (c1,c2) SELECT c1 + 1024, c2 + 1024 FROM tbl_a $STR_SEMICOLON
  FLUSH TABLES;
