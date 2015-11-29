create database daemon_memcached;

use daemon_memcached;

#-- ------------------------------------------------------------------------
#-- Table `containers`
#--
#-- A container record describes an InnoDB table used for data storage by
#-- daemon_memcached NoSQL plugin.
#--
#-- Column `name` should contain the protocol type, interface address and
#-- port number on which this container should be listen. Supported
#-- protocols are:
#--
#--     TCP - for example: tcp://192.168.1.100:11211
#--     UDP - for example: udp://*:11211 (* - listen on all interfaces)
#--     Unix stream sockets - for example: unix:/patch/to/socket.sock
#--
#-- Columns `db_schema` and `db_table` should contain names of database and
#-- table for this container.
#--
#-- Column `key_columns` should contain the name of column which makes up
#-- the memcached key.
#--
#-- There must be a unique index on the `key column` and this unique index
#-- name must be specified in the `unique_idx_name_on_key` column of the
#-- table.
#--
#-- Column `value_columns` is comma-separated lists of the columns that make
#-- up the memcached value.
#--
#-- Column `sep` should contain a character which will be used to separate
#-- values from individual columns in concatenated memcached value.
#-- ------------------------------------------------------------------------

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
	`sep` VARCHAR(32) NOT NULL DEFAULT "|"
) ENGINE = InnoDB;
