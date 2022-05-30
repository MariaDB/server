--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
let $MASTER_1_COMMENT_CONST_TABLE_JOIN=
  COMMENT 'table "tbl_person"';
let $MASTER_1_COMMENT_CONST_TABLE_JOIN=
  COMMENT 'table "tbl_ncd_cm_person"';
let $MASTER_1_ROW_FORMAT_CONST_TABLE_JOIN=
  ROW_FORMAT = Dynamic;
let $MASTER_1_PART_CONST_TABLE_JOIN=
  PARTITION BY LIST COLUMNS (region) PARTITIONS 1
  (PARTITION pt1 values in (510411)
   COMMENT = 'tbl "tbl_person", srv "s_2_1"' MAX_ROWS = 0 MIN_ROWS = 0);
let $MASTER_1_PART_CONST_TABLE2_JOIN=
  PARTITION BY LIST COLUMNS (region) PARTITIONS 1
  (PARTITION pt1 values in (510411)
   COMMENT = 'tbl "tbl_ncd_cm_person", srv "s_2_1"' MAX_ROWS = 0 MIN_ROWS = 0);
let $CHILD2_1_ROW_FORMAT_CONST_TABLE_JOIN=
  ROW_FORMAT = Dynamic;
let $CHILD2_1_DROP_CONST_TABLE_JOIN=
  DROP TABLE IF EXISTS tbl_person;
let $CHILD2_1_CREATE_CONST_TABLE_JOIN=
  CREATE TABLE tbl_person (
    id VARCHAR(50) NOT NULL,
    hr_status VARCHAR(50) NULL DEFAULT NULL,
    region_code VARCHAR(50) NULL DEFAULT NULL,
    region INT(11) NOT NULL,
    PRIMARY KEY (id, region) USING BTREE
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET $CHILD2_1_ROW_FORMAT_CONST_TABLE_JOIN;
let $CHILD2_1_DROP_CONST_TABLE2_JOIN=
  DROP TABLE IF EXISTS tbl_ncd_cm_person;
let $CHILD2_1_CREATE_CONST_TABLE2_JOIN=
  CREATE TABLE tbl_ncd_cm_person (
    id VARCHAR(50) NOT NULL,
    person_id VARCHAR(50) NULL DEFAULT '',
    diseaseKind_id VARCHAR(50) NULL DEFAULT NULL,
    region INT(11) NOT NULL,
    PRIMARY KEY (id, region) USING BTREE
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET $CHILD2_1_ROW_FORMAT_CONST_TABLE_JOIN;
let $CHILD2_1_SELECT_CONST_TABLE_JOIN=
  SELECT * FROM tbl_person;
let $CHILD2_1_SELECT_CONST_TABLE2_JOIN=
  SELECT * FROM tbl_ncd_cm_person;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%select %';
--let $OUTPUT_CHILD_GROUP2_BACKUP= $OUTPUT_CHILD_GROUP2
--let $OUTPUT_CHILD_GROUP2= 1
--let $USE_GENERAL_LOG_BACKUP= $USE_GENERAL_LOG
--let $USE_GENERAL_LOG= 1
