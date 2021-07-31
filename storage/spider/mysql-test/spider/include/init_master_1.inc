--source ../include/init_spider.inc
SET spider_direct_order_limit= 10000;
SET spider_init_sql_alloc_size= 1;
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT_2_1=
    COMMENT='database "auto_test_remote", table "ta_r"'
    CONNECTION='host "localhost", port "$CHILD2_1_MYPORT", user "root",
    password ""';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT_2_1=
    COMMENT='database "auto_test_remote", table "ta_r"'
    CONNECTION='host "localhost", socket "$CHILD2_1_MYSOCK", user "root",
    password ""';
}
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT2_2_1=
    COMMENT='database "auto_test_remote", table "ta_r_no_idx"'
    CONNECTION='host "localhost", port "$CHILD2_1_MYPORT", user "root",
    password "", prt "2000000"';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT2_2_1=
    COMMENT='database "auto_test_remote", table "ta_r_no_idx"'
    CONNECTION='host "localhost", socket "$CHILD2_1_MYSOCK", user "root",
    password "", prt "2000000"';
}
let $MASTER_1_COMMENT_P_2_1=
  COMMENT='table "ta_r3"'
  PARTITION BY KEY(a) (
    PARTITION pt1 COMMENT='srv "s_2_1", table "ta_r2",
    priority "1000"',
    PARTITION pt2 COMMENT='srv "s_2_2", priority "1000001"'
  );
let $MASTER_1_COMMENT2_P_2_1=
  COMMENT='table "ta_r3"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4) COMMENT='srv "s_2_1",
    table "ta_r2", priority "1000"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='srv "s_2_2", priority "1000001"'
  );
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT3_2_1=
    COMMENT='database "auto_test_remote", table "ta_r_auto_inc"'
    CONNECTION='host "localhost", port "$CHILD2_1_MYPORT", user "root",
    password ""';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT3_2_1=
    COMMENT='database "auto_test_remote", table "ta_r_auto_inc"'
    CONNECTION='host "localhost", socket "$CHILD2_1_MYSOCK", user "root",
    password ""';
}
let $MASTER_1_COMMENT3_P_2_1=
  COMMENT='table "ta_r_int"'
  PARTITION BY LIST(MOD(a, 2)) (
    PARTITION pt1 VALUES IN (0)
    COMMENT='srv "s_2_1", priority "1000"',
    PARTITION pt2 VALUES IN (1)
    COMMENT='srv "s_2_2", priority "1000001"'
  );
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT4_2_1=
    COMMENT='database "auto_test_remote", table "ta_r_int"'
    CONNECTION='host "localhost", port "$CHILD2_1_MYPORT", user "root",
    password ""';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT4_2_1=
    COMMENT='database "auto_test_remote", table "ta_r_int"'
    CONNECTION='host "localhost", socket "$CHILD2_1_MYSOCK", user "root",
    password ""';
}
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT5_2_1=
    COMMENT='database "auto_test_remote", table "ta_r_3"'
    CONNECTION='host "localhost", port "$CHILD2_1_MYPORT", user "root",
    password ""';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT5_2_1=
    COMMENT='database "auto_test_remote", table "ta_r_3"'
    CONNECTION='host "localhost", socket "$CHILD2_1_MYSOCK", user "root",
    password ""';
}
let $MASTER_1_COMMENT6_P_1_1=
  COMMENT='database "auto_test_remote", table "ta_ob"'
  PARTITION BY LIST COLUMNS (e) PARTITIONS 1
  (PARTITION pt1 values in (510411) COMMENT = 'srv "s_2_1"');
if ($VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT_FT_2_1=
    COMMENT='database "auto_test_remote", table "ft_r"'
    CONNECTION='host "localhost", port "$CHILD2_1_MYPORT", user "root",
    password ""';
}
if (!$VERSION_COMPILE_OS_WIN)
{
  let $MASTER_1_COMMENT_FT_2_1=
    COMMENT='database "auto_test_remote", table "ft_r"'
    CONNECTION='host "localhost", socket "$CHILD2_1_MYSOCK", user "root",
    password ""';
}
let $MASTER_1_COMMENT2_FT_P_2_1=
  COMMENT='table "ft_r3"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4) COMMENT='srv "s_2_1",
    table "ft_r2", priority "1000"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='srv "s_2_2", priority "1000001"'
  );
let $MASTER_1_COMMENT_GM_2_1=
  COMMENT='srv "s_2_1", table "gm_r"';
let $MASTER_1_COMMENT2_GM_P_2_1=
  COMMENT='table "gm_r3"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4) COMMENT='srv "s_2_1",
    table "gm_r2", priority "1000"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='srv "s_2_2", priority "1000001"'
  );
let $MASTER_1_COMMENT_LOCK1=
  COMMENT 'tbl "t1_1 t1_2", srv "s_2_1 s_2_2"';
let $MASTER_1_COMMENT_LOCK2=
  COMMENT 'tbl "t2_1 t2_2", srv "s_2_2 s_2_1"';
let $MASTER_1_COMMENT_INCREMENT1_1=
  COMMENT 'aim "0", tbl "t1_1", srv "s_2_1"';
let $MASTER_1_COMMENT_INCREMENT1_P_1=
  COMMENT 'aim "0"'
  PARTITION BY LIST(MOD(id, 2)) (
    PARTITION pt1 VALUES IN (0)
    COMMENT='tbl "t1_1", srv "s_2_1"',
    PARTITION pt2 VALUES IN (1)
    COMMENT='tbl "t1_2", srv "s_2_2"'
  );
let $MASTER_1_COMMENT_READONLY1_1=
  COMMENT 'read_only_mode "1", tbl "t1_1", srv "s_2_1"';
let $MASTER_1_COMMENT_ERROR_MODE1_1=
  COMMENT 'erm "1", ewm "1", tbl "ter1_1", srv "s_2_1"';
let $MASTER_1_COMMENT_TEXT_PK1_1=
  COMMENT 'tbl "t1", srv "s_2_1"';
let $MASTER_1_COMMENT_TEXT_KEY1_1=
  COMMENT 'tbl "t1", srv "s_2_1"';
let $MASTER_1_COMMENT_MDEV_25985=
  COMMENT='table "t1"'
  PARTITION BY LIST COLUMNS(`a`) (
    PARTITION `pt1` DEFAULT COMMENT = 'srv "s_2_1"'
  );
let $MASTER_1_CHECK_DIRECT_UPDATE_STATUS=
  SHOW STATUS LIKE 'Spider_direct_update';
let $MASTER_1_CHECK_DIRECT_DELETE_STATUS=
  SHOW STATUS LIKE 'Spider_direct_delete';
let $MASTER_1_CHECK_DIRECT_ORDER_LIMIT_STATUS=
  SHOW STATUS LIKE 'Spider_direct_order_limit';
let $MASTER_1_CHECK_DIRECT_AGGREGATE_STATUS=
  SHOW STATUS LIKE 'Spider_direct_aggregate';
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
