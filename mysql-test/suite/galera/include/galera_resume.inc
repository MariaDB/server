--echo Resuming node ...
--perl
        my $pid_filename = $ENV{'_SUSPEND_NODE_PIDFILE'};
        my $mysqld_pid = `cat $pid_filename`;
        chomp($mysqld_pid);
        system("kill -18 $mysqld_pid");
        exit(0);
EOF

