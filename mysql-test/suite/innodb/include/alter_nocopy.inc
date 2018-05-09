CREATE TABLE t1(f1 INT PRIMARY KEY, f2 INT NOT NULL,
		f3 INT AS (f2 * f2) VIRTUAL,
		f4 INT NOT NULL UNIQUE,
		f5 INT NOT NULL,
		INDEX`idx`(f2))ENGINE=INNODB;

CREATE TABLE t2(f1 INT NOT NULL, f2 INT NOT NULL,
		INDEX(f1),
		FOREIGN KEY `fidx` (f1) REFERENCES t1(f1))ENGINE=INNODB;

INSERT INTO t1(f1, f2, f4, f5) VALUES(1, 2, 3, 4);

SELECT @@alter_algorithm;

--enable_info
--error $error_code
--eval ALTER TABLE t1 ADD INDEX idx1(f4)

--error $error_code
--eval ALTER TABLE t1 DROP INDEX idx

--error $error_code
--eval ALTER TABLE t1 ADD UNIQUE INDEX u1(f2)

--error $error_code
--eval ALTER TABLE t1 DROP INDEX f4

SET foreign_key_checks = 0;
--error $error_code
--eval ALTER TABLE t1 ADD FOREIGN KEY(f5) REFERENCES t2(f1)

DROP TABLE t2, t1;
--disable_info
