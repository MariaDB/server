# This script assumes that the caller did the following;
#     LET $MYSQLD_DATADIR = `select @@datadir`;
#     LET $INNODB_PAGE_SIZE = `select @@innodb_page_size`;
--echo === information_schema.innodb_sys_tablespaces and innodb_sys_datafiles ===
--disable_query_log
--replace_regex  /#P#/#p#/  /#SP#/#sp#/
--replace_result ./ MYSQLD_DATADIR/  $MYSQLD_DATADIR/ MYSQLD_DATADIR/  $MYSQLD_DATADIR MYSQLD_DATADIR/  $MYSQL_TMP_DIR MYSQL_TMP_DIR  $INNODB_PAGE_SIZE DEFAULT
SELECT name 'Space_Name',
	@@GLOBAL.innodb_page_size 'Page_Size',
	page_size 'Zip_Size',
	filename 'Path'
	FROM information_schema.innodb_sys_tablespaces
	WHERE name != 'innodb_system'
	AND name NOT LIKE 'mysql/%'
	ORDER BY space;
--enable_query_log
