# Skip the test unless the filesystem holding $sparse_file_dir stores files
# with holes.  The caller sets $sparse_file_dir to a directory on the
# filesystem where it builds its own sparse file, because vardir and datadir 
# can be on different filesystems.
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

if (!$sparse_file_dir)
{
  --die have_sparse_files.inc requires the caller to set sparse_file_dir
}
# The probe cannot tell a directory it may not write from a filesystem
# without holes, thus a missing directory ends the test rather than skipping
# it.
--file_exists $sparse_file_dir
# mysqltest keeps variables in a case insensitive hash and exports a value
# under whichever spelling created the entry, so the exported name has to
# differ from $sparse_file_dir by more than case.
--let SPARSE_PROBE_DIR= $sparse_file_dir

--error 0,1
perl;
use strict;
use warnings;

my $probe= "$ENV{SPARSE_PROBE_DIR}/sparse_probe";
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
  --skip Requires a filesystem that stores files with holes
}
