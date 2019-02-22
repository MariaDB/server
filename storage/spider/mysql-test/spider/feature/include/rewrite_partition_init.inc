--source rewrite_init.inc
if (!$HAS_REWRITE)
{
  --source rewrite_partition_deinit.inc
  skip Test requires audit rewrite;
}
--connection master_1
INSERT INTO mysql.spider_rewrite_tables VALUES (1, '%', '%');
INSERT INTO mysql.spider_rewrite_table_tables VALUES (1, 1, 'LIST', 'MOD(pkey,4)', '', '', 'srv "s_2_1"', '');
INSERT INTO mysql.spider_rewrite_table_partitions VALUES
 (1, 1, 1, 'pt1', '0,1', 'database "auto_test_remote"', ''),
 (1, 1, 2, 'pt2', '2,3', 'database "auto_test_remote2"', '');
select spider_flush_rewrite_cache();
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey FROM auto_test_remote.tbl_a ORDER BY pkey $STR_SEMICOLON
  SELECT pkey FROM auto_test_remote2.tbl_a ORDER BY pkey;
