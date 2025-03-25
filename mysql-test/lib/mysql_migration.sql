-- 1. populate_mysql_datadir.sh starts official MySQL server in docker
-- 2. mysql_migration.sql defines the databases/tables populated into the datadir
--    Theoretically these data should reflect all the incompatibilities to be tested

-- This is needed for mariadb-upgrade to work
CREATE DATABASE test;


-- Create tables partitioned with "native" InnoDb partition handler
-- Later will test reading them with MariaDB
CREATE DATABASE idb_partition_db1;
USE idb_partition_db1;

CREATE TABLE idb_partition_t1 (
    id INT,
    year INT
);

ALTER TABLE idb_partition_t1
    PARTITION BY HASH(id)
    PARTITIONS 4;

INSERT INTO idb_partition_t1(id, year)
VALUES
    (0,1997),
    (1,1998),
    (2,1999),
    (3,2000);


-- Create a table compressed with MySQL punch-hole compression
-- Later will test reading them with MariaDB
CREATE DATABASE idb_page_comp_db1;
USE idb_page_comp_db1;

CREATE TABLE idb_page_comp_t1
    (id INT,
    year INT)
    COMPRESSION="zlib";

-- Insert large data to make sure the compression will take place
DELIMITER $$
CREATE PROCEDURE populatePageCompTable()
BEGIN
    DECLARE id INT DEFAULT 0;
    WHILE id < 3000 DO
        INSERT idb_page_comp_t1 (id, year) 
            VALUES (id, 2000); 
        SET id = id + 1;
    END While;
END $$

call populatePageCompTable;


-- Create a table encrypted with MySQL keyring_file plug-in
-- Later will test reading them with MariaDB
CREATE DATABASE idb_encryption_db1;
USE idb_encryption_db1;

CREATE TABLE idb_encryption_t1
    (id INT,
    year INT)
    ENCRYPTION = 'Y';

INSERT INTO idb_encryption_t1(id, year)
VALUES
    (0,2001),
    (1,2002),
    (2,2003),
    (3,2004);


-- Create a table inside a manually created tablespace with MySQL CREATE TABLESPACE command
-- Later will test reading them with MariaDB
CREATE DATABASE idb_tablespace_db1;
USE idb_tablespace_db1;

CREATE TABLESPACE idb_tablespace_ts1 ADD DATAFILE 'idb_tablespace_ts1.ibd' Engine=InnoDB;

CREATE TABLE idb_tablespace_t1
    (id INT,
    year INT)
    TABLESPACE = idb_tablespace_ts1;

INSERT INTO idb_tablespace_t1(id, year)
VALUES
    (0,2001),
    (1,2002),
    (2,2003),
    (3,2004);


-- Create a regular table in MySQL
-- Later will test reading them with MariaDB
CREATE DATABASE normal_db1;
USE normal_db1;

CREATE TABLE normal_t1
    (id INT,
    year INT);

INSERT INTO normal_t1(id, year)
VALUES
    (0,2001),
    (1,2002),
    (2,2003),
    (3,2004);


-- Create temp table to fill in the first 32 rollback segments (MDEV-12289)
-- This does not cover scenario of an old 5.5/6 with unclean shutdown,
-- upgrading to 5.7 and then to MariaDB, which will produce garbage data in the
-- first 32 undo segments. But it at least tests 5.7 to MariaDB upgrade with
-- some data in the first 32 segments.
CREATE TEMPORARY TABLE normal_t1_tmp SELECT * FROM normal_t1;


COMMIT;
