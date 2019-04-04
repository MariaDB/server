--source ../../include/init_vp.inc
--source ../../../../../spider/mysql-test/spider/include/init_spider.inc
SET spider_internal_sql_log_off= 0;
SET spider_connect_timeout= 600;
SET @optimizer_switch_backup = @@optimizer_switch;
SET optimizer_switch = 'engine_condition_pushdown=on';
let $MASTER_1_COMMENT_2_1=
  COMMENT='default_database "auto_test_remote"'
  CONNECTION='table_name_list "ta_r ta_r_2"';
let $MASTER_1_COMMENT2_2_1=
  COMMENT='default_database "auto_test_remote"'
  CONNECTION='table_name_list "ta_r_no_idx"';
#let $MASTER_1_COMMENT_P_2_1=
#  COMMENT='default_database "auto_test_remote"'
#  CONNECTION='table_name_list "ta_r2 ta_r2_2"';
#If you apply the patch, you can use table partitioning test.
let $MASTER_1_COMMENT_P_2_1=
  COMMENT='default_database "auto_test_remote2"'
  CONNECTION='table_name_list "ta_r2 ta_r2_2"'
  PARTITION BY KEY(a) (
    PARTITION pt1 COMMENT='default_database "auto_test_remote"',
    PARTITION pt2 COMMENT='table_name_list "ta_r3 ta_r3_2"'
  );
#let $MASTER_1_COMMENT2_P_2_1=
#  COMMENT='default_database "auto_test_remote"'
#  CONNECTION='table_name_list "ta_r2 ta_r2_2"';
#If you apply the patch, you can use table partitioning test.
let $MASTER_1_COMMENT2_P_2_1=
  COMMENT='default_database "auto_test_remote2"'
  CONNECTION='table_name_list "ta_r2 ta_r2_2"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4)
    COMMENT='default_database "auto_test_remote"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='table_name_list "ta_r3 ta_r3_2"'
  );
let $MASTER_1_COMMENT3_2_1=
  COMMENT='default_database "auto_test_remote"'
  CONNECTION='table_name_list "ta_r_auto_inc ta_r_auto_inc_2"';
#let $MASTER_1_COMMENT3_P_2_1=
#  COMMENT='default_database "auto_test_remote"'
#  CONNECTION='table_name_list "ta_r_int ta_r_int_2"';
#If you apply the patch, you can use table partitioning test.
let $MASTER_1_COMMENT3_P_2_1=
  CONNECTION='table_name_list "ta_r_int ta_r_int_2"'
  PARTITION BY LIST(MOD(a, 2)) (
    PARTITION pt1 VALUES IN (0)
    COMMENT='default_database "auto_test_remote"',
    PARTITION pt2 VALUES IN (1)
    COMMENT='default_database "auto_test_remote2"'
  );
let $MASTER_1_COMMENT4_2_1=
  COMMENT='default_database "auto_test_remote"'
  CONNECTION='table_name_list "ta_r_int ta_r_int_2"';
let $MASTER_1_COMMENT5_2_1=
  COMMENT='default_database "auto_test_remote"'
  CONNECTION='table_name_list "ta_r_3"';
let $MASTER_1_COMMENT_FT_2_1=
  COMMENT='default_database "auto_test_remote"'
  CONNECTION='table_name_list "ft_r ft_r_2"';
let $MASTER_1_COMMENT2_FT_P_2_1=
  COMMENT='default_database "auto_test_remote2"'
  CONNECTION='table_name_list "ft_r2 ft_r2_2"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4)
    COMMENT='default_database "auto_test_remote"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='table_name_list "ft_r3 ft_r3_2"'
  );
let $MASTER_1_COMMENT_GM_2_1=
  COMMENT='default_database "auto_test_remote"'
  CONNECTION='table_name_list "gm_r gm_r_2"';
let $MASTER_1_COMMENT2_GM_P_2_1=
  COMMENT='default_database "auto_test_remote2"'
  CONNECTION='table_name_list "gm_r2 gm_r2_2"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4)
    COMMENT='default_database "auto_test_remote"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='table_name_list "gm_r3 gm_r3_2"'
  );
