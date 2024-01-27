# Create the all privilege tables for a catalog
# Must be run with a user from the 'def' catalog with CATALOG_PRIV

--source include/have_catalogs.inc
--source include/have_innodb.inc

--disable_query_log
--disable_result_log
--exec $MYSQL_DUMP --databases mysql --skip-comments > $log_file
--enable_result_log
--enable_query_log
