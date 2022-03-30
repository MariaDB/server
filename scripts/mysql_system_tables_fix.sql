-- Copyright (C) 2003, 2013 Oracle and/or its affiliates.
-- Copyright (C) 2010, 2022, MariaDB Corporation
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
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

# This part converts any old privilege tables to privilege tables suitable
# for current version of MySQL

# You can safely ignore all 'Duplicate column' and 'Unknown column' errors
# because these just mean that your tables are already up to date.
# This script is safe to run even if your tables are already up to date!

# Warning message(s) produced for a statement can be printed by explicitly
# adding a 'SHOW WARNINGS' after the statement.

set sql_mode='';
set default_storage_engine=Aria;
set enforce_storage_engine=NULL;
set alter_algorithm=DEFAULT;

set @have_innodb= (select count(engine) from information_schema.engines where engine='INNODB' and support != 'NO');

# MDEV-21873: 10.2 to 10.3 upgrade doesn't remove semi-sync reference from
# mysql.plugin table.
# As per suggested fix, check INFORMATION_SCHEMA.PLUGINS
# and if semisync plugins aren't there, delete them from mysql.plugin.
DELETE FROM mysql.plugin WHERE name="rpl_semi_sync_master" AND NOT EXISTS (SELECT * FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME="rpl_semi_sync_master");
DELETE FROM mysql.plugin WHERE name="rpl_semi_sync_slave" AND NOT EXISTS (SELECT * FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME="rpl_semi_sync_slave");

--
-- Ensure that all tables are of type Aria and transactional
--

ALTER TABLE user ENGINE=Aria transactional=1;
ALTER TABLE db ENGINE=Aria transactional=1;
ALTER TABLE func ENGINE=Aria transactional=1;
ALTER TABLE procs_priv ENGINE=Aria transactional=1;
ALTER TABLE tables_priv ENGINE=Aria transactional=1;
ALTER TABLE columns_priv ENGINE=Aria transactional=1;
ALTER TABLE roles_mapping ENGINE=Aria transactional=1;
ALTER TABLE plugin ENGINE=Aria transactional=1;
ALTER TABLE servers ENGINE=Aria transactional=1;
ALTER TABLE time_zone_name ENGINE=Aria transactional=1;
ALTER TABLE time_zone ENGINE=Aria transactional=1;
ALTER TABLE time_zone_transition ENGINE=Aria transactional=1;
ALTER TABLE time_zone_transition_type ENGINE=Aria transactional=1;
ALTER TABLE time_zone_leap_second ENGINE=Aria transactional=1;
ALTER TABLE proc ENGINE=Aria transactional=1;
ALTER TABLE event ENGINE=Aria transactional=1;
ALTER TABLE proxies_priv ENGINE=Aria transactional=1;

-- The following tables doesn't have to be transactional
ALTER TABLE help_topic ENGINE=Aria transactional=0;
ALTER TABLE help_category ENGINE=Aria transactional=0;
ALTER TABLE help_relation ENGINE=Aria transactional=0;
ALTER TABLE help_keyword ENGINE=Aria transactional=0;
ALTER TABLE table_stats ENGINE=Aria transactional=0;
ALTER TABLE column_stats ENGINE=Aria transactional=0;
ALTER TABLE index_stats ENGINE=Aria transactional=0;

ALTER TABLE user add File_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;

# Detect whether or not we had the Grant_priv column
SET @hadGrantPriv:=0;
SELECT @hadGrantPriv:=1 FROM user WHERE Grant_priv IS NOT NULL;

ALTER TABLE user add Grant_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
                 add References_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
                 add Index_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
                 add Alter_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;
ALTER TABLE db add Grant_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
               add References_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
               add Index_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
               add Alter_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;

# Fix privileges for old tables
UPDATE user SET Grant_priv=File_priv,References_priv=Create_priv,Index_priv=Create_priv,Alter_priv=Create_priv WHERE @hadGrantPriv = 0;
UPDATE db SET References_priv=Create_priv,Index_priv=Create_priv,Alter_priv=Create_priv WHERE @hadGrantPriv = 0;
#
# The second alter changes ssl_type to new 4.0.2 format
# Adding columns needed by GRANT .. REQUIRE (openssl)

