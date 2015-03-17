-- Copyright (C) 2003, 2013 Oracle and/or its affiliates.
-- Copyright (C) 2010, 2015 MariaDB Corporation.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# WARNING: Do not use this script to upgrade system tables older that v5.1.

# This part converts old privilege tables to privilege tables suitable
# for current version of MySQL/MariaDB server.

# You can safely ignore all 'Duplicate column' and 'Unknown column' errors
# because these just mean that your tables are already up to date.
# This script is safe to run even if your tables are already up to date!

# Warning message(s) produced for a statement can be printed by explicitly
# adding a 'SHOW WARNINGS' after the statement.

set sql_mode='';
set storage_engine=MyISAM;

#
# Modify log tables.
#
SET @old_log_state = @@global.general_log;
SET GLOBAL general_log = 'OFF';
ALTER TABLE general_log
  MODIFY event_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  MODIFY user_host MEDIUMTEXT NOT NULL,
  MODIFY thread_id INTEGER NOT NULL,
  MODIFY server_id INTEGER UNSIGNED NOT NULL,
  MODIFY command_type VARCHAR(64) NOT NULL,
  MODIFY argument MEDIUMTEXT NOT NULL,
  MODIFY thread_id BIGINT(21) UNSIGNED NOT NULL;
SET GLOBAL general_log = @old_log_state;

SET @old_log_state = @@global.slow_query_log;
SET GLOBAL slow_query_log = 'OFF';
ALTER TABLE slow_log
  ADD COLUMN thread_id BIGINT(21) UNSIGNED NOT NULL AFTER sql_text;
ALTER TABLE slow_log
  ADD COLUMN rows_affected INTEGER NOT NULL AFTER thread_id;
ALTER TABLE slow_log
  MODIFY start_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  MODIFY user_host MEDIUMTEXT NOT NULL,
  MODIFY query_time TIME(6) NOT NULL,
  MODIFY lock_time TIME(6) NOT NULL,
  MODIFY rows_sent INTEGER NOT NULL,
  MODIFY rows_examined INTEGER NOT NULL,
  MODIFY db VARCHAR(512) NOT NULL,
  MODIFY last_insert_id INTEGER NOT NULL,
  MODIFY insert_id INTEGER NOT NULL,
  MODIFY server_id INTEGER UNSIGNED NOT NULL,
  MODIFY sql_text MEDIUMTEXT NOT NULL,
  MODIFY thread_id BIGINT(21) UNSIGNED NOT NULL;
SET GLOBAL slow_query_log = @old_log_state;

#
# Modify plugin table.
#
ALTER TABLE plugin
  MODIFY name varchar(64) COLLATE utf8_general_ci NOT NULL DEFAULT '',
  MODIFY dl varchar(128) COLLATE utf8_general_ci NOT NULL DEFAULT '',
  CONVERT TO CHARACTER SET utf8 COLLATE utf8_general_ci;

#
# Add max_user_connections resource limit.
# This is signed in MariaDB so that if one sets it to -1 then the user
# cannot connect anymore.
#
ALTER TABLE user ADD max_user_connections int(11) DEFAULT '0' NOT NULL AFTER max_connections;
ALTER TABLE user MODIFY max_user_connections int(11) DEFAULT '0' NOT NULL AFTER max_connections;

#
# procs_priv
#
ALTER TABLE procs_priv
  MODIFY Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP AFTER Proc_priv;

#
# proc
#

# Correct the name fields to not binary, and expand sql_data_access
ALTER TABLE proc MODIFY sql_mode
                        set('REAL_AS_FLOAT',
                            'PIPES_AS_CONCAT',
                            'ANSI_QUOTES',
                            'IGNORE_SPACE',
                            'IGNORE_BAD_TABLE_OPTIONS',
                            'ONLY_FULL_GROUP_BY',
                            'NO_UNSIGNED_SUBTRACTION',
                            'NO_DIR_IN_CREATE',
                            'POSTGRESQL',
                            'ORACLE',
                            'MSSQL',
                            'DB2',
                            'MAXDB',
                            'NO_KEY_OPTIONS',
                            'NO_TABLE_OPTIONS',
                            'NO_FIELD_OPTIONS',
                            'MYSQL323',
                            'MYSQL40',
                            'ANSI',
                            'NO_AUTO_VALUE_ON_ZERO',
                            'NO_BACKSLASH_ESCAPES',
                            'STRICT_TRANS_TABLES',
                            'STRICT_ALL_TABLES',
                            'NO_ZERO_IN_DATE',
                            'NO_ZERO_DATE',
                            'INVALID_DATES',
                            'ERROR_FOR_DIVISION_BY_ZERO',
                            'TRADITIONAL',
                            'NO_AUTO_CREATE_USER',
                            'HIGH_NOT_PRECEDENCE',
                            'NO_ENGINE_SUBSTITUTION',
                            'PAD_CHAR_TO_FULL_LENGTH'
                            ) DEFAULT '' NOT NULL,
                 DEFAULT CHARACTER SET utf8;

