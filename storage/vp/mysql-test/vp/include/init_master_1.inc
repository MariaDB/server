--source ../include/init_vp.inc
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
  COMMENT='tnl "t1_1", default_database "auto_test_remote"';
let $MASTER_1_COMMENT_INCREMENT1_P_1=
  PARTITION BY LIST(MOD(id, 2)) (
    PARTITION pt1 VALUES IN (0)
    COMMENT='tnl "t1_1", default_database "auto_test_remote"',
    PARTITION pt2 VALUES IN (1)
    COMMENT='tnl "t1_2", default_database "auto_test_remote2"'
  );
let $MASTER_1_COMMENT_TEXT_PK1_1=
  COMMENT 'tnl "t1", default_database "auto_test_remote"';
let $MASTER_1_COMMENT_TEXT_KEY1_1=
  COMMENT 'tnl "t1", default_database "auto_test_remote"';
let $MASTER_1_CHECK_DIRECT_UPDATE_STATUS=
  SHOW STATUS LIKE 'Vp_direct_update';
let $MASTER_1_CHECK_DIRECT_DELETE_STATUS=
  SHOW STATUS LIKE 'Vp_direct_delete';
let $MASTER_1_CHECK_DIRECT_ORDER_LIMIT_STATUS=
  SHOW STATUS LIKE 'Vp_direct_order_limit';
let $MASTER_1_CHECK_DIRECT_AGGREGATE_STATUS=
  SHOW STATUS LIKE 'Vp_direct_aggregate';
let $MASTER_1_AUTO_INCREMENT_INCREMENT1=
  SET SESSION AUTO_INCREMENT_INCREMENT = 1;
let $MASTER_1_AUTO_INCREMENT_INCREMENT2=
  SET SESSION AUTO_INCREMENT_INCREMENT = 777;
let $MASTER_1_AUTO_INCREMENT_OFFSET1=
  SET SESSION AUTO_INCREMENT_OFFSET = 1;
let $MASTER_1_AUTO_INCREMENT_OFFSET2=
  SET SESSION AUTO_INCREMENT_OFFSET = 777;
let $MASTER_1_AUTO_INCREMENT_OFFSET3=
  SET SESSION AUTO_INCREMENT_OFFSET = 1;
let $MASTER_1_AUTO_INCREMENT_OFFSET4=
  SET SESSION AUTO_INCREMENT_OFFSET = 777;
