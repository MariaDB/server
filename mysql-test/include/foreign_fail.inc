set @save_dbug=@@debug_dbug;
if ($create_statement)
{
  eval $create_statement;
}
if ($insert_statement)
{
  eval $insert_statement;
}
if ($insert_statement2)
{
  eval $insert_statement;
}
if ($create_statement2)
{
  eval $create_statement2;
}
if ($show_statement0)
{
  eval $show_statement0;
}

if ($crash_statement)
{
  --echo # State before crash
}

if (!$crash_statement)
{
  --echo # State before failure
}

--list_files_write_file $DATADIR.files.txt $DATADIR/test
--replace_result #p# #P# #sp# #SP#
--replace_regex /#sql-ib[1-9][0-9]*\.ibd\n//
--cat_file $DATADIR.files.txt
--remove_file $DATADIR.files.txt

if ($show_statement1)
{
  eval $show_statement1;
}

--eval set @@debug_dbug= $debug_dbug

if ($crash_statement)
{
  --exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
  --disable_reconnect
  # CR_SERVER_LOST
  --error 2013
  --eval $crash_statement

  --echo # State after crash (before recovery)
  --list_files_write_file $DATADIR.files.txt $DATADIR/test
  --replace_result #p# #P# #sp# #SP# #tmp# #TMP#
  --replace_regex /sql-exchange.*\./sql-exchange./ /sql(f?)-shadow-[0-9a-f]*-/sql\1-shadow-/ /sql-alter-[0-9a-f]*-/sql-alter-/ /#sql-ib[1-9][0-9]*\.ibd\n//
  --cat_file $DATADIR.files.txt
  --remove_file $DATADIR.files.txt

  --exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
  --enable_reconnect
  --source include/wait_until_connected_again.inc

  --echo # State after crash recovery
  --list_files_write_file $DATADIR.files.txt $DATADIR/test
  --replace_result #p# #P# #sp# #SP#
  --replace_regex /sql-alter-[0-9a-f]*-/sql-alter-/ /#sql-ib[1-9][0-9]*\.ibd\n//
  --cat_file $DATADIR.files.txt
  --remove_file $DATADIR.files.txt
  # TODO: when ALTER crashes it leaves tmp garbage. Should be fixed via DDL log
  # (though engine files can left too).
  --remove_files_wildcard $DATADIR/test #sql-alter*.*
}

if (!$crash_statement)
{
  # accept all errors
  --disable_abort_on_error
  --replace_regex /#sqlx-[0-9a-f_]*/#sqlx-nnnn_nnnn/i
  --eval $fail_statement
  --enable_abort_on_error
  --echo # State after failure
  --replace_result #p# #p#
  --list_files $datadir/test
}

if ($fixup_statement)
{
  set foreign_key_checks= 0;
  eval $fixup_statement;
  set foreign_key_checks= 1;
}

if ($show_statement2)
{
  eval $show_statement2;
}
set session debug_dbug=@save_dbug;

