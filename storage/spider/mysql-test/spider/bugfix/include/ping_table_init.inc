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
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey FROM tbl_a ORDER BY pkey;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE argument LIKE '%select %';
--connection master_1
if ($VERSION_COMPILE_OS_WIN)
{
  let $PING_COMMAND=
    SELECT spider_ping_table('.\\auto_test_local\\tbl_a', 0, 2, 1,
      ' where pkey = 1', @@server_id, 1, 0, 0, 0);
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $PING_COMMAND=
    SELECT spider_ping_table('./auto_test_local/tbl_a', 0, 2, 1,
      ' where pkey = 1', @@server_id, 1, 0, 0, 0);
}
--disable_query_log
INSERT INTO mysql.spider_link_mon_servers SET
  db_name = '%',
  table_name = '%',
  link_id = '%',
  sid = @@server_id,
  server = 's_1',
  scheme = null,
  host = null,
  port = null,
  socket = null,
  username = null,
  password = null,
  ssl_ca = null,
  ssl_capath = null,
  ssl_cert = null,
  ssl_cipher = null,
  ssl_key = null,
  ssl_verify_server_cert = 0,
  default_file = null,
  default_group = null
;
--enable_query_log
