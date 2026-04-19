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
  use POSIX ":sys_wait_h";
  
  my $server_num = $ENV{extra_server_num_env} or die "extra_server_num not set";
  my $vardir = $ENV{MYSQLTEST_VARDIR} or die "MYSQLTEST_VARDIR not set";
  my $info_file = "$vardir/tmp/extra_server_$server_num.info";
  
  unless (-f $info_file) {
    print "Warning: Info file not found: $info_file\n";
    print "Server may not be running or already stopped.\n";
    exit 0;
  }
  
  # Read PID and other info
  open my $fh, '<', $info_file or die "Cannot read $info_file: $!";
  my ($pid, $pid_file, $socket);
  while (<$fh>) {
    chomp;
    $pid = $1 if /^PID=(.+)/;
    $pid_file = $1 if /^PID_FILE=(.+)/;
    $socket = $1 if /^SOCKET=(.+)/;
  }
  close $fh;
  
  unless ($pid) {
    print "Warning: No PID found in $info_file\n";
    unlink $info_file;
    exit 0;
  }
  
  print "Stopping extra server $server_num (PID: $pid)...\n";
  
  # Send SIGTERM to server
  if (kill 0, $pid) {
    kill 'TERM', $pid;
    
    # Wait for process to exit (up to 10 seconds)
    my $max_wait = 10;
    my $waited = 0;
    while ($waited < $max_wait) {
      my $result = waitpid($pid, WNOHANG);
      if ($result == $pid || $result == -1) {
        print "Server process $pid has exited\n";
        last;
      }
      sleep 1;
      $waited++;
    }
    
    # If still running, send SIGKILL
    if ($waited >= $max_wait && kill 0, $pid) {
      print "Server did not stop gracefully, sending SIGKILL...\n";
      kill 'KILL', $pid;
      sleep 1;
    }
  } else {
    print "Process $pid is not running\n";
  }
  
  # Cleanup files
  unlink $info_file if -f $info_file;
  unlink $pid_file if $pid_file && -f $pid_file;
  unlink $socket if $socket && -S $socket;
  
  print "Extra server $server_num stopped\n";
EOF

--let $include_filename= stop_extra_server.inc [server $extra_server_num]
--source include/end_include_file.inc
