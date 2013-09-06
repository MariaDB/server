let $MASTER_1_HS_COMMENT_TMP=
  COMMENT='';
let $MASTER_1_HS_COMMENT_2_1=
  COMMENT='srv "s_2_1", table "hs_r", uhr "1", uhw "1", hrp "$CHILD2_1_HSRPORT", hwp "$CHILD2_1_HSWPORT", hwr "0"';
let $MASTER_1_HS_COMMENT_P_2_1=
  COMMENT='uhr "1", uhw "1", hwr "0"'
  PARTITION BY RANGE(a) (
    PARTITION pt1 VALUES LESS THAN (4)
    COMMENT='srv "s_2_1", table "hs_r2", hrp "$CHILD2_1_HSRPORT", hwp "$CHILD2_1_HSWPORT"',
    PARTITION pt2 VALUES LESS THAN MAXVALUE
    COMMENT='srv "s_2_2", table "hs_r3", hrp "$CHILD2_2_HSRPORT", hwp "$CHILD2_2_HSWPORT"'
  );