let $MASTER_1_COMMENT_LOCK1=
  COMMENT 'tnl "auto_test_remote.t1_1 auto_test_remote2.t1_2"';
let $MASTER_1_COMMENT_LOCK2=
  COMMENT 'tnl "auto_test_remote2.t2_1 auto_test_remote.t2_2"';
let $MASTER_1_COMMENT_INCREMENT1_1=
  COMMENT='tnl "ai1_1", default_database "auto_test_remote"';
let $MASTER_1_COMMENT_INCREMENT1_P_1=
  PARTITION BY LIST(MOD(id, 2)) (
    PARTITION pt1 VALUES IN (0)
    COMMENT='tnl "ai1_1", default_database "auto_test_remote"',
    PARTITION pt2 VALUES IN (1)
    COMMENT='tnl "ai1_2", default_database "auto_test_remote2"'
  );
let $MASTER_1_COMMENT_READONLY1_1=
  COMMENT='tnl "ro1_1", default_database "auto_test_remote"';
let $MASTER_1_COMMENT_ERROR_MODE1_1=
  COMMENT 'tnl "em1_1", default_database "auto_test_remote"';
let $MASTER_1_COMMENT_TEXT_PK1_1=
  COMMENT 'tnl "t1", default_database "auto_test_remote"';
let $MASTER_1_COMMENT_TEXT_KEY1_1=
  COMMENT 'tnl "t2", default_database "auto_test_remote"';
let $MASTER_1_AUTO_INCREMENT_INCREMENT1=
  SET SESSION AUTO_INCREMENT_INCREMENT = 1 $STR_SEMICOLON
  SELECT spider_direct_sql('SET SESSION AUTO_INCREMENT_INCREMENT = 1', '',
    'srv "s_2_1"') $STR_SEMICOLON
  SELECT spider_direct_sql('SET SESSION AUTO_INCREMENT_INCREMENT = 1', '',
    'srv "s_2_2"');
let $MASTER_1_AUTO_INCREMENT_INCREMENT2=
  SET SESSION AUTO_INCREMENT_INCREMENT = 777 $STR_SEMICOLON
  SELECT spider_direct_sql('SET SESSION AUTO_INCREMENT_INCREMENT = 4', '',
    'srv "s_2_1"') $STR_SEMICOLON
  SELECT spider_direct_sql('SET SESSION AUTO_INCREMENT_INCREMENT = 4', '',
    'srv "s_2_2"');
let $MASTER_1_AUTO_INCREMENT_OFFSET1=
  SET SESSION AUTO_INCREMENT_OFFSET = 1 $STR_SEMICOLON
  SELECT spider_bg_direct_sql('SET SESSION AUTO_INCREMENT_OFFSET = 1', '',
    'srv "s_2_1"') $STR_SEMICOLON
  SELECT spider_bg_direct_sql('SET SESSION AUTO_INCREMENT_OFFSET = 1', '',
    'srv "s_2_2"');
let $MASTER_1_AUTO_INCREMENT_OFFSET2=
  SET SESSION AUTO_INCREMENT_OFFSET = 777 $STR_SEMICOLON
  SELECT spider_bg_direct_sql('SET SESSION AUTO_INCREMENT_OFFSET = 2', '',
    'srv "s_2_1"') $STR_SEMICOLON
  SELECT spider_bg_direct_sql('SET SESSION AUTO_INCREMENT_OFFSET = 3', '',
    'srv "s_2_2"');
let $MASTER_1_AUTO_INCREMENT_OFFSET3=
  SET SESSION AUTO_INCREMENT_OFFSET = 1;
