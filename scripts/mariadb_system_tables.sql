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

CREATE TABLE IF NOT EXISTS db (   Host char(255) binary DEFAULT '' NOT NULL, Db char(64) binary DEFAULT '' NOT NULL, User char(128) binary DEFAULT '' NOT NULL, Select_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Insert_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Update_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Delete_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Create_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Drop_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Grant_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, References_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Index_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Alter_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Create_tmp_table_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Lock_tables_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Create_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Show_view_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Create_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Alter_routine_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Execute_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Event_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Trigger_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, Delete_history_priv enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, PRIMARY KEY /*Host */(Host,Db,User), KEY User (User) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='Database privileges';

-- Remember for later if db table already existed
set @had_db_table= @@warning_count != 0;

CREATE TABLE IF NOT EXISTS global_priv (Host char(255) binary DEFAULT '', User char(128) binary DEFAULT '', Priv JSON NOT NULL DEFAULT '{}' CHECK(JSON_VALID(Priv)), PRIMARY KEY (Host,User)) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='Users and global privileges';

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

CREATE TABLE IF NOT EXISTS roles_mapping ( Host char(255) binary DEFAULT '' NOT NULL, User char(128) binary DEFAULT '' NOT NULL, Role char(128) binary DEFAULT '' NOT NULL, Admin_option enum('N','Y') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, UNIQUE (Host, User, Role)) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='Granted roles';

CREATE TABLE IF NOT EXISTS func (  name char(64) binary DEFAULT '' NOT NULL, ret tinyint(1) DEFAULT '0' NOT NULL, dl char(128) DEFAULT '' NOT NULL, type enum ('function','aggregate') COLLATE utf8_general_ci NOT NULL, PRIMARY KEY (name) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='User defined functions';


CREATE TABLE IF NOT EXISTS plugin ( name varchar(64) DEFAULT '' NOT NULL, dl varchar(128) DEFAULT '' NOT NULL, PRIMARY KEY (name) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_general_ci comment='MySQL plugins';


CREATE TABLE IF NOT EXISTS servers ( Server_name char(64) NOT NULL DEFAULT '', Host varchar(2048) NOT NULL DEFAULT '', Db char(64) NOT NULL DEFAULT '', Username char(128) NOT NULL DEFAULT '', Password char(64) NOT NULL DEFAULT '', Port INT(4) NOT NULL DEFAULT '0', Socket char(64) NOT NULL DEFAULT '', Wrapper char(64) NOT NULL DEFAULT '', Owner varchar(512) NOT NULL DEFAULT '', PRIMARY KEY (Server_name)) engine=Aria transactional=1 CHARACTER SET utf8 comment='MySQL Foreign Servers table';


CREATE TABLE IF NOT EXISTS tables_priv ( Host char(255) binary DEFAULT '' NOT NULL, Db char(64) binary DEFAULT '' NOT NULL, User char(128) binary DEFAULT '' NOT NULL, Table_name char(64) binary DEFAULT '' NOT NULL, Grantor varchar(384) DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, Table_priv set('Select','Insert','Update','Delete','Create','Drop','Grant','References','Index','Alter','Create View','Show view','Trigger','Delete versioning rows') COLLATE utf8_general_ci DEFAULT '' NOT NULL, Column_priv set('Select','Insert','Update','References') COLLATE utf8_general_ci DEFAULT '' NOT NULL, PRIMARY KEY (Host,Db,User,Table_name), KEY Grantor (Grantor) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='Table privileges';

CREATE TEMPORARY TABLE tmp_user_sys LIKE tables_priv;
INSERT INTO tmp_user_sys (Host,Db,User,Table_name,Grantor,Timestamp,Table_priv) VALUES ('localhost','mysql','mariadb.sys','global_priv','root@localhost','0','Select,Delete');
INSERT IGNORE INTO tables_priv SELECT * FROM tmp_user_sys WHERE 0 <> @need_sys_user_creation;
DROP TABLE tmp_user_sys;

CREATE TABLE IF NOT EXISTS columns_priv ( Host char(255) binary DEFAULT '' NOT NULL, Db char(64) binary DEFAULT '' NOT NULL, User char(128) binary DEFAULT '' NOT NULL, Table_name char(64) binary DEFAULT '' NOT NULL, Column_name char(64) binary DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, Column_priv set('Select','Insert','Update','References') COLLATE utf8_general_ci DEFAULT '' NOT NULL, PRIMARY KEY (Host,Db,User,Table_name,Column_name) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='Column privileges';


CREATE TABLE IF NOT EXISTS help_topic ( help_topic_id int unsigned not null, name char(64) not null, help_category_id smallint unsigned not null, description text not null, example text not null, url text not null, primary key (help_topic_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help topics';


CREATE TABLE IF NOT EXISTS help_category ( help_category_id smallint unsigned not null, name  char(64) not null, parent_category_id smallint unsigned null, url text not null, primary key (help_category_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help categories';


CREATE TABLE IF NOT EXISTS help_relation ( help_topic_id int unsigned not null references help_topic, help_keyword_id  int unsigned not null references help_keyword, primary key (help_keyword_id, help_topic_id) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='keyword-topic relation';


CREATE TABLE IF NOT EXISTS help_keyword (   help_keyword_id  int unsigned not null, name char(64) not null, primary key (help_keyword_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help keywords';


CREATE TABLE IF NOT EXISTS time_zone_name (   Name char(64) NOT NULL, Time_zone_id int unsigned NOT NULL, PRIMARY KEY /*Name*/ (Name) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone names';


CREATE TABLE IF NOT EXISTS time_zone (   Time_zone_id int unsigned NOT NULL auto_increment, Use_leap_seconds enum('Y','N') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, PRIMARY KEY /*TzId*/ (Time_zone_id) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zones';


CREATE TABLE IF NOT EXISTS time_zone_transition (   Time_zone_id int unsigned NOT NULL, Transition_time bigint signed NOT NULL, Transition_type_id int unsigned NOT NULL, PRIMARY KEY /*TzIdTranTime*/ (Time_zone_id, Transition_time) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone transitions';


CREATE TABLE IF NOT EXISTS time_zone_transition_type (   Time_zone_id int unsigned NOT NULL, Transition_type_id int unsigned NOT NULL, `Offset` int signed DEFAULT 0 NOT NULL, Is_DST tinyint unsigned DEFAULT 0 NOT NULL, Abbreviation char(8) DEFAULT '' NOT NULL, PRIMARY KEY /*TzIdTrTId*/ (Time_zone_id, Transition_type_id) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone transition types';


CREATE TABLE IF NOT EXISTS time_zone_leap_second (   Transition_time bigint signed NOT NULL, Correction int signed NOT NULL, PRIMARY KEY /*TranTime*/ (Transition_time) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Leap seconds information for time zones';

CREATE TABLE IF NOT EXISTS proc (db char(64) collate utf8_bin DEFAULT '' NOT NULL, name char(64) DEFAULT '' NOT NULL, type enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY') NOT NULL, specific_name char(64) DEFAULT '' NOT NULL, language enum('SQL') DEFAULT 'SQL' NOT NULL, sql_data_access enum( 'CONTAINS_SQL', 'NO_SQL', 'READS_SQL_DATA', 'MODIFIES_SQL_DATA') DEFAULT 'CONTAINS_SQL' NOT NULL, is_deterministic enum('YES','NO') DEFAULT 'NO' NOT NULL, security_type enum('INVOKER','DEFINER') DEFAULT 'DEFINER' NOT NULL, param_list blob NOT NULL, returns longblob NOT NULL, body longblob NOT NULL, definer varchar(384) collate utf8_bin DEFAULT '' NOT NULL, created timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, modified timestamp NOT NULL DEFAULT '0000-00-00 00:00:00', sql_mode set( 'REAL_AS_FLOAT', 'PIPES_AS_CONCAT', 'ANSI_QUOTES', 'IGNORE_SPACE', 'IGNORE_BAD_TABLE_OPTIONS', 'ONLY_FULL_GROUP_BY', 'NO_UNSIGNED_SUBTRACTION', 'NO_DIR_IN_CREATE', 'POSTGRESQL', 'ORACLE', 'MSSQL', 'DB2', 'MAXDB', 'NO_KEY_OPTIONS', 'NO_TABLE_OPTIONS', 'NO_FIELD_OPTIONS', 'MYSQL323', 'MYSQL40', 'ANSI', 'NO_AUTO_VALUE_ON_ZERO', 'NO_BACKSLASH_ESCAPES', 'STRICT_TRANS_TABLES', 'STRICT_ALL_TABLES', 'NO_ZERO_IN_DATE', 'NO_ZERO_DATE', 'INVALID_DATES', 'ERROR_FOR_DIVISION_BY_ZERO', 'TRADITIONAL', 'NO_AUTO_CREATE_USER', 'HIGH_NOT_PRECEDENCE', 'NO_ENGINE_SUBSTITUTION', 'PAD_CHAR_TO_FULL_LENGTH', 'EMPTY_STRING_IS_NULL', 'SIMULTANEOUS_ASSIGNMENT', 'TIME_ROUND_FRACTIONAL') DEFAULT '' NOT NULL, comment text collate utf8_bin NOT NULL, character_set_client char(32) collate utf8_bin, collation_connection char(64) collate utf8_bin, db_collation char(64) collate utf8_bin, body_utf8 longblob, aggregate enum('NONE', 'GROUP') DEFAULT 'NONE' NOT NULL, PRIMARY KEY (db,name,type)) engine=Aria transactional=1 character set utf8 comment='Stored Procedures';

CREATE TABLE IF NOT EXISTS procs_priv ( Host char(255) binary DEFAULT '' NOT NULL, Db char(64) binary DEFAULT '' NOT NULL, User char(128) binary DEFAULT '' NOT NULL, Routine_name char(64) COLLATE utf8_general_ci DEFAULT '' NOT NULL, Routine_type enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY') NOT NULL, Grantor varchar(384) DEFAULT '' NOT NULL, Proc_priv set('Execute','Alter Routine','Grant') COLLATE utf8_general_ci DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, PRIMARY KEY (Host,Db,User,Routine_name,Routine_type), KEY Grantor (Grantor) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='Procedure privileges';


-- Create general_log if CSV is enabled.
SET @have_csv = (SELECT support FROM information_schema.engines WHERE engine = 'CSV');
SET @str = IF (@have_csv = 'YES', 'CREATE TABLE IF NOT EXISTS general_log (event_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, user_host MEDIUMTEXT NOT NULL, thread_id BIGINT(21) UNSIGNED NOT NULL, server_id INTEGER UNSIGNED NOT NULL, command_type VARCHAR(64) NOT NULL, argument MEDIUMTEXT NOT NULL) engine=CSV CHARACTER SET utf8 comment="General log"', 'SET @dummy = 0');

PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- Create slow_log if CSV is enabled.

SET @str = IF (@have_csv = 'YES', 'CREATE TABLE IF NOT EXISTS slow_log (start_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, user_host MEDIUMTEXT NOT NULL, query_time TIME(6) NOT NULL, lock_time TIME(6) NOT NULL, rows_sent INTEGER NOT NULL, rows_examined INTEGER NOT NULL, db VARCHAR(512) NOT NULL, last_insert_id INTEGER NOT NULL, insert_id INTEGER NOT NULL, server_id INTEGER UNSIGNED NOT NULL, sql_text MEDIUMTEXT NOT NULL, thread_id BIGINT(21) UNSIGNED NOT NULL, rows_affected INTEGER NOT NULL) engine=CSV CHARACTER SET utf8 comment="Slow log"', 'SET @dummy = 0');

PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

CREATE TABLE IF NOT EXISTS event ( db char(64) CHARACTER SET utf8 COLLATE utf8_bin NOT NULL default '', name char(64) CHARACTER SET utf8 NOT NULL default '', body longblob NOT NULL, definer varchar(384) CHARACTER SET utf8 COLLATE utf8_bin NOT NULL default '', execute_at DATETIME default NULL, interval_value int(11) default NULL, interval_field ENUM('YEAR','QUARTER','MONTH','DAY','HOUR','MINUTE','WEEK','SECOND','MICROSECOND','YEAR_MONTH','DAY_HOUR','DAY_MINUTE','DAY_SECOND','HOUR_MINUTE','HOUR_SECOND','MINUTE_SECOND','DAY_MICROSECOND','HOUR_MICROSECOND','MINUTE_MICROSECOND','SECOND_MICROSECOND') default NULL, created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, modified TIMESTAMP NOT NULL DEFAULT '0000-00-00 00:00:00', last_executed DATETIME default NULL, starts DATETIME default NULL, ends DATETIME default NULL, status ENUM('ENABLED','DISABLED','SLAVESIDE_DISABLED') NOT NULL default 'ENABLED', on_completion ENUM('DROP','PRESERVE') NOT NULL default 'DROP', sql_mode  set('REAL_AS_FLOAT','PIPES_AS_CONCAT','ANSI_QUOTES','IGNORE_SPACE','IGNORE_BAD_TABLE_OPTIONS','ONLY_FULL_GROUP_BY','NO_UNSIGNED_SUBTRACTION','NO_DIR_IN_CREATE','POSTGRESQL','ORACLE','MSSQL','DB2','MAXDB','NO_KEY_OPTIONS','NO_TABLE_OPTIONS','NO_FIELD_OPTIONS','MYSQL323','MYSQL40','ANSI','NO_AUTO_VALUE_ON_ZERO','NO_BACKSLASH_ESCAPES','STRICT_TRANS_TABLES','STRICT_ALL_TABLES','NO_ZERO_IN_DATE','NO_ZERO_DATE','INVALID_DATES','ERROR_FOR_DIVISION_BY_ZERO','TRADITIONAL','NO_AUTO_CREATE_USER','HIGH_NOT_PRECEDENCE','NO_ENGINE_SUBSTITUTION','PAD_CHAR_TO_FULL_LENGTH','EMPTY_STRING_IS_NULL','SIMULTANEOUS_ASSIGNMENT','TIME_ROUND_FRACTIONAL') DEFAULT '' NOT NULL, comment char(64) CHARACTER SET utf8 COLLATE utf8_bin NOT NULL default '', originator INTEGER UNSIGNED NOT NULL, time_zone char(64) CHARACTER SET latin1 NOT NULL DEFAULT 'SYSTEM', character_set_client char(32) collate utf8_bin, collation_connection char(64) collate utf8_bin, db_collation char(64) collate utf8_bin, body_utf8 longblob, PRIMARY KEY (db, name) ) engine=Aria transactional=1 DEFAULT CHARSET=utf8 COMMENT 'Events';

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
  Host CHAR(255) CHARACTER SET utf8 COLLATE utf8_bin COMMENT 'The host name of the master.',
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

CREATE TABLE IF NOT EXISTS proxies_priv (Host char(255) binary DEFAULT '' NOT NULL, User char(128) binary DEFAULT '' NOT NULL, Proxied_host char(255) binary DEFAULT '' NOT NULL, Proxied_user char(128) binary DEFAULT '' NOT NULL, With_grant BOOL DEFAULT 0 NOT NULL, Grantor varchar(384) DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, PRIMARY KEY /*Host*/ (Host,User,Proxied_host,Proxied_user), KEY Grantor (Grantor) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin comment='User proxy privileges';

-- Remember for later if proxies_priv table already existed
set @had_proxies_priv_table= @@warning_count != 0;

-- The following needs to be done both for new installations
-- and for upgrades
CREATE TEMPORARY TABLE tmp_proxies_priv LIKE proxies_priv;
INSERT INTO tmp_proxies_priv VALUES ('localhost', 'root', '', '', TRUE, '', now());
INSERT INTO proxies_priv SELECT * FROM tmp_proxies_priv WHERE @had_proxies_priv_table=0;
DROP TABLE tmp_proxies_priv;

--
-- Tables unique for MariaDB
--

CREATE TABLE IF NOT EXISTS table_stats (db_name varchar(64) NOT NULL, table_name varchar(64) NOT NULL, cardinality bigint(21) unsigned DEFAULT NULL, PRIMARY KEY (db_name,table_name) ) engine=Aria transactional=0 CHARACTER SET utf8 COLLATE utf8_bin comment='Statistics on Tables';

CREATE TABLE IF NOT EXISTS column_stats (db_name varchar(64) NOT NULL, table_name varchar(64) NOT NULL, column_name varchar(64) NOT NULL, min_value varbinary(255) DEFAULT NULL, max_value varbinary(255) DEFAULT NULL, nulls_ratio decimal(12,4) DEFAULT NULL, avg_length decimal(12,4) DEFAULT NULL, avg_frequency decimal(12,4) DEFAULT NULL, hist_size tinyint unsigned, hist_type enum('SINGLE_PREC_HB','DOUBLE_PREC_HB','JSON_HB'), histogram longblob, PRIMARY KEY (db_name,table_name,column_name) ) engine=Aria transactional=0 CHARACTER SET utf8 COLLATE utf8_bin comment='Statistics on Columns';

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
