--echo
--echo this test is for MDEV-18987
--echo
--echo drop and create databases

--connection master_1
--disable_warnings
CREATE DATABASE auto_test_local;
USE auto_test_local;

--connection child2_1
SET @old_log_output = @@global.log_output;
SET GLOBAL log_output = 'TABLE,FILE';
CREATE DATABASE auto_test_remote;
USE auto_test_remote;

--connection child2_2
SET @old_log_output = @@global.log_output;
SET GLOBAL log_output = 'TABLE,FILE';
CREATE DATABASE auto_test_remote2;
USE auto_test_remote2;
--enable_warnings

--echo
--echo create table and insert

--connection child2_1
--disable_query_log
echo CHILD2_1_CREATE_TABLES;
eval $CHILD2_1_CREATE_TABLES;
--enable_query_log
TRUNCATE TABLE mysql.general_log;

--connection child2_2
--disable_query_log
echo CHILD2_2_CREATE_TABLES;
eval $CHILD2_2_CREATE_TABLES;
--enable_query_log
TRUNCATE TABLE mysql.general_log;

--connection master_1
--disable_query_log
echo CREATE TABLE tbl_a (
    pkey int NOT NULL,
    val int NOT NULL,
    PRIMARY KEY (pkey)
) MASTER_1_ENGINE MASTER_1_CHARSET MASTER_1_COMMENT_2_1;
eval CREATE TABLE tbl_a (
    pkey int NOT NULL,
    val int NOT NULL,
    PRIMARY KEY (pkey)
) $MASTER_1_ENGINE $MASTER_1_CHARSET $MASTER_1_COMMENT_2_1;
--enable_query_log
INSERT INTO tbl_a (pkey,val) VALUES (0,0),(1,1),(2,2),(3,3),(4,4),(5,5),(6,6),(7,7),(8,8),(9,9);
--disable_query_log
--echo SELECT pkey,val INTO OUTFILE 'MYSQLTEST_VARDIR/tmp/spider_outfile.tsv' FROM tbl_a ORDER BY pkey;
eval SELECT pkey,val INTO OUTFILE '$MYSQLTEST_VARDIR/tmp/spider_outfile.tsv' FROM tbl_a ORDER BY pkey;
--enable_query_log
eval $COMMAND_BEFORE_LOAD_DATA;

--echo
--echo select test 1

--connection child2_1
TRUNCATE TABLE mysql.general_log;

--connection child2_2
TRUNCATE TABLE mysql.general_log;

--connection master_1
--disable_query_log
--echo LOAD DATA $OPTION_LOCAL INFILE 'MYSQLTEST_VARDIR/tmp/spider_outfile.tsv' $OPTION_WORD INTO TABLE tbl_a;
eval LOAD DATA $OPTION_LOCAL INFILE '$MYSQLTEST_VARDIR/tmp/spider_outfile.tsv' $OPTION_WORD INTO TABLE tbl_a;
--enable_query_log
--remove_file $MYSQLTEST_VARDIR/tmp/spider_outfile.tsv

--connection child2_1
eval $CHILD2_1_SELECT_ARGUMENT1;
eval $CHILD2_1_SELECT_TABLES;

--connection child2_2
eval $CHILD2_2_SELECT_ARGUMENT1;
eval $CHILD2_2_SELECT_TABLES;

--echo
--echo deinit
--disable_warnings

--connection master_1
DROP DATABASE IF EXISTS auto_test_local;

--connection child2_1
DROP DATABASE IF EXISTS auto_test_remote;
SET GLOBAL log_output = @old_log_output;

--connection child2_2
DROP DATABASE IF EXISTS auto_test_remote2;
SET GLOBAL log_output = @old_log_output;

--enable_warnings
