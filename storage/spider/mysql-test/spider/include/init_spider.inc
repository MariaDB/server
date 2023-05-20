let $VERSION_COMPILE_OS_WIN=
  `SELECT IF(@@version_compile_os like 'Win%', 1, 0)`;
if ($VERSION_COMPILE_OS_WIN)
{
  INSTALL PLUGIN spider SONAME 'ha_spider.dll';
  CREATE FUNCTION spider_direct_sql RETURNS INT SONAME 'ha_spider.dll';
  CREATE AGGREGATE FUNCTION spider_bg_direct_sql RETURNS INT SONAME 'ha_spider.dll';
  CREATE FUNCTION spider_ping_table RETURNS INT SONAME 'ha_spider.dll';
  CREATE FUNCTION spider_copy_tables RETURNS INT SONAME 'ha_spider.dll';
  CREATE FUNCTION spider_flush_table_mon_cache RETURNS INT SONAME 'ha_spider.dll';
  eval CREATE SERVER s_2_1 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_remote',
    USER 'root',
    PASSWORD '',
    PORT $CHILD2_1_MYPORT
  );
  eval CREATE SERVER s_2_2 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_remote2',
    USER 'root',
    PASSWORD '',
    PORT $CHILD2_2_MYPORT
  );
  eval CREATE SERVER s_2_3 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_remote3',
    USER 'root',
    PASSWORD '',
    PORT $CHILD2_3_MYPORT
  );
  eval CREATE SERVER s_3_1 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_local',
    USER 'root',
    PASSWORD '',
    PORT $CHILD3_1_MYPORT
  );
  eval CREATE SERVER s_3_2 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_local',
    USER 'root',
    PASSWORD '',
    PORT $CHILD3_2_MYPORT
  );
  eval CREATE SERVER s_3_3 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_local',
    USER 'root',
    PASSWORD '',
    PORT $CHILD2_3_MYPORT
  );
}
if (!$VERSION_COMPILE_OS_WIN)
{
  INSTALL PLUGIN spider SONAME 'ha_spider.so';
  CREATE FUNCTION spider_direct_sql RETURNS INT SONAME 'ha_spider.so';
  CREATE AGGREGATE FUNCTION spider_bg_direct_sql RETURNS INT SONAME 'ha_spider.so';
  CREATE FUNCTION spider_ping_table RETURNS INT SONAME 'ha_spider.so';
  CREATE FUNCTION spider_copy_tables RETURNS INT SONAME 'ha_spider.so';
  CREATE FUNCTION spider_flush_table_mon_cache RETURNS INT SONAME 'ha_spider.so';
  eval CREATE SERVER s_2_1 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_remote',
    USER 'root',
    PASSWORD '',
    SOCKET '$CHILD2_1_MYSOCK'
  );
  eval CREATE SERVER s_2_2 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_remote2',
    USER 'root',
    PASSWORD '',
    SOCKET '$CHILD2_2_MYSOCK'
  );
  eval CREATE SERVER s_2_3 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_remote3',
    USER 'root',
    PASSWORD '',
    SOCKET '$CHILD2_3_MYSOCK'
  );
  eval CREATE SERVER s_3_1 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_local',
    USER 'root',
    PASSWORD '',
    SOCKET '$CHILD3_1_MYSOCK'
  );
  eval CREATE SERVER s_3_2 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_local',
    USER 'root',
    PASSWORD '',
    SOCKET '$CHILD3_2_MYSOCK'
  );
  eval CREATE SERVER s_3_3 FOREIGN DATA WRAPPER mysql OPTIONS (
    HOST 'localhost',
    DATABASE 'auto_test_local',
    USER 'root',
    PASSWORD '',
    SOCKET '$CHILD3_3_MYSOCK'
  );
}

let $SERVER_NAME=
  `SELECT SUBSTRING_INDEX(SUBSTRING_INDEX(version(), '-', 2), '-', -1)`;
let $SERVER_MAJOR_VERSION=
  `SELECT SUBSTRING_INDEX(version(), '.', 1)`;
let $SERVER_MINOR_VERSION=
  `SELECT SUBSTRING_INDEX(SUBSTRING_INDEX(version(), '.', 2), '.', -1)`;
let $PLUGIN_VERSION=
  `SELECT SUBSTRING_INDEX(plugin_version, '.', 1)
     FROM information_schema.plugins
    WHERE plugin_name = 'SPIDER'`;

