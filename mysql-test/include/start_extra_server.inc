# ==== Purpose ====
#
# Start an additional mysqld server instance while mysql-test-run is running.
# This creates a new data directory (copied from var/install.db), picks a
# non-conflicting port and socket, and starts the server.
#
# ==== Usage ====
#
# --let $extra_server_num= 1
# [--let $extra_server_port= 13307]
# [--let $extra_server_socket= /path/to/socket]
# --source include/start_extra_server.inc
#
# After sourcing this file, the following variables will be set:
#   $EXTRA_SERVER_PORT    - Port number of the extra server
#   $EXTRA_SERVER_SOCKET  - Socket path of the extra server
#   $EXTRA_SERVER_DATADIR - Data directory of the extra server
#   $EXTRA_SERVER_PID     - Process ID of the extra server
#
# You can then connect to the server using:
#   --connect (conn_name, 127.0.0.1, root, , test, $EXTRA_SERVER_PORT)
#
# ==== Parameters ====
#
# $extra_server_num
#   Required. A unique number identifying this extra server instance.
#   Must be unique across all extra servers started in the same test.
#
# $extra_server_port
#   Optional. Custom port number for the server. If not specified,
#   the port will be auto-calculated as MASTER_MYPORT + 10 + server_num.
#
# $extra_server_socket
#   Optional. Custom socket path. If not specified, will use
#   $MYSQLTEST_VARDIR/tmp/extra_server_N.sock
#

if (!$extra_server_num)
{
  --die extra_server_num must be set before sourcing start_extra_server.inc
}

--let $include_filename= start_extra_server.inc [server $extra_server_num]
--source include/begin_include_file.inc

# Export mysqltest variables to environment for Perl
--let extra_server_num_env= $extra_server_num
--let extra_server_port_env= $extra_server_port
--let extra_server_socket_env= $extra_server_socket

--perl
  use strict;
  use warnings;
  
  my $server_num = $ENV{extra_server_num_env} or die "extra_server_num not set";
  my $port = $ENV{extra_server_port_env} || "";
  my $socket = $ENV{extra_server_socket_env} || "";
  
  my $script = "$ENV{MYSQL_TEST_DIR}/lib/start_extra_server.pl";
  die "Script not found: $script\n" unless -f $script;
  
  my $cmd = "perl $script $server_num";
  $cmd .= " $port" if $port;
  $cmd .= " $socket" if $socket;
  
  print "Executing: $cmd\n";
  my $result = system($cmd);
  if ($result != 0) {
    die "Failed to start extra server $server_num (exit code: $result)\n";
  }
EOF

# Read connection info from the info file
--let $extra_server_info_file= $MYSQLTEST_VARDIR/tmp/extra_server_$extra_server_num.info

--perl
  use strict;
  use warnings;
  
  my $file = $ENV{extra_server_info_file};
  die "Info file not found: $file\n" unless -f $file;
  
  open my $fh, '<', $file or die "Cannot read $file: $!";
  my %info;
  while (<$fh>) {
    chomp;
    if (/^(\w+)=(.+)/) {
      $info{$1} = $2;
    }
  }
  close $fh;
  
  # Export to environment for mysqltest
  $ENV{EXTRA_SERVER_PORT} = $info{PORT} if $info{PORT};
  $ENV{EXTRA_SERVER_SOCKET} = $info{SOCKET} if $info{SOCKET};
  $ENV{EXTRA_SERVER_DATADIR} = $info{DATADIR} if $info{DATADIR};
  $ENV{EXTRA_SERVER_PID} = $info{PID} if $info{PID};
  
  print "Extra server $ENV{extra_server_num_env} connection info:\n";
  print "  PORT=$info{PORT}\n" if $info{PORT};
  print "  SOCKET=$info{SOCKET}\n" if $info{SOCKET};
EOF

--let $include_filename= start_extra_server.inc [server $extra_server_num]
--source include/end_include_file.inc
