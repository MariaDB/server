--
-- The SQL script to create PostgreSQL data for odbc_postgresql.test
--
-- Run this script as a admin user:
-- sudo -u postgres psql < storage/connect/mysql-test/connect/t/odbc_postgresql.sql

SET NAMES 'UTF8';

DROP DATABASE IF EXISTS mtr;
DROP USER IF EXISTS mtr;

CREATE USER mtr WITH PASSWORD 'mtr';
CREATE DATABASE mtr OWNER=mtr ENCODING='UTF8';
GRANT ALL PRIVILEGES ON DATABASE mtr TO mtr;
\c mtr
SET role mtr;
CREATE TABLE t1 (a INT NOT NULL);
INSERT INTO t1 VALUES (10),(20),(30);
CREATE VIEW v1 AS SELECT * FROM t1;
CREATE TABLE t2 (a INT NOT NULL);
INSERT INTO t2 VALUES (40),(50),(60);
CREATE SCHEMA schema1 AUTHORIZATION mtr;
CREATE TABLE schema1.t1 (a CHAR(10) NOT NULL);
INSERT INTO schema1.t1 VALUES ('aaa'),('bbb'),('ccc'),('яяя');
CREATE VIEW schema1.v1 AS SELECT * FROM schema1.t1;
CREATE TABLE schema1.t2 (a CHAR(10) NOT NULL);
INSERT INTO schema1.t2 VALUES ('xxx'),('yyy'),('zzz'),('ÄÖÜ');
CREATE TABLE schema1.t3 (a CHAR(10) NOT NULL, b CHAR(10) NOT NULL);
INSERT INTO schema1.t3 VALUES ('xxx', 'aaa'),('yyy', 'bbb'),('zzz', 'ccc'),('ÄÖÜ', 'яяя');
CREATE TABLE schema1.space_in_column_name ("my space column" CHAR(20) NOT NULL);
INSERT INTO schema1.space_in_column_name VALUES ('My value');
\dt schema1.*