ALTER TABLE user
ADD ssl_type enum('','ANY','X509', 'SPECIFIED') DEFAULT '' NOT NULL,
ADD ssl_cipher BLOB NOT NULL,
ADD x509_issuer BLOB NOT NULL,
ADD x509_subject BLOB NOT NULL;
ALTER TABLE user MODIFY ssl_type enum('','ANY','X509', 'SPECIFIED') DEFAULT '' NOT NULL;

#
# tables_priv
#
ALTER TABLE tables_priv
  ADD KEY Grantor (Grantor);

ALTER TABLE tables_priv
  MODIFY Host char(255) NOT NULL default '',
  MODIFY Db char(64) NOT NULL default '',
  MODIFY User char(128) binary NOT NULL default '',
  MODIFY Table_name char(64) NOT NULL default '',
  MODIFY Grantor varchar(384) COLLATE utf8_bin NOT NULL default '',
  ENGINE=Aria,
  CONVERT TO CHARACTER SET utf8 COLLATE utf8_bin;

ALTER TABLE tables_priv
  MODIFY Column_priv set('Select','Insert','Update','References')
    COLLATE utf8_general_ci DEFAULT '' NOT NULL,
  MODIFY Table_priv set('Select','Insert','Update','Delete','Create',
                        'Drop','Grant','References','Index','Alter',
                        'Create View','Show view','Trigger','Delete versioning rows')
    COLLATE utf8_general_ci DEFAULT '' NOT NULL,
  COMMENT='Table privileges';

#
# columns_priv
#
#
# Name change of Type -> Column_priv from MySQL 3.22.12
#
ALTER TABLE columns_priv
  CHANGE Type Column_priv set('Select','Insert','Update','References')
    COLLATE utf8_general_ci DEFAULT '' NOT NULL;

ALTER TABLE columns_priv
  MODIFY Host char(255) NOT NULL default '',
  MODIFY Db char(64) NOT NULL default '',
  MODIFY User char(128) binary NOT NULL default '',
  MODIFY Table_name char(64) NOT NULL default '',
  MODIFY Column_name char(64) NOT NULL default '',
  ENGINE=Aria,
  CONVERT TO CHARACTER SET utf8 COLLATE utf8_bin,
  COMMENT='Column privileges';

ALTER TABLE columns_priv
  MODIFY Column_priv set('Select','Insert','Update','References')
    COLLATE utf8_general_ci DEFAULT '' NOT NULL;

#
#  Add the new 'type' column to the func table.
#

ALTER TABLE func add type enum ('function','aggregate') COLLATE utf8_general_ci NOT NULL;

#
#  Change the user,db and host tables to current format
#

# Detect whether we had Show_db_priv
SET @hadShowDbPriv:=0;
SELECT @hadShowDbPriv:=1 FROM user WHERE Show_db_priv IS NOT NULL;

ALTER TABLE user
ADD Show_db_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Alter_priv,
ADD Super_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Show_db_priv,
ADD Create_tmp_table_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Super_priv,
ADD Lock_tables_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_tmp_table_priv,
ADD Execute_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Lock_tables_priv,
ADD Repl_slave_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Execute_priv,
ADD Repl_client_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Repl_slave_priv;

# Convert privileges so that users have similar privileges as before

UPDATE user SET Show_db_priv= Select_priv, Super_priv=Process_priv, Execute_priv=Process_priv, Create_tmp_table_priv='Y', Lock_tables_priv='Y', Repl_slave_priv=file_priv, Repl_client_priv=File_priv where user<>"" AND @hadShowDbPriv = 0;


#  Add fields that can be used to limit number of questions and connections
#  for some users.

ALTER TABLE user
ADD max_questions int(11) NOT NULL DEFAULT 0 AFTER x509_subject,
ADD max_updates   int(11) unsigned NOT NULL DEFAULT 0 AFTER max_questions,
ADD max_connections int(11) unsigned NOT NULL DEFAULT 0 AFTER max_updates;


#
#  Add Create_tmp_table_priv and Lock_tables_priv to db and host
#

