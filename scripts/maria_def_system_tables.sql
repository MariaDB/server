-- Copyright (c) 2023 MariaDB Foundation
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
-- The MariaDB server system tables for the 'def' catalog.
--

CREATE TABLE IF NOT EXISTS func (  name char(64) binary DEFAULT '' NOT NULL, ret tinyint(1) DEFAULT '0' NOT NULL, dl char(128) DEFAULT '' NOT NULL, type enum ('function','aggregate') COLLATE utf8_general_ci NOT NULL, PRIMARY KEY (name) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_bin   comment='User defined functions';

CREATE TABLE IF NOT EXISTS plugin ( name varchar(64) DEFAULT '' NOT NULL, dl varchar(128) DEFAULT '' NOT NULL, PRIMARY KEY (name) ) engine=Aria transactional=1 CHARACTER SET utf8 COLLATE utf8_general_ci comment='MariaDB plugins';

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

SET @str=IF(@have_innodb <> 0, @create_transaction_registry, "SET @dummy = 0");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

CREATE TABLE IF NOT EXISTS help_topic ( help_topic_id int unsigned not null, name char(64) not null, help_category_id smallint unsigned not null, description text not null, example text not null, url text not null, primary key (help_topic_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help topics';


CREATE TABLE IF NOT EXISTS help_category ( help_category_id smallint unsigned not null, name  char(64) not null, parent_category_id smallint unsigned null, url text not null, primary key (help_category_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help categories';


CREATE TABLE IF NOT EXISTS help_relation ( help_topic_id int unsigned not null references help_topic, help_keyword_id  int unsigned not null references help_keyword, primary key (help_keyword_id, help_topic_id) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='keyword-topic relation';


CREATE TABLE IF NOT EXISTS help_keyword (   help_keyword_id  int unsigned not null, name char(64) not null, primary key (help_keyword_id), unique index (name) ) engine=Aria transactional=0 CHARACTER SET utf8 comment='help keywords';


CREATE TABLE IF NOT EXISTS time_zone_name (   Name char(64) NOT NULL, Time_zone_id int unsigned NOT NULL, PRIMARY KEY /*Name*/ (Name) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone names';


CREATE TABLE IF NOT EXISTS time_zone (   Time_zone_id int unsigned NOT NULL auto_increment, Use_leap_seconds enum('Y','N') COLLATE utf8_general_ci DEFAULT 'N' NOT NULL, PRIMARY KEY /*TzId*/ (Time_zone_id) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zones';


CREATE TABLE IF NOT EXISTS time_zone_transition (   Time_zone_id int unsigned NOT NULL, Transition_time bigint signed NOT NULL, Transition_type_id int unsigned NOT NULL, PRIMARY KEY /*TzIdTranTime*/ (Time_zone_id, Transition_time) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone transitions';


CREATE TABLE IF NOT EXISTS time_zone_transition_type (   Time_zone_id int unsigned NOT NULL, Transition_type_id int unsigned NOT NULL, `Offset` int signed DEFAULT 0 NOT NULL, Is_DST tinyint unsigned DEFAULT 0 NOT NULL, Abbreviation char(8) DEFAULT '' NOT NULL, PRIMARY KEY /*TzIdTrTId*/ (Time_zone_id, Transition_type_id) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Time zone transition types';

CREATE TABLE IF NOT EXISTS time_zone_leap_second (   Transition_time bigint signed NOT NULL, Correction int signed NOT NULL, PRIMARY KEY /*TranTime*/ (Transition_time) ) engine=Aria transactional=1 CHARACTER SET utf8   comment='Leap seconds information for time zones';

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
