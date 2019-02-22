--source rewrite_init.inc
if (!$HAS_REWRITE)
{
  --source rewrite_subpartition_deinit.inc
  skip Test requires audit rewrite;
}
--connection master_1
INSERT INTO mysql.spider_rewrite_tables VALUES (1, '%', '%');
INSERT INTO mysql.spider_rewrite_table_tables VALUES (1, 1, 'RANGE', 'pkey', 'HASH', 'pkey', 'srv "s_2_1"', '');
INSERT INTO mysql.spider_rewrite_table_partitions VALUES
 (1, 1, 1, 'pt1', '5', '', ''),
 (1, 1, 2, 'pt2', 'MAXVALUE', '', '');
INSERT INTO mysql.spider_rewrite_table_subpartitions VALUES
 (1, 1, 1, 1, 'spt1', '', 'database "auto_test_remote"', ''),
 (1, 1, 1, 2, 'spt2', '', 'database "auto_test_remote2"', ''),
 (1, 1, 2, 1, 'spt3', '', 'database "auto_test_remote3"', ''),
 (1, 1, 2, 2, 'spt4', '', 'database "auto_test_remote4"', '');
select spider_flush_rewrite_cache();
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey FROM auto_test_remote.tbl_a ORDER BY pkey $STR_SEMICOLON
  SELECT pkey FROM auto_test_remote2.tbl_a ORDER BY pkey $STR_SEMICOLON
  SELECT pkey FROM auto_test_remote3.tbl_a ORDER BY pkey $STR_SEMICOLON
  SELECT pkey FROM auto_test_remote4.tbl_a ORDER BY pkey;
