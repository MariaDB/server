--require include/not_valgrind.require
--disable_query_log
eval select $VALGRIND_TEST+0 as using_valgrind;
--enable_query_log
