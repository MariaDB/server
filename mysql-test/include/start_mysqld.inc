# Include this script only after using shutdown_mysqld.inc
# where $_expect_file_name was initialized.
# Write file to make mysql-test-run.pl start up the server again

# restart_noprint defines how much is printed to the .result file
# if 0 (default) then '# result' and restart_parameters are printed
# if 1 then print #result but not the content of restart_parameters
# if 2 then nothing is printed

if (!$restart_noprint)
{
  --let $restart_noprint=0
}

if ($restart_parameters)
{
  --exec echo "restart: $restart_parameters" > $_expect_file_name
  if (!$restart_noprint)
  {
    --replace_result $MYSQL_TEST_DIR MYSQL_TEST_DIR $MYSQLTEST_VARDIR MYSQLTEST_VARDIR 
    --exec echo "# restart: $restart_parameters"
  }
  if ($restart_noprint == 1)
  {
    --exec echo "# restart: with restart_parameters"
  }
}
if (!$restart_parameters)
{
  --exec echo "restart" > $_expect_file_name
  if ($restart_noprint < 2)
  {
    --exec echo "# restart"
  }
}

# Turn on reconnect
--enable_reconnect

# Call script that will poll the server waiting for it to be back online again
--source include/wait_until_connected_again.inc

# Turn off reconnect again
--disable_reconnect
