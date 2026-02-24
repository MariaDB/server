/* Copyright (C) 2010-2020 Kentoku Shiba
   Copyright (C) 2019-2020 MariaDB corp

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*
  This SQL script creates system tables for SPIDER
    or fixes incompatibilities if ones already exist.
*/

static LEX_STRING spider_init_queries[] = {
  /* Use the default SQL_MODE for this connection. */
  {C_STRING_WITH_LEN(
    "SET @@SQL_MODE = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,"
                      "NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION';"
  )},
  {C_STRING_WITH_LEN(
    "SET @@OLD_MODE = CONCAT(@@OLD_MODE, ',UTF8_IS_UTF8MB3');"
  )},
  {C_STRING_WITH_LEN(
    "SET tx_read_only = off;"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_xa("
    "  format_id int not null default 0,"
    "  gtrid_length int not null default 0,"
    "  bqual_length int not null default 0,"
    "  data char(128) charset binary not null default '',"
    "  status char(8) not null default '',"
    "  primary key (data, format_id, gtrid_length),"
    "  key idx1 (status)"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_xa_member("
    "  format_id int not null default 0,"
    "  gtrid_length int not null default 0,"
    "  bqual_length int not null default 0,"
    "  data char(128) charset binary not null default '',"
    "  scheme char(64) not null default '',"
    "  host char(64) not null default '',"
    "  port char(5) not null default '',"
    "  socket text not null,"
    "  username char(64) not null default '',"
    "  password char(64) not null default '',"
    "  ssl_ca text,"
    "  ssl_capath text,"
    "  ssl_cert text,"
    "  ssl_cipher char(64) default null,"
    "  ssl_key text,"
    "  ssl_verify_server_cert tinyint not null default 0,"
    "  default_file text,"
    "  default_group char(64) default null,"
    "  dsn char(64) default null,"
    "  filedsn text default null,"
    "  driver char(64) default null,"
    "  key idx1 (data, format_id, gtrid_length, host)"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_xa_failed_log("
    "  format_id int not null default 0,"
    "  gtrid_length int not null default 0,"
    "  bqual_length int not null default 0,"
    "  data char(128) charset binary not null default '',"
    "  scheme char(64) not null default '',"
    "  host char(64) not null default '',"
    "  port char(5) not null default '',"
    "  socket text not null,"
    "  username char(64) not null default '',"
    "  password char(64) not null default '',"
    "  ssl_ca text,"
    "  ssl_capath text,"
    "  ssl_cert text,"
    "  ssl_cipher char(64) default null,"
    "  ssl_key text,"
    "  ssl_verify_server_cert tinyint not null default 0,"
    "  default_file text,"
    "  default_group char(64) default null,"
    "  dsn char(64) default null,"
    "  filedsn text default null,"
    "  driver char(64) default null,"
    "  thread_id int default null,"
    "  status char(8) not null default '',"
    "  failed_time timestamp not null default current_timestamp,"
    "  key idx1 (data, format_id, gtrid_length, host)"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_tables("
    "  db_name char(64) not null default '',"
    "  table_name char(199) not null default '',"
    "  link_id int not null default 0,"
    "  priority bigint not null default 0,"
    "  server char(64) default null,"
    "  scheme char(64) default null,"
    "  host char(64) default null,"
    "  port char(5) default null,"
    "  socket text,"
    "  username char(64) default null,"
    "  password char(64) default null,"
    "  ssl_ca text,"
    "  ssl_capath text,"
    "  ssl_cert text,"
    "  ssl_cipher char(64) default null,"
    "  ssl_key text,"
    "  ssl_verify_server_cert tinyint not null default 0,"
    "  monitoring_binlog_pos_at_failing tinyint not null default 0,"
    "  default_file text,"
    "  default_group char(64) default null,"
    "  dsn char(64) default null,"
    "  filedsn text default null,"
    "  driver char(64) default null,"
    "  tgt_db_name char(64) default null,"
    "  tgt_table_name char(64) default null,"
    "  link_status tinyint not null default 1,"
    "  block_status tinyint not null default 0,"
    "  static_link_id char(64) default null,"
    "  primary key (db_name, table_name, link_id),"
    "  key idx1 (priority),"
    "  unique key uidx1 (db_name, table_name, static_link_id)"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_link_mon_servers("
    "  db_name char(64) not null default '',"
    "  table_name char(199) not null default '',"
    "  link_id char(64) not null default '',"
    "  sid int unsigned not null default 0,"
    "  server char(64) default null,"
    "  scheme char(64) default null,"
    "  host char(64) default null,"
    "  port char(5) default null,"
    "  socket text,"
    "  username char(64) default null,"
    "  password char(64) default null,"
    "  ssl_ca text,"
    "  ssl_capath text,"
    "  ssl_cert text,"
    "  ssl_cipher char(64) default null,"
    "  ssl_key text,"
    "  ssl_verify_server_cert tinyint not null default 0,"
    "  default_file text,"
    "  default_group char(64) default null,"
    "  dsn char(64) default null,"
    "  filedsn text default null,"
    "  driver char(64) default null,"
    "  primary key (db_name, table_name, link_id, sid)"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_link_failed_log("
    "  db_name char(64) not null default '',"
    "  table_name char(199) not null default '',"
    "  link_id char(64) not null default '',"
    "  failed_time timestamp not null default current_timestamp"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_table_position_for_recovery("
    "  db_name char(64) not null default '',"
    "  table_name char(199) not null default '',"
    "  failed_link_id int not null default 0,"
    "  source_link_id int not null default 0,"
    "  file text,"
    "  position text,"
    "  gtid text,"
    "  primary key (db_name, table_name, failed_link_id, source_link_id)"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_table_sts("
    "  db_name char(64) not null default '',"
    "  table_name char(199) not null default '',"
    "  data_file_length bigint unsigned not null default 0,"
    "  max_data_file_length bigint unsigned not null default 0,"
    "  index_file_length bigint unsigned not null default 0,"
    "  records bigint unsigned not null default 0,"
    "  mean_rec_length bigint unsigned not null default 0,"
    "  check_time datetime not null default '0000-00-00 00:00:00',"
    "  create_time datetime not null default '0000-00-00 00:00:00',"
    "  update_time datetime not null default '0000-00-00 00:00:00',"
    "  checksum bigint unsigned default null,"
    "  primary key (db_name, table_name)"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_table_crd("
    "  db_name char(64) not null default '',"
    "  table_name char(199) not null default '',"
    "  key_seq int unsigned not null default 0,"
    "  cardinality bigint not null default 0,"
    "  primary key (db_name, table_name, key_seq)"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
/*
  If tables already exist and their definition differ
  from the latest ones, we fix them here.
*/
/*
  Fix for 0.5
*/
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add if not exists server char(64) default null,"
    "  add if not exists scheme char(64) default null,"
    "  add if not exists host char(64) default null,"
    "  add if not exists port char(5) default null,"
    "  add if not exists socket char(64) default null,"
    "  add if not exists username char(64) default null,"
    "  add if not exists password char(64) default null,"
    "  add if not exists tgt_db_name char(64) default null,"
    "  add if not exists tgt_table_name char(64) default null,"
    "  algorithm=copy, lock=shared;"
  )},
/*
  Fix for version 0.17
*/
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_xa'"
    "    AND COLUMN_NAME = 'data';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'binary(128)' then"
    "  alter table mysql.spider_xa"
    "    modify data binary(128) not null default '',"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_xa_member'"
    "    AND COLUMN_NAME = 'data';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'binary(128)' then"
    "  alter table mysql.spider_xa_member"
    "    modify data binary(128) not null default '',"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
/*
  Fix for version 2.7
*/
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists link_id int not null default 0 after table_name,"
    "  drop primary key,"
    "  add primary key (db_name, table_name, link_id),"
    "  algorithm=copy, lock=shared;"
  )},
/*
  Fix for version 2.8
*/
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists link_status tinyint not null default 1,"
    "  algorithm=copy, lock=shared;"
  )},
/*
  Fix for version 2.10
*/
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_member"
    "  add column if not exists ssl_ca char(64) default null after password,"
    "  add column if not exists ssl_capath char(64) default null after ssl_ca,"
    "  add column if not exists ssl_cert char(64) default null after ssl_capath,"
    "  add column if not exists ssl_cipher char(64) default null after ssl_cert,"
    "  add column if not exists ssl_key char(64) default null after ssl_cipher,"
    "  add column if not exists ssl_verify_server_cert tinyint not null default 0"
    "    after ssl_key,"
    "  add column if not exists default_file char(64) default null"
    "    after ssl_verify_server_cert,"
    "  add column if not exists default_group char(64) default null after default_file,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists ssl_ca char(64) default null after password,"
    "  add column if not exists ssl_capath char(64) default null after ssl_ca,"
    "  add column if not exists ssl_cert char(64) default null after ssl_capath,"
    "  add column if not exists ssl_cipher char(64) default null after ssl_cert,"
    "  add column if not exists ssl_key char(64) default null after ssl_cipher,"
    "  add column if not exists ssl_verify_server_cert tinyint not null default 0"
    "    after ssl_key,"
    "  add column if not exists default_file char(64) default null"
    "    after ssl_verify_server_cert,"
    "  add column if not exists default_group char(64) default null after default_file,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_link_mon_servers"
    "  add column if not exists ssl_ca char(64) default null after password,"
    "  add column if not exists ssl_capath char(64) default null after ssl_ca,"
    "  add column if not exists ssl_cert char(64) default null after ssl_capath,"
    "  add column if not exists ssl_cipher char(64) default null after ssl_cert,"
    "  add column if not exists ssl_key char(64) default null after ssl_cipher,"
    "  add column if not exists ssl_verify_server_cert tinyint not null default 0"
    "    after ssl_key,"
    "  add column if not exists default_file char(64) default null"
    "    after ssl_verify_server_cert,"
    "  add column if not exists default_group char(64) default null after default_file,"
    "  algorithm=copy, lock=shared;"
  )},
/*
  Fix for version 2.28
*/
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_link_mon_servers'"
    "    AND COLUMN_NAME = 'sid';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'int(10) unsigned' then"
    "  alter table mysql.spider_link_mon_servers"
    "  modify sid int unsigned not null default 0,"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
/*
  Fix for version 3.1
*/
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_xa_member'"
    "    AND COLUMN_NAME = 'socket';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type = 'char(64)' then"
    "  alter table mysql.spider_xa_member"
    "    drop primary key,"
    "    add index idx1 (data, format_id, gtrid_length, host),"
    "    modify socket text not null,"
    "    modify ssl_ca text,"
    "    modify ssl_capath text,"
    "    modify ssl_cert text,"
    "    modify ssl_key text,"
    "    modify default_file text,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_tables'"
    "    AND COLUMN_NAME = 'socket';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type = 'char(64)' then"
    "  alter table mysql.spider_tables"
    "    modify socket text,"
    "    modify ssl_ca text,"
    "    modify ssl_capath text,"
    "    modify ssl_cert text,"
    "    modify ssl_key text,"
    "    modify default_file text,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_link_mon_servers'"
    "    AND COLUMN_NAME = 'socket';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type = 'char(64)' then"
    "  alter table mysql.spider_link_mon_servers"
    "    modify socket text,"
    "    modify ssl_ca text,"
    "    modify ssl_capath text,"
    "    modify ssl_cert text,"
    "    modify ssl_key text,"
    "    modify default_file text,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
/*
  Fix for version 3.3.0
*/
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add if not exists monitoring_binlog_pos_at_failing tinyint not null default 0"
    "    after ssl_verify_server_cert,"
    "    algorithm=copy, lock=shared;"
  )},
/*
  Fix for version 3.3.6
*/
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists block_status tinyint not null default 0"
    "    after link_status,"
    "    algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists static_link_id char(64) default null after block_status,"
    "  add unique index if not exists uidx1 (db_name, table_name, static_link_id),"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_link_mon_servers'"
    "    AND COLUMN_NAME = 'link_id';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'char(64)' then"
    "  alter table mysql.spider_link_mon_servers"
    "  modify link_id char(64) not null default '',"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_link_failed_log'"
    "    AND COLUMN_NAME = 'link_id';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'char(64)' then"
    "  alter table mysql.spider_link_failed_log"
    "  modify link_id char(64) not null default '',"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
/*
  Fix for version 3.3.10
*/
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_tables'"
    "    AND COLUMN_NAME = 'table_name';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'char(199)' then"
    "  alter table mysql.spider_tables"
    "  modify table_name char(199) not null default '',"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_link_mon_servers'"
    "    AND COLUMN_NAME = 'table_name';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'char(199)' then"
    "  alter table mysql.spider_link_mon_servers"
    "  modify table_name char(199) not null default '',"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_link_failed_log'"
    "    AND COLUMN_NAME = 'table_name';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'char(199)' then"
    "  alter table mysql.spider_link_failed_log"
    "  modify table_name char(199) not null default '',"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_table_position_for_recovery'"
    "    AND COLUMN_NAME = 'table_name';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'char(199)' then"
    "  alter table mysql.spider_table_position_for_recovery"
    "  modify table_name char(199) not null default '',"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_table_sts'"
    "    AND COLUMN_NAME = 'table_name';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'char(199)' then"
    "  alter table mysql.spider_table_sts"
    "  modify table_name char(199) not null default '',"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_table_crd'"
    "    AND COLUMN_NAME = 'table_name';"
  )},
  {C_STRING_WITH_LEN(
    "if @col_type != 'char(199)' then"
    "  alter table mysql.spider_table_crd"
    "  modify table_name char(199) not null default '',"
    "  algorithm=copy, lock=shared;"
    "end if;"
  )},
