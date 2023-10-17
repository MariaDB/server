--let $_expect_file_name= `select regexp_replace(@@tmpdir, '^.*/','')`
--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/$_expect_file_name.expect

# There should be a debug crash after using this .inc file
--exec echo "wait" > $_expect_file_name
