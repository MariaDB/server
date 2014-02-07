# Copyright (C) 2010-2013 Kentoku Shiba
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

# This SQL script creates system tables for SPIDER
#   or fixes incompatibilities if ones already exist.

-- Create system tables if not exist
create table if not exists mysql.spider_xa(
  format_id int not null default 0,
  gtrid_length int not null default 0,
  bqual_length int not null default 0,
  data char(128) charset binary not null default '',
  status char(8) not null default '',
  primary key (data, format_id, gtrid_length),
  key idx1 (status)
) engine=MyISAM default charset=utf8 collate=utf8_bin;
create table if not exists mysql.spider_xa_member(
  format_id int not null default 0,
  gtrid_length int not null default 0,
  bqual_length int not null default 0,
  data char(128) charset binary not null default '',
  scheme char(64) not null default '',
  host char(64) not null default '',
  port char(5) not null default '',
  socket text not null,
  username char(64) not null default '',
  password char(64) not null default '',
  ssl_ca text,
  ssl_capath text,
  ssl_cert text,
  ssl_cipher char(64) default null,
  ssl_key text,
  ssl_verify_server_cert tinyint not null default 0,
  default_file text,
  default_group char(64) default null,
  key idx1 (data, format_id, gtrid_length, host)
) engine=MyISAM default charset=utf8 collate=utf8_bin;
create table if not exists mysql.spider_xa_failed_log(
  format_id int not null default 0,
  gtrid_length int not null default 0,
  bqual_length int not null default 0,
  data char(128) charset binary not null default '',
  scheme char(64) not null default '',
  host char(64) not null default '',
  port char(5) not null default '',
  socket text not null,
  username char(64) not null default '',
  password char(64) not null default '',
  ssl_ca text,
  ssl_capath text,
  ssl_cert text,
  ssl_cipher char(64) default null,
  ssl_key text,
  ssl_verify_server_cert tinyint not null default 0,
  default_file text,
  default_group char(64) default null,
  thread_id int default null,
  status char(8) not null default '',
  failed_time timestamp not null default current_timestamp,
  key idx1 (data, format_id, gtrid_length, host)
) engine=MyISAM default charset=utf8 collate=utf8_bin;
create table if not exists mysql.spider_tables(
  db_name char(64) not null default '',
  table_name char(64) not null default '',
  link_id int not null default 0,
  priority bigint not null default 0,
  server char(64) default null,
  scheme char(64) default null,
  host char(64) default null,
  port char(5) default null,
  socket text,
  username char(64) default null,
  password char(64) default null,
  ssl_ca text,
  ssl_capath text,
  ssl_cert text,
  ssl_cipher char(64) default null,
  ssl_key text,
  ssl_verify_server_cert tinyint not null default 0,
  default_file text,
  default_group char(64) default null,
  tgt_db_name char(64) default null,
  tgt_table_name char(64) default null,
  link_status tinyint not null default 1,
  primary key (db_name, table_name, link_id),
  key idx1 (priority)
) engine=MyISAM default charset=utf8 collate=utf8_bin;
create table if not exists mysql.spider_link_mon_servers(
  db_name char(64) not null default '',
  table_name char(64) not null default '',
  link_id char(5) not null default '',
  sid int unsigned not null default 0,
  server char(64) default null,
  scheme char(64) default null,
  host char(64) default null,
  port char(5) default null,
  socket text,
  username char(64) default null,
  password char(64) default null,
  ssl_ca text,
  ssl_capath text,
  ssl_cert text,
  ssl_cipher char(64) default null,
  ssl_key text,
  ssl_verify_server_cert tinyint not null default 0,
  default_file text,
  default_group char(64) default null,
  primary key (db_name, table_name, link_id, sid)
) engine=MyISAM default charset=utf8 collate=utf8_bin;
create table if not exists mysql.spider_link_failed_log(
  db_name char(64) not null default '',
  table_name char(64) not null default '',
  link_id int not null default 0,
  failed_time timestamp not null default current_timestamp
) engine=MyISAM default charset=utf8 collate=utf8_bin;

-- If tables already exist and their definition differ from the latest ones,
--   we fix them here.
drop procedure if exists mysql.spider_fix_one_table;
drop procedure if exists mysql.spider_fix_system_tables;
delimiter //
create procedure mysql.spider_fix_one_table
  (tab_name char(255), test_col_name char(255), _sql text)
begin
  set @col_exists := 0;
  select 1 into @col_exists from INFORMATION_SCHEMA.COLUMNS
    where TABLE_SCHEMA = 'mysql'
      AND TABLE_NAME = tab_name
      AND COLUMN_NAME = test_col_name;
  if @col_exists = 0 then
    select @stmt := _sql;
    prepare sp_stmt1 from @stmt;
    execute sp_stmt1;
  end if;
