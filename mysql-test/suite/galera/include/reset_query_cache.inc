--disable_query_log
--disable_result_log
--connection node_1
flush query cache;
reset query cache;

--connection node_2
flush query cache;
reset query cache;
--enable_result_log
--enable_query_log
