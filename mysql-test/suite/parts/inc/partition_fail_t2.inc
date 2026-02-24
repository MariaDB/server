# Include file to test failure with error injection.
# To be used with WL#4445: EXCHANGE PARTITION WITH TABLE.
--eval $create_statement2
--eval $insert_statement2
--eval $create_statement
--eval $insert_statement
--let $dbug_flag= `select @@session.debug_dbug`
--echo # $dbug_flag: BEFORE failure
--replace_result #p# #P#
if (!$DATADIR)
{
  --let $DATADIR= `SELECT @@datadir;`
}
--list_files $DATADIR/test
SHOW CREATE TABLE t1;
--sorted_result
SELECT * FROM t1;
SHOW CREATE TABLE t2;
--sorted_result
SELECT * FROM t2;
# accept all errors
--disable_abort_on_error
--replace_regex /#sql-exchange-[0-9a-f_\-]*/#sql-exchange/i
--eval $fail_statement
--enable_abort_on_error
--echo # $dbug_flag: AFTER failure
--replace_result #p# #P#
--list_files $DATADIR/test
SHOW CREATE TABLE t1;
--sorted_result
SELECT * FROM t1;
DROP TABLE t1;
--error 0, ER_NO_SUCH_TABLE
SHOW CREATE TABLE t2;
--sorted_result
--error 0, ER_NO_SUCH_TABLE
SELECT * FROM t2;
# TODO: everything fails with ER_NO_SUCH_TABLE
#       but DROP TABLE fails with ER_BAD_TABLE_ERROR! Why?
--error 0, ER_BAD_TABLE_ERROR
DROP TABLE t2;
