let $algorithm = $alter_algorithm;
let $error_code = 0;

--echo #
--echo # sql_mode: $sql_mode   alter_algorithm: $alter_algorithm
--echo #

if ($algorithm == "NOCOPY") {
 let $error_code = ER_ALTER_OPERATION_NOT_SUPPORTED, ER_ALTER_OPERATION_NOT_SUPPORTED_REASON;
}

if ($algorithm == "INSTANT") {
 let $error_code = ER_ALTER_OPERATION_NOT_SUPPORTED, ER_ALTER_OPERATION_NOT_SUPPORTED_REASON;
}

CREATE TABLE t1(f1 INT NOT NULL,
		f2 INT NOT NULL,
		f3 INT NULL,
		f4 INT as (f2) STORED,
		f5 INT as (f3) STORED,
		PRIMARY KEY(f1))ROW_FORMAT=COMPRESSED, ENGINE=INNODB;
INSERT INTO t1(f1, f2, f3) VALUES(1, 1, 1);

--enable_info
--echo # All the following cases needs table rebuild

--echo # Add and Drop primary key
--error $error_code
--eval ALTER TABLE t1 ADD COLUMN col1 INT NOT NULL,DROP PRIMARY KEY,ADD PRIMARY KEY(col1), ALGORITHM=$alter_algorithm

--echo # Make existing column NULLABLE
--error $error_code
--eval ALTER TABLE t1 MODIFY f2 INT, ALGORITHM=$alter_algorithm

--echo # Drop Stored Column
--error $error_code
--eval ALTER TABLE t1 DROP COLUMN f5, ALGORITHM=$alter_algorithm

--echo # Add base non-generated column as a last column in the compressed table
--error $error_code
--eval ALTER TABLE t1 ADD COLUMN f6 INT NOT NULL, ALGORITHM=$alter_algorithm

--echo # Add base non-generated column but not in the last position
--error $error_code
--eval ALTER TABLE t1 ADD COLUMN f7 INT NOT NULL after f3, ALGORITHM=$alter_algorithm

--echo # Force the table to rebuild
--error $error_code
--eval ALTER TABLE t1 FORCE, ALGORITHM=$alter_algorithm

--echo # Row format changes
--error $error_code
--eval ALTER TABLE t1 ROW_FORMAT=COMPRESSED, ALGORITHM=$alter_algorithm

--echo # Engine table
--error $error_code
--eval ALTER TABLE t1 ENGINE=INNODB, ALGORITHM=$alter_algorithm

ALTER TABLE t1 FORCE, ALGORITHM=DEFAULT;

DROP TABLE t1;
--disable_info

if ($algorithm == "NOCOPY") {
 let $error_code = 0;
}

if ($algorithm == "INSTANT") {
 let $error_code = ER_ALTER_OPERATION_NOT_SUPPORTED_REASON;
}

CREATE TABLE t1(f1 INT PRIMARY KEY, f2 INT NOT NULL,
		f3 INT AS (f2 * f2) VIRTUAL,
		f4 INT NOT NULL UNIQUE,
		f5 INT NOT NULL,
		INDEX idx(f2))ENGINE=INNODB;

CREATE TABLE t2(f1 INT NOT NULL, f2 INT NOT NULL,
		INDEX(f1),
		FOREIGN KEY fidx(f1) REFERENCES t1(f1))ENGINE=INNODB;

INSERT INTO t1(f1, f2, f4, f5) VALUES(1, 2, 3, 4);

--enable_info
--error $error_code
--eval ALTER TABLE t1 ADD INDEX idx1(f4), page_compressed=1, ALGORITHM=$alter_algorithm

--error $error_code
--eval ALTER TABLE t1 DROP INDEX idx, page_compression_level=5, ALGORITHM=$alter_algorithm

--error $error_code
--eval ALTER TABLE t1 ADD UNIQUE INDEX u1(f2), ALGORITHM=$alter_algorithm

--error $error_code
--eval ALTER TABLE t1 DROP INDEX f4, page_compression_level=9, ALGORITHM=$alter_algorithm

SET foreign_key_checks = 0;
--error $error_code
--eval ALTER TABLE t1 ADD FOREIGN KEY(f5) REFERENCES t2(f1), ALGORITHM=$alter_algorithm

DROP TABLE t2, t1;
--disable_info

CREATE TABLE t1(f1 INT NOT NULL,
                f2 INT NOT NULL,
                f3 INT AS (f2 * f2) VIRTUAL,
		INDEX idx (f2))engine=innodb;

CREATE TABLE t2(f1 INT NOT NULL, f2 INT NOT NULL,
		f3 VARCHAR(10),
		INDEX(f1))ENGINE=INNODB;

INSERT INTO t1(f1, f2) VALUES(1, 1);

--enable_info
--echo # Add column at the end of the table
--eval ALTER TABLE t1 ADD COLUMN f4 char(100) default 'BIG WALL', ALGORITHM=$alter_algorithm

--echo # Change virtual column expression
--eval ALTER TABLE t1 CHANGE f3 f3 INT AS (f2 * f2) VIRTUAL, ALGORITHM=$alter_algorithm

--echo # Add virtual column
--eval ALTER TABLE t1 ADD COLUMN f5 INT AS (f2) VIRTUAL, ALGORITHM=$alter_algorithm

--echo # Rename Column
--eval ALTER TABLE t1 CHANGE f3 vcol INT AS (f2) VIRTUAL, ALGORITHM=$alter_algorithm

--echo # Rename table
--eval ALTER TABLE t1 RENAME t3, ALGORITHM=$alter_algorithm

--echo # Drop Virtual Column
--eval ALTER TABLE t3 DROP COLUMN vcol, ALGORITHM=$alter_algorithm

--echo # Column length varies
--eval ALTER TABLE t2 CHANGE f3 f3 VARCHAR(20), ALGORITHM=$alter_algorithm

SET foreign_key_checks = 0;
--eval ALTER TABLE t3 ADD FOREIGN KEY fidx(f2) REFERENCES t2(f1), ALGORITHM=$alter_algorithm

SET foreign_key_checks = 1;
--eval ALTER TABLE t3 DROP FOREIGN KEY fidx, ALGORITHM=$alter_algorithm

DROP TABLE t3, t2;
--disable_info
