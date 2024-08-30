
#
# Drop database created by the s3 tests
#

--disable_query_log
use test;
--eval drop database $database;
--enable_query_log
