--echo
--echo drop and create databases
--connection master_1
--disable_warnings
CREATE DATABASE auto_test_local;
USE auto_test_local;

--connection slave1_1
CREATE DATABASE auto_test_local;
USE auto_test_local;

--connection child2_1
SET @old_log_output = @@global.log_output;
SET GLOBAL log_output = 'TABLE,FILE';
CREATE DATABASE auto_test_remote;
USE auto_test_remote;
--enable_warnings

--echo
--echo create table and insert

--connection child2_1
--disable_query_log
--disable_ps_protocol
echo CHILD2_1_CREATE_TABLES;
eval $CHILD2_1_CREATE_TABLES;
--enable_ps_protocol
--enable_query_log
TRUNCATE TABLE mysql.general_log;

--connection master_1
save_master_pos;

--connection slave1_1
sync_with_master;

--connection master_1
SET SESSION sql_log_bin= 0;
--disable_query_log
echo CREATE TABLE tbl_a (
    pkey int NOT NULL,
    f float DEFAULT NULL,
    PRIMARY KEY (pkey)
) MASTER_1_ENGINE2 MASTER_1_CHARSET MASTER_1_COMMENT_2_1;
eval CREATE TABLE tbl_a (
    pkey int NOT NULL,
    f float DEFAULT NULL,
    PRIMARY KEY (pkey)
) $MASTER_1_ENGINE2 $MASTER_1_CHARSET $MASTER_1_COMMENT_2_1;
--enable_query_log
SET SESSION sql_log_bin= 1;

--connection slave1_1
--disable_query_log
echo CREATE TABLE tbl_a (
    pkey int NOT NULL,
    f float DEFAULT NULL,
    PRIMARY KEY (pkey)
) MASTER_1_ENGINE MASTER_1_CHARSET MASTER_1_COMMENT_2_1;
eval CREATE TABLE tbl_a (
    pkey int NOT NULL,
    f float DEFAULT NULL,
    PRIMARY KEY (pkey)
) $MASTER_1_ENGINE $MASTER_1_CHARSET $MASTER_1_COMMENT_2_1;
--enable_query_log

--connection master_1
INSERT INTO tbl_a (pkey, f) VALUES (0,NULL);
INSERT INTO tbl_a (pkey, f) VALUES (1,0.671437);
DELETE FROM tbl_a WHERE pkey = 0;
DELETE FROM tbl_a WHERE pkey = 1;
save_master_pos;

--connection slave1_1
sync_with_master;

--connection master_1
SET SESSION sql_log_bin= 0;

--connection child2_1
eval $CHILD2_1_SELECT_ARGUMENT1;
eval $CHILD2_1_SELECT_TABLES;

--connection slave1_1
SELECT pkey, f FROM tbl_a ORDER BY pkey;

--echo
--echo deinit
--disable_warnings
--connection master_1
DROP DATABASE IF EXISTS auto_test_local;

--connection slave1_1
DROP DATABASE IF EXISTS auto_test_local;

--connection child2_1
DROP DATABASE IF EXISTS auto_test_remote;
SET GLOBAL log_output = @old_log_output;

--enable_warnings
