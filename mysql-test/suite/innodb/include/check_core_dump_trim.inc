# ------------------------------------------------------------------------------
#                              Test Summary
# ------------------------------------------------------------------------------
# This test checks whether the mariadbd core dump file is expected to be TRIMMED
# or NOT TRIMMED based on how memory is mapped by the mariadbd process.
#
# What this script does:
#
# 1. It retrieves MariaDB runtime values:
#      - pid_file (location of mariadbd PID)
#      - innodb_buffer_pool_size
#
# 2. Using the PID file, it finds the running mariadbd process ID.
#
# 3. It reads /proc/<pid>/smaps to inspect the memory regions allocated
#    by the mariadbd process.
#
# 4. From the smaps output, it identifies candidate memory regions that:
#      - are smaller than 192M
#      - contain the "dd" VmFlag (indicating the region is eligible for
#        core dump trimming)
#
# 5. It sums the size of these candidate memory regions.
#
# 6. If the candidate memory is large enough to cover the buffer pool,
#    the script concludes that the buffer pool will not be part of core dump.
#    Otherwise, it reports it to be part of core dump.
#
# Purpose:
# This helps verify whether InnoDB buffer pool will be excluded from the
# core dump, preventing unnecessarily large core files.
# ------------------------------------------------------------------------------

# Get the full path name of the PID file
--let $pid_file= query_get_value(SELECT @@pid_file, @@pid_file, 1)
--let PIDFILE= $pid_file

# Get innodb_buffer_pool_size
--let $innodb_buffer_pool_size= query_get_value(SELECT @@innodb_buffer_pool_size, @@innodb_buffer_pool_size, 1)
--let BUFFER_POOL_SIZE= $innodb_buffer_pool_size

perl;

use strict;
use warnings;

my $pid_file = $ENV{'PIDFILE'} or die "PIDFILE not set";

# Buffer pool size
my $buffer_pool_size = $ENV{'BUFFER_POOL_SIZE'};

# Convert to KB
my $buffer_pool_kb = int($buffer_pool_size / 1024);

# Ignore regions larger than 192M
my $max_region_kb = 192 * 1024;

# Sum of candidate regions
my $candidate_sum_kb = 0;

# Get PID of mariadbd
open(my $fh, '<', $pid_file) || die "Cannot open pid file $pid_file\n";
my $pid = <$fh>;
$pid =~ s/\s//g;
close($fh);

if ($pid eq "") {
  die "Couldn't retrieve PID from PID file.\n";
}

# Open smaps
my $smaps_file = "/proc/$pid/smaps";

open(my $sm, '<', $smaps_file) or die "Cannot open $smaps_file\n";

my $current_size_kb = 0;

while (my $line = <$sm>) {

  if ($line =~ /^Size:\s+(\d+)\s+kB/) {
    $current_size_kb = $1;
  }

  if ($line =~ /^VmFlags:\s+(.*)/) {
    my $flags = $1;

    # Candidate region conditions
    if ($current_size_kb <= $max_region_kb &&
        $flags =~ /\bdd\b/) {
        $candidate_sum_kb += $current_size_kb;
    }
  }
}

close($sm);

if ($candidate_sum_kb >= $buffer_pool_kb) {
  print "Buffer pool not part of core dump.\n";
}
else {
  print "Buffer pool part of core dump.\n";
}

EOF
