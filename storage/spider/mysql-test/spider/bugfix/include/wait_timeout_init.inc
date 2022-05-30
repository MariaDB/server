--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
if (!$HAVE_PARTITION)
{
  --source wait_timeout_deinit.inc
  --enable_result_log
  --enable_query_log
  --enable_warnings
  skip Test requires partitioning;
}
--enable_result_log
--enable_query_log
--enable_warnings
--let $MASTER_1_COMMENT_2_1_BACKUP= $MASTER_1_COMMENT_2_1
let $MASTER_1_COMMENT_2_1=
  COMMENT='table "tbl_a"'
  PARTITION BY KEY(pkey) (
    PARTITION pt1 COMMENT='srv "s_2_1"',
    PARTITION pt2 COMMENT='srv "s_2_2"'
  );
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
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%insert %';
--let $CHILD2_2_DROP_TABLES_BACKUP= $CHILD2_2_DROP_TABLES
let $CHILD2_2_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_2_CREATE_TABLES_BACKUP= $CHILD2_2_CREATE_TABLES
let $CHILD2_2_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
  ) $CHILD2_2_ENGINE $CHILD2_2_CHARSET;
--let $CHILD2_2_SELECT_TABLES_BACKUP= $CHILD2_2_SELECT_TABLES
let $CHILD2_2_SELECT_TABLES=
  SELECT pkey FROM tbl_a ORDER BY pkey;
let $CHILD2_2_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%insert %';
--connection child2_1
set @old_wait_timeout= @@wait_timeout;
set global wait_timeout= 1;
--connection child2_2
set @old_wait_timeout= @@wait_timeout;
set global wait_timeout= 1;
