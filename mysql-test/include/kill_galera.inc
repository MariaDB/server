# Kill the server the current connection is on, and wait until its process
# is really gone.
#
# USAGE
#
#    [--let $kill_signal = TERM]     # defaults to KILL
#    --source include/kill_galera.inc
#
# $kill_signal applies to this call only and is reset afterwards. Give the
# signal by name and never by number: signal numbers are not portable, and
# "kill -9" fails as an unknown signal on FreeBSD.
#
# On return the process no longer exists, so its pid file, ports and data
# directory are free: the node can be restarted, or its grastate.dat read,
# straight away. Waiting only for the connection to drop is not enough for
# that. The connection dies as soon as the server stops serving it, which for
# a signal that lets mysqld shut down gracefully is long before the process
# actually exits, and tests used to paper over the difference with a --sleep.
#
# What this does NOT wait for is the rest of the cluster noticing. A test that
# needs the surviving nodes to have processed the loss of this one has to wait
# for that itself, on a surviving node, e.g. for wsrep_cluster_size to drop or
# for wsrep_cluster_status to become non-Primary.

--echo Killing server ...

if (!$kill_signal)
{
--let $kill_signal = KILL
}

# Write file to make mysql-test-run.pl expect the crash, but don't start it
--let $_expect_file_name= `select regexp_replace(@@tmpdir, '^.*/','')`
--let $_expect_file_name= $MYSQLTEST_VARDIR/tmp/$_expect_file_name.expect
--write_line wait $_expect_file_name

# Kill the connected server
--disable_reconnect
--let KILL_SIGNAL_VALUE = $kill_signal
--let KILL_NODE_PIDFILE = `SELECT @@pid_file`

--perl
        my $kill_sig = $ENV{'KILL_SIGNAL_VALUE'};
        my $pid_filename = $ENV{'KILL_NODE_PIDFILE'};
        my $mysqld_pid = `cat $pid_filename`;
        chomp($mysqld_pid);
        die "No pid found in $pid_filename\n"
                unless $mysqld_pid =~ /^\d+$/;
        system("kill -s $kill_sig $mysqld_pid");

        # Wait for the process to be gone, and not merely for it to stop
        # serving connections. my_safe_process is its parent and is sitting
        # in waitpid() on it, so it is reaped as soon as it exits and kill(0)
        # stops finding it.
        my $waited = 0;
        while (kill(0, $mysqld_pid)) {
                die "Server pid $mysqld_pid did not exit\n" if $waited >= 60;
                select(undef, undef, undef, 0.1);
                $waited += 0.1;
        }
        exit(0);
EOF

--let $kill_signal =

--source include/wait_until_disconnected.inc
