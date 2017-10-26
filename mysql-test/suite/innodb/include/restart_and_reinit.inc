#
# few innodb tests depend on innodb tablespace being completelty clean and new
#
# this file deletes old innodb files and restarts mysqld
#
source include/not_embedded.inc;

--disable_query_log
let $innodb_index_stats = query_get_value(show create table mysql.innodb_index_stats, Create Table, 1);
let $innodb_table_stats = query_get_value(show create table mysql.innodb_table_stats, Create Table, 1);
let $database=`select database()`;
drop table mysql.innodb_index_stats, mysql.innodb_table_stats;

let $datadir= `SELECT @@datadir`;
--source include/shutdown_mysqld.inc
remove_files_wildcard $datadir ib_logfile*;
remove_file $datadir/ibdata1;
--source include/start_mysqld.inc

--disable_query_log
use mysql;
eval $innodb_table_stats;
eval $innodb_index_stats;
eval use $database;
--enable_query_log
