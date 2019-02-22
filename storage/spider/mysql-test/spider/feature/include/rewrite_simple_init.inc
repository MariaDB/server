--source rewrite_init.inc
if (!$HAS_REWRITE)
{
  --source rewrite_simple_deinit.inc
  skip Test requires audit rewrite;
}
--connection master_1
INSERT INTO mysql.spider_rewrite_tables VALUES (1, '%', '%');
INSERT INTO mysql.spider_rewrite_table_tables VALUES (1, 1, '', '', '', '', 'srv "s_2_1"', '');
select spider_flush_rewrite_cache();
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey FROM auto_test_remote.tbl_a ORDER BY pkey;
