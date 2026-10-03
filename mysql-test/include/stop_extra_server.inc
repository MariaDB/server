# ==== Purpose ====
#
# Stop an extra server instance that was started with start_extra_server.inc
#
# ==== Usage ====
#
# --let $extra_server_num= 1
# --source include/stop_extra_server.inc
#
# ==== Parameters ====
#
# $extra_server_num
#   Required. The number of the extra server to stop (same number used
#   when starting it with start_extra_server.inc).
#

if (!$extra_server_num)
{
  --die extra_server_num must be set before sourcing stop_extra_server.inc
}

--let $include_filename= stop_extra_server.inc [server $extra_server_num]
--source include/begin_include_file.inc

# Export mysqltest variable to environment for Perl
--let extra_server_num_env= $extra_server_num

--perl
  use strict;
  use warnings;
  use lib "$ENV{MYSQL_TEST_DIR}/lib";
  # Where the extra server keeps its files, and the format of the info file
  # start_extra_server.pl left behind - see lib/My/ExtraServer.pm
  use My::ExtraServer;

  my $server_num = $ENV{extra_server_num_env} or die "extra_server_num not set";
  my $vardir = $ENV{MYSQLTEST_VARDIR} or die "MYSQLTEST_VARDIR not set";

  my %info = extra_server_read_info($vardir, $server_num);
  my $info_file = extra_server_info_file($vardir, $server_num);
  unless (%info) {
    print "Warning: no info file for extra server $server_num\n";
    print "Server may not be running or already stopped.\n";
    exit 0;
  }

  my $pid = $info{PID};
  unless ($pid) {
    print "Warning: no PID in the info file of extra server $server_num\n";
    unlink $info_file;
    exit 0;
  }

  # Nothing volatile - no pid, no paths - goes to stdout: this runs inside a
  # test and everything printed here lands in its result.

  # The server is not a child of mysqltest, so waitpid() cannot poll it -
  # "kill 0" (signal 0, an existence check) is used instead.
  if (kill 0, $pid) {
    kill 'TERM', $pid;

    my $max_wait = 10;
    for my $waited (1 .. $max_wait) {
      last unless kill(0, $pid);
      sleep 1;
    }

    if (kill 0, $pid) {
      print "Server did not stop gracefully, sending SIGKILL\n";
      kill 'KILL', $pid;
      sleep 1;
    }
  }

  # Cleanup files
  unlink $info_file;
  unlink $info{PID_FILE} if $info{PID_FILE};
  unlink $info{SOCKET} if $info{SOCKET} && -S $info{SOCKET};

  print "Extra server $server_num stopped\n";
EOF

--let $include_filename= stop_extra_server.inc [server $extra_server_num]
--source include/end_include_file.inc