end;//

create procedure mysql.spider_fix_system_tables()
begin
  -- Fix for 0.5
  call mysql.spider_fix_one_table('spider_tables', 'server',
   'alter table mysql.spider_tables
    add server char(64) default null,
    add scheme char(64) default null,
    add host char(64) default null,
    add port char(5) default null,
    add socket char(64) default null,
    add username char(64) default null,
    add password char(64) default null,
    add tgt_db_name char(64) default null,
    add tgt_table_name char(64) default null');
  
  -- Fix for version 0.17
  select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS
    where TABLE_SCHEMA = 'mysql'
      AND TABLE_NAME = 'spider_xa'
      AND COLUMN_NAME = 'data';
  if @col_type != 'binary(128)' then
    alter table mysql.spider_xa modify data binary(128) not null default '';
  end if;
  select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS
    where TABLE_SCHEMA = 'mysql'
      AND TABLE_NAME = 'spider_xa_member'
      AND COLUMN_NAME = 'data';
  if @col_type != 'binary(128)' then
    alter table mysql.spider_xa_member modify data binary(128) not null default '';
  end if;
  
  -- Fix for version 2.7
  call mysql.spider_fix_one_table('spider_tables', 'link_id',
   'alter table mysql.spider_tables
    add column link_id int not null default 0 after table_name,
    drop primary key,
    add primary key (db_name, table_name, link_id)');
  
  -- Fix for version 2.8
  call mysql.spider_fix_one_table('spider_tables', 'link_status',
   'alter table mysql.spider_tables
    add column link_status tinyint not null default 1');
  
  -- Fix for version 2.10
  call mysql.spider_fix_one_table('spider_xa_member', 'ssl_ca',
   'alter table mysql.spider_xa_member
    add column ssl_ca char(64) default null after password,
    add column ssl_capath char(64) default null after ssl_ca,
    add column ssl_cert char(64) default null after ssl_capath,
    add column ssl_cipher char(64) default null after ssl_cert,
    add column ssl_key char(64) default null after ssl_cipher,
    add column ssl_verify_server_cert tinyint not null default 0 after ssl_key,
    add column default_file char(64) default null after ssl_verify_server_cert,
    add column default_group char(64) default null after default_file');
  call mysql.spider_fix_one_table('spider_tables', 'ssl_ca',
   'alter table mysql.spider_tables
    add column ssl_ca char(64) default null after password,
    add column ssl_capath char(64) default null after ssl_ca,
    add column ssl_cert char(64) default null after ssl_capath,
    add column ssl_cipher char(64) default null after ssl_cert,
    add column ssl_key char(64) default null after ssl_cipher,
    add column ssl_verify_server_cert tinyint not null default 0 after ssl_key,
    add column default_file char(64) default null after ssl_verify_server_cert,
    add column default_group char(64) default null after default_file');
  call mysql.spider_fix_one_table('spider_link_mon_servers', 'ssl_ca',
   'alter table mysql.spider_link_mon_servers
    add column ssl_ca char(64) default null after password,
    add column ssl_capath char(64) default null after ssl_ca,
    add column ssl_cert char(64) default null after ssl_capath,
    add column ssl_cipher char(64) default null after ssl_cert,
    add column ssl_key char(64) default null after ssl_cipher,
    add column ssl_verify_server_cert tinyint not null default 0 after ssl_key,
    add column default_file char(64) default null after ssl_verify_server_cert,
    add column default_group char(64) default null after default_file');

  -- Fix for version 2.25
  select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS
    where TABLE_SCHEMA = 'mysql'
      AND TABLE_NAME = 'spider_link_mon_servers'
      AND COLUMN_NAME = 'link_id';
  if @col_type != 'char(5)' then
    alter table mysql.spider_link_mon_servers
    modify link_id char(5) not null default '';
  end if;

  -- Fix for version 2.28
  select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS
    where TABLE_SCHEMA = 'mysql'
      AND TABLE_NAME = 'spider_link_mon_servers'
      AND COLUMN_NAME = 'sid';
  if @col_type != 'int(10) unsigned' then
    alter table mysql.spider_link_mon_servers
    modify sid int unsigned not null default 0;
  end if;

  -- Fix for version 3.1
  select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS
    where TABLE_SCHEMA = 'mysql'
      AND TABLE_NAME = 'spider_xa_member'
      AND COLUMN_NAME = 'socket';
  if @col_type = 'char(64)' then
    alter table mysql.spider_xa_member
      drop primary key,
      add index idx1 (data, format_id, gtrid_length, host),
      modify socket text not null,
      modify ssl_ca text,
      modify ssl_capath text,
      modify ssl_cert text,
      modify ssl_key text,
      modify default_file text;
  end if;
  select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS
    where TABLE_SCHEMA = 'mysql'
      AND TABLE_NAME = 'spider_tables'
      AND COLUMN_NAME = 'socket';
  if @col_type = 'char(64)' then
    alter table mysql.spider_tables
      modify socket text,
      modify ssl_ca text,
      modify ssl_capath text,
      modify ssl_cert text,
      modify ssl_key text,
      modify default_file text;
  end if;
  select COLUMN_TYPE INTO @col_type from INFORMATION_SCHEMA.COLUMNS
    where TABLE_SCHEMA = 'mysql'
      AND TABLE_NAME = 'spider_link_mon_servers'
      AND COLUMN_NAME = 'socket';
  if @col_type = 'char(64)' then
    alter table mysql.spider_link_mon_servers
      modify socket text,
      modify ssl_ca text,
      modify ssl_capath text,
      modify ssl_cert text,
      modify ssl_key text,
      modify default_file text;
  end if;
end;//
delimiter ;
call mysql.spider_fix_system_tables;
drop procedure mysql.spider_fix_one_table;
drop procedure mysql.spider_fix_system_tables;

-- Install a plugin and UDFs
drop procedure if exists mysql.spider_plugin_installer;
delimiter //
create procedure mysql.spider_plugin_installer()
begin
  set @win_plugin := IF(@@version_compile_os like 'Win%', 1, 0);
  set @have_spider_plugin := 0;
  select @have_spider_plugin := 1 from INFORMATION_SCHEMA.plugins where PLUGIN_NAME = 'SPIDER';
  if @have_spider_plugin = 0 then 
    if @win_plugin = 0 then 
      install plugin spider soname 'ha_spider.so';
    else
      install plugin spider soname 'ha_spider.dll';
    end if;
  end if;
  set @have_spider_i_s_alloc_mem_plugin := 0;
  select @have_spider_i_s_alloc_mem_plugin := 1 from INFORMATION_SCHEMA.plugins where PLUGIN_NAME = 'SPIDER_ALLOC_MEM';
  if @have_spider_i_s_alloc_mem_plugin = 0 then 
    if @win_plugin = 0 then 
      install plugin spider_alloc_mem soname 'ha_spider.so';
    else
      install plugin spider_alloc_mem soname 'ha_spider.dll';
    end if;
  end if;
  set @have_spider_direct_sql_udf := 0;
  select @have_spider_direct_sql_udf := 1 from mysql.func where name = 'spider_direct_sql';
  if @have_spider_direct_sql_udf = 0 then
    if @win_plugin = 0 then 
      create function spider_direct_sql returns int soname 'ha_spider.so';
    else
      create function spider_direct_sql returns int soname 'ha_spider.dll';
    end if;
  end if;
  set @have_spider_bg_direct_sql_udf := 0;
  select @have_spider_bg_direct_sql_udf := 1 from mysql.func where name = 'spider_bg_direct_sql';
  if @have_spider_bg_direct_sql_udf = 0 then
    if @win_plugin = 0 then 
      create aggregate function spider_bg_direct_sql returns int soname 'ha_spider.so';
    else
      create aggregate function spider_bg_direct_sql returns int soname 'ha_spider.dll';
    end if;
  end if;
  set @have_spider_ping_table_udf := 0;
  select @have_spider_ping_table_udf := 1 from mysql.func where name = 'spider_ping_table';
  if @have_spider_ping_table_udf = 0 then
    if @win_plugin = 0 then 
      create function spider_ping_table returns int soname 'ha_spider.so';
    else
      create function spider_ping_table returns int soname 'ha_spider.dll';
    end if;
  end if;
  set @have_spider_copy_tables_udf := 0;
  select @have_spider_copy_tables_udf := 1 from mysql.func where name = 'spider_copy_tables';
  if @have_spider_copy_tables_udf = 0 then
    if @win_plugin = 0 then 
      create function spider_copy_tables returns int soname 'ha_spider.so';
    else
      create function spider_copy_tables returns int soname 'ha_spider.dll';
    end if;
  end if;

  set @have_spider_flush_table_mon_cache_udf := 0;
  select @have_spider_flush_table_mon_cache_udf := 1 from mysql.func where name = 'spider_flush_table_mon_cache';
  if @have_spider_flush_table_mon_cache_udf = 0 then
    if @win_plugin = 0 then 
      create function spider_flush_table_mon_cache returns int soname 'ha_spider.so';
    else
      create function spider_flush_table_mon_cache returns int soname 'ha_spider.dll';
    end if;
  end if;

end;//
delimiter ;
call mysql.spider_plugin_installer;
drop procedure mysql.spider_plugin_installer;
