# Skip the test unless the vardir filesystem stores files with holes.
#
# A test that builds a large file by seeking past its end and writing a
# single byte relies on the skipped range staying unallocated.  HFS on
# macOS allocates every block of that range, so the file consumes its full
# apparent size and the write fails with ENOSPC once the range exceeds the
# free space on the volume.
#
# The probe writes one byte 64MB past the start of an empty file and compares
# the allocated block count against that offset.  A filesystem that leaves a
# hole allocates only the block holding the byte.  The offset has to stay well
# above 16MB, since APFS fills the range of a file smaller than that rather
# than recording a hole.

--error 0,1
perl;
use strict;
use warnings;

my $probe= "$ENV{MYSQLTEST_VARDIR}/tmp/sparse_probe";
my $offset= 64 * 1024 * 1024;
my $sparse= 0;

if (open(my $fh, '>', $probe))
{
  binmode $fh;
  if (seek($fh, $offset, 0) and print($fh chr(0)) and close($fh))
  {
    my $blocks= (stat $probe)[12];
    $sparse= 1 if defined($blocks) and $blocks * 512 < $offset;
  }
}
unlink $probe;
exit($sparse ? 0 : 1);
EOF
if ($errno)
{
  --skip Requires a vardir filesystem that stores files with holes
}