ALTER TABLE db
ADD Create_tmp_table_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
ADD Lock_tables_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;

alter table user change max_questions max_questions int(11) unsigned DEFAULT 0  NOT NULL;


alter table db comment='Database privileges';
alter table user comment='Users and global privileges';
alter table func comment='User defined functions';

# Convert all tables to UTF-8 with binary collation
# and reset all char columns to correct width
ALTER TABLE user
  MODIFY Host char(255) NOT NULL default '',
  MODIFY User char(128) binary NOT NULL default '',
  ENGINE=Aria, CONVERT TO CHARACTER SET utf8 COLLATE utf8_bin;

# In MySQL 5.7.6 the Password column is removed. Recreate it to preserve the number
# of columns MariaDB expects in the user table.
ALTER TABLE user
  ADD Password char(41) character set latin1 collate latin1_bin NOT NULL default '' AFTER User;

# In MySQL the Unix socket authentication plugin has a different name. Thus the
# references to it need to be renamed in the user table. Thanks to the WHERE
# clauses this applies only to MySQL->MariaDB upgrades and nothing else.
UPDATE user
  SET plugin='unix_socket' WHERE plugin='auth_socket';
DELETE FROM plugin
  WHERE name='auth_socket';

ALTER TABLE user
  MODIFY Password char(41) character set latin1 collate latin1_bin NOT NULL default '',
  MODIFY Select_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Insert_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Update_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Delete_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Create_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Drop_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Reload_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Shutdown_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Process_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY File_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Grant_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY References_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Index_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Alter_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Show_db_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Super_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Create_tmp_table_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Lock_tables_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Execute_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Repl_slave_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY Repl_client_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY ssl_type enum('','ANY','X509', 'SPECIFIED') COLLATE utf8_general_ci DEFAULT '' NOT NULL;

ALTER TABLE db
  MODIFY Host char(255) NOT NULL default '',
  MODIFY Db char(64) NOT NULL default '',
  MODIFY User char(128) binary NOT NULL default '',
  ENGINE=Aria, CONVERT TO CHARACTER SET utf8 COLLATE utf8_bin;
ALTER TABLE db
  MODIFY  Select_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Insert_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Update_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Delete_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Create_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Drop_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Grant_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  References_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Index_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Alter_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Create_tmp_table_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL,
  MODIFY  Lock_tables_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;


ALTER TABLE func
  ENGINE=Aria, CONVERT TO CHARACTER SET utf8 COLLATE utf8_bin;
ALTER TABLE func
  MODIFY type enum ('function','aggregate') COLLATE utf8_general_ci NOT NULL;

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

ALTER TABLE plugin
  MODIFY name varchar(64) COLLATE utf8_general_ci NOT NULL DEFAULT '',
  MODIFY dl varchar(128) COLLATE utf8_general_ci NOT NULL DEFAULT '',
  CONVERT TO CHARACTER SET utf8 COLLATE utf8_general_ci;

#
# Detect whether we had Create_view_priv
#
SET @hadCreateViewPriv:=0;
SELECT @hadCreateViewPriv:=1 FROM user WHERE Create_view_priv IS NOT NULL;

#
# Create VIEWs privileges (v5.0)
#
ALTER TABLE db ADD Create_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Lock_tables_priv;
ALTER TABLE db MODIFY Create_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Lock_tables_priv;


ALTER TABLE user ADD Create_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Repl_client_priv;
ALTER TABLE user MODIFY Create_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Repl_client_priv;

#
# Show VIEWs privileges (v5.0)
#
ALTER TABLE db ADD Show_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_view_priv;
ALTER TABLE db MODIFY Show_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_view_priv;

ALTER TABLE user ADD Show_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_view_priv;
ALTER TABLE user MODIFY Show_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_view_priv;

#
# Assign create/show view privileges to people who have create provileges
#
UPDATE user SET Create_view_priv=Create_priv, Show_view_priv=Create_priv where user<>"" AND @hadCreateViewPriv = 0;

#
#
#
SET @hadCreateRoutinePriv:=0;
SELECT @hadCreateRoutinePriv:=1 FROM user WHERE Create_routine_priv IS NOT NULL;

