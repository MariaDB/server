# ==== Purpose ====
#
# send kill signal to mysqld process.
#
# ==== Usage ====
#
# [--let $_kill_signal= 9]
# --source include/send_kill_to_mysqld.inc
#
# Parameters:
#
#   $_kill_signal
#     Which signal should be sent to mysqld. (default: 9 / SIGKILL)

if ($_kill_signal == '')
{
  --let $_kill_signal = 9
}
--echo Sending kill signal $_kill_signal to mysqld ...

# Write file to make mysql-test-run.pl expect the crash, but don't start it
--source include/expect_crash.inc

# Kill the connected server
--disable_reconnect
--let KILL_NODE_PIDFILE = `SELECT @@pid_file`
--let KILL_SIGNAL = $_kill_signal
--perl
  my $pid_filename = $ENV{'KILL_NODE_PIDFILE'};
  my $mysqld_pid = `cat $pid_filename`;
  my $kill_signal = $ENV{'KILL_SIGNAL'};
  chomp($mysqld_pid);
  chomp($kill_signal);
  system("kill -$kill_signal $mysqld_pid");
  exit(0);
EOF

--source include/wait_until_disconnected.inc
