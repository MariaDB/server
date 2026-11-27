--disable_view_protocol
--disable_cursor_protocol
SET @old_log_output= @@global.log_output;
SET @old_general_log= @@global.general_log;
SET @old_slow_query_log= @@global.slow_query_log;
SET @old_long_query_time= @@session.long_query_time;
SET GLOBAL log_output= 'TABLE', general_log= ON, slow_query_log= ON;
SET SESSION long_query_time= 0;
TRUNCATE mysql.general_log;
TRUNCATE mysql.slow_log;

CREATE USER u1@localhost IDENTIFIED BY 'pw01';
CREATE USER u2@localhost IDENTIFIED VIA mysql_native_password
  USING PASSWORD('pw02');
ALTER USER u1@localhost IDENTIFIED BY 'pw03', u2@localhost IDENTIFIED BY "pw04";
GRANT SELECT ON test.* TO u1@localhost IDENTIFIED BY 'pw05';
create user u3@localhost identified/*c*/by 'pw06';
SET PASSWORD FOR u1@localhost = PASSWORD('pw07');
--disable_warnings
SET PASSWORD FOR u1@localhost = OLD_PASSWORD('pw08');
--enable_warnings
ALTER USER u1@localhost IDENTIFIED BY 'it''s pw09\'';
CHANGE MASTER TO MASTER_HOST='127.0.0.1', MASTER_PASSWORD='pw10';
RESET SLAVE ALL;
CREATE SERVER s1 FOREIGN DATA WRAPPER mysql OPTIONS (HOST 'h', PASSWORD 'pw11');
ALTER SERVER s1 OPTIONS (PASSWORD 'pw12');
DROP SERVER s1;
CREATE TABLE t1 (a INT) PASSWORD='pw13';
DROP TABLE t1;
PREPARE s FROM 'CREATE USER u4@localhost IDENTIFIED BY ''pw16''';
EXECUTE s;
DEALLOCATE PREPARE s;
EXECUTE IMMEDIATE 'ALTER USER u4@localhost IDENTIFIED BY ''pw17''';
PREPARE s FROM 'SELECT ''not masked''';
EXECUTE s;
DEALLOCATE PREPARE s;
CREATE PROCEDURE p1() SET PASSWORD FOR u4@localhost = PASSWORD('pw18');
SET log_disabled_statements= '', log_slow_disabled_statements= '';
CALL p1();
SET log_disabled_statements= DEFAULT, log_slow_disabled_statements= DEFAULT;
DROP PROCEDURE p1;
SET STATEMENT max_statement_time=0 FOR
  CREATE USER u5@localhost IDENTIFIED BY 'pw19';
--disable_ps_protocol
--error ER_PARSE_ERROR
CREATE USER u6@localhost IDENTIFIED BY 'pw20' garbage;
--error ER_PARSE_ERROR
CREATE USER u7@localhost garbage 'not masked';
--delimiter //
CREATE USER u8@localhost IDENTIFIED BY 'pw21'; SELECT 'not masked either'//
--delimiter ;
--error ER_PARSE_ERROR
PREPARE s9 FROM 'CREATE USER u9@localhost IDENTIFIED BY ''pw22'' garbage';
--error ER_PARSE_ERROR
EXECUTE IMMEDIATE 'CREATE USER u10@localhost IDENTIFIED BY ''pw23''' USING;
--enable_ps_protocol

SET GLOBAL general_log= OFF, slow_query_log= OFF;
SELECT DISTINCT argument FROM mysql.general_log
  WHERE argument REGEXP 'pw|secret|masked';
SELECT DISTINCT sql_text FROM mysql.slow_log
  WHERE sql_text REGEXP 'pw|secret|masked';

DROP USER u1@localhost, u2@localhost, u3@localhost, u4@localhost, u5@localhost,
  u8@localhost;
TRUNCATE mysql.general_log;
TRUNCATE mysql.slow_log;
SET GLOBAL log_output= @old_log_output, general_log= @old_general_log;
SET GLOBAL slow_query_log= @old_slow_query_log;
SET SESSION long_query_time= @old_long_query_time;
--enable_cursor_protocol
--enable_view_protocol