#
# Create PROCEDUREs privileges (v5.0)
#
ALTER TABLE db ADD Create_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Show_view_priv;
ALTER TABLE db MODIFY Create_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Show_view_priv;

ALTER TABLE user ADD Create_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Show_view_priv;
ALTER TABLE user MODIFY Create_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Show_view_priv;

#
# Alter PROCEDUREs privileges (v5.0)
#
ALTER TABLE db ADD Alter_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_routine_priv;
ALTER TABLE db MODIFY Alter_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_routine_priv;

ALTER TABLE user ADD Alter_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_routine_priv;
ALTER TABLE user MODIFY Alter_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Create_routine_priv;

ALTER TABLE db ADD Execute_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Alter_routine_priv;
ALTER TABLE db MODIFY Execute_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Alter_routine_priv;

#
# Assign create/alter routine privileges to people who have create privileges
#
UPDATE user SET Create_routine_priv=Create_priv, Alter_routine_priv=Alter_priv where user<>"" AND @hadCreateRoutinePriv = 0;
UPDATE db SET Create_routine_priv=Create_priv, Alter_routine_priv=Alter_priv, Execute_priv=Select_priv where user<>"" AND @hadCreateRoutinePriv = 0;

#
# Add max_user_connections resource limit
# this is signed in MariaDB so that if one sets it's to -1 then the user
# can't connect anymore.
#
ALTER TABLE user ADD max_user_connections int(11) DEFAULT '0' NOT NULL AFTER max_connections;
ALTER TABLE user MODIFY max_user_connections int(11) DEFAULT '0' NOT NULL AFTER max_connections;

#
# user.Create_user_priv
#

SET @hadCreateUserPriv:=0;
SELECT @hadCreateUserPriv:=1 FROM user WHERE Create_user_priv IS NOT NULL;

ALTER TABLE user ADD Create_user_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Alter_routine_priv;
ALTER TABLE user MODIFY Create_user_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Alter_routine_priv;
UPDATE user LEFT JOIN db USING (Host,User) SET Create_user_priv='Y'
  WHERE @hadCreateUserPriv = 0 AND
        (user.Grant_priv = 'Y' OR db.Grant_priv = 'Y');

#
# procs_priv
#

ALTER TABLE procs_priv
  ENGINE=Aria,
  CONVERT TO CHARACTER SET utf8 COLLATE utf8_bin;

ALTER TABLE procs_priv
  MODIFY Proc_priv set('Execute','Alter Routine','Grant')
    COLLATE utf8_general_ci DEFAULT '' NOT NULL;

ALTER IGNORE TABLE procs_priv
  MODIFY Routine_name char(64)
    COLLATE utf8_general_ci DEFAULT '' NOT NULL;

ALTER TABLE procs_priv
  ADD Routine_type enum('FUNCTION','PROCEDURE')
    COLLATE utf8_general_ci NOT NULL AFTER Routine_name;

ALTER TABLE procs_priv
  MODIFY Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP AFTER Proc_priv;

#
# proc
#

# Correct the name fields to not binary, and expand sql_data_access
ALTER TABLE proc MODIFY name char(64) DEFAULT '' NOT NULL,
                 MODIFY specific_name char(64) DEFAULT '' NOT NULL,
                 MODIFY sql_data_access
                        enum('CONTAINS_SQL',
                             'NO_SQL',
                             'READS_SQL_DATA',
                             'MODIFIES_SQL_DATA'
                            ) DEFAULT 'CONTAINS_SQL' NOT NULL,
                 MODIFY body longblob NOT NULL,
                 MODIFY returns longblob NOT NULL,
                 MODIFY sql_mode
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
                            'PAD_CHAR_TO_FULL_LENGTH',
                            'EMPTY_STRING_IS_NULL',
                            'SIMULTANEOUS_ASSIGNMENT',
                            'TIME_ROUND_FRACTIONAL'
                            ) DEFAULT '' NOT NULL,
                 DEFAULT CHARACTER SET utf8;

