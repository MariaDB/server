--source ../include/init_spider.inc
SET spider_direct_order_limit= 10000;
SET spider_init_sql_alloc_size= 1;
SET spider_conn_recycle_mode= 2;
let $MASTER_1_COMMENT_2_1=
  COMMENT='database "$ORACLE_DATABASE", table "ta_r", wrapper "oracle",
  pk_name "pk_s_2_1_ta_r"'
  CONNECTION='host "$ORACLE_HOST", port "$ORACLE_PORT", user "$ORACLE_USER",
  password "$ORACLE_PASSWORD"';
let $MASTER_1_COMMENT2_2_1=
  COMMENT='database "$ORACLE_DATABASE", table "ta_r_no_idx", wrapper "oracle",
  pk_name "pk_s_2_1_ta_r_no_idx"'
  CONNECTION='host "$ORACLE_HOST", port "$ORACLE_PORT", user "$ORACLE_USER",
  password "$ORACLE_PASSWORD", prt "2000000"';
let $MASTER_1_COMMENT_P_2_1=
  COMMENT='table "ta_r3"'
  PARTITION BY KEY(a) (
    PARTITION pt1 COMMENT='srv "s_2_1", table "ta_r2",
    priority "1000", pk_name "pk_s_2_1_ta_r2"',
    PARTITION pt2 COMMENT='srv "s_2_2", priority "1000001",
    pk_name "pk_s_2_2_ta_r3"'
  );
let $MASTER_1_COMMENT2_P_2_1=
  COMMENT='table "ta_r3"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4) COMMENT='srv "s_2_1",
    table "ta_r2", priority "1000", pk_name "pk_s_2_1_ta_r2"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='srv "s_2_2", priority "1000001", pk_name "pk_s_2_2_ta_r3"'
  );
let $MASTER_1_COMMENT3_2_1=
  COMMENT='database "$ORACLE_DATABASE", table "ta_r_auto_inc",
  wrapper "oracle"'
  CONNECTION='host "$ORACLE_HOST", port "$ORACLE_PORT", user "$ORACLE_USER",
  password "$ORACLE_PASSWORD", pk_name "pk_s_2_1_ta_r_auto_inc"';
let $MASTER_1_COMMENT3_P_2_1=
  COMMENT='table "s_2_1_ta_r_int"'
  PARTITION BY LIST(MOD(a, 2)) (
    PARTITION pt1 VALUES IN (0)
    COMMENT='srv "s_2_1", priority "1000", pk_name "pk_s_2_1_ta_r_int"',
    PARTITION pt2 VALUES IN (1)
    COMMENT='srv "s_2_2", priority "1000001", pk_name "pk_s_2_2_ta_r_int",
    table "s_2_2_ta_r_int"'
  );
let $MASTER_1_COMMENT4_2_1=
  COMMENT='database "$ORACLE_DATABASE", table "s_2_1_ta_r_int",
  wrapper "oracle"'
  CONNECTION='host "$ORACLE_HOST", port "$ORACLE_PORT", user "$ORACLE_USER",
  password "$ORACLE_PASSWORD", pk_name "pk_s_2_1_ta_r_int"';
let $MASTER_1_COMMENT5_2_1=
  COMMENT='database "$ORACLE_DATABASE", table "ta_r_3", wrapper "oracle"'
  CONNECTION='host "$ORACLE_HOST", port "$ORACLE_PORT", user "$ORACLE_USER",
  password "$ORACLE_PASSWORD", pk_name "pk_s_2_1_ta_r_3"';
let $MASTER_1_COMMENT_FT_2_1=
  COMMENT='database "$ORACLE_DATABASE", table "ft_r", wrapper "oracle"'
  CONNECTION='host "$ORACLE_HOST", port "$ORACLE_PORT", user "$ORACLE_USER",
  password "$ORACLE_PASSWORD", pk_name "pk_s_2_1_ft_r"';
let $MASTER_1_COMMENT2_FT_P_2_1=
  COMMENT='table "ft_r3"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4) COMMENT='srv "s_2_1",
    table "ft_r2", priority "1000", pk_name "pk_s_2_1_ft_r2"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='srv "s_2_2", priority "1000001", pk_name "pk_s_2_2_ft_r3"'
  );
let $MASTER_1_COMMENT_GM_2_1=
  COMMENT='srv "s_2_1", table "gm_r", pk_name "pk_s_2_1_gm_r"';
