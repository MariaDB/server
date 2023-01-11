
SET GLOBAL innodb_encrypt_tables=ON;
SET SESSION innodb_default_encryption_key_id=100;

CREATE DATABASE db;
USE db;

CREATE TABLE t1 (
   id int PRIMARY KEY,
   str varchar(50)
);

INSERT INTO t1(id, str) VALUES
    (1, 'abc'),
    (2, 'def'),
    (3, 'ghi');

FLUSH TABLES t1 FOR EXPORT;