# Correct the character set and collation
# Reset some fields after the conversion
ALTER TABLE proc CONVERT TO CHARACTER SET utf8,
                  MODIFY db char(64) binary DEFAULT '' NOT NULL,
                  MODIFY definer varchar(384) binary DEFAULT '' NOT NULL,
                  MODIFY comment text binary NOT NULL;

ALTER TABLE proc ADD character_set_client
                     char(32) collate utf8_bin DEFAULT NULL
                     AFTER comment;
ALTER TABLE proc MODIFY character_set_client
                        char(32) collate utf8_bin DEFAULT NULL;

ALTER TABLE proc MODIFY type enum('FUNCTION',
                                  'PROCEDURE',
                                  'PACKAGE',
                                  'PACKAGE BODY') NOT NULL;

ALTER TABLE procs_priv MODIFY Routine_type enum('FUNCTION',
                                                'PROCEDURE',
                                                'PACKAGE',
                                                'PACKAGE BODY') NOT NULL;

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

# MDEV-7773: Stored Aggregate Functions
ALTER TABLE proc ADD aggregate enum('NONE', 'GROUP') DEFAULT 'NONE' NOT NULL
                     AFTER body_utf8;

# Update definer of Add/DropGeometryColumn procedures to 'mariadb.sys'
# To consider the scenarios in MDEV-23102, only update the definer when it's 'root'
UPDATE proc SET Definer = 'mariadb.sys@localhost' WHERE Definer = 'root@localhost' AND Name = 'AddGeometryColumn';
UPDATE proc SET Definer = 'mariadb.sys@localhost' WHERE Definer = 'root@localhost' AND Name = 'DropGeometryColumn';

#
# EVENT privilege
#
SET @hadEventPriv := 0;
SELECT @hadEventPriv :=1 FROM user WHERE Event_priv IS NOT NULL;

ALTER TABLE user ADD Event_priv enum('N','Y') character set utf8 DEFAULT 'N' NOT NULL AFTER Create_user_priv;
ALTER TABLE user MODIFY Event_priv enum('N','Y') character set utf8 DEFAULT 'N' NOT NULL AFTER Create_user_priv;

UPDATE user SET Event_priv=Super_priv WHERE @hadEventPriv = 0;

ALTER TABLE db ADD Event_priv enum('N','Y') character set utf8 DEFAULT 'N' NOT NULL;
ALTER TABLE db MODIFY Event_priv enum('N','Y') character set utf8 DEFAULT 'N' NOT NULL;

#
# EVENT table
#
ALTER TABLE event DROP PRIMARY KEY, ADD PRIMARY KEY(db, name);
# Add sql_mode column just in case.
ALTER TABLE event ADD sql_mode set ('IGNORE_BAD_TABLE_OPTIONS') AFTER on_completion;
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
                            'PAD_CHAR_TO_FULL_LENGTH',
                            'EMPTY_STRING_IS_NULL',
                            'SIMULTANEOUS_ASSIGNMENT',
                            'TIME_ROUND_FRACTIONAL'
                            ) DEFAULT '' NOT NULL AFTER on_completion;
ALTER TABLE event MODIFY name char(64) CHARACTER SET utf8 NOT NULL default '';

ALTER TABLE event ADD COLUMN originator INT UNSIGNED NOT NULL AFTER comment;
ALTER TABLE event MODIFY COLUMN originator INT UNSIGNED NOT NULL;

ALTER TABLE event MODIFY COLUMN status ENUM('ENABLED','DISABLED','SLAVESIDE_DISABLED') NOT NULL default 'ENABLED';

ALTER TABLE event ADD COLUMN time_zone char(64) CHARACTER SET latin1
        NOT NULL DEFAULT 'SYSTEM' AFTER originator;

ALTER TABLE event ADD character_set_client
                      char(32) collate utf8_bin DEFAULT NULL
                      AFTER time_zone;
ALTER TABLE event MODIFY character_set_client
                         char(32) collate utf8_bin DEFAULT NULL;

ALTER TABLE event ADD collation_connection
                      char(32) collate utf8_bin DEFAULT NULL
                      AFTER character_set_client;
