# Re-create all privilege tables for a catalog
# The file is created by create_catalog_priv_tables.inc

--source include/have_catalogs.inc
--source include/have_innodb.inc

--disable_query_log
--disable_result_log
--source $log_file
--enable_result_log
--enable_query_log
