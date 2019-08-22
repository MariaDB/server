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
CREATE DATABASE auto_test_remote2;
CREATE DATABASE auto_test_remote3;
CREATE DATABASE auto_test_remote4;
--enable_warnings

--echo
--echo create table and insert

--connection child2_1
TRUNCATE TABLE mysql.general_log;

--connection master_1
--disable_query_log
echo CREATE TABLE tbl_a (
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
) CHILD2_1_ENGINE CHILD2_1_CHARSET;
eval CREATE TABLE tbl_a (
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--enable_query_log
SHOW CREATE TABLE tbl_a;
INSERT INTO tbl_a (pkey) VALUES (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);

--echo
--echo select test
SELECT * FROM tbl_a ORDER BY pkey;

--connection child2_1
--replace_regex /-[0-9a-f]{12}-[0-9a-f]+-/-xxxxxxxxxxxx-xxxxx-/
eval $CHILD2_1_SELECT_ARGUMENT1;
eval $CHILD2_1_SELECT_TABLES;

--echo
--echo deinit
--disable_warnings

--connection master_1
DROP DATABASE IF EXISTS auto_test_local;

--connection child2_1
DROP DATABASE IF EXISTS auto_test_remote;
DROP DATABASE IF EXISTS auto_test_remote2;
DROP DATABASE IF EXISTS auto_test_remote3;
DROP DATABASE IF EXISTS auto_test_remote4;
SET GLOBAL log_output = @old_log_output;

--enable_warnings