let $MASTER_1_COMMENT2_GM_P_2_1=
  COMMENT='table "gm_r3"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4) COMMENT='srv "s_2_1",
    table "gm_r2", priority "1000", pk_name "pk_s_2_1_gm_r2"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='srv "s_2_2", priority "1000001", pk_name "pk_s_2_2_gm_r3"'
  );
let $MASTER_1_COMMENT_LOCK1=
  COMMENT 'tbl "t1_1 t1_2", srv "s_2_1 s_2_2",
  pk_name "pk_s_2_1_t1_1 pk_s_2_2_t1_2"';
let $MASTER_1_COMMENT_LOCK2=
  COMMENT 'tbl "t2_1 t2_2", srv "s_2_2 s_2_1",
  pk_name "pk_s_2_2_t2_1 pk_s_2_1_t2_2"';
let $MASTER_1_COMMENT_INCREMENT1_1=
  COMMENT 'aim "2", sqn "seq_t1_1", tbl "t1_1", srv "s_2_1",
  pk_name "pk_s_2_1_t1_1"';
let $MASTER_1_COMMENT_INCREMENT1_P_1=
  COMMENT 'aim "2"'
  PARTITION BY LIST(MOD(id, 2)) (
    PARTITION pt1 VALUES IN (0)
    COMMENT='sqn "seq_t1_1", tbl "t1_1", srv "s_2_1", pk_name "pk_s_2_1_t1_1"',
    PARTITION pt2 VALUES IN (1)
    COMMENT='sqn "seq_t1_2", tbl "t1_2", srv "s_2_2", pk_name "pk_s_2_2_t1_2"'
  );
let $MASTER_1_COMMENT_READONLY1_1=
  COMMENT 'read_only_mode "1", tbl "t1_1", srv "s_2_1",
  pk_name "pk_s_2_1_t1_1"';
let $MASTER_1_COMMENT_ERROR_MODE1_1=
  COMMENT 'erm "1", ewm "1", tbl "ter1_1", srv "s_2_1",
  pk_name "pk_s_2_1_ter1_1"';
let $MASTER_1_COMMENT_TEXT_PK1_1=
  COMMENT 'tbl "t1", srv "s_2_1", pk_name "pk_s_2_1_t1"';
let $MASTER_1_COMMENT_TEXT_KEY1_1=
  COMMENT 'tbl "t1", srv "s_2_1", pk_name "pk_s_2_1_t1"';
let $MASTER_1_CHECK_DIRECT_UPDATE_STATUS=
  SHOW GLOBAL STATUS LIKE 'Spider_direct_update%';
let $MASTER_1_CHECK_DIRECT_DELETE_STATUS=
  SHOW GLOBAL STATUS LIKE 'Spider_direct_delete%';
let $MASTER_1_CHECK_DIRECT_ORDER_LIMIT_STATUS=
  SHOW GLOBAL STATUS LIKE 'Spider_direct_order_limit%';
let $MASTER_1_CHECK_DIRECT_AGGREGATE_STATUS=
  SHOW GLOBAL STATUS LIKE 'Spider_direct_aggregate%';
let $MASTER_1_AUTO_INCREMENT_INCREMENT1=
  SET SESSION AUTO_INCREMENT_INCREMENT = 1 $STR_SEMICOLON
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_1" START WITH 1 INCREMENT BY 1', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_2"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_2" START WITH 1 INCREMENT BY 1', '', 'srv "s_2_2"');
let $MASTER_1_AUTO_INCREMENT_INCREMENT2=
  SET SESSION AUTO_INCREMENT_INCREMENT = 777 $STR_SEMICOLON
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_1" START WITH 2 INCREMENT BY 4', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_2"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_2" START WITH 3 INCREMENT BY 4', '', 'srv "s_2_2"');
let $MASTER_1_AUTO_INCREMENT_OFFSET1=
  SET SESSION AUTO_INCREMENT_OFFSET = 1 $STR_SEMICOLON
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_1" START WITH 1 INCREMENT BY 1', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_2"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_2" START WITH 1 INCREMENT BY 1', '', 'srv "s_2_2"');
let $MASTER_1_AUTO_INCREMENT_OFFSET2=
  SET SESSION AUTO_INCREMENT_OFFSET = 777 $STR_SEMICOLON
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_1" START WITH 2 INCREMENT BY 4', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_2"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_2" START WITH 3 INCREMENT BY 4', '', 'srv "s_2_2"');
let $MASTER_1_AUTO_INCREMENT_OFFSET3=
  SET SESSION AUTO_INCREMENT_OFFSET = 1;
let $MASTER_1_AUTO_INCREMENT_OFFSET4=
  SET SESSION AUTO_INCREMENT_OFFSET = 777;
