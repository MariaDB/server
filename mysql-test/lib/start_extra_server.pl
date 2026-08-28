#!/usr/bin/env perl
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

use strict;
use warnings;
use File::Basename;
use File::Copy;
use POSIX ":sys_wait_h";
use lib dirname(__FILE__);   # mysql-test/lib, for My::ExtraServer
# Where this server keeps its files and how it is started - shared with
# mariadb-test-run.pl, see lib/My/ExtraServer.pm
use My::ExtraServer;

# Parse arguments
my $vardir = $ENV{MYSQLTEST_VARDIR} or die "MYSQLTEST_VARDIR not set\n";
my $server_num = shift @ARGV or die "Usage: $0 <server_num> [port] [socket]\n";
my $custom_port = shift @ARGV;
my $custom_socket = shift @ARGV;

my $port = $custom_port ||
           extra_server_port($ENV{MASTER_MYPORT}, $server_num);
my $socket = $custom_socket ||
             extra_server_default_socket($vardir, $server_num);

# Create data directory
my $datadir = extra_server_prepare_datadir($vardir, $server_num,
                                           sub { print "$_[0]\n" });

# Start mysqld
my $mysqld = $ENV{MYSQLD} or die "MYSQLD environment variable not set\n";
die "mysqld binary not found at $mysqld\n" unless -x $mysqld;

my $pid_file = extra_server_pid_file($vardir, $server_num);
my $log_file = extra_server_err_log($vardir, $server_num);
my $general_log_file = extra_server_general_log($vardir, $server_num);

my @mysqld_args = extra_server_mysqld_args($mysqld, $vardir, $server_num,
                                           $port, $socket);

print "Starting mysqld on port $port with socket $socket...\n";
print "Command: " . join(" ", @mysqld_args) . "\n";

# Fork and start server
my $pid = fork();
die "Fork failed: $!\n" unless defined $pid;

if ($pid == 0) {
    # Child process - start server
    # Redirect stdout/stderr to log file
    open STDOUT, '>>', $log_file or die "Cannot redirect STDOUT: $!\n";
    open STDERR, '>>', $log_file or die "Cannot redirect STDERR: $!\n";
    exec(@mysqld_args) or die "Failed to exec mysqld: $!\n";
}

# Parent - wait for server to be ready
print "Server process started with PID $pid\n";
print "Waiting for server to be ready...\n";

# Wait for socket file to appear (up to 30 seconds)
my $max_wait = 30;
my $waited = 0;
while ($waited < $max_wait) {
    if (-S $socket) {
        print "Socket file created: $socket\n";
        last;
    }
    sleep 1;
    $waited++;
    
    # Check if process is still alive
    my $result = waitpid($pid, WNOHANG);
    if ($result == $pid) {
        die "Server process died during startup. Check $log_file for errors.\n";
    }
}

if ($waited >= $max_wait) {
    kill 'TERM', $pid;
    die "Timeout waiting for server to start. Check $log_file for errors.\n";
}

# Additional wait for server to be fully ready
sleep 2;

# Write connection info to file
my $info_file = extra_server_info_file($vardir, $server_num);
open my $fh, '>', $info_file or die "Cannot write $info_file: $!\n";
print $fh "HOST=127.0.0.1\n";
print $fh "PORT=$port\n";
print $fh "SOCKET=$socket\n";
print $fh "DATADIR=$datadir\n";
print $fh "PID=$pid\n";
print $fh "PID_FILE=$pid_file\n";
print $fh "LOG_FILE=$log_file\n";
print $fh "GENERAL_LOG_FILE=$general_log_file\n";
close $fh;

print "Extra server $server_num started successfully\n";
print "Connection info written to $info_file\n";
print "  Host: 127.0.0.1\n";
print "  Port: $port\n";
print "  Socket: $socket\n";
print "  Datadir: $datadir\n";
print "  General log: $general_log_file\n";

exit 0;
