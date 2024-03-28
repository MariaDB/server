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
set sql_safe_updates='OFF';
set default_storage_engine=Aria;
set enforce_storage_engine=NULL;
set alter_algorithm='DEFAULT';

set @have_innodb= (select count(engine) from information_schema.engines where engine='INNODB' and support != 'NO');


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

set @a=(SELECT concat_ws(',',OBJECT_TYPE,OBJECT_SCHEMA,OBJECT_NAME,COUNT_STAR,SUM_TIMER_WAIT,MIN_TIMER_WAIT,AVG_TIMER_WAIT,MAX_TIMER_WAIT) FROM performance_schema.objects_summary_global_by_type WHERE object_schema='test' AND object_name='user');
ALTER TABLE user
ADD ssl_type enum('','ANY','X509', 'SPECIFIED') DEFAULT '' NOT NULL,
ADD ssl_cipher BLOB NOT NULL,
ADD x509_issuer BLOB NOT NULL,
ADD x509_subject BLOB NOT NULL;

SELECT @a = concat_ws(',',OBJECT_TYPE,OBJECT_SCHEMA,OBJECT_NAME,COUNT_STAR,SUM_TIMER_WAIT,MIN_TIMER_WAIT,AVG_TIMER_WAIT,MAX_TIMER_WAIT) FROM performance_schema.objects_summary_global_by_type WHERE object_schema='test' AND object_name='user';
ALTER TABLE user MODIFY ssl_type enum('','ANY','X509', 'SPECIFIED') DEFAULT '' NOT NULL;
SELECT @a = concat_ws(',',OBJECT_TYPE,OBJECT_SCHEMA,OBJECT_NAME,COUNT_STAR,SUM_TIMER_WAIT,MIN_TIMER_WAIT,AVG_TIMER_WAIT,MAX_TIMER_WAIT) FROM performance_schema.objects_summary_global_by_type WHERE object_schema='test' AND object_name='user';

#
# tables_priv
#
ALTER TABLE tables_priv
  ADD KEY Grantor (Grantor);

ALTER TABLE tables_priv
  MODIFY Host char(60) NOT NULL default '',
  MODIFY Db char(64) NOT NULL default '',
  MODIFY User char(80) binary NOT NULL default '',
  MODIFY Table_name char(64) NOT NULL default '',
  MODIFY Grantor char(141) COLLATE utf8_bin NOT NULL default '',
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
  MODIFY Host char(60) NOT NULL default '',
  MODIFY Db char(64) NOT NULL default '',
  MODIFY User char(80) binary NOT NULL default '',
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
  MODIFY Host char(60) NOT NULL default '',
  MODIFY User char(80) binary NOT NULL default '',
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
# Delete plugins that are now inbuilt but might not have been before (MDEV-32043)
DELETE plugin
  FROM information_schema.PLUGINS is_p
  JOIN plugin ON plugin.name = is_p.PLUGIN_NAME
  WHERE is_p.PLUGIN_LIBRARY IS NULL;

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
  MODIFY Host char(60) NOT NULL default '',
  MODIFY Db char(64) NOT NULL default '',
  MODIFY User char(80) binary NOT NULL default '',
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
  MODIFY server_id INTEGER UNSIGNED NOT NULL,
  MODIFY command_type VARCHAR(64) NOT NULL,
  MODIFY argument MEDIUMTEXT NOT NULL,
  MODIFY thread_id BIGINT(21) UNSIGNED NOT NULL;
SET GLOBAL general_log = @old_log_state;

SET @old_log_state = @@global.slow_query_log;
SET GLOBAL slow_query_log = 'OFF';
ALTER TABLE slow_log
  MODIFY start_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  MODIFY user_host MEDIUMTEXT NOT NULL,
  MODIFY query_time TIME(6) NOT NULL,
  MODIFY lock_time TIME(6) NOT NULL,
  MODIFY rows_sent BIGINT UNSIGNED NOT NULL,
  MODIFY rows_examined BIGINT UNSIGNED NOT NULL,
  MODIFY db VARCHAR(512) NOT NULL,
  MODIFY last_insert_id INTEGER NOT NULL,
  MODIFY insert_id INTEGER NOT NULL,
  MODIFY server_id INTEGER UNSIGNED NOT NULL,
  MODIFY sql_text MEDIUMTEXT NOT NULL;
ALTER TABLE slow_log
  ADD COLUMN thread_id BIGINT(21) UNSIGNED NOT NULL AFTER sql_text;
ALTER TABLE slow_log
  MODIFY thread_id BIGINT(21) UNSIGNED NOT NULL,
  ADD COLUMN rows_affected BIGINT UNSIGNED NOT NULL AFTER thread_id;
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
                  MODIFY definer char(141) binary DEFAULT '' NOT NULL,
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
ALTER TABLE user ADD default_role char(80) binary DEFAULT '' NOT NULL AFTER is_role;
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

DELIMITER //
IF 'BASE TABLE' = (select table_type from information_schema.tables where table_schema=database() and table_name='user') THEN
  CREATE TABLE IF NOT EXISTS global_priv (Host char(60) binary DEFAULT '', User char(80) binary DEFAULT '', Priv JSON NOT NULL DEFAULT '{}' CHECK(JSON_VALID(Priv)), PRIMARY KEY Host (Host,User)) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='Users and global privileges'
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

