#
# few innodb tests depend on innodb tablespace being completelty clean and new
#
# this file deletes old innodb files and restarts mysqld
#
source include/not_embedded.inc;

--disable_query_log
call mtr.add_suppression("InnoDB: New log files created");
call mtr.add_suppression("InnoDB: Creating foreign key constraint system tables");

let $innodb_index_stats = query_get_value(show create table mysql.innodb_index_stats, Create Table, 1);
let $innodb_table_stats = query_get_value(show create table mysql.innodb_table_stats, Create Table, 1);
let $database=`select database()`;
drop table mysql.innodb_index_stats, mysql.innodb_table_stats;

let $_server_id= `SELECT @@server_id`;
let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/mysqld.$_server_id.expect;
let $datadir= `SELECT @@datadir`;
exec echo "wait" > $_expect_file_name;
shutdown_server;
remove_file $datadir/ib_logfile0;
remove_file $datadir/ib_logfile1;
remove_file $datadir/ibdata1;
exec echo "restart" > $_expect_file_name;
enable_reconnect;
source include/wait_until_connected_again.inc;
disable_reconnect;

--disable_query_log
use mysql;
eval $innodb_table_stats;
eval $innodb_index_stats;
eval use $database;
--enable_query_log