/*
  Fix for version 3.3.15
*/
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_table_sts"
    "  add column if not exists checksum bigint unsigned default null after update_time,"
    "  algorithm=copy, lock=shared;"
  )},
/*
  Fix for MariaDB 10.4: Crash-Safe system tables
*/
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_link_failed_log';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_link_failed_log"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_link_mon_servers';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_link_mon_servers"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_table_crd';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_table_crd"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_table_position_for_recovery';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_table_position_for_recovery"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_table_sts';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_table_sts"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_tables';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_tables"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_xa';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_xa"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_xa_failed_log';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_xa_failed_log"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
  {C_STRING_WITH_LEN(
    "select ENGINE INTO @engine_name from INFORMATION_SCHEMA.TABLES"
    "  where TABLE_SCHEMA = 'mysql'"
    "    AND TABLE_NAME = 'spider_xa_member';"
  )},
  {C_STRING_WITH_LEN(
    "if @engine_name != 'Aria' then"
    "  alter table mysql.spider_xa_member"
    "    engine=Aria transactional=1,"
    "    algorithm=copy, lock=shared;"
    "end if;"
  )},
/*
  Fix for version 3.4
*/
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_link_mon_servers"
    "  add column if not exists dsn char(64) default null after default_group,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists dsn char(64) default null after default_group,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_failed_log"
    "  add column if not exists dsn char(64) default null after default_group,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_member"
    "  add column if not exists dsn char(64) default null after default_group,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_link_mon_servers"
    "  add column if not exists filedsn text default null after dsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists filedsn text default null after dsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_failed_log"
    "  add column if not exists filedsn text default null after dsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_member"
    "  add column if not exists filedsn text default null after dsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_link_mon_servers"
    "  add column if not exists driver char(64) default null after filedsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists driver char(64) default null after filedsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_failed_log"
    "  add column if not exists driver char(64) default null after filedsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_member"
    "  add column if not exists driver char(64) default null after filedsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_link_mon_servers"
    "  add column if not exists filedsn text default null after dsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists filedsn text default null after dsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_failed_log"
    "  add column if not exists filedsn text default null after dsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_member"
    "  add column if not exists filedsn text default null after dsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_link_mon_servers"
    "  add column if not exists driver char(64) default null after filedsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_tables"
    "  add column if not exists driver char(64) default null after filedsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_failed_log"
    "  add column if not exists driver char(64) default null after filedsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "alter table mysql.spider_xa_member"
    "  add column if not exists driver char(64) default null after filedsn,"
    "  algorithm=copy, lock=shared;"
  )},
  {C_STRING_WITH_LEN(
    "set @win_plugin := IF(@@version_compile_os like 'Win%', 1, 0);"
  )},
  /* Install UDFs. If udf is not initialised, then install by
  inserting into mysql.func */
  {C_STRING_WITH_LEN(
    "if @win_plugin = 0 then"
    "  begin not atomic"
    "    declare exit handler for 1041, 1123"
    "      replace into mysql.func values"
    "        ('spider_direct_sql', 2, 'ha_spider.so', 'function'),"
    "        ('spider_bg_direct_sql', 2, 'ha_spider.so', 'aggregate'),"
    "        ('spider_ping_table', 2, 'ha_spider.so', 'function'),"
    "        ('spider_copy_tables', 2, 'ha_spider.so', 'function'),"
    "        ('spider_flush_table_mon_cache', 2, 'ha_spider.so', 'function');"
    "    create function if not exists spider_direct_sql returns int"
    "      soname 'ha_spider.so';"
    "    create aggregate function if not exists spider_bg_direct_sql returns int"
    "      soname 'ha_spider.so';"
    "    create function if not exists spider_ping_table returns int"
    "      soname 'ha_spider.so';"
    "    create function if not exists spider_copy_tables returns int"
    "      soname 'ha_spider.so';"
    "    create function if not exists spider_flush_table_mon_cache returns int"
    "      soname 'ha_spider.so';"
    "  end;"
    "else"
    "  begin not atomic"
    "    declare exit handler for 1041, 1123"
    "      replace into mysql.func values"
    "        ('spider_direct_sql', 2, 'ha_spider.dll', 'function'),"
    "        ('spider_bg_direct_sql', 2, 'ha_spider.dll', 'aggregate'),"
    "        ('spider_ping_table', 2, 'ha_spider.dll', 'function'),"
    "        ('spider_copy_tables', 2, 'ha_spider.dll', 'function'),"
    "        ('spider_flush_table_mon_cache', 2, 'ha_spider.dll', 'function');"
    "    create function if not exists spider_direct_sql returns int"
    "      soname 'ha_spider.dll';"
    "    create aggregate function if not exists spider_bg_direct_sql returns int"
    "      soname 'ha_spider.dll';"
    "    create function if not exists spider_ping_table returns int"
    "      soname 'ha_spider.dll';"
    "    create function if not exists spider_copy_tables returns int"
    "      soname 'ha_spider.dll';"
    "    create function if not exists spider_flush_table_mon_cache returns int"
    "      soname 'ha_spider.dll';"
    "  end;"
    "end if;"
   )}
};
