let $CHILD2_2_HS_DROP_TABLES=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "hs_r3"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_2"');
let $CHILD2_2_HS_CREATE_TABLES=
  SELECT spider_direct_sql('CREATE TABLE "hs_r3" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\'),
    "d" INT DEFAULT 11,
    CONSTRAINT "pk_s_2_2_hs_r3" PRIMARY KEY("a")
  )', '', 'srv "s_2_2"');
let $CHILD2_2_HS_SELECT_TABLES=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\'), "d" FROM "hs_r3" ORDER BY "a"', '', 'srv "s_2_2"');
