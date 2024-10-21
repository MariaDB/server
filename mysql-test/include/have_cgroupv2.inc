--source include/linux.inc
--error 0,1
perl;
use strict;
use warnings;
use File::Spec;

# Read the cgroup file
my $cgroup_file = '/proc/self/cgroup';
open my $fh, '<', $cgroup_file or exit 1;
my $line = <$fh>;
close $fh;

# Process the cgroup file content
$line =~ s/^0:://;
chomp($line);

# Construct the path
my $path = File::Spec->catdir('/sys/fs/cgroup', $line, 'memory.pressure');

# Check if the file is writable exists
exit 0 if -w $path;
exit 1;
EOF
if ($errno)
{
  --skip Requires cgroupv2
}
