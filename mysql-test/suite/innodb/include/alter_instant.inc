CREATE TABLE t1(f1 INT NOT NULL,
		f2 INT NOT NULL,
		f3 INT AS (f2 * f2) VIRTUAL)engine=innodb;

INSERT INTO t1(f1, f2) VALUES(1, 1);

--echo #
--echo # ALGORITHM=$algorithm_type
--echo #

--enable_info
--echo # Add column at the end of the table
--error $error_code
--eval ALTER TABLE t1 ADD COLUMN f4 char(100) default "BIG WALL", ALGORITHM=$algorithm_type

--echo # Change virtual column expression
--error $error_code
--eval ALTER TABLE t1 CHANGE f3 f3 INT AS (f2 * f2) VIRTUAL, ALGORITHM=$algorithm_type

--echo # Add virtual column
--error $error_code
--eval ALTER TABLE t1 ADD COLUMN f5 INT AS (f2) VIRTUAL, ALGORITHM=$algorithm_type

--echo # Rename Column
--error $error_code
--eval ALTER TABLE t1 CHANGE f3 vcol INT AS (f2) VIRTUAL, ALGORITHM=$algorithm_type

--echo # Rename table
--error $error_code
--eval ALTER TABLE t1 RENAME t2, algorithm=$algorithm_type

DROP TABLE t2;
--disable_info