# Reset some fields after the conversion
ALTER TABLE proc  MODIFY definer
                         char(141) collate utf8_bin DEFAULT '' NOT NULL;

SELECT CASE WHEN COUNT(*) > 0 THEN 
CONCAT ("WARNING: NULL values of the 'character_set_client' column ('mysql.proc' table) have been updated with a default value (", @@character_set_client, "). Please verify if necessary.")
ELSE NULL 
END 
AS value FROM proc WHERE character_set_client IS NULL;

UPDATE proc SET character_set_client = @@character_set_client 
                     WHERE character_set_client IS NULL;

ALTER TABLE proc ADD collation_connection
                     char(32) collate utf8_bin DEFAULT NULL
                     AFTER character_set_client;
ALTER TABLE proc MODIFY collation_connection
                        char(32) collate utf8_bin DEFAULT NULL;

SELECT CASE WHEN COUNT(*) > 0 THEN 
CONCAT ("WARNING: NULL values of the 'collation_connection' column ('mysql.proc' table) have been updated with a default value (", @@collation_connection, "). Please verify if necessary.")
ELSE NULL 
END 
AS value FROM proc WHERE collation_connection IS NULL;

UPDATE proc SET collation_connection = @@collation_connection
                     WHERE collation_connection IS NULL;

ALTER TABLE proc ADD db_collation
                     char(32) collate utf8_bin DEFAULT NULL
                     AFTER collation_connection;
ALTER TABLE proc MODIFY db_collation
                        char(32) collate utf8_bin DEFAULT NULL;

SELECT CASE WHEN COUNT(*) > 0 THEN 
CONCAT ("WARNING: NULL values of the 'db_collation' column ('mysql.proc' table) have been updated with default values. Please verify if necessary.")
ELSE NULL
END
AS value FROM proc WHERE db_collation IS NULL;

UPDATE proc AS p SET db_collation  = 
                     ( SELECT DEFAULT_COLLATION_NAME 
                       FROM INFORMATION_SCHEMA.SCHEMATA 
                       WHERE SCHEMA_NAME = p.db)
                     WHERE db_collation IS NULL;

ALTER TABLE proc ADD body_utf8 longblob DEFAULT NULL
                     AFTER db_collation;
ALTER TABLE proc MODIFY body_utf8 longblob DEFAULT NULL;

# Change comment from char(64) to text
ALTER TABLE proc MODIFY comment
                        text collate utf8_bin NOT NULL;

#
# EVENT table
#
# Update list of sql_mode values.
ALTER TABLE event MODIFY sql_mode
                        set('REAL_AS_FLOAT',
                            'PIPES_AS_CONCAT',
                            'ANSI_QUOTES',
                            'IGNORE_SPACE',
                            'IGNORE_BAD_TABLE_OPTIONS',
                            'ONLY_FULL_GROUP_BY',
                            'NO_UNSIGNED_SUBTRACTION',
                            'NO_DIR_IN_CREATE',
                            'POSTGRESQL',
                            'ORACLE',
                            'MSSQL',
                            'DB2',
                            'MAXDB',
                            'NO_KEY_OPTIONS',
                            'NO_TABLE_OPTIONS',
                            'NO_FIELD_OPTIONS',
                            'MYSQL323',
                            'MYSQL40',
                            'ANSI',
                            'NO_AUTO_VALUE_ON_ZERO',
                            'NO_BACKSLASH_ESCAPES',
                            'STRICT_TRANS_TABLES',
                            'STRICT_ALL_TABLES',
                            'NO_ZERO_IN_DATE',
                            'NO_ZERO_DATE',
                            'INVALID_DATES',
                            'ERROR_FOR_DIVISION_BY_ZERO',
                            'TRADITIONAL',
                            'NO_AUTO_CREATE_USER',
                            'HIGH_NOT_PRECEDENCE',
                            'NO_ENGINE_SUBSTITUTION',
                            'PAD_CHAR_TO_FULL_LENGTH'
                            ) DEFAULT '' NOT NULL AFTER on_completion;

#
# user.Create_tablespace_priv
#

SET @hadCreateTablespacePriv := 0;
SELECT @hadCreateTablespacePriv :=1 FROM user WHERE Create_tablespace_priv LIKE '%';

ALTER TABLE user ADD Create_tablespace_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Trigger_priv;
ALTER TABLE user MODIFY Create_tablespace_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Trigger_priv;

UPDATE user SET Create_tablespace_priv = Super_priv WHERE @hadCreateTablespacePriv = 0;

