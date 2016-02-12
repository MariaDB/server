CREATE DATABASE IF NOT EXISTS daemon_memcached;

USE daemon_memcached;

-- ------------------------------------------------------------------------
-- Following are set of "configuration tables" that are used to configure
-- the daemon_memcached NoSQL interface plugin.
-- ------------------------------------------------------------------------

-- ------------------------------------------------------------------------
-- Table `containers`
--
-- A container record describes an InnoDB table used for data storage by
-- daemon_memcached NoSQL plugin.
--
-- Column `name` should contain the protocol type, interface address and
-- port number on which this container should be listen. Supported
-- protocols are:
--
--     TCP - for example: tcp://192.168.1.100:11211
--     UDP - for example: udp://*:11211 (* - listen on all interfaces)
--     Unix stream sockets - for example: unix:/patch/to/socket.sock
--
-- Columns `db_schema` and `db_table` should contain names of database and
-- table for this container.
--
-- Column `key_columns` should contain the name of column which makes up
-- the memcached key.
--
-- There must be a unique index on the `key column` and this unique index
-- name must be specified in the `unique_idx_name_on_key` column of the
-- table.
--
-- Column `value_columns` is comma-separated lists of the columns that make
-- up the memcached value.
--
-- Column `sep` should contain a character which will be used to separate
-- values from individual columns in concatenated memcached value.
-- ------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `containers` (
	`name` varchar(50) not null primary key,
	`db_schema` VARCHAR(250) NOT NULL,
	`db_table` VARCHAR(250) NOT NULL,
	`key_columns` VARCHAR(250) NOT NULL,
	`value_columns` VARCHAR(250),
	`flags` VARCHAR(250) NOT NULL DEFAULT "0",
	`cas_column` VARCHAR(250),
	`expire_time_column` VARCHAR(250),
	`unique_idx_name_on_key` VARCHAR(250) NOT NULL,
	`sep` VARCHAR(32) NOT NULL DEFAULT "|")
ENGINE = InnoDB;

-- ------------------------------------------------------------------------
-- This is an example
--
-- We create a InnoDB table `demo_test` is the `test` database and insert
-- an entry into `containers` table to tell daemon_memcached plugin that
-- we have such InnoDB table as back store:
--
-- c1 -> name of the column which contains memcached key
-- c2 -> names of the columns which contains memcached value
-- c3 -> name of the column which contains memcached flags
-- c4 -> name of the column which contains memcached cas value
-- c5 -> name of the column which contains memcached expire time
-- PRIMARY -> use key named PRIMARY to search for memcached key
-- | -> use this character as separator of values from multiple columns
-- ------------------------------------------------------------------------

INSERT INTO containers VALUES ("tcp://*:11211", "test", "demo_test",
			       "c1", "c2", "c3", "c4", "c5", "PRIMARY", "|");

INSERT INTO containers VALUES ("udp://*:11211", "test", "demo_test",
			       "c1", "c2", "c3", "c4", "c5", "PRIMARY", "|");

CREATE DATABASE IF NOT EXISTS test;
USE test;

-- ------------------------------------------------------------------------
-- Key   (c1) -- must be VARCHAR or CHAR type, memcached supports key up to
--               255 bytes
-- Value (c2) -- must be VARCHAR, CHAR or any size integer type
-- Flag  (c3) -- is a 32 bits integer
-- CAS   (c4) -- is a 64 bits integer, per memcached define
-- Exp   (c5) -- is again a 32 bits integer
-- ------------------------------------------------------------------------

CREATE TABLE demo_test (c1 VARCHAR(32),
			c2 VARCHAR(1024),
			c3 INT, c4 BIGINT UNSIGNED, c5 INT, PRIMARY KEY(c1))
ENGINE = InnoDB;

INSERT INTO demo_test VALUES ("AA", "HELLO WORLD", 8, 0, 0);