ALTER TABLE event MODIFY collation_connection
                         char(32) collate utf8_bin DEFAULT NULL;

ALTER TABLE event ADD db_collation
                      char(32) collate utf8_bin DEFAULT NULL
                      AFTER collation_connection;
ALTER TABLE event MODIFY db_collation
                         char(32) collate utf8_bin DEFAULT NULL;

ALTER TABLE event ADD body_utf8 longblob DEFAULT NULL
                      AFTER db_collation;
ALTER TABLE event MODIFY body_utf8 longblob DEFAULT NULL;

alter table event MODIFY definer varchar(384) collate utf8_bin NOT NULL DEFAULT '';

# Enable event scheduler if the event table was not up to date before.
set global event_scheduler=original;

#
# TRIGGER privilege
#

SET @hadTriggerPriv := 0;
SELECT @hadTriggerPriv :=1 FROM user WHERE Trigger_priv IS NOT NULL;

ALTER TABLE user ADD Trigger_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Event_priv;
ALTER TABLE user MODIFY Trigger_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Event_priv;

ALTER TABLE db ADD Trigger_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;
ALTER TABLE db MODIFY Trigger_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL;

UPDATE user SET Trigger_priv=Super_priv WHERE @hadTriggerPriv = 0;

#
# user.Create_tablespace_priv
#

SET @hadCreateTablespacePriv := 0;
SELECT @hadCreateTablespacePriv :=1 FROM user WHERE Create_tablespace_priv IS NOT NULL;

ALTER TABLE user ADD Create_tablespace_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Trigger_priv;
ALTER TABLE user MODIFY Create_tablespace_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER Trigger_priv;

UPDATE user SET Create_tablespace_priv = Super_priv WHERE @hadCreateTablespacePriv = 0;

#
# System versioning
#

ALTER TABLE user change Truncate_versioning_priv Delete_history_priv enum('N','Y') COLLATE utf8_general_ci NOT NULL DEFAULT 'N';
ALTER TABLE db change Truncate_versioning_priv Delete_history_priv enum('N','Y') COLLATE utf8_general_ci NOT NULL DEFAULT 'N';

SET @had_user_delete_history_priv := 0;
SELECT @had_user_delete_history_priv :=1 FROM user WHERE Delete_history_priv IS NOT NULL;

ALTER TABLE user add Delete_history_priv enum('N','Y') COLLATE utf8_general_ci NOT NULL DEFAULT 'N' after Create_tablespace_priv;
ALTER TABLE user modify Delete_history_priv enum('N','Y') COLLATE utf8_general_ci NOT NULL DEFAULT 'N';
ALTER TABLE db add Delete_history_priv enum('N','Y') COLLATE utf8_general_ci NOT NULL DEFAULT 'N' after Trigger_priv;
ALTER TABLE db modify Delete_history_priv enum('N','Y') COLLATE utf8_general_ci NOT NULL DEFAULT 'N';

UPDATE user SET Delete_history_priv = Super_priv WHERE @had_user_delete_history_priv = 0;

ALTER TABLE user ADD plugin char(64) CHARACTER SET latin1 DEFAULT '' NOT NULL AFTER max_user_connections,
                 ADD authentication_string TEXT NOT NULL AFTER plugin;
ALTER TABLE user CHANGE auth_string authentication_string TEXT NOT NULL;

