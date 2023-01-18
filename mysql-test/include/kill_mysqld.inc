--let $_expect_file_name= `select regexp_replace(@@tmpdir, '^.*/','')`
--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/$_expect_file_name.expect

--echo # Kill the server
--exec echo "wait" > $_expect_file_name
--shutdown_server 0
--source include/wait_until_disconnected.inc