# MDEV-22683 - upgrade Host and Owner of servers
ALTER TABLE servers
  MODIFY Host varchar(2048) NOT NULL DEFAULT '',
  MODIFY Owner varchar(512) NOT NULL DEFAULT '';
-- Copyright (c) 2007, 2018, Oracle and/or its affiliates.
-- Copyright (c) 2008, 2019, MariaDB Corporation.
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

--
-- The system tables of MySQL Server
--

set sql_mode='';

set @orig_storage_engine=@@default_storage_engine;
set default_storage_engine=Aria;

set system_versioning_alter_history=keep;

set @have_innodb= (select count(engine) from information_schema.engines where engine='INNODB' and support != 'NO');
SET @innodb_or_aria=IF(@have_innodb <> 0, 'InnoDB', 'Aria');

CREATE TABLE IF NOT EXISTS db (   Host char(60) binary DEFAULT '' NOT NULL, Db char(64) binary DEFAULT '' NOT NULL, User char(80) binary DEFAULT '' NOT NULL, Select_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Insert_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Update_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Delete_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Create_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Drop_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Grant_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, References_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Index_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Alter_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Create_tmp_table_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Lock_tables_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Create_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Show_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Create_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Alter_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Execute_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Event_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Trigger_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Delete_history_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, PRIMARY KEY /*Host */(Host,Db,User), KEY User (User) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='Database privileges';

-- Remember for later if db table already existed
set @had_db_table= @@warning_count != 0;

CREATE TABLE IF NOT EXISTS global_priv (Host char(60) binary DEFAULT '', User char(80) binary DEFAULT '', Priv JSON NOT NULL DEFAULT '{}' CHECK(JSON_VALID(Priv)), PRIMARY KEY (Host,User)) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='Users and global privileges';

set @exists_user_view= EXISTS (SELECT * FROM information_schema.VIEWS WHERE TABLE_CATALOG = 'def' and TABLE_SCHEMA = 'mysql' and TABLE_NAME='user');

set @exists_user_view_by_root= EXISTS (SELECT * FROM information_schema.VIEWS WHERE TABLE_CATALOG = 'def' and TABLE_SCHEMA = 'mysql' and TABLE_NAME='user' and DEFINER = 'mariadb.sys@localhost');

set @need_sys_user_creation= (( NOT @exists_user_view) OR @exists_user_view_by_root);

CREATE TEMPORARY TABLE tmp_user_sys LIKE global_priv;
INSERT INTO tmp_user_sys (Host,User,Priv) VALUES ('localhost','mariadb.sys','{"access":0,"plugin":"mysql_native_password","authentication_string":"","account_locked":true,"password_last_changed":0}');
INSERT IGNORE INTO global_priv SELECT * FROM tmp_user_sys WHERE 0 <> @need_sys_user_creation;
DROP TABLE tmp_user_sys;


CREATE DEFINER='mariadb.sys'@'localhost' SQL SECURITY DEFINER VIEW IF NOT EXISTS user AS SELECT
  Host,
  User,
  IF(JSON_VALUE(Priv, '$.plugin') IN ('mysql_native_password', 'mysql_old_password'), IFNULL(JSON_VALUE(Priv, '$.authentication_string'), ''), '') AS Password,
  IF(JSON_VALUE(Priv, '$.access') &         1, 'Y', 'N') AS Select_priv,
  IF(JSON_VALUE(Priv, '$.access') &         2, 'Y', 'N') AS Insert_priv,
  IF(JSON_VALUE(Priv, '$.access') &         4, 'Y', 'N') AS Update_priv,
  IF(JSON_VALUE(Priv, '$.access') &         8, 'Y', 'N') AS Delete_priv,
  IF(JSON_VALUE(Priv, '$.access') &        16, 'Y', 'N') AS Create_priv,
  IF(JSON_VALUE(Priv, '$.access') &        32, 'Y', 'N') AS Drop_priv,
  IF(JSON_VALUE(Priv, '$.access') &        64, 'Y', 'N') AS Reload_priv,
  IF(JSON_VALUE(Priv, '$.access') &       128, 'Y', 'N') AS Shutdown_priv,
  IF(JSON_VALUE(Priv, '$.access') &       256, 'Y', 'N') AS Process_priv,
  IF(JSON_VALUE(Priv, '$.access') &       512, 'Y', 'N') AS File_priv,
  IF(JSON_VALUE(Priv, '$.access') &      1024, 'Y', 'N') AS Grant_priv,
  IF(JSON_VALUE(Priv, '$.access') &      2048, 'Y', 'N') AS References_priv,
  IF(JSON_VALUE(Priv, '$.access') &      4096, 'Y', 'N') AS Index_priv,
  IF(JSON_VALUE(Priv, '$.access') &      8192, 'Y', 'N') AS Alter_priv,
  IF(JSON_VALUE(Priv, '$.access') &     16384, 'Y', 'N') AS Show_db_priv,
  IF(JSON_VALUE(Priv, '$.access') &     32768, 'Y', 'N') AS Super_priv,
  IF(JSON_VALUE(Priv, '$.access') &     65536, 'Y', 'N') AS Create_tmp_table_priv,
  IF(JSON_VALUE(Priv, '$.access') &    131072, 'Y', 'N') AS Lock_tables_priv,
  IF(JSON_VALUE(Priv, '$.access') &    262144, 'Y', 'N') AS Execute_priv,
  IF(JSON_VALUE(Priv, '$.access') &    524288, 'Y', 'N') AS Repl_slave_priv,
  IF(JSON_VALUE(Priv, '$.access') &   1048576, 'Y', 'N') AS Repl_client_priv,
  IF(JSON_VALUE(Priv, '$.access') &   2097152, 'Y', 'N') AS Create_view_priv,
  IF(JSON_VALUE(Priv, '$.access') &   4194304, 'Y', 'N') AS Show_view_priv,
  IF(JSON_VALUE(Priv, '$.access') &   8388608, 'Y', 'N') AS Create_routine_priv,
  IF(JSON_VALUE(Priv, '$.access') &  16777216, 'Y', 'N') AS Alter_routine_priv,
  IF(JSON_VALUE(Priv, '$.access') &  33554432, 'Y', 'N') AS Create_user_priv,
  IF(JSON_VALUE(Priv, '$.access') &  67108864, 'Y', 'N') AS Event_priv,
  IF(JSON_VALUE(Priv, '$.access') & 134217728, 'Y', 'N') AS Trigger_priv,
  IF(JSON_VALUE(Priv, '$.access') & 268435456, 'Y', 'N') AS Create_tablespace_priv,
  IF(JSON_VALUE(Priv, '$.access') & 536870912, 'Y', 'N') AS Delete_history_priv,
  ELT(IFNULL(JSON_VALUE(Priv, '$.ssl_type'), 0) + 1, '', 'ANY','X509', 'SPECIFIED') AS ssl_type,
  IFNULL(JSON_VALUE(Priv, '$.ssl_cipher'), '') AS ssl_cipher,
  IFNULL(JSON_VALUE(Priv, '$.x509_issuer'), '') AS x509_issuer,
  IFNULL(JSON_VALUE(Priv, '$.x509_subject'), '') AS x509_subject,
  CAST(IFNULL(JSON_VALUE(Priv, '$.max_questions'), 0) AS UNSIGNED) AS max_questions,
  CAST(IFNULL(JSON_VALUE(Priv, '$.max_updates'), 0) AS UNSIGNED) AS max_updates,
  CAST(IFNULL(JSON_VALUE(Priv, '$.max_connections'), 0) AS UNSIGNED) AS max_connections,
  CAST(IFNULL(JSON_VALUE(Priv, '$.max_user_connections'), 0) AS SIGNED) AS max_user_connections,
  IFNULL(JSON_VALUE(Priv, '$.plugin'), '') AS plugin,
  IFNULL(JSON_VALUE(Priv, '$.authentication_string'), '') AS authentication_string,
  IF(IFNULL(JSON_VALUE(Priv, '$.password_last_changed'), 1) = 0, 'Y', 'N') AS password_expired,
  ELT(IFNULL(JSON_VALUE(Priv, '$.is_role'), 0) + 1, 'N', 'Y') AS is_role,
  IFNULL(JSON_VALUE(Priv, '$.default_role'), '') AS default_role,
  CAST(IFNULL(JSON_VALUE(Priv, '$.max_statement_time'), 0.0) AS DECIMAL(12,6)) AS max_statement_time
  FROM global_priv;

-- Remember for later if user table already existed
set @had_user_table= @@warning_count != 0;

CREATE TABLE IF NOT EXISTS roles_mapping ( Host char(60) binary DEFAULT '' NOT NULL, User char(80) binary DEFAULT '' NOT NULL, Role char(80) binary DEFAULT '' NOT NULL, Admin_option enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, UNIQUE (Host, User, Role)) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='Granted roles';

CREATE TABLE IF NOT EXISTS func (  name char(64) binary DEFAULT '' NOT NULL, ret tinyint(1) DEFAULT '0' NOT NULL, dl char(128) DEFAULT '' NOT NULL, type enum ('function','aggregate') COLLATE utf8_general_ci NOT NULL, PRIMARY KEY (name) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='User defined functions';


CREATE TABLE IF NOT EXISTS plugin ( name varchar(64) DEFAULT '' NOT NULL, dl varchar(128) DEFAULT '' NOT NULL, PRIMARY KEY (name) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_general_ci comment='MySQL plugins';


CREATE TABLE IF NOT EXISTS servers ( Server_name char(64) NOT NULL DEFAULT '', Host varchar(2048) NOT NULL DEFAULT '', Db char(64) NOT NULL DEFAULT '', Username char(80) NOT NULL DEFAULT '', Password char(64) NOT NULL DEFAULT '', Port INT(4) NOT NULL DEFAULT '0', Socket char(64) NOT NULL DEFAULT '', Wrapper char(64) NOT NULL DEFAULT '', Owner varchar(512) NOT NULL DEFAULT '', PRIMARY KEY (Server_name)) engine=Aria transactional=1 CHARACTER SET utf8 comment='MySQL Foreign Servers table';


CREATE TABLE IF NOT EXISTS tables_priv ( Host char(60) binary DEFAULT '' NOT NULL, Db char(64) binary DEFAULT '' NOT NULL, User char(80) binary DEFAULT '' NOT NULL, Table_name char(64) binary DEFAULT '' NOT NULL, Grantor char(141) DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, Table_priv set('Select','Insert','Update','Delete','Create','Drop','Grant','References','Index','Alter','Create View','Show view','Trigger','Delete versioning rows') COLLATE utf8_general_ci DEFAULT '' NOT NULL, Column_priv set('Select','Insert','Update','References') COLLATE utf8_general_ci DEFAULT '' NOT NULL, PRIMARY KEY (Host,Db,User,Table_name), KEY Grantor (Grantor) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='Table privileges';

CREATE TEMPORARY TABLE tmp_user_sys LIKE tables_priv;
INSERT INTO tmp_user_sys (Host,Db,User,Table_name,Grantor,Timestamp,Table_priv) VALUES ('localhost','mysql','mariadb.sys','global_priv','root@localhost','0','Select,Delete');
INSERT IGNORE INTO tables_priv SELECT * FROM tmp_user_sys WHERE 0 <> @need_sys_user_creation;
DROP TABLE tmp_user_sys;

CREATE TABLE IF NOT EXISTS columns_priv ( Host char(60) binary DEFAULT '' NOT NULL, Db char(64) binary DEFAULT '' NOT NULL, User char(80) binary DEFAULT '' NOT NULL, Table_name char(64) binary DEFAULT '' NOT NULL, Column_name char(64) binary DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, Column_priv set('Select','Insert','Update','References') COLLATE utf8_general_ci DEFAULT '' NOT NULL, PRIMARY KEY (Host,Db,User,Table_name,Column_name) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='Column privileges';


CREATE TABLE IF NOT EXISTS help_topic ( help_topic_id int unsigned not null, name char(64) not null, help_category_id smallint unsigned not null, description text not null, example text not null, url text not null, primary key (help_topic_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help topics';


CREATE TABLE IF NOT EXISTS help_category ( help_category_id smallint unsigned not null, name  char(64) not null, parent_category_id smallint unsigned null, url text not null, primary key (help_category_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help categories';


CREATE TABLE IF NOT EXISTS help_relation ( help_topic_id int unsigned not null references help_topic, help_keyword_id  int unsigned not null references help_keyword, primary key (help_keyword_id, help_topic_id) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='keyword-topic relation';


CREATE TABLE IF NOT EXISTS help_keyword (   help_keyword_id  int unsigned not null, name char(64) not null, primary key (help_keyword_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help keywords';


CREATE TABLE IF NOT EXISTS time_zone_name (   Name char(64) NOT NULL, Time_zone_id int unsigned NOT NULL, PRIMARY KEY /*Name*/ (Name) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone names';


CREATE TABLE IF NOT EXISTS time_zone (   Time_zone_id int unsigned NOT NULL auto_increment, Use_leap_seconds enum('Y','N') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, PRIMARY KEY /*TzId*/ (Time_zone_id) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zones';


CREATE TABLE IF NOT EXISTS time_zone_transition (   Time_zone_id int unsigned NOT NULL, Transition_time bigint signed NOT NULL, Transition_type_id int unsigned NOT NULL, PRIMARY KEY /*TzIdTranTime*/ (Time_zone_id, Transition_time) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone transitions';


CREATE TABLE IF NOT EXISTS time_zone_transition_type (   Time_zone_id int unsigned NOT NULL, Transition_type_id int unsigned NOT NULL, Offset int signed DEFAULT 0 NOT NULL, Is_DST tinyint unsigned DEFAULT 0 NOT NULL, Abbreviation char(8) DEFAULT '' NOT NULL, PRIMARY KEY /*TzIdTrTId*/ (Time_zone_id, Transition_type_id) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone transition types';


CREATE TABLE IF NOT EXISTS time_zone_leap_second (   Transition_time bigint signed NOT NULL, Correction int signed NOT NULL, PRIMARY KEY /*TranTime*/ (Transition_time) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Leap seconds information for time zones';

CREATE TABLE IF NOT EXISTS proc (db char(64) collate utf8_bin DEFAULT '' NOT NULL, name char(64) DEFAULT '' NOT NULL, type enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY') NOT NULL, specific_name char(64) DEFAULT '' NOT NULL, language enum('SQL') DEFAULT 'SQL' NOT NULL, sql_data_access enum( 'CONTAINS_SQL', 'NO_SQL', 'READS_SQL_DATA', 'MODIFIES_SQL_DATA') DEFAULT 'CONTAINS_SQL' NOT NULL, is_deterministic enum('YES','NO') DEFAULT 'NO' NOT NULL, security_type enum('INVOKER','DEFINER') DEFAULT 'DEFINER' NOT NULL, param_list blob NOT NULL, returns longblob NOT NULL, body longblob NOT NULL, definer char(141) collate utf8_bin DEFAULT '' NOT NULL, created timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, modified timestamp NOT NULL DEFAULT '0000-00-00 00:00:00', sql_mode set( 'REAL_AS_FLOAT', 'PIPES_AS_CONCAT', 'ANSI_QUOTES', 'IGNORE_SPACE', 'IGNORE_BAD_TABLE_OPTIONS', 'ONLY_FULL_GROUP_BY', 'NO_UNSIGNED_SUBTRACTION', 'NO_DIR_IN_CREATE', 'POSTGRESQL', 'ORACLE', 'MSSQL', 'DB2', 'MAXDB', 'NO_KEY_OPTIONS', 'NO_TABLE_OPTIONS', 'NO_FIELD_OPTIONS', 'MYSQL323', 'MYSQL40', 'ANSI', 'NO_AUTO_VALUE_ON_ZERO', 'NO_BACKSLASH_ESCAPES', 'STRICT_TRANS_TABLES', 'STRICT_ALL_TABLES', 'NO_ZERO_IN_DATE', 'NO_ZERO_DATE', 'INVALID_DATES', 'ERROR_FOR_DIVISION_BY_ZERO', 'TRADITIONAL', 'NO_AUTO_CREATE_USER', 'HIGH_NOT_PRECEDENCE', 'NO_ENGINE_SUBSTITUTION', 'PAD_CHAR_TO_FULL_LENGTH', 'EMPTY_STRING_IS_NULL', 'SIMULTANEOUS_ASSIGNMENT', 'TIME_ROUND_FRACTIONAL') DEFAULT '' NOT NULL, comment text collate utf8_bin NOT NULL, character_set_client char(32) collate utf8_bin, collation_connection char(32) collate utf8_bin, db_collation char(32) collate utf8_bin, body_utf8 longblob, aggregate enum('NONE', 'GROUP') DEFAULT 'NONE' NOT NULL, PRIMARY KEY (db,name,type)) engine=Aria transactional=1 character set utf8 comment='Stored Procedures';

CREATE TABLE IF NOT EXISTS procs_priv ( Host char(60) binary DEFAULT '' NOT NULL, Db char(64) binary DEFAULT '' NOT NULL, User char(80) binary DEFAULT '' NOT NULL, Routine_name char(64) COLLATE utf8_general_ci DEFAULT '' NOT NULL, Routine_type enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY') NOT NULL, Grantor char(141) DEFAULT '' NOT NULL, Proc_priv set('Execute','Alter Routine','Grant') COLLATE utf8_general_ci DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, PRIMARY KEY (Host,Db,User,Routine_name,Routine_type), KEY Grantor (Grantor) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='Procedure privileges';


-- Create general_log if CSV is enabled.
SET @have_csv = (SELECT support FROM information_schema.engines WHERE engine = 'CSV');
SET @str = IF (@have_csv = 'YES', 'CREATE TABLE IF NOT EXISTS general_log (event_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, user_host MEDIUMTEXT NOT NULL, thread_id BIGINT(21) UNSIGNED NOT NULL, server_id INTEGER UNSIGNED NOT NULL, command_type VARCHAR(64) NOT NULL, argument MEDIUMTEXT NOT NULL) engine=CSV CHARACTER SET utf8 comment="General log"', 'SET @dummy = 0');

PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- Create slow_log if CSV is enabled.

SET @str = IF (@have_csv = 'YES', 'CREATE TABLE IF NOT EXISTS slow_log (start_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, user_host MEDIUMTEXT NOT NULL, query_time TIME(6) NOT NULL, lock_time TIME(6) NOT NULL, rows_sent BIGINT UNSIGNED NOT NULL, rows_examined BIGINT UNSIGNED NOT NULL, db VARCHAR(512) NOT NULL, last_insert_id INTEGER NOT NULL, insert_id INTEGER NOT NULL, server_id INTEGER UNSIGNED NOT NULL, sql_text MEDIUMTEXT NOT NULL, thread_id BIGINT(21) UNSIGNED NOT NULL, rows_affected BIGINT UNSIGNED NOT NULL) engine=CSV CHARACTER SET utf8 comment="Slow log"', 'SET @dummy = 0');

PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

CREATE TABLE IF NOT EXISTS event ( db char(64) CHARACTER SET utf8 COLLATE utf8_bin NOT NULL default '', name char(64) CHARACTER SET utf8 NOT NULL default '', body longblob NOT NULL, definer char(141) CHARACTER SET utf8 COLLATE utf8_bin NOT NULL default '', execute_at DATETIME default NULL, interval_value int(11) default NULL, interval_field ENUM('YEAR','QUARTER','MONTH','DAY','HOUR','MINUTE','WEEK','SECOND','MICROSECOND','YEAR_MONTH','DAY_HOUR','DAY_MINUTE','DAY_SECOND','HOUR_MINUTE','HOUR_SECOND','MINUTE_SECOND','DAY_MICROSECOND','HOUR_MICROSECOND','MINUTE_MICROSECOND','SECOND_MICROSECOND') default NULL, created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, modified TIMESTAMP NOT NULL DEFAULT '0000-00-00 00:00:00', last_executed DATETIME default NULL, starts DATETIME default NULL, ends DATETIME default NULL, status ENUM('ENABLED','DISABLED','SLAVESIDE_DISABLED') NOT NULL default 'ENABLED', on_completion ENUM('DROP','PRESERVE') NOT NULL default 'DROP', sql_mode  set('REAL_AS_FLOAT','PIPES_AS_CONCAT','ANSI_QUOTES','IGNORE_SPACE','IGNORE_BAD_TABLE_OPTIONS','ONLY_FULL_GROUP_BY','NO_UNSIGNED_SUBTRACTION','NO_DIR_IN_CREATE','POSTGRESQL','ORACLE','MSSQL','DB2','MAXDB','NO_KEY_OPTIONS','NO_TABLE_OPTIONS','NO_FIELD_OPTIONS','MYSQL323','MYSQL40','ANSI','NO_AUTO_VALUE_ON_ZERO','NO_BACKSLASH_ESCAPES','STRICT_TRANS_TABLES','STRICT_ALL_TABLES','NO_ZERO_IN_DATE','NO_ZERO_DATE','INVALID_DATES','ERROR_FOR_DIVISION_BY_ZERO','TRADITIONAL','NO_AUTO_CREATE_USER','HIGH_NOT_PRECEDENCE','NO_ENGINE_SUBSTITUTION','PAD_CHAR_TO_FULL_LENGTH','EMPTY_STRING_IS_NULL','SIMULTANEOUS_ASSIGNMENT','TIME_ROUND_FRACTIONAL') DEFAULT '' NOT NULL, comment char(64) CHARACTER SET utf8 COLLATE utf8_bin NOT NULL default '', originator INTEGER UNSIGNED NOT NULL, time_zone char(64) CHARACTER SET latin1 NOT NULL DEFAULT 'SYSTEM', character_set_client char(32) collate utf8_bin, collation_connection char(32) collate utf8_bin, db_collation char(32) collate utf8_bin, body_utf8 longblob, PRIMARY KEY (db, name) ) engine=Aria transactional=1 DEFAULT CHARSET=utf8 COMMENT 'Events';

SET @create_innodb_table_stats="CREATE TABLE IF NOT EXISTS innodb_table_stats (
	database_name			VARCHAR(64) NOT NULL,
	table_name			VARCHAR(199) NOT NULL,
	last_update			TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	n_rows				BIGINT UNSIGNED NOT NULL,
	clustered_index_size		BIGINT UNSIGNED NOT NULL,
	sum_of_other_index_sizes	BIGINT UNSIGNED NOT NULL,
	PRIMARY KEY (database_name, table_name)
) ENGINE=INNODB DEFAULT CHARSET=utf8 COLLATE=utf8_bin STATS_PERSISTENT=0";

SET @create_innodb_index_stats="CREATE TABLE IF NOT EXISTS innodb_index_stats (
	database_name			VARCHAR(64) NOT NULL,
	table_name			VARCHAR(199) NOT NULL,
	index_name			VARCHAR(64) NOT NULL,
	last_update			TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	/* there are at least:
	stat_name='size'
	stat_name='n_leaf_pages'
	stat_name='n_diff_pfx%' */
	stat_name			VARCHAR(64) NOT NULL,
	stat_value			BIGINT UNSIGNED NOT NULL,
	sample_size			BIGINT UNSIGNED,
	stat_description		VARCHAR(1024) NOT NULL,
	PRIMARY KEY (database_name, table_name, index_name, stat_name)
) ENGINE=INNODB DEFAULT CHARSET=utf8 COLLATE=utf8_bin STATS_PERSISTENT=0";

SET @create_transaction_registry="CREATE TABLE IF NOT EXISTS transaction_registry (
	transaction_id			BIGINT UNSIGNED NOT NULL,
	commit_id			BIGINT UNSIGNED NOT NULL,
	begin_timestamp			TIMESTAMP(6) NOT NULL DEFAULT '0000-00-00 00:00:00.000000',
	commit_timestamp		TIMESTAMP(6) NOT NULL DEFAULT '0000-00-00 00:00:00.000000',
	isolation_level			ENUM('READ-UNCOMMITTED', 'READ-COMMITTED',
					'REPEATABLE-READ', 'SERIALIZABLE') NOT NULL,
	PRIMARY KEY (transaction_id),
	UNIQUE KEY (commit_id),
	INDEX (begin_timestamp),
	INDEX (commit_timestamp, transaction_id)
) ENGINE=INNODB DEFAULT CHARSET=utf8 COLLATE=utf8_bin STATS_PERSISTENT=0";

SET @str=IF(@have_innodb <> 0, @create_innodb_table_stats, "SET @dummy = 0");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @str=IF(@have_innodb <> 0, @create_innodb_index_stats, "SET @dummy = 0");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @str=IF(@have_innodb <> 0, @create_transaction_registry, "SET @dummy = 0");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd="CREATE TABLE IF NOT EXISTS slave_relay_log_info (
  Number_of_lines INTEGER UNSIGNED NOT NULL COMMENT 'Number of lines in the file or rows in the table. Used to version table definitions.', 
  Relay_log_name TEXT CHARACTER SET utf8 COLLATE utf8_bin NOT NULL COMMENT 'The name of the current relay log file.', 
  Relay_log_pos BIGINT UNSIGNED NOT NULL COMMENT 'The relay log position of the last executed event.', 
  Master_log_name TEXT CHARACTER SET utf8 COLLATE utf8_bin NOT NULL COMMENT 'The name of the master binary log file from which the events in the relay log file were read.', 
  Master_log_pos BIGINT UNSIGNED NOT NULL COMMENT 'The master log position of the last executed event.', 
  Sql_delay INTEGER NOT NULL COMMENT 'The number of seconds that the slave must lag behind the master.', 
  Number_of_workers INTEGER UNSIGNED NOT NULL,
  Id INTEGER UNSIGNED NOT NULL COMMENT 'Internal Id that uniquely identifies this record.',  
  PRIMARY KEY(Id)) DEFAULT CHARSET=utf8 STATS_PERSISTENT=0 COMMENT 'Relay Log Information'";

SET @str=CONCAT(@cmd, ' ENGINE=', @innodb_or_aria);
-- Don't create the table; MariaDB will have another implementation
#PREPARE stmt FROM @str;
#EXECUTE stmt;
#DROP PREPARE stmt;

SET @cmd= "CREATE TABLE IF NOT EXISTS slave_master_info (
  Number_of_lines INTEGER UNSIGNED NOT NULL COMMENT 'Number of lines in the file.', 
  Master_log_name TEXT CHARACTER SET utf8 COLLATE utf8_bin NOT NULL COMMENT 'The name of the master binary log currently being read from the master.', 
  Master_log_pos BIGINT UNSIGNED NOT NULL COMMENT 'The master log position of the last read event.', 
  Host CHAR(64) CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The host name of the master.', 
  User_name TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The user name used to connect to the master.', 
  User_password TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The password used to connect to the master.', 
  Port INTEGER UNSIGNED NOT NULL COMMENT 'The network port used to connect to the master.', 
  Connect_retry INTEGER UNSIGNED NOT NULL COMMENT 'The period (in seconds) that the slave will wait before trying to reconnect to the master.', 
  Enabled_ssl BOOLEAN NOT NULL COMMENT 'Indicates whether the server supports SSL connections.', 
  Ssl_ca TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The file used for the Certificate Authority (CA) certificate.', 
  Ssl_capath TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The path to the Certificate Authority (CA) certificates.', 
  Ssl_cert TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The name of the SSL certificate file.', 
  Ssl_cipher TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The name of the cipher in use for the SSL connection.', 
  Ssl_key TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The name of the SSL key file.', 
  Ssl_verify_server_cert BOOLEAN NOT NULL COMMENT 'Whether to verify the server certificate.', 
  Heartbeat FLOAT NOT NULL COMMENT '', 
  Bind TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'Displays which interface is employed when connecting to the MySQL server', 
  Ignored_server_ids TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The number of server IDs to be ignored, followed by the actual server IDs', 
  Uuid TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The master server uuid.', 
  Retry_count BIGINT UNSIGNED NOT NULL COMMENT 'Number of reconnect attempts, to the master, before giving up.', 
  Ssl_crl TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The file used for the Certificate Revocation List (CRL)', 
  Ssl_crlpath TEXT CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The path used for Certificate Revocation List (CRL) files', 
  Enabled_auto_position BOOLEAN NOT NULL COMMENT 'Indicates whether GTIDs will be used to retrieve events from the master.', 
  PRIMARY KEY(Host, Port)) DEFAULT CHARSET=utf8 STATS_PERSISTENT=0 COMMENT 'Master Information'";

SET @str=CONCAT(@cmd, ' ENGINE=', @innodb_or_aria);
-- Don't create the table; MariaDB will have another implementation
#PREPARE stmt FROM @str;
#EXECUTE stmt;
#DROP PREPARE stmt;

SET @cmd= "CREATE TABLE IF NOT EXISTS slave_worker_info (
  Id INTEGER UNSIGNED NOT NULL, 
  Relay_log_name TEXT CHARACTER SET utf8 COLLATE utf8_bin NOT NULL, 
  Relay_log_pos BIGINT UNSIGNED NOT NULL, 
  Master_log_name TEXT CHARACTER SET utf8 COLLATE utf8_bin NOT NULL, 
  Master_log_pos BIGINT UNSIGNED NOT NULL, 
  Checkpoint_relay_log_name TEXT CHARACTER SET utf8 COLLATE utf8_bin NOT NULL, 
  Checkpoint_relay_log_pos BIGINT UNSIGNED NOT NULL, 
  Checkpoint_master_log_name TEXT CHARACTER SET utf8 COLLATE utf8_bin NOT NULL, 
  Checkpoint_master_log_pos BIGINT UNSIGNED NOT NULL, 
  Checkpoint_seqno INT UNSIGNED NOT NULL, 
  Checkpoint_group_size INTEGER UNSIGNED NOT NULL, 
  Checkpoint_group_bitmap BLOB NOT NULL, 
  PRIMARY KEY(Id)) DEFAULT CHARSET=utf8 STATS_PERSISTENT=0 COMMENT 'Worker Information'";

SET @str=CONCAT(@cmd, ' ENGINE=', @innodb_or_aria);
-- Don't create the table; MariaDB will have another implementation
#PREPARE stmt FROM @str;
#EXECUTE stmt;
#DROP PREPARE stmt;

CREATE TABLE IF NOT EXISTS proxies_priv (Host char(60) binary DEFAULT '' NOT NULL, User char(80) binary DEFAULT '' NOT NULL, Proxied_host char(60) binary DEFAULT '' NOT NULL, Proxied_user char(80) binary DEFAULT '' NOT NULL, With_grant BOOL DEFAULT 0 NOT NULL, Grantor char(141) DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, PRIMARY KEY /*Host*/ (Host,User,Proxied_host,Proxied_user), KEY Grantor (Grantor) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='User proxy privileges';

-- Remember for later if proxies_priv table already existed
set @had_proxies_priv_table= @@warning_count != 0;

-- The following needs to be done both for new installations
-- and for upgrades
CREATE TEMPORARY TABLE tmp_proxies_priv LIKE proxies_priv;
INSERT INTO tmp_proxies_priv VALUES ('localhost', 'root', '', '', TRUE, '', now());
REPLACE INTO tmp_proxies_priv SELECT 'localhost',IFNULL(@auth_root_socket, 'root'), '', '', TRUE, '', now() FROM DUAL;
INSERT INTO proxies_priv SELECT * FROM tmp_proxies_priv WHERE @had_proxies_priv_table=0;
DROP TABLE tmp_proxies_priv;

--
-- Tables unique for MariaDB
--

CREATE TABLE IF NOT EXISTS table_stats (db_name varchar(64) NOT NULL, table_name varchar(64) NOT NULL, cardinality bigint(21) unsigned DEFAULT NULL, PRIMARY KEY (db_name,table_name) ) engine=Aria transactional=0 CHARACTER SET utf8 COLLATE utf8_bin comment='Statistics on Tables';

CREATE TABLE IF NOT EXISTS column_stats (db_name varchar(64) NOT NULL, table_name varchar(64) NOT NULL, column_name varchar(64) NOT NULL, min_value varbinary(255) DEFAULT NULL, max_value varbinary(255) DEFAULT NULL, nulls_ratio decimal(12,4) DEFAULT NULL, avg_length decimal(12,4) DEFAULT NULL, avg_frequency decimal(12,4) DEFAULT NULL, hist_size tinyint unsigned, hist_type enum('SINGLE_PREC_HB','DOUBLE_PREC_HB'), histogram varbinary(255), PRIMARY KEY (db_name,table_name,column_name) ) engine=Aria transactional=0 CHARACTER SET utf8 COLLATE utf8_bin comment='Statistics on Columns';

CREATE TABLE IF NOT EXISTS index_stats (db_name varchar(64) NOT NULL, table_name varchar(64) NOT NULL, index_name varchar(64) NOT NULL, prefix_arity int(11) unsigned NOT NULL, avg_frequency decimal(12,4) DEFAULT NULL, PRIMARY KEY (db_name,table_name,index_name,prefix_arity) ) engine=Aria transactional=0 CHARACTER SET utf8 COLLATE utf8_bin comment='Statistics on Indexes';

-- Note: This definition must be kept in sync with the one used in
-- build_gtid_pos_create_query() in sql/slave.cc
SET @cmd= "CREATE TABLE IF NOT EXISTS gtid_slave_pos (
  domain_id INT UNSIGNED NOT NULL,
  sub_id BIGINT UNSIGNED NOT NULL,
  server_id INT UNSIGNED NOT NULL,
  seq_no BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (domain_id, sub_id)) CHARSET=latin1
COMMENT='Replication slave GTID position'";
SET @str=CONCAT(@cmd, ' ENGINE=', @innodb_or_aria);
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

set default_storage_engine=@orig_storage_engine;

--
-- Drop some tables not used anymore in MariaDB
--

drop table if exists mysql.ndb_binlog_index;
drop table if exists mysql.host;
--
-- PERFORMANCE SCHEMA INSTALLATION
-- Note that this script is also reused by mysql_upgrade,
-- so we have to be very careful here to not destroy any
-- existing database named 'performance_schema' if it
-- can contain user data.
-- In case of downgrade, it's ok to drop unknown tables
-- from a future version, as long as they belong to the
-- performance schema engine.
--

set @have_old_pfs= (select count(*) from information_schema.schemata where schema_name='performance_schema');

SET @cmd="SET @broken_tables = (select count(*) from information_schema.tables  where engine != 'PERFORMANCE_SCHEMA' and table_schema='performance_schema')";

-- Work around for bug#49542
SET @str = IF(@have_old_pfs = 1, @cmd, 'SET @broken_tables = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd="SET @broken_views = (select count(*) from information_schema.views where table_schema='performance_schema')";

-- Work around for bug#49542
SET @str = IF(@have_old_pfs = 1, @cmd, 'SET @broken_views = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @broken_routines = (select count(*) from mysql.proc where db='performance_schema');

SET @broken_events = (select count(*) from mysql.event where db='performance_schema');

SET @broken_pfs= (select @broken_tables + @broken_views + @broken_routines + @broken_events);

--
-- The performance schema database.
-- Only drop and create the database if this is safe (no broken_pfs).
-- This database is created, even in --without-perfschema builds,
-- so that the database name is always reserved by the MySQL implementation.
--

SET @cmd= "DROP DATABASE IF EXISTS performance_schema";

SET @str = IF(@broken_pfs = 0, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd= "CREATE DATABASE performance_schema character set utf8";

SET @str = IF(@broken_pfs = 0, @cmd, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;
