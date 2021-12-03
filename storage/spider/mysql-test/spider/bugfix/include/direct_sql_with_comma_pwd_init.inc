--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
let $DIRECT_SQL_COMMAND=
  SELECT spider_direct_sql('SELECT 22', 'tmp_a', 'srv "s_2_1", database "test", password "pass,1234", user "tu"');
--connection child2_1
GRANT ALL ON *.* TO tu@'%' IDENTIFIED BY 'pass,1234';
