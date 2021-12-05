# Include file to decrease test code duplication

let $log_bin= `select @@global.log_bin`;
if ($log_bin)
{
  RESET MASTER;
  let $keep_include_silent= 1;
  let $grep_script= ALTER;
  let $binlog_file=master-bin.000001;
}

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
--echo # $dbug_flag: BEFORE failure
--replace_result #p# #P# #sp# #SP#
--let $DATADIR= `select @@datadir`
--list_files $DATADIR/test
SHOW CREATE TABLE t1;
--sorted_result
SELECT * FROM t1;
--disable_abort_on_error

--disable_query_log
connect (con_backup,localhost,root,,);
BACKUP STAGE START;
connection default;
--enable_query_log

--eval $fail_statement
--enable_abort_on_error
--echo # $dbug_flag: AFTER failure
show warnings;
--replace_result #p# #P# #sp# #SP#
--list_files $DATADIR/test
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
--connection con_backup
backup stage end;
--connection default
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

if ($log_bin)
{
  RESET MASTER;
}
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
--echo # $dbug_flag: BEFORE failure (under LOCK TABLE)
--replace_result #p# #P# #sp# #SP#
--list_files $DATADIR/test
SHOW CREATE TABLE t1;
--sorted_result
SELECT * FROM t1;
if (!$create_statement2)
{
  LOCK TABLE t1 WRITE;
}
if ($create_statement2)
{
  LOCK TABLES t1 WRITE, t2 WRITE;
}
--disable_abort_on_error
--eval $fail_statement
--enable_abort_on_error
--echo # $dbug_flag: AFTER failure (under LOCK TABLE)
show warnings;
--replace_result #p# #P# #sp# #SP#
--list_files $DATADIR/test
SHOW CREATE TABLE t1;
--sorted_result
SELECT * FROM t1;
if ($show_statement)
{
  --error 0,ER_TABLE_NOT_LOCKED
  --eval $show_statement
}
if ($select_statement)
{
  --sorted_result
  --error 0,ER_TABLE_NOT_LOCKED
  --eval $select_statement
}
if ($log_bin)
{
  --source include/show_binlog_events.inc
}
UNLOCK TABLES;
DROP TABLE t1;
if ($drop_statement)
{
  --eval $drop_statement
}
