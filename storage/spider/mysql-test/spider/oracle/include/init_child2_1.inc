--connection master_1
SELECT spider_direct_sql('ALTER SESSION SET NLS_DATE_FORMAT=\'YYYY-MM-DD HH24:MI:SS\'', '', 'srv "s_2_1"');
SELECT spider_direct_sql('ALTER SESSION SET NLS_TIME_FORMAT=\'HH24:MI:SSXFF\'', '', 'srv "s_2_1"');
SELECT spider_direct_sql('ALTER SESSION SET NLS_TIMESTAMP_FORMAT=\'YYYY-MM-DD HH24:MI:SSXFF\'', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_TABLES=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "ta_r"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_TABLES=
  SELECT spider_direct_sql('CREATE TABLE "ta_r" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\'),
    CONSTRAINT "pk_s_2_1_ta_r" PRIMARY KEY("a")
  )', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE INDEX "idx1" ON "ta_r"("b")', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_TABLES=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\') FROM "ta_r" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_TABLES2=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "ta_r2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_TABLES2=
  SELECT spider_direct_sql('CREATE TABLE "ta_r2" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\'),
    CONSTRAINT "pk_s_2_1_ta_r2" PRIMARY KEY("a")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_TABLES2=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\') FROM "ta_r2" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_TABLES3=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "ta_r_no_idx"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_TABLES3=
  SELECT spider_direct_sql('CREATE TABLE "ta_r_no_idx" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\')
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_TABLES3=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\') FROM "ta_r_no_idx" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_TABLES4=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "ta_r_auto_inc"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_TABLES4=
  SELECT spider_direct_sql('CREATE TABLE "ta_r_auto_inc" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\'),
    CONSTRAINT "pk_s_2_1_ta_r_auto_inc" PRIMARY KEY("a")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_TABLES4=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\') FROM "ta_r_auto_inc"
  ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_TABLES5=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "s_2_1_ta_r_int"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_TABLES5=
  SELECT spider_direct_sql('CREATE TABLE "s_2_1_ta_r_int" (
    "a" INT DEFAULT 3,
    "b" INT DEFAULT 10,
    "c" INT DEFAULT 11,
    CONSTRAINT "pk_s_2_1_ta_r_int" PRIMARY KEY("a")
  )', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE INDEX "idx1_s_2_1_ta_r_int" ON "s_2_1_ta_r_int"("b")', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE INDEX "idx2_s_2_1_ta_r_int" ON "s_2_1_ta_r_int"("c")', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_TABLES5=
  SELECT spider_direct_sql('SELECT "a", "b", "c" FROM "s_2_1_ta_r_int" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_TABLES6=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "ta_r_3"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_TABLES6=
  SELECT spider_direct_sql('CREATE TABLE "ta_r_3" (
    "a" INT DEFAULT 10,
    "b" CHAR(1) DEFAULT \'c\',
    "c" DATE DEFAULT TO_DATE(\'1999-10-10 10:10:10\', \'YYYY-MM-DD HH24:MI:SS\')
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_TABLES6=
  SELECT spider_direct_sql('SELECT "a", "b", TO_CHAR("c", \'YYYY-MM-DD HH24:MI:SS\') FROM "ta_r_3" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_FT_TABLES=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "ft_r"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_FT_TABLES=
  SELECT spider_direct_sql('CREATE TABLE "ft_r" (
    "a" INT DEFAULT 0,
    "b" TEXT,
    "c" TEXT,
    "d" TEXT,
    CONSTRAINT "pk_s_2_1_ft_r" PRIMARY KEY("a"),
    FULLTEXT INDEX "ft_idx1"("b"),
    FULLTEXT INDEX "ft_idx2"("c")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_FT_TABLES=
  SELECT spider_direct_sql('SELECT "a", "b", "c", "d" FROM "ft_r" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_FT_TABLES2=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "ft_r2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_FT_TABLES2=
  SELECT spider_direct_sql('CREATE TABLE "ft_r2" (
    "a" INT DEFAULT 0,
    "b" TEXT,
    "c" TEXT,
    "d" TEXT,
    CONSTRAINT "pk_s_2_1_ft_r2" PRIMARY KEY("a"),
    FULLTEXT INDEX "ft_idx1"("b"),
    FULLTEXT INDEX "ft_idx2"("c")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_FT_TABLES2=
  SELECT spider_direct_sql('SELECT "a", "b", "c", "d" FROM "ft_r2" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_GM_TABLES=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "gm_r"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_GM_TABLES=
  SELECT spider_direct_sql('CREATE TABLE "gm_r" (
    "a" INT DEFAULT 0,
    "b" GEOMETRY NOT NULL,
    "c" GEOMETRY NOT NULL,
    CONSTRAINT "pk_s_2_1_gm_r" PRIMARY KEY("a"),
    SPATIAL INDEX "sp_idx1"("b"),
    SPATIAL INDEX "sp_idx2"("c")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_GM_TABLES=
  SELECT spider_direct_sql('SELECT "a", "b", "c" FROM "gm_r" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_GM_TABLES2=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "gm_r2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_GM_TABLES2=
  SELECT spider_direct_sql('CREATE TABLE "gm_r2" (
    "a" INT DEFAULT 0,
    "b" GEOMETRY NOT NULL,
    "c" GEOMETRY NOT NULL,
    CONSTRAINT "pk_s_2_1_gm_r2" PRIMARY KEY("a"),
    SPATIAL INDEX "sp_idx1"("b"),
    SPATIAL INDEX "sp_idx2"("c")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_GM_TABLES2=
  SELECT spider_direct_sql('SELECT "a", "b", "c" FROM "gm_r2" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_LOCK_TABLES1=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_LOCK_TABLES1=
  SELECT spider_direct_sql('CREATE TABLE "t1_1" (
    "id" INT NOT NULL,
    CONSTRAINT "pk_s_2_1_t1_1" PRIMARY KEY ("id")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_LOCK_TABLES2=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "t2_2"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_LOCK_TABLES2=
  SELECT spider_direct_sql('CREATE TABLE "t2_2" (
    "id" INT NOT NULL,
    CONSTRAINT "pk_s_2_1_t2_2" PRIMARY KEY ("id")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_INCREMENT_TABLES1=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_INCREMENT_TABLES1=
  SELECT spider_direct_sql('CREATE TABLE "t1_1" (
    "id" INT NOT NULL,
    CONSTRAINT "pk_s_2_1_t1_1" PRIMARY KEY ("id")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_INCREMENT_TABLES1=
  SELECT spider_direct_sql('SELECT "id" FROM "t1_1" ORDER BY "id"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_TEXT_PK_TABLES1=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "t1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_TEXT_PK_TABLES1=
  SELECT spider_direct_sql('CREATE TABLE "t1" (
    "a" VARCHAR(255),
    CONSTRAINT "pk_s_2_1_t1" PRIMARY KEY ("a")
  )', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_TEXT_PK_TABLES1=
  SELECT spider_direct_sql('SELECT "a" FROM "t1" ORDER BY "a"', '', 'srv "s_2_1"');
let $CHILD2_1_DROP_TEXT_KEY_TABLES1=
  SELECT spider_direct_sql('begin execute immediate \'DROP TABLE "t1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"');
let $CHILD2_1_CREATE_TEXT_KEY_TABLES1=
  SELECT spider_direct_sql('CREATE TABLE "t1" (
    "a" VARCHAR(255),
    "b" VARCHAR(255),
    "c" VARCHAR(255),
    CONSTRAINT "pk_s_2_1_t1" PRIMARY KEY ("c")
  )', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE INDEX "idx1_t1" ON "t1"("a","b")', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE INDEX "idx2_t1" ON "t1"("b")', '', 'srv "s_2_1"');
let $CHILD2_1_SELECT_TEXT_KEY_TABLES1=
  SELECT spider_direct_sql('SELECT "a", "b", "c" FROM "t1" ORDER BY "c"', '', 'srv "s_2_1"');
let $CHILD2_1_AUTO_INCREMENT_INCREMENT1=
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_1" START WITH 1 INCREMENT BY 1', '', 'srv "s_2_1"');
let $CHILD2_1_AUTO_INCREMENT_INCREMENT2=
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_1" START WITH 2 INCREMENT BY 4', '', 'srv "s_2_1"');
let $CHILD2_1_AUTO_INCREMENT_OFFSET1=
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_1" START WITH 1 INCREMENT BY 1', '', 'srv "s_2_1"');
let $CHILD2_1_AUTO_INCREMENT_OFFSET2=
  SELECT spider_direct_sql('begin execute immediate \'DROP SEQUENCE "seq_t1_1"\'$STR_SEMICOLON exception when others then null$STR_SEMICOLON end$STR_SEMICOLON', '', 'srv "s_2_1"')
  $STR_SEMICOLON
  SELECT spider_direct_sql('CREATE SEQUENCE "seq_t1_1" START WITH 2 INCREMENT BY 4', '', 'srv "s_2_1"');
