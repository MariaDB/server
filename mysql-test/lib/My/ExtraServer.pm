# -*- cperl -*-
# Copyright (c) 2026, MariaDB Corporation.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA

package My::ExtraServer;

#
# Where an "extra server" keeps its files and how it is started.
#
# An extra server is a mysqld instance next to the ones of the test run.
# mtr --replay-server uses extra server EXTRA_SERVER_NUM as the replay server:
# the second server that recorded optimizer contexts are replayed on.
#
# Everything below is shared by the two places that have to agree on it:
# lib/start_extra_server.pl, which starts the server, and
# mariadb-test-run.pl, which prints the very same command line in
# --replay-server-manual mode and later watches and removes those files.
#

use strict;
use warnings;
use My::File::Path;   # mkpath / rmtree / copytree, as the rest of mtr uses
use base qw(Exporter);

our @EXPORT= qw(EXTRA_SERVER_NUM
                extra_server_dir extra_server_datadir
                extra_server_socket extra_server_default_socket
                extra_server_pid_file extra_server_err_log
                extra_server_general_log extra_server_info_file
                extra_server_install_db extra_server_port
                extra_server_mysqld_args extra_server_prepare_datadir
                extra_server_write_info extra_server_read_info);

#
# The replay server is extra server number 1. There is only ever one of it,
# which is why --replay-server refuses to run with --parallel > 1.
#
use constant EXTRA_SERVER_NUM => 1;

# The base port to fall back on when the run has none (MASTER_MYPORT unset),
# and the size of a port group to assume when mtr has not told us
# (MTR_PORT_GROUP_SIZE) - mariadb-test-run.pl's own default.
use constant EXTRA_SERVER_BASE_PORT       => 10000;
use constant EXTRA_SERVER_PORT_GROUP_SIZE => 30;

# Everything of extra server $num lives under this directory ...
sub extra_server_dir     { my ($vardir, $num)= @_; "$vardir/extra_server_$num" }
sub extra_server_datadir { extra_server_dir(@_) . "/data" }
sub extra_server_pid_file{ extra_server_dir(@_) . "/mysqld.pid" }

# ... including the socket, which mtr --replay-server prefers over the default
# below because the tmp directory is cleaned up while tests run.
sub extra_server_socket  { extra_server_dir(@_) . "/mysqld.sock" }

# The socket start_extra_server.pl picks when it is not given one.
sub extra_server_default_socket
{ my ($vardir, $num)= @_; "$vardir/tmp/extra_server_$num.sock" }

# The logs go where all other logs of the run go ...
sub extra_server_err_log
{ my ($vardir, $num)= @_; "$vardir/log/extra_server_$num.err" }
sub extra_server_general_log
{ my ($vardir, $num)= @_; "$vardir/log/extra_server_$num.log" }

# ... and the connection info start_extra_server.pl hands back to its caller
# (HOST=, PORT=, SOCKET=, PID= ... one per line) goes to the tmp directory.
sub extra_server_info_file
{ my ($vardir, $num)= @_; "$vardir/tmp/extra_server_$num.info" }

# The datadir template mysql_install_db() prepared for this run
sub extra_server_install_db { my ($vardir)= @_; "$vardir/install.db" }

#
# The port of extra server $num, counted down from the top of the run's port
# group. My::ConfigFactory::fix_port() hands out the ports of a group from the
# bottom upwards, so a server sitting near the bottom is in the way of any
# configuration that needs more than a handful of ports.
#
sub extra_server_port
{
  my ($base_port, $num)= @_;
  my $group_size= $ENV{MTR_PORT_GROUP_SIZE} || EXTRA_SERVER_PORT_GROUP_SIZE;
  return ($base_port || EXTRA_SERVER_BASE_PORT) + $group_size - $num;
}

#
# The command line of extra server $num. The binary comes first, the way both
# exec() and "gdb --args" want it; a caller that only needs the arguments
# takes a slice.
#
# $for_debugger adds mysqld's --gdb, which is wanted only when a debugger is
# really going to be attached: it clears TEST_CORE_ON_SIGNAL, so a server
# started with it leaves neither a core file nor a stack trace in its error
# log when it crashes - the very things wanted from an unattended run.
#
sub extra_server_mysqld_args
{
  my ($mysqld, $vardir, $num, $port, $socket, $for_debugger)= @_;
  return ($mysqld,
          "--no-defaults",
          "--datadir=" . extra_server_datadir($vardir, $num),
          "--port=$port",
          "--socket=$socket",
          "--pid-file=" . extra_server_pid_file($vardir, $num),
          "--log-error=" . extra_server_err_log($vardir, $num),
          "--general-log=1",
          "--general-log-file=" . extra_server_general_log($vardir, $num),
          "--skip-networking=0",
          "--skip-grant-tables",
          "--key-buffer-size=1M",
          "--sort-buffer-size=256K",
          "--max-heap-table-size=1M",
          ($for_debugger ? ("--gdb") : ()));
}

#
# Give extra server $num a fresh datadir, copied from the install.db of this
# run, and make sure the directories it writes to exist. Returns the datadir,
# dies if it cannot be prepared. $report, if given, is called with progress
# messages (print for a script, mtr_report for mtr).
#
sub extra_server_prepare_datadir
{
  my ($vardir, $num, $report)= @_;
  my $install_db= extra_server_install_db($vardir);
  my $dir= extra_server_dir($vardir, $num);
  my $datadir= extra_server_datadir($vardir, $num);

  die "install.db not found at $install_db\n" unless -d $install_db;

  mkpath($dir) unless -d $dir;
  mkpath("$vardir/log") unless -d "$vardir/log";

  if (-d $datadir) {
    $report->("Removing existing datadir: $datadir") if $report;
    rmtree($datadir);
  }

  $report->("Copying $install_db to $datadir...") if $report;
  # The same copy mariadb-test-run.pl makes for every other mysqld. copytree()
  # makes the copies writable, which install.db itself is not.
  copytree($install_db, $datadir);
  die "Failed to copy $install_db to $datadir\n" unless -d $datadir;

  return $datadir;
}


#
# The connection info of extra server $num: what start_extra_server.pl hands
# back to whoever started it, "KEY=value", one per line. Written and read here
# rather than at the four places that used to know the format.
#
sub extra_server_write_info
{
  my ($vardir, $num, %info)= @_;
  my $file= extra_server_info_file($vardir, $num);

  open my $fh, '>', $file or die "Cannot write $file: $!\n";
  print $fh "$_=$info{$_}\n" for sort keys %info;
  close $fh;
  return $file;
}

# Returns the info as a hash, empty if the file is not there or unreadable
sub extra_server_read_info
{
  my ($vardir, $num)= @_;
  my $file= extra_server_info_file($vardir, $num);
  my %info;

  return () unless -f $file;
  open my $fh, '<', $file or return ();
  while (<$fh>) {
    chomp;
    $info{$1}= $2 if /^(\w+)=(.+)/;
  }
  close $fh;
  return %info;
}

1;
