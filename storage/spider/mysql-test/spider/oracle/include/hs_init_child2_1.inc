let $CHILD2_1_HS_DROP_TABLES=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "hs_r"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_HS_CREATE_TABLES=
  SELECT spider_direct_sql('CREATE TABLE "hs_r" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\'),
    "d" INT DEFAULT 11,
    CONSTRAINT "pk_s_2_1_hs_r" PRIMARY KEY("a")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_HS_SELECT_TABLES=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\'), "d" FROM "hs_r" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_HS_DROP_TABLES2=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "hs_r2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_HS_CREATE_TABLES2=
  SELECT spider_direct_sql('CREATE TABLE "hs_r2" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\'),
    "d" INT DEFAULT 11,
    CONSTRAINT "pk_s_2_1_hs_r2" PRIMARY KEY("a")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_HS_SELECT_TABLES2=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\'), "d" FROM "hs_r2" ORDER BY "a"', '', 'srv "s_2_1"');
