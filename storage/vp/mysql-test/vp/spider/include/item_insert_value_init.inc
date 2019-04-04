--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings

--connection child2_1
--disable_warnings
DROP DATABASE IF EXISTS auto_test_remote1;
CREATE DATABASE auto_test_remote1;
DROP DATABASE IF EXISTS auto_test_remote10;
CREATE DATABASE auto_test_remote10;
--enable_warnings

--let $MASTER_1_COMMENT_2_1_BACKUP= $MASTER_1_COMMENT_2_1
let $MASTER_1_COMMENT_2_1=
  COMMENT='table_name_list "tbl_1 tbl_4", pk_correspond_mode "1"';
let $VP_CHARSET=
  DEFAULT CHARSET=utf8;
--let $CHILD2_1_DROP_TABLES_BACKUP= $CHILD2_1_DROP_TABLES
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS `auto_test_remote1`.`tbl_a` $STR_SEMICOLON
  DROP TABLE IF EXISTS `auto_test_remote10`.`tbl_a`;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE `auto_test_remote1`.`tbl_a` (
    `id` bigint(20) NOT NULL,
    `id_2` bigint(20) NOT NULL,
    `id_3` bigint(20) NOT NULL,
    `id_4` bigint(20) DEFAULT NULL,
    PRIMARY KEY (`id`)
  ) $CHILD2_1_ENGINE $VP_CHARSET $STR_SEMICOLON
  CREATE TABLE `auto_test_remote10`.`tbl_a` (
    `id` bigint(20) NOT NULL,
    PRIMARY KEY (`id`)
  ) $CHILD2_1_ENGINE $VP_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT id FROM `auto_test_remote1`.`tbl_a` ORDER BY id $STR_SEMICOLON
  SELECT id FROM `auto_test_remote10`.`tbl_a` ORDER BY id;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE argument LIKE '%insert %';
--let $OUTPUT_CHILD_GROUP2_BACKUP= $OUTPUT_CHILD_GROUP2
--let $OUTPUT_CHILD_GROUP2= 1
--let $USE_GENERAL_LOG_BACKUP= $USE_GENERAL_LOG
--let $USE_GENERAL_LOG= 1

--connection master_1
--disable_warnings
DROP DATABASE IF EXISTS auto_test_local;
CREATE DATABASE auto_test_local;
USE auto_test_local;
--enable_warnings

let $VP_HIDDEN_COMMENT_2_1=
  COMMENT='srv "s_2_1", table "tbl_a", database "auto_test_remote1"';
let $VP_HIDDEN_COMMENT_4_1=
  COMMENT='srv "s_2_1", table "tbl_a", database "auto_test_remote10"';

eval CREATE TABLE `tbl_1` (
  `id` bigint(20) NOT NULL,
  `id_2` bigint(20) NOT NULL,
  `id_3` bigint(20) NOT NULL,
  `id_4` bigint(20) DEFAULT NULL,
  PRIMARY KEY (`id`)
) $MASTER_1_HIDDEN_ENGINE $VP_CHARSET $VP_HIDDEN_COMMENT_2_1;

eval CREATE TABLE `tbl_4` (
  `id` bigint(20) NOT NULL,
  PRIMARY KEY (`id`)
) $MASTER_1_HIDDEN_ENGINE $VP_CHARSET $VP_HIDDEN_COMMENT_4_1;
