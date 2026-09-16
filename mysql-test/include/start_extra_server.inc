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
# After sourcing this file, the following mysqltest variables will be set:
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
#   Optional. Custom port number for the server. If not specified, the port
#   is taken from the top of the run's port group - see extra_server_port()
#   in lib/My/ExtraServer.pm.
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
  use lib "$ENV{MYSQL_TEST_DIR}/lib";
  # Where the extra server keeps its files, and the format of the info file it
  # hands back - see lib/My/ExtraServer.pm
  use My::ExtraServer;

  my $server_num = $ENV{extra_server_num_env} or die "extra_server_num not set";
  my $vardir = $ENV{MYSQLTEST_VARDIR} or die "MYSQLTEST_VARDIR not set";
  my $port = $ENV{extra_server_port_env} || "";
  my $socket = $ENV{extra_server_socket_env} || "";

  my $script = "$ENV{MYSQL_TEST_DIR}/lib/start_extra_server.pl";
  die "Script not found: $script\n" unless -f $script;

  # No shell: a vardir or socket path with a space in it stays one argument,
  # and the empty placeholders keep $socket out of the port position.
  my @cmd = ($^X, $script, $server_num, $port, $socket);

  # The script is chatty and prints volatile paths and pids, none of which
  # belongs in a test result - keep its output back unless it fails.
  my $out = "";
  if (open my $ph, '-|', @cmd) {
    local $/;
    $out = <$ph> // "";
    close $ph;
  }
  if ($?) {
    print "Failed to start extra server $server_num:\n$out";
    die "start_extra_server.pl exited with status $?\n";
  }

  my %info = extra_server_read_info($vardir, $server_num);
  unless (%info) {
    print $out;
    die "Info file not found: " .
        extra_server_info_file($vardir, $server_num) . "\n";
  }

  # Hand the connection info back as mysqltest variables. --perl runs in a
  # process of its own, so %ENV changes made here would be lost; the file of
  # "let" statements written below is sourced by the caller instead.
  open my $fh, '>', "$ENV{MYSQL_TMP_DIR}/extra_server_vars.inc"
    or die "Cannot write $ENV{MYSQL_TMP_DIR}/extra_server_vars.inc: $!\n";
  print $fh "let \$EXTRA_SERVER_$_= $info{$_};\n"
    for grep { defined $info{$_} } qw(PORT SOCKET DATADIR PID);
  close $fh;

EOF

--source $MYSQL_TMP_DIR/extra_server_vars.inc
--remove_file $MYSQL_TMP_DIR/extra_server_vars.inc

--let $include_filename= start_extra_server.inc [server $extra_server_num]
--source include/end_include_file.inc