ALTER TABLE user ADD plugin char(64) DEFAULT '',  ADD authentication_string TEXT;
ALTER TABLE user ADD password_expired ENUM('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;
ALTER TABLE user ADD is_role enum('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;
ALTER TABLE user ADD default_role char(80) binary DEFAULT '' NOT NULL;
ALTER TABLE user ADD max_statement_time decimal(12,6) DEFAULT 0 NOT NULL;
ALTER TABLE user MODIFY plugin char(64) CHARACTER SET latin1 DEFAULT '' NOT NULL, MODIFY authentication_string TEXT NOT NULL;
-- Somewhere above, we ran ALTER TABLE user .... CONVERT TO CHARACTER SET utf8 COLLATE utf8_bin.
--  we want password_expired column to have collation utf8_general_ci.
ALTER TABLE user MODIFY password_expired ENUM('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;
ALTER TABLE user MODIFY is_role enum('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;

-- Need to pre-fill mysql.proxies_priv with access for root even when upgrading from
-- older versions

CREATE TEMPORARY TABLE tmp_proxies_priv LIKE proxies_priv;
INSERT INTO tmp_proxies_priv VALUES ('localhost', 'root', '', '', TRUE, '', now());
INSERT INTO proxies_priv SELECT * FROM tmp_proxies_priv WHERE @had_proxies_priv_table=0;
DROP TABLE tmp_proxies_priv;

-- Checking for any duplicate hostname and username combination are exists.
-- If exits we will throw error.
DROP PROCEDURE IF EXISTS mysql.count_duplicate_host_names;
DELIMITER //
CREATE PROCEDURE mysql.count_duplicate_host_names()
BEGIN
  SET @duplicate_hosts=(SELECT count(*) FROM mysql.user GROUP BY user, lower(host) HAVING count(*) > 1 LIMIT 1);
  IF @duplicate_hosts > 1 THEN
    SIGNAL SQLSTATE '45000'  SET MESSAGE_TEXT = 'Multiple accounts exist for @user_name, @host_name that differ only in Host lettercase; remove all except one of them';
  END IF;
END //
DELIMITER ;
CALL mysql.count_duplicate_host_names();
-- Get warnings (if any)
SHOW WARNINGS;
DROP PROCEDURE mysql.count_duplicate_host_names;

# Convering the host name to lower case for existing users
UPDATE user SET host=LOWER( host ) WHERE LOWER( host ) <> host;

# update timestamp fields in the innodb stat tables
set @str="alter table mysql.innodb_index_stats modify last_update timestamp not null default current_timestamp on update current_timestamp";
set @str=if(@have_innodb <> 0, @str, "set @dummy = 0");
prepare stmt from @str;
execute stmt;

set @str="alter table mysql.innodb_table_stats modify last_update timestamp not null default current_timestamp on update current_timestamp";
set @str=if(@have_innodb <> 0, @str, "set @dummy = 0");
prepare stmt from @str;
execute stmt;

set @str=replace(@str, "innodb_index_stats", "innodb_table_stats");
prepare stmt from @str;
execute stmt;

SET @innodb_index_stats_fk= (select count(*) from information_schema.referential_constraints where constraint_schema='mysql' and table_name = 'innodb_index_stats' and referenced_table_name = 'innodb_table_stats' and constraint_name = 'innodb_index_stats_ibfk_1');
SET @str=IF(@innodb_index_stats_fk > 0 and @have_innodb > 0, "ALTER TABLE mysql.innodb_index_stats DROP FOREIGN KEY `innodb_index_stats_ibfk_1`", "SET @dummy = 0");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt; 

# MDEV-4332 longer user names
alter table user         modify User         char(80)  binary not null default '';
alter table db           modify User         char(80)  binary not null default '';
alter table tables_priv  modify User         char(80)  binary not null default '';
alter table columns_priv modify User         char(80)  binary not null default '';
alter table procs_priv   modify User         char(80)  binary not null default '';
alter table proc         modify definer      char(141) collate utf8_bin not null default '';
alter table event        modify definer      char(141) collate utf8_bin not null default '';
alter table proxies_priv modify User         char(80)  COLLATE utf8_bin not null default '';
alter table proxies_priv modify Proxied_user char(80)  COLLATE utf8_bin not null default '';
alter table proxies_priv modify Grantor      char(141) COLLATE utf8_bin not null default '';
alter table servers      modify Username     char(80)                   not null default '';
alter table procs_priv   modify Grantor      char(141) COLLATE utf8_bin not null default '';
alter table tables_priv  modify Grantor      char(141) COLLATE utf8_bin not null default '';

# Activate the new, possibly modified privilege tables.
# This should not be needed, but gives us some extra testing that the above
# changes was correct

flush privileges;

--
-- Upgrade help tables
--

ALTER TABLE help_category MODIFY url TEXT NOT NULL;
ALTER TABLE help_topic MODIFY url TEXT NOT NULL;

