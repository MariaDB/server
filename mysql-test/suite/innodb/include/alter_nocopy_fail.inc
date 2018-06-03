CREATE TABLE t1(f1 INT NOT NULL,
		f2 INT NOT NULL,
		f3 INT NULL,
		f4 INT as (f2) STORED,
		f5 INT as (f3) STORED,
		PRIMARY KEY(f1))ROW_FORMAT=COMPRESSED, ENGINE=INNODB;
INSERT INTO t1(f1, f2, f3) VALUES(1, 1, 1);

SELECT @@alter_algorithm;

--enable_info
--echo # All the following cases needs table rebuild

--echo # Add and Drop primary key
--error $error_code
--eval ALTER TABLE t1 ADD COLUMN col1 INT NOT NULL,DROP PRIMARY KEY,ADD PRIMARY KEY(col1)

--echo # Make existing column NULLABLE
--error $error_code
--eval ALTER TABLE t1 MODIFY f2 INT

--echo # Make existing column NON-NULLABLE
--error $error_code
--eval ALTER TABLE t1 MODIFY f3 INT NOT NULL

--echo # Drop Stored Column
--error $error_code
--eval ALTER TABLE t1 DROP COLUMN f5

--echo # Add base non-generated column as a last column in the compressed table
--error $error_code
--eval ALTER TABLE t1 ADD COLUMN f6 INT NOT NULL

--echo # Add base non-generated column but not in the last position
--error $error_code
--eval ALTER TABLE t1 ADD COLUMN f7 INT NOT NULL after f3

--echo # Force the table to rebuild
--error $error_code
--eval ALTER TABLE t1 FORCE

--echo # Row format changes
--error $error_code
--eval ALTER TABLE t1 ROW_FORMAT=COMPRESSED

--echo # Engine table
--error $error_code
--eval ALTER TABLE t1 ENGINE=INNODB

DROP TABLE t1;
--disable_info
