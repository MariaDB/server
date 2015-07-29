#
# This macro suspends the current node
#

--let _SUSPEND_NODE_PIDFILE = `SELECT @@pid_file`
--echo Suspending node ...

--perl
        my $pid_filename = $ENV{'_SUSPEND_NODE_PIDFILE'};
        my $mysqld_pid = `cat $pid_filename`;
        chomp($mysqld_pid);
        system("kill -19 $mysqld_pid");
        exit(0);
EOF
