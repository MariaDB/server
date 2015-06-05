--connection master_1
SELECT spider_direct_sql('ALTER SESSION SET NLS_DATE_FORMAT=\'YYYY-MM-DD HH24:MI:SS\'', '', 'srv "s_2_3"');
SELECT spider_direct_sql('ALTER SESSION SET NLS_TIME_FORMAT=\'HH24:MI:SSXFF\'', '', 'srv "s_2_3"');
SELECT spider_direct_sql('ALTER SESSION SET NLS_TIMESTAMP_FORMAT=\'YYYY-MM-DD HH24:MI:SSXFF\'', '', 'srv "s_2_3"');
let $CHILD2_3_DROP_TABLES=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "ta_r4"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_3"');
let $CHILD2_3_CREATE_TABLES=
  SELECT spider_direct_sql('CREATE TABLE "ta_r4" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\'),
    CONSTRAINT "pk_s_2_3_ta_r4" PRIMARY KEY("a")
  )', '', 'srv "s_2_3"');
let $CHILD2_3_SELECT_TABLES=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\') FROM "ta_r4" ORDER BY "a"', '', 'srv "s_2_3"');
