eval INSERT INTO mysql.spider_link_mon_servers
(db_name, table_name, link_id, sid, server, scheme, host, port, socket,
 username, password, ssl_ca, ssl_capath, ssl_cert, ssl_cipher, ssl_key,
 ssl_verify_server_cert, default_file, default_group) VALUES
('%auto_test_local%', '%ta_l%', '%', $CHILD3_1_SERVER_ID, 's_3_1', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL),
('%auto_test_local%', '%ta_l%', '%', $CHILD3_2_SERVER_ID, 's_3_2', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL),
('%auto_test_local%', '%ta_l%', '%', $CHILD3_3_SERVER_ID, 's_3_3', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL);
let $MASTER_1_CHECK_LINK_STATUS=
  SELECT db_name, table_name, link_id, link_status FROM mysql.spider_tables
  ORDER BY db_name, table_name, link_id;
let $MASTER_1_CHECK_LINK_FAILED_LOG=
  SELECT db_name, table_name, link_id FROM mysql.spider_link_failed_log;
let $MASTER_1_SET_RECOVERY_STATUS_2_1=
  ALTER TABLE ta_l
  CONNECTION='host "localhost", user "root", password "",
  msi "$CHILD3_1_SERVER_ID", mkd "2",
  database "auto_test_remote auto_test_remote2", lst "0 2"';
let $MASTER_1_SET_OK_STATUS_2_1=
  ALTER TABLE ta_l
  CONNECTION='host "localhost", user "root", password "",
  msi "$CHILD3_1_SERVER_ID", mkd "2",
  database "auto_test_remote auto_test_remote2", lst "0 1"';
let $MASTER_1_SET_OK_STATUS_AS_2_1=
  ALTER TABLE ta_l
  CONNECTION='host "localhost", user "root", password "",
  msi "$CHILD3_1_SERVER_ID", mkd "2", alc "1",
  database "auto_test_remote auto_test_remote2", lst "1 0"';
let $MASTER_1_COPY_TABLES_2_1=
  SELECT spider_copy_tables('ta_l', '0', '1');

if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT_HA_2_1=
    COMMENT='port "$CHILD2_1_MYPORT $CHILD2_2_MYPORT", table "ta_r ta_r3"'
    CONNECTION='host "localhost", user "root", password "",
    msi "$CHILD3_1_SERVER_ID", mkd "2",
    database "auto_test_remote auto_test_remote2"';
  let $MASTER_1_COMMENT_HA_AS_2_1=
    COMMENT='port "$CHILD2_1_MYPORT $CHILD2_2_MYPORT", table "ta_r ta_r3"'
    CONNECTION='host "localhost", user "root", password "",
    msi "$CHILD3_1_SERVER_ID", mkd "2", alc "1",
    database "auto_test_remote auto_test_remote2"';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT_HA_2_1=
    COMMENT='socket "$CHILD2_1_MYSOCK $CHILD2_2_MYSOCK", table "ta_r ta_r3"'
    CONNECTION='host "localhost", user "root", password "",
    msi "$CHILD3_1_SERVER_ID", mkd "2",
    database "auto_test_remote auto_test_remote2"';
  let $MASTER_1_COMMENT_HA_AS_2_1=
    COMMENT='socket "$CHILD2_1_MYSOCK $CHILD2_2_MYSOCK", table "ta_r ta_r3"'
    CONNECTION='host "localhost", user "root", password "",
    msi "$CHILD3_1_SERVER_ID", mkd "2", alc "1",
    database "auto_test_remote auto_test_remote2"';
}
let $MASTER_1_COMMENT_HA_P_2_1=
  COMMENT='msi "$CHILD3_1_SERVER_ID", mkd "2"'
  PARTITION BY KEY(a) (
    PARTITION pt1 COMMENT='srv "s_2_1 s_2_2", tbl "ta_r ta_r3",
    priority "1000"',
    PARTITION pt2 COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2 ta_r4",
    priority "1000001"'
  );
let $MASTER_1_COMMENT_HA_AS_P_2_1=
  COMMENT='msi "$CHILD3_1_SERVER_ID", mkd "2", alc "1"'
  PARTITION BY KEY(a) (
    PARTITION pt1 COMMENT='srv "s_2_1 s_2_2", tbl "ta_r ta_r3",
    priority "1000"',
    PARTITION pt2 COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2 ta_r4",
    priority "1000001"'
  );
let $MASTER_1_SET_RECOVERY_STATUS_P_2_1=
  ALTER TABLE ta_l2
  PARTITION BY KEY(a) (
    PARTITION pt1 COMMENT='srv "s_2_1 s_2_2", tbl "ta_r ta_r3",
    priority "1000"',
    PARTITION pt2 COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2 ta_r4",
    priority "1000001", lst "0 2"'
  );
let $MASTER_1_SET_OK_STATUS_P_2_1=
  ALTER TABLE ta_l2
  PARTITION BY KEY(a) (
    PARTITION pt1 COMMENT='srv "s_2_1 s_2_2", tbl "ta_r ta_r3",
    priority "1000"',
    PARTITION pt2 COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2 ta_r4",
    priority "1000001", lst "0 1"'
  );
let $MASTER_1_SET_OK_STATUS_AS_P_2_1=
  ALTER TABLE ta_l2
  PARTITION BY KEY(a) (
    PARTITION pt1 COMMENT='srv "s_2_1 s_2_2", tbl "ta_r ta_r3",
    priority "1000"',
    PARTITION pt2 COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2 ta_r4",
    priority "1000001", lst "1 0"'
  );
let $MASTER_1_COPY_TABLES_P_2_1=
  SELECT spider_copy_tables('ta_l2#P#pt2', '0', '1');
let $MASTER_1_CHECK_HA_STATUS=
  SHOW STATUS LIKE 'Spider_mon_table_cache_version%';
let $MASTER_1_CHANGE_HA_MON=
  SELECT spider_flush_table_mon_cache();
