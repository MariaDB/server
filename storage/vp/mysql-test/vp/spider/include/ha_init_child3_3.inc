--let $CHILD3_3_ENGINE_TYPE=Spider
--let $CHILD3_3_ENGINE=ENGINE=Spider
--source ../../../../../spider/mysql-test/spider/include/init_spider.inc
eval INSERT INTO mysql.spider_link_mon_servers
(db_name, table_name, link_id, sid, server, scheme, host, port, socket,
 username, password, ssl_ca, ssl_capath, ssl_cert, ssl_cipher, ssl_key,
 ssl_verify_server_cert, default_file, default_group) VALUES
('%auto_test%', '%a%', '%', $CHILD3_1_SERVER_ID, 's_3_1', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL),
('%auto_test%', '%a%', '%', $CHILD3_2_SERVER_ID, 's_3_2', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL),
('%auto_test%', '%a%', '%', $CHILD3_3_SERVER_ID, 's_3_3', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL);
let $CHILD3_3_CHECK_LINK_STATUS=
  SELECT db_name, table_name, link_id, link_status FROM mysql.spider_tables
  ORDER BY db_name, table_name, link_id;
let $CHILD3_3_CHECK_LINK_FAILED_LOG=
  SELECT db_name, table_name, link_id FROM mysql.spider_link_failed_log;
let $CHILD3_3_SET_RECOVERY_STATUS_2_1=
  ALTER TABLE auto_test_remote.ha_r_2
  COMMENT='srv "s_2_1 s_2_2", tbl "ta_r_2 ta_r3_2", lst "0 2"';
let $CHILD3_3_SET_OK_STATUS_2_1=
  ALTER TABLE auto_test_remote.ha_r_2
  COMMENT='srv "s_2_1 s_2_2", tbl "ta_r_2 ta_r3_2", lst "0 1"';
let $CHILD3_3_SET_OK_STATUS_AS_2_1=
  ALTER TABLE auto_test_remote.as_r_2
  COMMENT='srv "s_2_1 s_2_2", tbl "ta_r_2 ta_r3_2", lst "1 0"';

let $CHILD3_3_DROP_TABLES_HA_2_1=
  SELECT 1;
let $CHILD3_3_CREATE_TABLES_HA_2_1=
  SELECT 1;
let $CHILD3_3_CREATE_TABLES_HA_AS_2_1=
  SELECT 1;
let $CHILD3_3_DROP_TABLES_HA_P_2_1=
  SELECT 1;
let $CHILD3_3_CREATE_TABLES_HA_P_2_1=
  SELECT 1;
let $CHILD3_3_CREATE_TABLES_HA_AS_P_2_1=
  SELECT 1;
let $CHILD3_3_SET_RECOVERY_STATUS_P_2_1=
  ALTER TABLE auto_test_remote.ha_r2_2
  COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2_2 ta_r4_2", lst "0 2"';
let $CHILD3_3_SET_OK_STATUS_P_2_1=
  ALTER TABLE auto_test_remote.ha_r2_2
  COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2_2 ta_r4_2", lst "0 1"';
let $CHILD3_3_SET_OK_STATUS_AS_P_2_1=
  ALTER TABLE auto_test_remote.as_r2_2
  COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2_2 ta_r4_2", lst "1 0"';

DROP DATABASE IF EXISTS auto_test_remote;
CREATE DATABASE auto_test_remote;
USE auto_test_remote;
eval CREATE TABLE ha_r (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  CONNECTION='msi "$CHILD3_1_SERVER_ID", mkd "2"'
  COMMENT='srv "s_2_1 s_2_2", tbl "ta_r ta_r3"';
eval CREATE TABLE as_r (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  CONNECTION='msi "$CHILD3_1_SERVER_ID", mkd "2", alc "1"'
  COMMENT='srv "s_2_1 s_2_2", tbl "ta_r ta_r3"';
eval CREATE TABLE ha_r_2 (
  a INT DEFAULT 10,
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  CONNECTION='msi "$CHILD3_1_SERVER_ID", mkd "2"'
  COMMENT='srv "s_2_1 s_2_2", tbl "ta_r_2 ta_r3_2"';
eval CREATE TABLE as_r_2 (
  a INT DEFAULT 10,
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  CONNECTION='msi "$CHILD3_1_SERVER_ID", mkd "2", alc "1"'
  COMMENT='srv "s_2_1 s_2_2", tbl "ta_r_2 ta_r3_2"';
eval CREATE TABLE ha_r2 (
  a INT DEFAULT 10,
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  CONNECTION='msi "$CHILD3_1_SERVER_ID", mkd "2"'
  COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2 ta_r4"';
eval CREATE TABLE as_r2 (
  a INT DEFAULT 10,
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  CONNECTION='msi "$CHILD3_1_SERVER_ID", mkd "2", alc "1"'
  COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2 ta_r4"';
eval CREATE TABLE ha_r2_2 (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  CONNECTION='msi "$CHILD3_1_SERVER_ID", mkd "2"'
  COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2_2 ta_r4_2"';
eval CREATE TABLE as_r2_2 (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  CONNECTION='msi "$CHILD3_1_SERVER_ID", mkd "2", alc "1"'
  COMMENT='srv "s_2_1 s_2_3", tbl "ta_r2_2 ta_r4_2"';
