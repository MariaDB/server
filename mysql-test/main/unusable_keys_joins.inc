--enable_prepare_warnings
CREATE TABLE t2 (not_indexed_col INT);
INSERT INTO t2 VALUES (20230101),(20230102);
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col INT UNSIGNED);
INSERT INTO t2 VALUES (20230101),(20230102);
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col BIGINT);
INSERT INTO t2 VALUES (20230101102030),(20230101102031);
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col BIGINT UNSIGNED);
INSERT INTO t2 VALUES (20230101102030),(20230101102031);
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col DECIMAL(30,6));
INSERT INTO t2 VALUES (20230101102030),(20230101102031);
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col FLOAT);
INSERT INTO t2 VALUES (20230101102030),(20230101102031);
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col DOUBLE);
INSERT INTO t2 VALUES (20230101102030),(20230101102031);
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col DATE);
INSERT INTO t2 VALUES ('2023-01-01'),('2023-01-02');
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col DATETIME);
INSERT INTO t2 VALUES ('2023-01-01 00:00:00'),('2023-01-01 00:00:01');
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col TIMESTAMP);
INSERT INTO t2 VALUES ('2023-01-01 00:00:00'),('2023-01-01 00:00:01');
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col VARBINARY(32));
INSERT INTO t2 VALUES (0x30),(0x31);
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col VARCHAR(32)) CHARACTER SET latin1;
INSERT INTO t2 VALUES ('2001-01-01'),('2001-01-02');
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;

CREATE TABLE t2 (not_indexed_col VARCHAR(32) CHARACTER SET utf8mb3);
INSERT INTO t2 VALUES ('2001-01-01'),('2001-01-02');
SELECT * FROM t1, t2 WHERE indexed_col=not_indexed_col;
DROP TABLE t2;
--disable_prepare_warnings
