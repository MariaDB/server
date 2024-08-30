--source delete_with_float_column_init.inc
--let $MASTER_1_COMMENT_2_1_BACKUP= $MASTER_1_COMMENT_2_1
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT_2_1=
    COMMENT='table "tbl_a",
      host "localhost",
      database "auto_test_remote",
      user "root",
      password "",
      port "$CHILD2_1_MYPORT"';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT_2_1=
    COMMENT='table "tbl_a",
      host "localhost",
      database "auto_test_remote",
      user "root",
      password "",
      socket "$CHILD2_1_MYSOCK"';
}
--let $CHILD2_1_DROP_TABLES_BACKUP= $CHILD2_1_DROP_TABLES
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    f float DEFAULT NULL,
    PRIMARY KEY (pkey)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
