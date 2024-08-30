#
# Create unique database to not conflict with concurrently running tests as
# the s3 database is shared
#

let $database=`select concat("s3_test_",replace(uuid(),"-",""))`;
--disable_query_log
--eval create database $database;
--eval use $database;
--enable_query_log
