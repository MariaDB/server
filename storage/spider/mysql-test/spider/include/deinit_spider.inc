DROP FUNCTION spider_direct_sql;
DROP FUNCTION spider_bg_direct_sql;
DROP FUNCTION spider_ping_table;
DROP FUNCTION spider_copy_tables;
DROP FUNCTION spider_flush_table_mon_cache;
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
