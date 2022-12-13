# Include file to decrease test code duplication

let $log_bin= `select @@global.log_bin`;
if ($log_bin)
{
  let $grep_script= ALTER;
  let $binlog_file=master-bin.000001;
  RESET MASTER;
}
let $keep_include_silent= 1;

if ($create_statement2)
{
  --eval $create_statement2
}
if ($insert_statement2)
{
  --eval $insert_statement2
}
--eval $create_statement
--eval $insert_statement
--let $dbug_flag= `select @@session.debug_dbug`
--echo # $dbug_flag: BEFORE crash

--list_files_write_file $DATADIR.files.txt $DATADIR/test
--replace_result #p# #P# #sp# #SP#
--cat_file $DATADIR.files.txt
--remove_file $DATADIR.files.txt
SHOW CREATE TABLE t1;
--sorted_result
SELECT * FROM t1;
if ($show_statement)
{
  --error 0,ER_NO_SUCH_TABLE
  --eval $show_statement
}
if ($select_statement)
{
  --sorted_result
  --error 0,ER_NO_SUCH_TABLE
  --eval $select_statement
}

--disable_query_log
connect (con_backup,localhost,root,,);
BACKUP STAGE START;
connection default;
--enable_query_log

--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--disable_reconnect
let $errno= 0;
# CR_SERVER_LOST
--error 0, 2013
--eval $crash_statement
let $error= $errno;

--echo # $dbug_flag: AFTER crash
--list_files_write_file $DATADIR.files.txt $DATADIR/test
--replace_result #p# #P# #sp# #SP# #tmp# #TMP#
--replace_regex /sql-exchange.*\./sql-exchange./ /sql-shadow-[0-9a-f]*-/sql-shadow-/
--cat_file $DATADIR.files.txt
--remove_file $DATADIR.files.txt
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--enable_reconnect
--source include/wait_until_connected_again.inc

--echo # $dbug_flag: AFTER recovery
--list_files_write_file $DATADIR.files.txt $DATADIR/test
--replace_result #p# #P# #sp# #SP#
--cat_file $DATADIR.files.txt
--remove_file $DATADIR.files.txt
SHOW CREATE TABLE t1;
--sorted_result
SELECT * FROM t1;
if ($show_statement)
{
  --error 0,ER_NO_SUCH_TABLE
  --eval $show_statement
}
if ($select_statement)
{
  --sorted_result
  --error 0,ER_NO_SUCH_TABLE
  --eval $select_statement
}

--source include/print_ddl_log.inc
--disable_query_log
if ($error == 0)
{
  --connection con_backup
  backup stage end;
  --connection default
}
--disconnect con_backup
--enable_query_log

if ($log_bin)
{
  --source include/show_binlog_events.inc
}
DROP TABLE t1;
if ($drop_statement)
{
  --eval $drop_statement
}