let $MASTER_1_AUTO_INCREMENT_OFFSET4=
  SET SESSION AUTO_INCREMENT_OFFSET = 777;
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_HIDDEN_COMMENT_2_1=
    COMMENT='database "auto_test_remote"'
    CONNECTION='host "localhost", port "$CHILD2_1_MYPORT", user "root",
    password ""';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_HIDDEN_COMMENT_2_1=
    COMMENT='database "auto_test_remote"'
    CONNECTION='host "localhost", socket "$CHILD2_1_MYSOCK", user "root",
    password ""';
}
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_HIDDEN_COMMENT_2_2=
    COMMENT='database "auto_test_remote2"'
    CONNECTION='host "localhost", port "$CHILD2_2_MYPORT", user "root",
    password ""';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_HIDDEN_COMMENT_2_2=
    COMMENT='database "auto_test_remote2"'
    CONNECTION='host "localhost", socket "$CHILD2_2_MYSOCK", user "root",
    password ""';
}
let $MASTER_1_HIDDEN_COMMENT_INCREMENT_2_1=
  COMMENT='aim "3", srv "s_2_1", bsz "0"';
let $MASTER_1_HIDDEN_COMMENT_INCREMENT_2_2=
  COMMENT='aim "3", srv "s_2_2", bsz "0"';
let $MASTER_1_HIDDEN_COMMENT_READONLY_2_1=
  COMMENT 'read_only_mode "1", tbl "ai1_1", srv "s_2_1"';
let $MASTER_1_HIDDEN_COMMENT_ERROR_MODE_2_1=
  COMMENT 'erm "1", ewm "1", tbl "ter1_1", srv "s_2_1"';
let $MASTER_1_HIDDEN_COMMENT_TEXT_PK_2_1=
  COMMENT='srv "s_2_1", tbl "t1"';
let $MASTER_1_HIDDEN_COMMENT_TEXT_KEY_2_1=
  COMMENT='srv "s_2_1", tbl "t1"';
