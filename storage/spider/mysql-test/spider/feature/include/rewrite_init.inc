--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log;
