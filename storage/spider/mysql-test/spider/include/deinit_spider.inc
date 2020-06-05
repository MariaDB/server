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
if (`SELECT IF($PLUGIN_VERSION = 3, 1, 0)`)
{
  let $HAS_REWRITE=
    `SELECT IF (STRCMP('$SERVER_NAME', 'MariaDB') = 0,
                IF ($SERVER_MAJOR_VERSION = 10,
                    IF ($SERVER_MINOR_VERSION < 4,  0, 1),
                    IF ($SERVER_MAJOR_VERSION < 10, 0, 1)),
                0)`;
  let $HAS_REWRITE= 0;
  if ($HAS_REWRITE)
  {
    DROP FUNCTION spider_flush_rewrite_cache;
    UNINSTALL PLUGIN spider_rewrite;
    DROP TABLE IF EXISTS mysql.spider_rewrite_tables;
    DROP TABLE IF EXISTS mysql.spider_rewrite_table_tables;
    DROP TABLE IF EXISTS mysql.spider_rewrite_table_partitions;
    DROP TABLE IF EXISTS mysql.spider_rewrite_table_subpartitions;
    DROP TABLE IF EXISTS mysql.spider_rewritten_tables;
  }
}
DROP FUNCTION spider_flush_table_mon_cache;
DROP FUNCTION spider_copy_tables;
DROP FUNCTION spider_ping_table;
DROP FUNCTION spider_bg_direct_sql;
DROP FUNCTION spider_direct_sql;
UNINSTALL PLUGIN spider_wrapper_protocols;
UNINSTALL PLUGIN spider_alloc_mem;
UNINSTALL PLUGIN spider;
DROP TABLE IF EXISTS mysql.spider_xa;
DROP TABLE IF EXISTS mysql.spider_xa_member;
DROP TABLE IF EXISTS mysql.spider_xa_failed_log;
DROP TABLE IF EXISTS mysql.spider_tables;
DROP TABLE IF EXISTS mysql.spider_link_mon_servers;
DROP TABLE IF EXISTS mysql.spider_link_failed_log;
DROP TABLE IF EXISTS mysql.spider_table_position_for_recovery;
DROP TABLE IF EXISTS mysql.spider_table_sts;
DROP TABLE IF EXISTS mysql.spider_table_crd;
if ($VERSION_COMPILE_OS_WIN)
{
  if ($MASTER_1_MYPORT)
  {
    DROP SERVER s_1;
  }
  if ($CHILD2_1_MYPORT)
  {
    DROP SERVER s_2_1;
  }
  if ($CHILD2_2_MYPORT)
  {
    DROP SERVER s_2_2;
  }
  if ($CHILD2_3_MYPORT)
  {
    DROP SERVER s_2_3;
  }
  if ($CHILD3_1_MYPORT)
  {
    DROP SERVER s_3_1;
  }
  if ($CHILD3_2_MYPORT)
  {
    DROP SERVER s_3_2;
  }
  if ($CHILD2_3_MYPORT)
  {
    DROP SERVER s_3_3;
  }
}
if (!$VERSION_COMPILE_OS_WIN)
{
  if ($MASTER_1_MYSOCK)
  {
    DROP SERVER s_1;
  }
  if ($CHILD2_1_MYSOCK)
  {
    DROP SERVER s_2_1;
  }
  if ($CHILD2_2_MYSOCK)
  {
    DROP SERVER s_2_2;
  }
  if ($CHILD2_3_MYSOCK)
  {
    DROP SERVER s_2_3;
  }
  if ($CHILD3_1_MYSOCK)
  {
    DROP SERVER s_3_1;
  }
  if ($CHILD3_2_MYSOCK)
  {
    DROP SERVER s_3_2;
  }
  if ($CHILD3_3_MYSOCK)
  {
    DROP SERVER s_3_3;
  }
}