DROP DATABASE IF EXISTS auto_test_remote;
CREATE DATABASE auto_test_remote;
USE auto_test_remote;
eval CREATE TABLE ta_r (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  PRIMARY KEY(a),
  KEY idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r_2 (
  a INT DEFAULT 10,
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r2 (
  a INT DEFAULT 10,
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r2_2 (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r_no_idx (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r_auto_inc (
  a INT AUTO_INCREMENT,
  b CHAR(1) DEFAULT 'c',
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r_auto_inc_2 (
  a INT AUTO_INCREMENT,
  b CHAR(1) DEFAULT 'c',
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r_int (
  a INT AUTO_INCREMENT,
  b INT DEFAULT 10,
  c INT DEFAULT 11,
  PRIMARY KEY(a),
  KEY idx2(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r_int_2 (
  a INT AUTO_INCREMENT,
  b INT DEFAULT 10,
  c INT DEFAULT 11,
  PRIMARY KEY(a),
  KEY idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ta_r_3 (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a, b, c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ft_r (
  a INT DEFAULT 0,
  b TEXT,
  c TEXT,
  PRIMARY KEY(a),
  FULLTEXT INDEX ft_idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ft_r_2 (
  a INT DEFAULT 0,
  c TEXT,
  d TEXT,
  PRIMARY KEY(a),
  FULLTEXT INDEX ft_idx2(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ft_r2 (
  a INT DEFAULT 0,
  b TEXT,
  c TEXT,
  PRIMARY KEY(a),
  FULLTEXT INDEX ft_idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ft_r2_2 (
  a INT DEFAULT 0,
  c TEXT,
  d TEXT,
  PRIMARY KEY(a),
  FULLTEXT INDEX ft_idx2(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE gm_r (
  a INT DEFAULT 0,
  b GEOMETRY NOT NULL,
  PRIMARY KEY(a),
  SPATIAL INDEX sp_idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE gm_r_2 (
  a INT DEFAULT 0,
  c GEOMETRY NOT NULL,
  PRIMARY KEY(a),
  SPATIAL INDEX sp_idx2(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE gm_r2 (
  a INT DEFAULT 0,
  b GEOMETRY NOT NULL,
  PRIMARY KEY(a),
  SPATIAL INDEX sp_idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE gm_r2_2 (
  a INT DEFAULT 0,
  c GEOMETRY NOT NULL,
  PRIMARY KEY(a),
  SPATIAL INDEX sp_idx2(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE t1_1 (
  id int(11) NOT NULL,
  PRIMARY KEY (id)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE t2_2 (
  id int(11) NOT NULL,
  PRIMARY KEY (id)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_1;
eval CREATE TABLE ai1_1 (
  id int(11) NOT NULL AUTO_INCREMENT,
  PRIMARY KEY (id)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_INCREMENT_2_1;
eval CREATE TABLE ro1_1 (
  id int(11) NOT NULL,
  PRIMARY KEY (id)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_READONLY_2_1;
eval CREATE TABLE em1_1 (
  id int(11) NOT NULL,
  PRIMARY KEY (id)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_ERROR_MODE_2_1;
eval CREATE TABLE t1 (
  a VARCHAR(255),
  PRIMARY KEY (a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET2
  $MASTER_1_HIDDEN_COMMENT_TEXT_PK_2_1;
eval CREATE TABLE t2 (
  a VARCHAR(255),
  b VARCHAR(255),
  c VARCHAR(255),
  KEY idx1(a,b),
  KEY idx2(b),
  PRIMARY KEY(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_TEXT_KEY_2_1;
DROP DATABASE IF EXISTS auto_test_remote2;
CREATE DATABASE auto_test_remote2;
USE auto_test_remote2;
eval CREATE TABLE ta_r3 (
  a INT DEFAULT 10,
  b CHAR(1) DEFAULT 'c',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE ta_r3_2 (
  a INT DEFAULT 10,
  c DATETIME DEFAULT '1999-10-10 10:10:10',
  PRIMARY KEY(a)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE ft_r3 (
  a INT DEFAULT 0,
  b TEXT,
  c TEXT,
  PRIMARY KEY(a),
  FULLTEXT INDEX ft_idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE ft_r3_2 (
  a INT DEFAULT 0,
  c TEXT,
  d TEXT,
  PRIMARY KEY(a),
  FULLTEXT INDEX ft_idx2(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE gm_r3 (
  a INT DEFAULT 0,
  b GEOMETRY NOT NULL,
  PRIMARY KEY(a),
  SPATIAL INDEX sp_idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE gm_r3_2 (
  a INT DEFAULT 0,
  c GEOMETRY NOT NULL,
  PRIMARY KEY(a),
  SPATIAL INDEX sp_idx2(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE ta_r_int (
  a INT AUTO_INCREMENT,
  b INT DEFAULT 10,
  c INT DEFAULT 11,
  PRIMARY KEY(a),
  KEY idx2(c)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE ta_r_int_2 (
  a INT AUTO_INCREMENT,
  b INT DEFAULT 10,
  c INT DEFAULT 11,
  PRIMARY KEY(a),
  KEY idx1(b)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE t1_2 (
  id int(11) NOT NULL,
  PRIMARY KEY (id)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE t2_1 (
  id int(11) NOT NULL,
  PRIMARY KEY (id)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_2_2;
eval CREATE TABLE ai1_2 (
  id int(11) NOT NULL AUTO_INCREMENT,
  PRIMARY KEY (id)
) $MASTER_1_HIDDEN_ENGINE $MASTER_1_HIDDEN_CHARSET
  $MASTER_1_HIDDEN_COMMENT_INCREMENT_2_2;
let $MASTER_1_CHECK_DIRECT_UPDATE_STATUS=
  SHOW STATUS LIKE 'Spider_direct_update';
let $MASTER_1_CHECK_DIRECT_DELETE_STATUS=
  SHOW STATUS LIKE 'Spider_direct_delete';
let $MASTER_1_CHECK_DIRECT_ORDER_LIMIT_STATUS=
  SHOW STATUS LIKE 'Spider_direct_order_limit';
let $MASTER_1_CHECK_DIRECT_AGGREGATE_STATUS=
  SHOW STATUS LIKE 'Spider_direct_aggregate';