ALTER TABLE user ADD password_expired ENUM('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER authentication_string;
ALTER TABLE user ADD password_last_changed timestamp DEFAULT CURRENT_TIMESTAMP NOT NULL after password_expired;
ALTER TABLE user ADD password_lifetime smallint unsigned DEFAULT NULL after password_last_changed;
ALTER TABLE user ADD account_locked enum('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL after password_lifetime;
ALTER TABLE user ADD is_role enum('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER account_locked;
ALTER TABLE user ADD default_role char(128) binary DEFAULT '' NOT NULL AFTER is_role;
ALTER TABLE user ADD max_statement_time decimal(12,6) DEFAULT 0 NOT NULL AFTER default_role;

-- Somewhere above, we ran ALTER TABLE user .... CONVERT TO CHARACTER SET utf8 COLLATE utf8_bin.
-- we want password_expired column to have collation utf8_general_ci.
-- Order columns correctly that were not ordered until MDEV-23201 (ff8ffef3e1915d7a9caa07d9461cd8d47c4baf98)

ALTER TABLE user MODIFY plugin char(64) CHARACTER SET latin1 DEFAULT '' NOT NULL AFTER max_user_connections,
                 MODIFY authentication_string TEXT NOT NULL AFTER plugin,
                 MODIFY password_expired ENUM('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER authentication_string,
                 MODIFY is_role enum('N', 'Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL AFTER password_expired,
                 MODIFY default_role char(80) binary DEFAULT '' NOT NULL AFTER is_role,
                 MODIFY max_statement_time decimal(12,6) DEFAULT 0 NOT NULL AFTER default_role,
-- MDEV-24122 formerly mysql5.7 users may have the following columns password_last_changed,
-- password_lifetime and account_locked. Ensure they are beyond the end of the user columns
-- used by MariaDB. MariaDB-10.4 will use these in the creation of mysql.global_priv.
-- password_last_changed has a DEFAULT/ON UPDATE of CURRENT_TIMESTAMP to keep track of
-- time until 10.4 added.
                 MODIFY IF EXISTS password_last_changed timestamp DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP AFTER max_statement_time,
                 MODIFY IF EXISTS password_lifetime smallint unsigned DEFAULT NULL AFTER password_last_changed,
                 MODIFY IF EXISTS account_locked enum('N', 'Y') CHARACTER SET utf8 DEFAULT 'N' NOT NULL after password_lifetime;

-- Checking for any duplicate hostname and username combination are exists.
-- If exits we will throw error.
DELIMITER //
BEGIN NOT ATOMIC
  SET @duplicate_hosts=(SELECT count(*) FROM mysql.user GROUP BY user, lower(host) HAVING count(*) > 1 LIMIT 1);
  IF @duplicate_hosts > 1 THEN
    SIGNAL SQLSTATE '45000'  SET MESSAGE_TEXT = 'Multiple accounts exist for @user_name, @host_name that differ only in Host lettercase; remove all except one of them';
  END IF;
END //
DELIMITER ;
-- Get warnings (if any)
SHOW WARNINGS;

# Convering the host name to lower case for existing users
UPDATE user SET host=LOWER( host ) WHERE LOWER( host ) <> host;

DELIMITER //
if @have_innodb then
  # fix bad data when upgrading from unfixed InnoDB (MDEV-13360)
  delete from innodb_index_stats where length(table_name) > 64;
  delete from innodb_table_stats where length(table_name) > 64;

  # update table_name and timestamp fields in the innodb stat tables
  alter table innodb_index_stats modify last_update timestamp not null default current_timestamp on update current_timestamp, modify table_name varchar(199);
  alter table innodb_table_stats modify last_update timestamp not null default current_timestamp on update current_timestamp, modify table_name varchar(199);

  alter table innodb_index_stats drop foreign key if exists innodb_index_stats_ibfk_1;
end if //
DELIMITER ;

# MDEV-4332 longer user names
alter table user         modify User         char(128)  binary not null default '';
alter table db           modify User         char(128)  binary not null default '';
alter table tables_priv  modify User         char(128)  binary not null default '';
alter table columns_priv modify User         char(128)  binary not null default '';
alter table procs_priv   modify User         char(128)  binary not null default '';
alter table proc         modify definer      varchar(384) collate utf8_bin not null default '';
alter table proxies_priv modify User         char(128)  COLLATE utf8_bin not null default '';
alter table proxies_priv modify Proxied_user char(128)  COLLATE utf8_bin not null default '';
alter table proxies_priv modify Grantor      varchar(384) COLLATE utf8_bin not null default '';
alter table servers      modify Username     char(128)                   not null default '';
alter table procs_priv   modify Grantor      varchar(384) COLLATE utf8_bin not null default '';
alter table tables_priv  modify Grantor      varchar(384) COLLATE utf8_bin not null default '';

# Activate the new, possible modified privilege tables
# This should not be needed, but gives us some extra testing that the above
# changes was correct

flush privileges;

--
-- Upgrade help tables
--

ALTER TABLE help_category MODIFY url TEXT NOT NULL;
ALTER TABLE help_topic MODIFY url TEXT NOT NULL;

# MDEV-7383 - varbinary on mix/max of column_stats
alter table column_stats modify min_value varbinary(255) DEFAULT NULL, modify max_value varbinary(255) DEFAULT NULL;

DELIMITER //
IF 'BASE TABLE' = (select table_type from information_schema.tables where table_schema=database() and table_name='user') THEN
  CREATE TABLE IF NOT EXISTS global_priv (Host char(255) binary DEFAULT '', User char(128) binary DEFAULT '', Priv JSON NOT NULL DEFAULT '{}' CHECK(JSON_VALID(Priv)), PRIMARY KEY Host (Host,User)) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='Users and global privileges'
  SELECT Host, User, JSON_COMPACT(JSON_OBJECT('access',
                             1*('Y'=Select_priv)+
                             2*('Y'=Insert_priv)+
                             4*('Y'=Update_priv)+
                             8*('Y'=Delete_priv)+
                            16*('Y'=Create_priv)+
                            32*('Y'=Drop_priv)+
                            64*('Y'=Reload_priv)+
                           128*('Y'=Shutdown_priv)+
                           256*('Y'=Process_priv)+
                           512*('Y'=File_priv)+
                          1024*('Y'=Grant_priv)+
                          2048*('Y'=References_priv)+
                          4096*('Y'=Index_priv)+
                          8192*('Y'=Alter_priv)+
                         16384*('Y'=Show_db_priv)+
                         32768*('Y'=Super_priv)+
                         65536*('Y'=Create_tmp_table_priv)+
                        131072*('Y'=Lock_tables_priv)+
                        262144*('Y'=Execute_priv)+
                        524288*('Y'=Repl_slave_priv)+
                       1048576*('Y'=Repl_client_priv)+
                       2097152*('Y'=Create_view_priv)+
                       4194304*('Y'=Show_view_priv)+
                       8388608*('Y'=Create_routine_priv)+
                      16777216*('Y'=Alter_routine_priv)+
                      33554432*('Y'=Create_user_priv)+
                      67108864*('Y'=Event_priv)+
                     134217728*('Y'=Trigger_priv)+
                     268435456*('Y'=Create_tablespace_priv)+
                     536870912*('Y'=Delete_history_priv),
                    'ssl_type', ssl_type-1,
                    'ssl_cipher', ssl_cipher,
                    'x509_issuer', x509_issuer,
                    'x509_subject', x509_subject,
                    'max_questions', max_questions,
                    'max_updates', max_updates,
                    'max_connections', max_connections,
                    'max_user_connections', max_user_connections,
                    'max_statement_time', max_statement_time,
                    'plugin', if(plugin>'',plugin,if(length(password)=16,'mysql_old_password','mysql_native_password')),
                    'authentication_string', if(plugin>'' and authentication_string>'',authentication_string,password),
                    'password_last_changed', if(password_expired='Y', 0, if(password_last_changed, UNIX_TIMESTAMP(password_last_changed), UNIX_TIMESTAMP())),
                    'password_lifetime', ifnull(password_lifetime, -1),
                    'account_locked', 'Y'=account_locked,
                    'default_role', default_role,
                    'is_role', 'Y'=is_role)) as Priv
  FROM user;
  DROP TABLE user;
END IF//

IF 1 = (SELECT count(*) FROM information_schema.VIEWS WHERE TABLE_CATALOG = 'def' and TABLE_SCHEMA = 'mysql' and TABLE_NAME='user' and (DEFINER = 'root@localhost' or (DEFINER = 'mariadb.sys@localhost' and VIEW_DEFINITION LIKE "%'N' AS `password_expired`%"))) THEN
  DROP VIEW IF EXISTS mysql.user;
END IF//

DELIMITER ;

--
-- Upgrade mysql.column_stats table
--

ALTER TABLE column_stats
  modify hist_type enum('SINGLE_PREC_HB','DOUBLE_PREC_HB','JSON_HB'),
  modify histogram longblob;
