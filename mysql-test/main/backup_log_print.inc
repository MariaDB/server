--disable_query_log
--source include/print_ddl_log.inc
--connection con1
backup stage end;
backup stage start;
--connection default
--enable_query_log
