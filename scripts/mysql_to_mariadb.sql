-- Script that changes MySQL 5.7 privilege tables to MariaDB 10.x
-- This should be run first with
-- mysql --force mysql < mysql_to_mariadb.sql
-- It's ok to ignore any errors, as these usually means that the tables are
-- already fixed.

-- After this script s run, one should run at least:
-- mysql_upgrade --upgrade-system-tables
-- to get the other tables in the mysql database fixed.

-- Drop not existing columnms
alter table mysql.user drop column `password_last_changed`, drop column `password_lifetime`, drop column `account_locked`;

-- Change existing columns
alter table mysql.user change column `authentication_string` `auth_string` text COLLATE utf8_bin NOT NULL;

-- Add new columns
alter table mysql.user add column  `Password` char(41) CHARACTER SET latin1 COLLATE latin1_bin NOT NULL DEFAULT '' after `user`, add column  `is_role` enum('N','Y') CHARACTER SET utf8 NOT NULL DEFAULT 'N' after `auth_string`;
alter table mysql.user add column `default_role` char(80) COLLATE utf8_bin NOT NULL DEFAULT '', add column `max_statement_time` decimal(12,6) NOT NULL DEFAULT '0.000000';

-- Fix passwords
update mysql.user set `password`=`auth_string`, plugin='' where plugin="mysql_native_password";
