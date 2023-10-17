--echo Killing server ...

# Write file to make mysql-test-run.pl expect the crash, but don't start it
--let $_expect_file_name= `select regexp_replace(@@tmpdir, '^.*/','')`
--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/$_expect_file_name.expect
--exec echo "wait" > $_expect_file_name

# Kill the connected server
--disable_reconnect
--let KILL_NODE_PIDFILE = `SELECT @@pid_file`

--perl
        my $pid_filename = $ENV{'KILL_NODE_PIDFILE'};
        my $mysqld_pid = `cat $pid_filename`;
        chomp($mysqld_pid);
        system("kill -9 $mysqld_pid");
        exit(0);
EOF

--source include/wait_until_disconnected.inc
