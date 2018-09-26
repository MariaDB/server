--source include/have_tokudb.inc
--source include/have_innodb.inc
--disable_query_log
set tokudb_enable_fast_update=1;
--source setup_fast_update_upsert.inc
--enable_query_log