if (`SELECT IF($PLUGIN_VERSION = 1, 1, 0)`)
{
  CREATE OR REPLACE TABLE mysql.spider_xa(
    format_id int not null default 0,
    gtrid_length int not null default 0,
    bqual_length int not null default 0,
    data char(128) charset binary not null default '',
    status char(8) not null default '',
    PRIMARY KEY (data, format_id, gtrid_length),
    KEY idx1 (status)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  CREATE OR REPLACE TABLE mysql.spider_xa_member(
    format_id int not null default 0,
    gtrid_length int not null default 0,
    bqual_length int not null default 0,
    data char(128) charset binary not null default '',
    scheme char(64) not null default '',
    host char(64) not null default '',
    port char(5) not null default '',
    socket char(64) not null default '',
    username char(64) not null default '',
    password char(64) not null default '',
    PRIMARY KEY (data, format_id, gtrid_length, host, port, socket)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  CREATE OR REPLACE TABLE mysql.spider_tables(
    db_name char(64) not null default '',
    table_name char(64) not null default '',
    priority bigint not null default 0,
    server char(64) default null,
    scheme char(64) default null,
    host char(64) default null,
    port char(5) default null,
    socket char(64) default null,
    username char(64) default null,
    password char(64) default null,
    tgt_db_name char(64) default null,
    tgt_table_name char(64) default null,
    PRIMARY KEY (db_name, table_name),
    KEY idx1 (priority)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
}
if (`SELECT IF($PLUGIN_VERSION = 2, 1, 0)`)
{
  CREATE OR REPLACE TABLE mysql.spider_xa(
    format_id int not null default 0,
    gtrid_length int not null default 0,
    bqual_length int not null default 0,
    data char(128) charset binary not null default '',
    status char(8) not null default '',
    PRIMARY KEY (data, format_id, gtrid_length),
    KEY idx1 (status)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  CREATE OR REPLACE TABLE mysql.spider_xa_member(
    format_id int not null default 0,
    gtrid_length int not null default 0,
    bqual_length int not null default 0,
    data char(128) charset binary not null default '',
    scheme char(64) not null default '',
    host char(64) not null default '',
    port char(5) not null default '',
    socket char(64) not null default '',
    username char(64) not null default '',
    password char(64) not null default '',
    ssl_ca char(64) default null,
    ssl_capath char(64) default null,
    ssl_cert char(64) default null,
    ssl_cipher char(64) default null,
    ssl_key char(64) default null,
    ssl_verify_server_cert tinyint not null default 0,
    default_file char(64) default null,
    default_group char(64) default null,
    PRIMARY KEY (data, format_id, gtrid_length, host, port, socket)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  DROP TABLE IF EXISTS mysql.spider_tables;
  CREATE TABLE mysql.spider_tables(
    db_name char(64) not null default '',
    table_name char(64) not null default '',
    link_id int not null default 0,
    priority bigint not null default 0,
    server char(64) default null,
    scheme char(64) default null,
    host char(64) default null,
    port char(5) default null,
    socket char(64) default null,
    username char(64) default null,
    password char(64) default null,
    ssl_ca char(64) default null,
    ssl_capath char(64) default null,
    ssl_cert char(64) default null,
    ssl_cipher char(64) default null,
    ssl_key char(64) default null,
    ssl_verify_server_cert tinyint not null default 0,
    default_file char(64) default null,
    default_group char(64) default null,
    tgt_db_name char(64) default null,
    tgt_table_name char(64) default null,
    link_status tinyint not null default 1,
    PRIMARY KEY (db_name, table_name, link_id),
    KEY idx1 (priority)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  DROP TABLE IF EXISTS mysql.spider_link_mon_servers;
  CREATE TABLE mysql.spider_link_mon_servers(
    db_name char(64) not null default '',
    table_name char(64) not null default '',
    link_id char(5) not null default '',
    sid int not null default 0,
    server char(64) default null,
    scheme char(64) default null,
    host char(64) default null,
    port char(5) default null,
    socket char(64) default null,
    username char(64) default null,
    password char(64) default null,
    ssl_ca char(64) default null,
    ssl_capath char(64) default null,
    ssl_cert char(64) default null,
    ssl_cipher char(64) default null,
    ssl_key char(64) default null,
    ssl_verify_server_cert tinyint not null default 0,
    default_file char(64) default null,
    default_group char(64) default null,
    PRIMARY KEY (db_name, table_name, link_id, sid)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  DROP TABLE IF EXISTS mysql.spider_link_failed_log;
  CREATE TABLE mysql.spider_link_failed_log(
    db_name char(64) not null default '',
    table_name char(64) not null default '',
    link_id int not null default 0,
    failed_time timestamp not null default current_timestamp
  ) ENGINE=MYISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
}
if (`SELECT IF($PLUGIN_VERSION = 3, 1, 0)`)
{
  CREATE OR REPLACE TABLE mysql.spider_xa(
    format_id int not null default 0,
    gtrid_length int not null default 0,
    bqual_length int not null default 0,
    data binary(128) not null default '',
    status char(8) not null default '',
    PRIMARY KEY (data, format_id, gtrid_length),
    KEY idx1 (status)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  CREATE OR REPLACE TABLE mysql.spider_xa_member(
    format_id int not null default 0,
    gtrid_length int not null default 0,
    bqual_length int not null default 0,
    data binary(128) not null default '',
    scheme char(64) not null default '',
    host char(64) not null default '',
    port char(5) not null default '',
    socket text not null default '',
    username char(64) not null default '',
    password char(64) not null default '',
    ssl_ca text default null,
    ssl_capath text default null,
    ssl_cert text default null,
    ssl_cipher char(64) default null,
    ssl_key text default null,
    ssl_verify_server_cert tinyint not null default 0,
    default_file text default null,
    default_group char(64) default null,
    KEY idx1 (data, format_id, gtrid_length, host)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  CREATE OR REPLACE TABLE mysql.spider_tables(
    db_name char(64) not null default '',
    table_name char(199) not null default '',
    link_id int not null default 0,
    priority bigint not null default 0,
    server char(64) default null,
    scheme char(64) default null,
    host char(64) default null,
    port char(5) default null,
    socket text default null,
    username char(64) default null,
    password char(64) default null,
    ssl_ca text default null,
    ssl_capath text default null,
    ssl_cert text default null,
    ssl_cipher char(64) default null,
    ssl_key text default null,
    ssl_verify_server_cert tinyint not null default 0,
    monitoring_binlog_pos_at_failing tinyint not null default 0,
    default_file text default null,
    default_group char(64) default null,
    tgt_db_name char(64) default null,
    tgt_table_name char(64) default null,
    link_status tinyint not null default 1,
    block_status tinyint not null default 0,
    static_link_id char(64) default null,
    PRIMARY KEY (db_name, table_name, link_id),
    KEY idx1 (priority)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  CREATE OR REPLACE TABLE mysql.spider_link_mon_servers(
    db_name char(64) not null default '',
    table_name char(199) not null default '',
    link_id char(64) not null default '',
    sid int unsigned not null default 0,
    server char(64) default null,
    scheme char(64) default null,
    host char(64) default null,
    port char(5) default null,
    socket text default null,
    username char(64) default null,
    password char(64) default null,
    ssl_ca text default null,
    ssl_capath text default null,
    ssl_cert text default null,
    ssl_cipher char(64) default null,
    ssl_key text default null,
    ssl_verify_server_cert tinyint not null default 0,
    default_file text default null,
    default_group char(64) default null,
    PRIMARY KEY (db_name, table_name, link_id, sid)
  ) ENGINE=MyISAM DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  CREATE OR REPLACE TABLE mysql.spider_link_failed_log(
    db_name char(64) not null default '',
    table_name char(199) not null default '',
    link_id char(64) not null default '',
    failed_time timestamp not null default current_timestamp
  ) ENGINE=$ENGINE_NAME DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  DROP TABLE IF EXISTS mysql.spider_table_position_for_recovery;
  eval
  CREATE TABLE mysql.spider_table_position_for_recovery(
    db_name char(64) not null default '',
    table_name char(199) not null default '',
    failed_link_id int not null default 0,
    source_link_id int not null default 0,
    file text,
    position text,
    gtid text,
    primary key (db_name, table_name, failed_link_id, source_link_id)
  ) ENGINE=$ENGINE_NAME DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  DROP TABLE IF EXISTS mysql.spider_table_sts;
  eval
  CREATE TABLE mysql.spider_table_sts(
    db_name char(64) not null default '',
    table_name char(199) not null default '',
    data_file_length bigint unsigned not null default 0,
    max_data_file_length bigint unsigned not null default 0,
    index_file_length bigint unsigned not null default 0,
    records bigint unsigned not null default 0,
    mean_rec_length bigint unsigned not null default 0,
    check_time datetime not null default '0000-00-00 00:00:00',
    create_time datetime not null default '0000-00-00 00:00:00',
    update_time datetime not null default '0000-00-00 00:00:00',
    primary key (db_name, table_name)
  ) ENGINE=$ENGINE_NAME DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
  DROP TABLE IF EXISTS mysql.spider_table_crd;
  eval
  CREATE TABLE mysql.spider_table_crd(
    db_name char(64) not null default '',
    table_name char(199) not null default '',
    key_seq int unsigned not null default 0,
    cardinality bigint not null default 0,
    primary key (db_name, table_name, key_seq)
  ) ENGINE=$ENGINE_NAME DEFAULT CHARSET=utf8 COLLATE=utf8_bin;
}

SET spider_internal_sql_log_off= 0;
