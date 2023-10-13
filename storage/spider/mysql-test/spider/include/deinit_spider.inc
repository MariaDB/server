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
    DROP TABLE IF EXISTS mysql.spider_rewrite_tables;
    DROP TABLE IF EXISTS mysql.spider_rewrite_table_tables;
    DROP TABLE IF EXISTS mysql.spider_rewrite_table_partitions;
    DROP TABLE IF EXISTS mysql.spider_rewrite_table_subpartitions;
    DROP TABLE IF EXISTS mysql.spider_rewritten_tables;
  }
}
--source clean_up_spider.inc
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
