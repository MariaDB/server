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
use File::Path qw(make_path remove_tree);
use File::Basename;
use File::Copy;
use POSIX ":sys_wait_h";

# Parse arguments
my $vardir = $ENV{MYSQLTEST_VARDIR} or die "MYSQLTEST_VARDIR not set\n";
my $server_num = shift @ARGV or die "Usage: $0 <server_num> [port] [socket]\n";
my $custom_port = shift @ARGV;
my $custom_socket = shift @ARGV;

# Calculate port (base + 10 + server_num if not custom)
my $base_port = $ENV{MASTER_MYPORT} || 10000;
my $port = $custom_port || ($base_port + 10 + $server_num);
my $socket = $custom_socket || "$vardir/tmp/extra_server_$server_num.sock";

# Create data directory
my $datadir = "$vardir/extra_server_$server_num/data";
my $install_db = "$vardir/install.db";

die "install.db not found at $install_db\n" unless -d $install_db;

# Create parent directory if needed
my $server_dir = "$vardir/extra_server_$server_num";
make_path($server_dir) unless -d $server_dir;

# Copy install.db to new datadir
if (-d $datadir) {
    print "Removing existing datadir: $datadir\n";
    remove_tree($datadir);
}

print "Copying $install_db to $datadir...\n";
# Use cp -a to preserve permissions and attributes
system("cp", "-a", $install_db, $datadir) == 0
    or die "Failed to copy $install_db to $datadir: $!\n";

# Ensure proper permissions on the datadir
system("chmod", "-R", "u+rwX", $datadir) == 0
    or warn "Warning: Failed to set permissions on $datadir\n";

# Start mysqld
my $mysqld = $ENV{MYSQLD} or die "MYSQLD environment variable not set\n";
die "mysqld binary not found at $mysqld\n" unless -x $mysqld;

my $pid_file = "$server_dir/mysqld.pid";
my $log_file = "$vardir/log/extra_server_$server_num.err";
my $general_log_file = "$vardir/log/extra_server_$server_num.log";

# Ensure log directory exists
make_path("$vardir/log") unless -d "$vardir/log";

my @mysqld_args = (
    $mysqld,
    "--no-defaults",
    "--datadir=$datadir",
    "--port=$port",
    "--socket=$socket",
    "--pid-file=$pid_file",
    "--log-error=$log_file",
    "--general-log=1",
    "--general-log-file=$general_log_file",
    "--skip-networking=0",
    "--skip-grant-tables",
    "--key-buffer-size=1M",
    "--sort-buffer-size=256K",
    "--max-heap-table-size=1M",
    "--gdb",
);

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
my $info_file = "$vardir/tmp/extra_server_$server_num.info";
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
